// query_bypass.v — PicoWAL FPGA command router + block copy engine
//
// ALL data movement goes through this FPGA. The two RP2354B picos are
// pure control plane — they issue register commands over dedicated SPI
// ports and never directly touch SPI buses, SRAM, or SATA nodes.
//
// ── Data path (FPGA-owned, zero pico involvement) ──
//   Upstream SPI → inspect addr[52] → bypass to downstream SATA nodes
//
// ── Query pico (control plane only) ──
//   Drains query FIFO via FPGA register reads.
//   Issues commands to FPGA:
//     CMD_IDX_READ   0x01  Read index block from SATA → FPGA returns data over SPI
//     CMD_COPY_OUT   0x02  Copy data block from SATA → stream to upstream output
//     CMD_MULTI_START 0x03 Begin multi-block gather
//     CMD_MULTI_END  0x04  Flush gathered response to upstream
//     CMD_FIFO_POP   0x10  Pop + read next query descriptor from SRAM FIFO
//
// ── Index pico (control plane only) ──
//   Receives write notifications via FPGA IRQ.
//   Issues commands to FPGA:
//     CMD_IDX_READ   0x01  Read index block from SATA → FPGA returns data over SPI
//     CMD_IDX_WRITE  0x05  Write index block to SATA (pico sends data over SPI → FPGA)
//     CMD_NOTIFY_ACK 0x11  Acknowledge write notification, clear IRQ
//
// ── SRAM ring buffer (FPGA-owned) ──
//   Incoming queries (addr[52]=1) are staged here automatically.
//   Query pico reads entries via CMD_FIFO_POP — FPGA returns the
//   5-word descriptor over SPI. Pico never addresses SRAM directly.
//
// Address format:
//   [63:53] tenant_id  (11b)
//   [52]    INDEX flag  (1=index, 0=data)
//   [51:42] card_id    (10b)
//   [41:0]  block      (42b)
//
`default_nettype none

module query_bypass (
    input  wire        clk,
    input  wire        rst_n,

    // --- Upstream SPI slave (from cluster fabric) ---
    input  wire        up_spi_sck,
    input  wire        up_spi_mosi,
    output wire        up_spi_miso,
    input  wire        up_spi_cs_n,

    // --- Downstream SPI master (to SATA KV data nodes) ---
    output wire        dn_spi_sck,
    output wire        dn_spi_mosi,
    input  wire        dn_spi_miso,
    output wire        dn_spi_cs_n,

    // --- Query pico SPI slave (pico is master, FPGA is slave) ---
    input  wire        qry_spi_sck,
    input  wire        qry_spi_mosi,
    output reg         qry_spi_miso,
    input  wire        qry_spi_cs_n,
    output reg         qry_irq,          // query FIFO not empty

    // --- Index pico SPI slave (pico is master, FPGA is slave) ---
    input  wire        idx_spi_sck,
    input  wire        idx_spi_mosi,
    output reg         idx_spi_miso,
    input  wire        idx_spi_cs_n,
    output reg         idx_irq,          // write landed, index needs update

    // --- SRAM interface (FPGA-owned, picos never touch) ---
    output reg  [17:0] sram_addr,
    output reg  [15:0] sram_wdata,
    input  wire [15:0] sram_rdata,
    output reg         sram_we_n,
    output reg         sram_oe_n,
    output reg         sram_ce_n,

    // --- Stats ---
    output reg  [31:0] bypass_count,
    output reg  [31:0] query_count,
    output reg  [31:0] copy_count,
    output reg  [31:0] index_update_count
);

    // =====================================================================
    // Pico command opcodes
    // =====================================================================
    localparam CMD_IDX_READ    = 8'h01,
               CMD_COPY_OUT    = 8'h02,
               CMD_MULTI_START = 8'h03,
               CMD_MULTI_END   = 8'h04,
               CMD_IDX_WRITE   = 8'h05,
               CMD_FIFO_POP    = 8'h10,
               CMD_NOTIFY_ACK  = 8'h11;

    // =====================================================================
    // Ingress: command shift register — 9 bytes (72 bits)
    // =====================================================================
    reg [71:0] cmd_shift;
    reg [6:0]  bit_count;

    wire [7:0]  cmd_flags = cmd_shift[71:64];
    wire [63:0] cmd_addr  = cmd_shift[63:0];
    wire        is_index  = cmd_addr[52];     // THE BIT
    wire        is_write  = cmd_flags[0];

    // =====================================================================
    // Ingress state machine
    // =====================================================================
    localparam [3:0] S_IDLE         = 4'd0,
                     S_SHIFT_CMD    = 4'd1,
                     S_DECIDE       = 4'd2,
                     S_BYPASS       = 4'd3,
                     S_QUEUE_W0     = 4'd4,
                     S_QUEUE_W1     = 4'd5,
                     S_QUEUE_W2     = 4'd6,
                     S_QUEUE_W3     = 4'd7,
                     S_QUEUE_W4     = 4'd8,
                     S_QUEUE_DONE   = 4'd9,
                     S_DONE         = 4'd10;

    reg [3:0] state;
    reg bypass_active;

    // SRAM ring buffer
    reg [17:0] ring_wptr, ring_rptr;
    reg [15:0] ring_count;
    localparam RING_BASE  = 18'h00000;
    localparam RING_LIMIT = 18'h01000;
    localparam ENTRY_WORDS = 18'd5;

    // =====================================================================
    // SPI edge detect (upstream)
    // =====================================================================
    reg up_sck_r, up_sck_rr;
    wire up_sck_rise = up_sck_r & ~up_sck_rr;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            up_sck_r  <= 0;
            up_sck_rr <= 0;
        end else begin
            up_sck_r  <= up_spi_sck;
            up_sck_rr <= up_sck_r;
        end
    end

    // =====================================================================
    // Ingress FSM — classify and route incoming commands
    // =====================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= S_IDLE;
            cmd_shift          <= 72'd0;
            bit_count          <= 7'd0;
            bypass_active      <= 1'b0;
            qry_irq            <= 1'b0;
            idx_irq            <= 1'b0;
            ring_wptr          <= RING_BASE;
            ring_rptr          <= RING_BASE;
            ring_count         <= 16'd0;
            sram_we_n          <= 1'b1;
            sram_oe_n          <= 1'b1;
            sram_ce_n          <= 1'b1;
            sram_addr          <= 18'd0;
            sram_wdata         <= 16'd0;
            bypass_count       <= 32'd0;
            query_count        <= 32'd0;
            copy_count         <= 32'd0;
            index_update_count <= 32'd0;
        end else begin
            // Default: deassert single-cycle pulses
            sram_we_n <= 1'b1;
            sram_ce_n <= 1'b1;

            case (state)
                S_IDLE: begin
                    bypass_active <= 1'b0;
                    bit_count     <= 7'd0;
                    qry_irq       <= (ring_count != 16'd0);
                    if (!up_spi_cs_n)
                        state <= S_SHIFT_CMD;
                end

                S_SHIFT_CMD: begin
                    if (up_spi_cs_n) begin
                        state <= S_IDLE;
                    end else if (up_sck_rise) begin
                        cmd_shift <= {cmd_shift[70:0], up_spi_mosi};
                        bit_count <= bit_count + 7'd1;
                        if (bit_count == 7'd71)
                            state <= S_DECIDE;
                    end
                end

                S_DECIDE: begin
                    if (!is_index) begin
                        // ── DATA: bypass to downstream ──
                        bypass_active <= 1'b1;
                        bypass_count  <= bypass_count + 32'd1;
                        // Notify index pico on writes so it can update indexes
                        if (is_write) begin
                            idx_irq            <= 1'b1;
                            index_update_count <= index_update_count + 32'd1;
                        end
                        state <= S_BYPASS;
                    end else begin
                        // ── QUERY: write to SRAM FIFO ──
                        query_count <= query_count + 32'd1;
                        state <= S_QUEUE_W0;
                    end
                end

                S_BYPASS: begin
                    if (up_spi_cs_n) begin
                        bypass_active <= 1'b0;
                        state <= S_DONE;
                    end
                end

                // Write 5 words: flags+addr packed into SRAM ring buffer
                S_QUEUE_W0: begin
                    sram_ce_n  <= 1'b0; sram_we_n <= 1'b0;
                    sram_addr  <= ring_wptr;
                    sram_wdata <= {cmd_flags, cmd_addr[63:56]};
                    state <= S_QUEUE_W1;
                end
                S_QUEUE_W1: begin
                    sram_ce_n  <= 1'b0; sram_we_n <= 1'b0;
                    sram_addr  <= ring_wptr + 18'd1;
                    sram_wdata <= cmd_addr[55:40];
                    state <= S_QUEUE_W2;
                end
                S_QUEUE_W2: begin
                    sram_ce_n  <= 1'b0; sram_we_n <= 1'b0;
                    sram_addr  <= ring_wptr + 18'd2;
                    sram_wdata <= cmd_addr[39:24];
                    state <= S_QUEUE_W3;
                end
                S_QUEUE_W3: begin
                    sram_ce_n  <= 1'b0; sram_we_n <= 1'b0;
                    sram_addr  <= ring_wptr + 18'd3;
                    sram_wdata <= cmd_addr[23:8];
                    state <= S_QUEUE_W4;
                end
                S_QUEUE_W4: begin
                    sram_ce_n  <= 1'b0; sram_we_n <= 1'b0;
                    sram_addr  <= ring_wptr + 18'd4;
                    sram_wdata <= {cmd_addr[7:0], 8'h00};
                    state <= S_QUEUE_DONE;
                end

                S_QUEUE_DONE: begin
                    // Advance write pointer with wrap
                    if (ring_wptr + ENTRY_WORDS >= RING_LIMIT)
                        ring_wptr <= RING_BASE;
                    else
                        ring_wptr <= ring_wptr + ENTRY_WORDS;
                    ring_count <= ring_count + 16'd1;
                    qry_irq    <= 1'b1;
                    state <= S_DONE;
                end

                S_DONE: begin
                    state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Query pico register interface (FPGA is SPI slave)
    //
    // Pico sends 1-byte opcode + optional address payload.
    // FPGA responds with data on MISO.
    //
    //   CMD_FIFO_POP  → FPGA reads 5 words from SRAM, shifts back over SPI
    //   CMD_IDX_READ  + 8-byte addr → FPGA issues read to downstream,
    //                   returns 4KB over SPI to pico
    //   CMD_COPY_OUT  + 8-byte addr → FPGA issues read to downstream,
    //                   streams 4KB directly to upstream (pico gets ACK only)
    //   CMD_IDX_WRITE + 8-byte addr + 4KB data → FPGA writes to downstream
    //
    // Implementation: shift register + sub-FSM (stub — real version needs
    // full SPI slave with byte framing). Key point: picos only see the
    // register interface; they never drive any bus directly.
    // =====================================================================

    // Query pico SPI edge detect
    reg qry_sck_r, qry_sck_rr;
    wire qry_sck_rise = qry_sck_r & ~qry_sck_rr;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            qry_sck_r  <= 0;
            qry_sck_rr <= 0;
            qry_spi_miso <= 1'b0;
        end else begin
            qry_sck_r  <= qry_spi_sck;
            qry_sck_rr <= qry_sck_r;
        end
    end

    // Index pico SPI edge detect
    reg idx_sck_r, idx_sck_rr;
    wire idx_sck_rise = idx_sck_r & ~idx_sck_rr;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            idx_sck_r  <= 0;
            idx_sck_rr <= 0;
            idx_spi_miso <= 1'b0;
        end else begin
            idx_sck_r  <= idx_spi_sck;
            idx_sck_rr <= idx_sck_r;
        end
    end

    // =====================================================================
    // Downstream SPI mux: bypass OR copy engine
    // =====================================================================
    assign dn_spi_sck  = bypass_active ? up_spi_sck  : 1'b0;
    assign dn_spi_mosi = bypass_active ? up_spi_mosi : 1'b0;
    assign dn_spi_cs_n = bypass_active ? up_spi_cs_n : 1'b1;

    // Upstream response: downstream data during bypass, else idle
    assign up_spi_miso = bypass_active ? dn_spi_miso : 1'b0;

endmodule
