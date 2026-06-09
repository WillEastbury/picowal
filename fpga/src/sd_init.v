`timescale 1ns / 1ps
//============================================================================
// sd_init.v — SD card SPI-mode initialization FSM for iCE40 HX8K
//
// Sequence: power-up clocks → CMD0 → CMD8 → ACMD41 loop → CMD58 → ready
//
// Architecture: shared SEND_CMD / POLL_R1 / READ_TAIL engine with a
// cmd_phase register that tracks which SD command is active.  S_DESELECT
// prepares the next command and sends its first byte before entering
// SEND_CMD, avoiding re-entry race conditions.
//============================================================================

module sd_init (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       start,
    // SPI master control
    output reg        spi_start,
    output reg  [7:0] spi_tx_data,
    output reg        spi_cs_assert,
    input  wire [7:0] spi_rx_data,
    input  wire       spi_done,
    // Status
    output reg        ready,
    output reg        card_sdhc,
    output reg        error,
    output reg        busy
);

    // ── State encoding ──────────────────────────────────────────────────
    localparam [3:0]
        S_IDLE      = 4'd0,
        S_POWER_CLK = 4'd1,
        S_SEND_CMD  = 4'd2,  // shared: sends bytes 1..5 from cmd_buf
        S_POLL_R1   = 4'd3,  // shared: polls for non-0xFF R1
        S_READ_TAIL = 4'd4,  // shared: reads 4 trailing bytes into resp_buf
        S_PROCESS   = 4'd5,  // dispatch on cmd_phase after R1/tail received
        S_DESELECT  = 4'd6,  // CS-high gap; sets up next command on exit
        S_DONE      = 4'd7,
        S_ERROR     = 4'd8;

    // ── Command phase ───────────────────────────────────────────────────
    localparam [2:0]
        PH_CMD0   = 3'd0,
        PH_CMD8   = 3'd1,
        PH_CMD55  = 3'd2,
        PH_ACMD41 = 3'd3,
        PH_CMD58  = 3'd4,
        PH_DONE   = 3'd5;

    reg [3:0]  state;
    reg [2:0]  cmd_phase;     // which SD command we're processing

    // ── Command buffer (6 bytes) and byte index ─────────────────────────
    reg [47:0] cmd_buf;
    reg [2:0]  cmd_idx;       // 0..5 — first byte sent before SEND_CMD

    // ── Response ────────────────────────────────────────────────────────
    reg [7:0]  r1;            // captured R1 byte
    reg [31:0] resp_buf;      // R7/R3 trailing 4 bytes (MSB-first)
    reg [1:0]  tail_cnt;      // remaining tail bytes (counts 3→0)
    reg [3:0]  poll_cnt;      // R1 poll attempts

    // ── Power-up clock counter (10 bytes = 80 clocks) ───────────────────
    reg [3:0]  pwr_cnt;

    // ── SD version & ACMD41 timeout ─────────────────────────────────────
    reg        sd_v2;
    reg [23:0] acmd41_timer;
    localparam ACMD41_TIMEOUT = 24'hFF_FFFF;

    // ── Helper: initiate one SPI byte ───────────────────────────────────
    task spi_send;
        input [7:0] d;
        input       cs;
        begin
            spi_tx_data   <= d;
            spi_cs_assert <= cs;
            spi_start     <= 1'b1;
        end
    endtask

    // ── Helper: load cmd_buf and send the first byte (CS asserted) ──────
    // Must be called exactly once before transitioning to S_SEND_CMD.
    task start_cmd;
        input [47:0] cmd;
        begin
            cmd_buf       <= cmd;
            cmd_idx       <= 3'd0;
            poll_cnt      <= 4'd0;
            spi_tx_data   <= cmd[47:40];
            spi_cs_assert <= 1'b1;
            spi_start     <= 1'b1;
        end
    endtask

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            cmd_phase    <= PH_CMD0;
            spi_start    <= 1'b0;
            spi_tx_data  <= 8'hFF;
            spi_cs_assert<= 1'b0;
            ready        <= 1'b0;
            card_sdhc    <= 1'b0;
            error        <= 1'b0;
            busy         <= 1'b0;
            cmd_buf      <= 48'd0;
            cmd_idx      <= 3'd0;
            r1           <= 8'd0;
            resp_buf     <= 32'd0;
            tail_cnt     <= 2'd0;
            poll_cnt     <= 4'd0;
            pwr_cnt      <= 4'd0;
            sd_v2        <= 1'b0;
            acmd41_timer <= 24'd0;
        end else begin
            spi_start <= 1'b0;  // default: one-cycle pulse

            case (state)

            // ────────────────────────────────────────────────────────────
            // IDLE — wait for start pulse
            // ────────────────────────────────────────────────────────────
            S_IDLE: begin
                ready     <= 1'b0;
                error     <= 1'b0;
                card_sdhc <= 1'b0;
                if (start) begin
                    busy         <= 1'b1;
                    pwr_cnt      <= 4'd0;
                    sd_v2        <= 1'b0;
                    acmd41_timer <= 24'd0;
                    spi_send(8'hFF, 1'b0);   // CS deasserted
                    state        <= S_POWER_CLK;
                end
            end

            // ────────────────────────────────────────────────────────────
            // POWER_UP — send ≥74 clocks (10 × 0xFF) with CS deasserted
            // ────────────────────────────────────────────────────────────
            S_POWER_CLK: begin
                if (spi_done) begin
                    if (pwr_cnt == 4'd9) begin
                        // 80 clocks done → send CMD0
                        cmd_phase <= PH_CMD0;
                        start_cmd({8'h40, 8'h00, 8'h00, 8'h00, 8'h00, 8'h95});
                        state     <= S_SEND_CMD;
                    end else begin
                        pwr_cnt <= pwr_cnt + 4'd1;
                        spi_send(8'hFF, 1'b0);
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // SEND_CMD — shared: send remaining 5 bytes of 6-byte command
            // The first byte was already sent by start_cmd / S_DESELECT.
            // ────────────────────────────────────────────────────────────
            S_SEND_CMD: begin
                if (spi_done) begin
                    if (cmd_idx < 3'd5) begin
                        cmd_idx <= cmd_idx + 3'd1;
                        spi_cs_assert <= 1'b1;
                        spi_start     <= 1'b1;
                        case (cmd_idx + 3'd1)
                            3'd1: spi_tx_data <= cmd_buf[39:32];
                            3'd2: spi_tx_data <= cmd_buf[31:24];
                            3'd3: spi_tx_data <= cmd_buf[23:16];
                            3'd4: spi_tx_data <= cmd_buf[15:8];
                            3'd5: spi_tx_data <= cmd_buf[7:0];
                            default: spi_tx_data <= 8'hFF;
                        endcase
                    end else begin
                        // All 6 bytes sent → poll for R1
                        poll_cnt <= 4'd0;
                        spi_send(8'hFF, 1'b1);
                        state    <= S_POLL_R1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // POLL_R1 — shared: poll for non-0xFF response byte
            // For CMD8/CMD58, dispatches directly to READ_TAIL when needed.
            // ────────────────────────────────────────────────────────────
            S_POLL_R1: begin
                if (spi_done) begin
                    if (spi_rx_data != 8'hFF) begin
                        r1 <= spi_rx_data;
                        // Commands with trailing bytes go to READ_TAIL
                        case (cmd_phase)
                            PH_CMD8: begin
                                if (spi_rx_data[2]) begin
                                    // Illegal command → SD v1, no tail
                                    sd_v2 <= 1'b0;
                                    state <= S_PROCESS;
                                end else begin
                                    // SD v2 → read 4-byte R7 tail
                                    sd_v2    <= 1'b1;
                                    tail_cnt <= 2'd3;
                                    resp_buf <= 32'd0;
                                    spi_send(8'hFF, 1'b1);
                                    state    <= S_READ_TAIL;
                                end
                            end
                            PH_CMD58: begin
                                if (spi_rx_data != 8'h00) begin
                                    state <= S_ERROR;
                                end else begin
                                    // Read 4-byte R3 (OCR) tail
                                    tail_cnt <= 2'd3;
                                    resp_buf <= 32'd0;
                                    spi_send(8'hFF, 1'b1);
                                    state    <= S_READ_TAIL;
                                end
                            end
                            default: state <= S_PROCESS;
                        endcase
                    end else if (poll_cnt == 4'd9) begin
                        state <= S_ERROR;
                    end else begin
                        poll_cnt <= poll_cnt + 4'd1;
                        spi_send(8'hFF, 1'b1);
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // READ_TAIL — shared: read 4 trailing response bytes
            // ────────────────────────────────────────────────────────────
            S_READ_TAIL: begin
                if (spi_done) begin
                    resp_buf <= {resp_buf[23:0], spi_rx_data};
                    if (tail_cnt == 2'd0) begin
                        state <= S_PROCESS;
                    end else begin
                        tail_cnt <= tail_cnt - 2'd1;
                        spi_send(8'hFF, 1'b1);
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // PROCESS — interpret R1/tail for each command phase and
            //           decide the next phase.  Always deselects before
            //           the next command; S_DESELECT sets up cmd_buf.
            // ────────────────────────────────────────────────────────────
            S_PROCESS: begin
                case (cmd_phase)

                PH_CMD0: begin
                    if (r1 == 8'h01) begin
                        cmd_phase <= PH_CMD8;
                        spi_send(8'hFF, 1'b0);
                        state     <= S_DESELECT;
                    end else
                        state <= S_ERROR;
                end

                PH_CMD8: begin
                    if (!sd_v2) begin
                        // SD v1 — skip CMD58, go straight to ACMD41
                        cmd_phase <= PH_CMD55;
                        spi_send(8'hFF, 1'b0);
                        state     <= S_DESELECT;
                    end else begin
                        // SD v2 — verify echo pattern
                        if (resp_buf[11:0] != 12'h1AA)
                            state <= S_ERROR;
                        else begin
                            cmd_phase <= PH_CMD55;
                            spi_send(8'hFF, 1'b0);
                            state     <= S_DESELECT;
                        end
                    end
                end

                PH_CMD55: begin
                    // R1 received for CMD55 — send ACMD41 next
                    cmd_phase <= PH_ACMD41;
                    spi_send(8'hFF, 1'b0);
                    state     <= S_DESELECT;
                end

                PH_ACMD41: begin
                    if (r1 == 8'h00) begin
                        // Card ready
                        if (sd_v2) begin
                            cmd_phase <= PH_CMD58;
                        end else begin
                            card_sdhc <= 1'b0;
                            cmd_phase <= PH_DONE;
                        end
                        spi_send(8'hFF, 1'b0);
                        state <= S_DESELECT;
                    end else if (r1 == 8'h01) begin
                        // Still initializing — retry CMD55+ACMD41
                        if (acmd41_timer >= ACMD41_TIMEOUT)
                            state <= S_ERROR;
                        else begin
                            acmd41_timer <= acmd41_timer + 24'd1;
                            cmd_phase    <= PH_CMD55;
                            spi_send(8'hFF, 1'b0);
                            state        <= S_DESELECT;
                        end
                    end else
                        state <= S_ERROR;
                end

                PH_CMD58: begin
                    // OCR byte 0 = resp_buf[31:24]; CCS = bit 30 = byte0[6]
                    card_sdhc <= resp_buf[30];
                    cmd_phase <= PH_DONE;
                    spi_send(8'hFF, 1'b0);
                    state     <= S_DESELECT;
                end

                default: state <= S_ERROR;

                endcase
            end

            // ────────────────────────────────────────────────────────────
            // DESELECT — send one 0xFF with CS deasserted to provide the
            //            required 8-clock gap.  On completion, either set
            //            up the next command (start_cmd → SEND_CMD) or
            //            finish (→ DONE).
            // ────────────────────────────────────────────────────────────
            S_DESELECT: begin
                if (spi_done) begin
                    case (cmd_phase)
                        PH_CMD8: begin
                            start_cmd({8'h48, 8'h00, 8'h00, 8'h01, 8'hAA, 8'h87});
                            state <= S_SEND_CMD;
                        end
                        PH_CMD55: begin
                            start_cmd({8'h77, 8'h00, 8'h00, 8'h00, 8'h00, 8'h65});
                            state <= S_SEND_CMD;
                        end
                        PH_ACMD41: begin
                            start_cmd({8'h69, 8'h40, 8'h00, 8'h00, 8'h00, 8'h77});
                            state <= S_SEND_CMD;
                        end
                        PH_CMD58: begin
                            start_cmd({8'h7A, 8'h00, 8'h00, 8'h00, 8'h00, 8'hFD});
                            state <= S_SEND_CMD;
                        end
                        PH_DONE: begin
                            state <= S_DONE;
                        end
                        default: state <= S_ERROR;
                    endcase
                end
            end

            // ────────────────────────────────────────────────────────────
            // Terminal states
            // ────────────────────────────────────────────────────────────
            S_DONE: begin
                ready         <= 1'b1;
                busy          <= 1'b0;
                spi_cs_assert <= 1'b0;
            end

            S_ERROR: begin
                error         <= 1'b1;
                busy          <= 1'b0;
                spi_cs_assert <= 1'b0;
            end

            default: state <= S_ERROR;

            endcase
        end
    end

endmodule
