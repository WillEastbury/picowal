// SATA PHY Layer Stub with OOB Controller
// Target: ECP5 FPGA (DCU SERDES stubbed for simulation)
//
// OOB sequence per SATA spec:
//   Host sends COMRESET → Device replies COMINIT →
//   Host sends COMWAKE  → Device replies COMWAKE →
//   Host sends D10.2/ALIGN → Device sends ALIGN →
//   Link ready (Gen1 1.5 Gbps)

module sata_phy #(
    // OOB burst / gap counts (in clk cycles).
    // Real hardware: COMRESET/COMINIT = 6 bursts, 160-UI burst, 480-UI gap
    //                COMWAKE           = 6 bursts, 160-UI burst, 160-UI gap
    // At 150 MHz with Gen1 (1.5 GHz, 1 UI = 0.667 ns):
    //   160 UI ≈ 107 ns ≈ 16 clocks;  480 UI ≈ 320 ns ≈ 48 clocks
    // Short mode shrinks everything for fast simulation.
    parameter OOB_SHORT       = 0,
    parameter BURST_LEN       = OOB_SHORT ? 4  : 16,
    parameter COMRESET_GAP    = OOB_SHORT ? 4  : 48,
    parameter COMWAKE_GAP     = OOB_SHORT ? 4  : 16,
    parameter OOB_BURST_COUNT = 6,
    parameter COMINIT_TIMEOUT = OOB_SHORT ? 64 : 1024,
    parameter COMWAKE_TIMEOUT = OOB_SHORT ? 64 : 1024,
    parameter ALIGN_TIMEOUT   = OOB_SHORT ? 64 : 1024,
    parameter RETRY_LIMIT     = 3
) (
    input  wire        clk,
    input  wire        rst_n,

    // --- Stubbed SERDES interface ---
    output reg  [31:0] tx_data,
    output reg  [3:0]  tx_charisk,
    output reg         tx_comreset,
    output reg         tx_comwake,
    input  wire [31:0] rx_data,
    input  wire [3:0]  rx_charisk,
    input  wire        rx_cominit,
    input  wire        rx_comwake,
    input  wire        rx_byte_aligned,

    // --- Link layer interface ---
    output reg         phy_ready,
    output reg  [1:0]  phy_speed,

    input  wire [31:0] link_tx_data,
    input  wire [3:0]  link_tx_isk,
    input  wire        link_tx_valid,
    output wire [31:0] link_rx_data,
    output wire [3:0]  link_rx_isk,
    output wire        link_rx_valid
);

    // SATA primitives (after 8b10b, K28.5 based)
    localparam [31:0] PRIM_ALIGN  = 32'h7B4A_4ABC; // K28.5 D10.2 D10.2 D27.3
    localparam [3:0]  ALIGN_ISK   = 4'b0001;        // first byte is K28.5
    localparam [31:0] PRIM_SYNC   = 32'hB538_7575;
    localparam [3:0]  SYNC_ISK    = 4'b0001;

    // OOB state machine
    localparam [2:0] ST_RESET        = 3'd0,
                     ST_WAIT_COMINIT = 3'd1,
                     ST_SEND_COMWAKE = 3'd2,
                     ST_WAIT_COMWAKE = 3'd3,
                     ST_WAIT_ALIGN   = 3'd4,
                     ST_READY        = 3'd5;

    reg [2:0]  state, state_next;
    reg [15:0] timer;          // general-purpose down-counter
    reg [3:0]  burst_cnt;      // counts OOB bursts sent
    reg [1:0]  retry_cnt;      // COMRESET retry counter
    reg        burst_phase;    // 0 = sending burst, 1 = in gap

    // Timer load values
    wire [15:0] comreset_burst_time = BURST_LEN - 1;
    wire [15:0] comreset_gap_time   = COMRESET_GAP - 1;
    wire [15:0] comwake_burst_time  = BURST_LEN - 1;
    wire [15:0] comwake_gap_time    = COMWAKE_GAP - 1;

    // RX passthrough to link layer (active only in READY)
    assign link_rx_data  = (state == ST_READY) ? rx_data      : 32'd0;
    assign link_rx_isk   = (state == ST_READY) ? rx_charisk   : 4'd0;
    assign link_rx_valid = (state == ST_READY) ? rx_byte_aligned : 1'b0;

    // Main state machine
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= ST_RESET;
            timer       <= 0;
            burst_cnt   <= 0;
            burst_phase <= 0;
            retry_cnt   <= 0;
            tx_comreset <= 0;
            tx_comwake  <= 0;
            tx_data     <= 32'd0;
            tx_charisk  <= 4'd0;
            phy_ready   <= 0;
            phy_speed   <= 2'd0;
        end else begin
            // defaults each cycle
            tx_comreset <= 1'b0;
            tx_comwake  <= 1'b0;

            case (state)
                // --------------------------------------------------------
                // ST_RESET: Send COMRESET (OOB_BURST_COUNT bursts)
                // --------------------------------------------------------
                ST_RESET: begin
                    phy_ready <= 1'b0;
                    phy_speed <= 2'd0;
                    tx_data   <= 32'd0;
                    tx_charisk <= 4'd0;

                    if (burst_cnt < OOB_BURST_COUNT) begin
                        tx_comreset <= ~burst_phase; // assert during burst
                        if (timer == 0) begin
                            if (!burst_phase) begin
                                // end of burst → start gap
                                burst_phase <= 1'b1;
                                timer       <= comreset_gap_time;
                            end else begin
                                // end of gap → next burst
                                burst_phase <= 1'b0;
                                burst_cnt   <= burst_cnt + 1'b1;
                                timer       <= comreset_burst_time;
                            end
                        end else begin
                            timer <= timer - 1'b1;
                        end
                    end else begin
                        // All bursts sent → wait for COMINIT
                        state     <= ST_WAIT_COMINIT;
                        timer     <= COMINIT_TIMEOUT - 1;
                        burst_cnt <= 0;
                        burst_phase <= 0;
                    end
                end

                // --------------------------------------------------------
                // ST_WAIT_COMINIT: Wait for device COMINIT
                // --------------------------------------------------------
                ST_WAIT_COMINIT: begin
                    if (rx_cominit) begin
                        state       <= ST_SEND_COMWAKE;
                        timer       <= comwake_burst_time;
                        burst_cnt   <= 0;
                        burst_phase <= 0;
                    end else if (timer == 0) begin
                        // timeout — retry or give up
                        if (retry_cnt < RETRY_LIMIT) begin
                            retry_cnt   <= retry_cnt + 1'b1;
                            state       <= ST_RESET;
                            timer       <= comreset_burst_time;
                            burst_cnt   <= 0;
                            burst_phase <= 0;
                        end else begin
                            // stay waiting (host keeps retrying in real HW,
                            // but for sim cap it)
                            state       <= ST_RESET;
                            timer       <= comreset_burst_time;
                            burst_cnt   <= 0;
                            burst_phase <= 0;
                            retry_cnt   <= 0;
                        end
                    end else begin
                        timer <= timer - 1'b1;
                    end
                end

                // --------------------------------------------------------
                // ST_SEND_COMWAKE: Send COMWAKE bursts
                // --------------------------------------------------------
                ST_SEND_COMWAKE: begin
                    if (burst_cnt < OOB_BURST_COUNT) begin
                        tx_comwake <= ~burst_phase;
                        if (timer == 0) begin
                            if (!burst_phase) begin
                                burst_phase <= 1'b1;
                                timer       <= comwake_gap_time;
                            end else begin
                                burst_phase <= 1'b0;
                                burst_cnt   <= burst_cnt + 1'b1;
                                timer       <= comwake_burst_time;
                            end
                        end else begin
                            timer <= timer - 1'b1;
                        end
                    end else begin
                        state     <= ST_WAIT_COMWAKE;
                        timer     <= COMWAKE_TIMEOUT - 1;
                        burst_cnt <= 0;
                        burst_phase <= 0;
                    end
                end

                // --------------------------------------------------------
                // ST_WAIT_COMWAKE: Wait for device COMWAKE reply
                // --------------------------------------------------------
                ST_WAIT_COMWAKE: begin
                    if (rx_comwake) begin
                        state <= ST_WAIT_ALIGN;
                        timer <= ALIGN_TIMEOUT - 1;
                    end else if (timer == 0) begin
                        // timeout → restart from COMRESET
                        state       <= ST_RESET;
                        timer       <= comreset_burst_time;
                        burst_cnt   <= 0;
                        burst_phase <= 0;
                    end else begin
                        timer <= timer - 1'b1;
                    end
                end

                // --------------------------------------------------------
                // ST_WAIT_ALIGN: Send ALIGN, wait for device ALIGN + byte lock
                // --------------------------------------------------------
                ST_WAIT_ALIGN: begin
                    // Continuously send ALIGN primitives
                    tx_data    <= PRIM_ALIGN;
                    tx_charisk <= ALIGN_ISK;

                    if (rx_byte_aligned &&
                        rx_data == PRIM_ALIGN &&
                        rx_charisk == ALIGN_ISK) begin
                        state     <= ST_READY;
                        phy_ready <= 1'b1;
                        phy_speed <= 2'd1; // Gen1
                    end else if (timer == 0) begin
                        // timeout → restart
                        state       <= ST_RESET;
                        timer       <= comreset_burst_time;
                        burst_cnt   <= 0;
                        burst_phase <= 0;
                        tx_data     <= 32'd0;
                        tx_charisk  <= 4'd0;
                    end else begin
                        timer <= timer - 1'b1;
                    end
                end

                // --------------------------------------------------------
                // ST_READY: PHY link is up — bridge link-layer ↔ SERDES
                // --------------------------------------------------------
                ST_READY: begin
                    if (link_tx_valid) begin
                        tx_data    <= link_tx_data;
                        tx_charisk <= link_tx_isk;
                    end else begin
                        // idle: send SYNC
                        tx_data    <= PRIM_SYNC;
                        tx_charisk <= SYNC_ISK;
                    end

                    // If device drops alignment, go back to reset
                    if (!rx_byte_aligned) begin
                        phy_ready   <= 1'b0;
                        phy_speed   <= 2'd0;
                        state       <= ST_RESET;
                        timer       <= comreset_burst_time;
                        burst_cnt   <= 0;
                        burst_phase <= 0;
                        retry_cnt   <= 0;
                    end
                end

                default: begin
                    state <= ST_RESET;
                end
            endcase
        end
    end

endmodule
