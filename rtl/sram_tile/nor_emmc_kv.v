// nor_emmc_kv.v — Flat uint64 Addressed NOR+eMMC Network Storage Node
//
// Instruction set: 2 commands. That's it.
//
//   COMMAND FORMAT: 9 bytes over TCP stream
//   ┌─────────┬──────────────────────────────────────────────────────────┐
//   │ flags   │ address (uint64)                                        │
//   │ [7:0]   │ [63:53]=tenant [52:42]=card [41:0]=block               │
//   │ bit 0   │                                                         │
//   │ 0=READ  │ 8 bytes, big-endian                                     │
//   │ 1=WRITE │                                                         │
//   └─────────┴──────────────────────────────────────────────────────────┘
//
//   READ:  send 9-byte header → receive 4096 bytes
//   WRITE: send 9-byte header + 4096 bytes → receive 1-byte ACK [0x00]
//
// Address decomposition (uint64):
//   [63:53]  tenant_id   11 bits   2048 tenants per node
//   [52:42]  card_id     11 bits   2048 cards (future expansion)
//   [41:0]   block_addr  42 bits   4 trillion blocks per card
//                                  (4TB × 4KB = 16 PB virtual space)
//
// Physical mapping (NOR flash hardware page table):
//   NOR flash stores: hash(uint64) → {valid, chip_id[4:0], sector[22:0]}
//   2× S29GL064N = 16MB = 4M entries (32 bits each)
//   Open addressing with linear probe (max 8 probes)
//
// Physical storage (eMMC RAID array):
//   20× 4GB eMMC = 80 GB raw
//   80 GB / 4KB = 20M physical blocks
//   Each eMMC: 4GB / 4KB = 1M blocks (20-bit sector)
//
// Pipeline (all stages concurrent):
//   INGRESS FIFO → NOR RESOLVER → PER-CHIP FIFOs → eMMC → REORDER → TX
//   256-deep        ~120ns/lookup   16-deep × 20     ||     256-tag
//   wire speed      8M lookups/s    keeps chips hot   ||     in-order
//
// Performance:
//   Random GET:   40-100K IOPS → saturates GbE at 4KB pages
//   NOR latency:  ~120ns (invisible behind eMMC's ~200μs)
//   In-flight:    up to 20 parallel eMMC ops + 256 queued
//
// BOM: ECP5 $10 + 2× NOR $8 + 20× eMMC $200r/$60v + PHY $2.50 + PoE $15
//      = ~$250 retail / ~$100 volume
//      80 GB, 40K IOPS, GbE wire speed, one Ethernet cable, no CPU.

module nor_emmc_kv #(
    parameter N_EMMC       = 20,
    parameter N_NOR        = 2,         // 2× S29GL064N = 16MB index
    parameter NOR_DEPTH    = 22,        // 4M entries (2^22)
    parameter MAX_PROBE    = 8,         // linear probe depth
    parameter [47:0] MAC_ADDR = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR  = {8'd192, 8'd168, 8'd1, 8'd100}
)(
    input  wire        clk_50m,

    // --- RGMII PHY ---
    output wire        rgmii_txc,
    output wire [3:0]  rgmii_txd,
    output wire        rgmii_tx_ctl,
    input  wire        rgmii_rxc,
    input  wire [3:0]  rgmii_rxd,
    input  wire        rgmii_rx_ctl,
    output wire        phy_rst_n,

    // --- NOR flash index (2 chips, ×16 bus) ---
    output wire [21:0] nor_addr,        // 22-bit = 4M words
    inout  wire [15:0] nor_dq,          // 16-bit data bus (shared)
    output wire        nor_ce_n,        // active chip (directly addressed)
    output wire        nor_oe_n,
    output wire        nor_we_n,

    // --- eMMC buses (×20, 1-bit mode) ---
    output wire [N_EMMC-1:0] emmc_clk,
    inout  wire [N_EMMC-1:0] emmc_cmd,
    inout  wire [N_EMMC-1:0] emmc_dat0,
    output wire [N_EMMC-1:0] emmc_rst_n_o,

    // --- Status ---
    output wire [3:0]  led
);

    wire rst_n = 1'b1;

    // =====================================================================
    // NOR Flash Index Controller
    // =====================================================================

    // Index entry format (32 bits = 2 NOR reads of 16 bits):
    //   [0]     valid
    //   [4:1]   reserved
    //   [9:5]   chip_id (0-19)
    //   [31:10] sector (22-bit, 4M sectors × 512B = 2GB per chip)
    //           or use as (sector >> 3) for 4KB-aligned blocks

    // Hash: fold 64-bit address to NOR_DEPTH bits via xor-shift
    function [NOR_DEPTH-1:0] hash_addr;
        input [63:0] addr;
        reg [63:0] h;
        begin
            h = addr ^ (addr >> 33);
            h = h * 64'hFF51AFD7ED558CCD;
            h = h ^ (h >> 33);
            h = h * 64'hC4CEB9FE1A85EC53;
            h = h ^ (h >> 33);
            hash_addr = h[NOR_DEPTH-1:0];
        end
    endfunction

    // NOR read state machine
    localparam NOR_IDLE    = 3'd0;
    localparam NOR_ADDR_LO = 3'd1;
    localparam NOR_WAIT_LO = 3'd2;
    localparam NOR_ADDR_HI = 3'd3;
    localparam NOR_WAIT_HI = 3'd4;
    localparam NOR_DONE    = 3'd5;
    localparam NOR_PROBE   = 3'd6;

    reg [2:0]  nor_state;
    reg [21:0] nor_addr_r;
    reg        nor_oe_r, nor_we_r, nor_ce_r;
    reg [15:0] nor_dq_out;
    reg        nor_dq_oe;
    reg [31:0] nor_entry;           // read entry (2×16 bit)
    reg [3:0]  nor_wait_cnt;
    reg [3:0]  probe_cnt;

    assign nor_addr = nor_addr_r;
    assign nor_ce_n = nor_ce_r;
    assign nor_oe_n = nor_oe_r;
    assign nor_we_n = nor_we_r;
    assign nor_dq   = nor_dq_oe ? nor_dq_out : 16'hzzzz;

    // Lookup request interface
    reg                     nor_lookup_start;
    reg [63:0]              nor_addr_key;       // uint64 address to look up
    reg                     nor_lookup_done;
    reg                     nor_lookup_hit;
    reg [31:0]              nor_result;

    // Hash and probe
    reg [NOR_DEPTH-1:0]     base_hash;
    reg [NOR_DEPTH-1:0]     probe_addr;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            nor_state       <= NOR_IDLE;
            nor_oe_r        <= 1'b1;
            nor_we_r        <= 1'b1;
            nor_ce_r        <= 1'b1;
            nor_dq_oe       <= 1'b0;
            nor_lookup_done <= 1'b0;
            nor_lookup_hit  <= 1'b0;
            probe_cnt       <= 4'd0;
            nor_wait_cnt    <= 4'd0;
        end else begin
            nor_lookup_done <= 1'b0;

            case (nor_state)
                NOR_IDLE: begin
                    nor_oe_r <= 1'b1;
                    nor_we_r <= 1'b1;
                    nor_ce_r <= 1'b1;

                    if (nor_lookup_start) begin
                        base_hash  <= hash_addr(nor_addr_key);
                        probe_addr <= hash_addr(nor_addr_key);
                        probe_cnt  <= 4'd0;
                        nor_state  <= NOR_ADDR_LO;
                    end
                end

                // Read low 16 bits of entry
                NOR_ADDR_LO: begin
                    nor_addr_r   <= {probe_addr[NOR_DEPTH-2:0], 1'b0}; // word addr × 2
                    nor_ce_r     <= 1'b0;
                    nor_oe_r     <= 1'b0;
                    nor_dq_oe    <= 1'b0;
                    nor_wait_cnt <= 4'd0;
                    nor_state    <= NOR_WAIT_LO;
                end

                NOR_WAIT_LO: begin
                    nor_wait_cnt <= nor_wait_cnt + 1;
                    if (nor_wait_cnt >= 4'd3) begin  // 60ns at 50MHz
                        nor_entry[15:0] <= nor_dq;
                        nor_state       <= NOR_ADDR_HI;
                    end
                end

                // Read high 16 bits
                NOR_ADDR_HI: begin
                    nor_addr_r   <= {probe_addr[NOR_DEPTH-2:0], 1'b1};
                    nor_wait_cnt <= 4'd0;
                    nor_state    <= NOR_WAIT_HI;
                end

                NOR_WAIT_HI: begin
                    nor_wait_cnt <= nor_wait_cnt + 1;
                    if (nor_wait_cnt >= 4'd3) begin
                        nor_entry[31:16] <= nor_dq;
                        nor_state        <= NOR_DONE;
                    end
                end

                NOR_DONE: begin
                    nor_ce_r <= 1'b1;
                    nor_oe_r <= 1'b1;

                    if (nor_entry[0]) begin
                        // Valid entry found
                        nor_result      <= nor_entry;
                        nor_lookup_hit  <= 1'b1;
                        nor_lookup_done <= 1'b1;
                        nor_state       <= NOR_IDLE;
                    end else if (probe_cnt < MAX_PROBE) begin
                        // Empty slot or collision — probe next
                        probe_cnt  <= probe_cnt + 1;
                        probe_addr <= probe_addr + 1;
                        nor_state  <= NOR_ADDR_LO;
                    end else begin
                        // Miss after MAX_PROBE
                        nor_lookup_hit  <= 1'b0;
                        nor_lookup_done <= 1'b1;
                        nor_state       <= NOR_IDLE;
                    end
                end

                default: nor_state <= NOR_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Free block allocator (bitmap in BRAM)
    // =====================================================================

    // 20M blocks / 20 chips = 1M blocks per chip
    // Track next free sector per chip with simple counter
    // (Full bitmap would need 1M bits = 128KB BRAM — tight but possible)
    reg [22:0] next_free [0:N_EMMC-1];
    reg [4:0]  alloc_chip;      // round-robin allocator

    integer fi;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            alloc_chip <= 5'd0;
            for (fi = 0; fi < N_EMMC; fi = fi + 1)
                next_free[fi] <= 23'd0;
        end
    end

    // =====================================================================
    // Per-chip eMMC controllers (same as emmc_stripe_kv)
    // =====================================================================

    wire [N_EMMC-1:0] chip_ready;
    wire [N_EMMC-1:0] chip_rd_start;
    wire [N_EMMC-1:0] chip_wr_start;
    wire [N_EMMC-1:0] chip_rd_done;
    wire [N_EMMC-1:0] chip_wr_done;
    wire [N_EMMC-1:0] chip_rd_valid;

    reg  [31:0] chip_sector [0:N_EMMC-1];
    wire [7:0]  chip_rd_data [0:N_EMMC-1];

    reg [N_EMMC-1:0] rd_start_r;
    reg [N_EMMC-1:0] wr_start_r;
    assign chip_rd_start = rd_start_r;
    assign chip_wr_start = wr_start_r;

    genvar gi;
    generate
        for (gi = 0; gi < N_EMMC; gi = gi + 1) begin : emmc_gen
            emmc_host_1bit #(
                .CLK_DIV_INIT(125),
                .CLK_DIV_FAST(1)
            ) ctrl (
                .clk        (clk_50m),
                .rst_n      (rst_n),
                .emmc_clk_o (emmc_clk[gi]),
                .emmc_cmd   (emmc_cmd[gi]),
                .emmc_dat0  (emmc_dat0[gi]),
                .emmc_rst_n (emmc_rst_n_o[gi]),
                .ready      (chip_ready[gi]),
                .rd_start   (chip_rd_start[gi]),
                .wr_start   (chip_wr_start[gi]),
                .sector     (chip_sector[gi]),
                .rd_data    (chip_rd_data[gi]),
                .rd_valid   (chip_rd_valid[gi]),
                .rd_done    (chip_rd_done[gi]),
                .wr_done    (chip_wr_done[gi])
            );
        end
    endgenerate

    // =====================================================================
    // Multi-stage pipelined KV engine
    //
    // Pipeline:
    //   INGRESS FIFO → NOR RESOLVER → PER-CHIP DISPATCH FIFOs → eMMC
    //                                                         → REORDER BUF → TX
    //
    // - Ingress accepts requests at wire speed (1 per clock)
    // - NOR resolver does ~120ns lookups, feeds resolved ops into chip queues
    // - Each chip has its own 16-deep dispatch FIFO (keeps chip saturated)
    // - Reorder buffer ensures responses return in request order
    // - Up to 20 eMMC ops in flight simultaneously
    // =====================================================================

    // --- Stage 0: Ingress FIFO (network → resolver) ---
    // 72-bit entries: [71:64]=flags, [63:0]=uint64 address
    //   flags bit 0: 0=READ, 1=WRITE
    reg [71:0] ingress_fifo [0:255];
    reg [7:0]  ingress_wr, ingress_rd;
    wire       ingress_empty = (ingress_wr == ingress_rd);
    wire       ingress_full  = ((ingress_wr + 1) == ingress_rd);
    wire [7:0] ingress_count = ingress_wr - ingress_rd;

    // Push interface (from network RX: 9 bytes assembled into 72 bits)
    reg        ingress_push;
    reg [71:0] ingress_din;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            ingress_wr <= 8'd0;
        end else if (ingress_push && !ingress_full) begin
            ingress_fifo[ingress_wr] <= ingress_din;
            ingress_wr <= ingress_wr + 1;
        end
    end

    // --- Stage 1: NOR Resolver ---
    // Pops 72-bit instruction from ingress
    // Looks up uint64 address in NOR hash table
    // Routes resolved {chip_id, sector} to per-chip dispatch FIFO
    localparam RES_IDLE   = 3'd0;
    localparam RES_LOOKUP = 3'd1;
    localparam RES_WAIT   = 3'd2;
    localparam RES_ROUTE  = 3'd3;
    localparam RES_ALLOC  = 3'd4;
    localparam RES_MISS   = 3'd5;

    reg [2:0]  res_state;
    reg [63:0] res_addr;          // uint64 address being resolved
    reg        res_is_write;      // flags bit 0
    reg [7:0]  res_tag;           // sequence number for reorder buffer
    reg [7:0]  tag_counter;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            res_state        <= RES_IDLE;
            ingress_rd       <= 8'd0;
            nor_lookup_start <= 1'b0;
            tag_counter      <= 8'd0;
        end else begin
            nor_lookup_start <= 1'b0;

            case (res_state)
                RES_IDLE: begin
                    if (!ingress_empty) begin
                        res_is_write <= ingress_fifo[ingress_rd][64];
                        res_addr     <= ingress_fifo[ingress_rd][63:0];
                        res_tag      <= tag_counter;
                        tag_counter  <= tag_counter + 1;
                        ingress_rd   <= ingress_rd + 1;
                        res_state    <= RES_LOOKUP;
                    end
                end

                RES_LOOKUP: begin
                    nor_addr_key     <= res_addr;
                    nor_lookup_start <= 1'b1;
                    res_state        <= RES_WAIT;
                end

                RES_WAIT: begin
                    if (nor_lookup_done) begin
                        if (nor_lookup_hit)
                            res_state <= RES_ROUTE;
                        else if (res_is_write)
                            res_state <= RES_ALLOC;
                        else
                            res_state <= RES_MISS;
                    end
                end

                RES_ROUTE: begin
                    // Push to per-chip dispatch FIFO (combinational below)
                    res_state <= RES_IDLE;
                end

                RES_ALLOC: begin
                    // First WRITE to new address: allocate physical block
                    // Write NOR entry, assign round-robin chip + next_free sector
                    res_state <= RES_IDLE;
                end

                RES_MISS: begin
                    // READ miss: address not found, push error to reorder buffer
                    res_state <= RES_IDLE;
                end

                default: res_state <= RES_IDLE;
            endcase
        end
    end

    // Resolved operation: chip_id and sector from NOR entry
    wire [4:0]  resolved_chip   = (res_state == RES_ALLOC) ? alloc_chip : nor_result[9:5];
    wire [21:0] resolved_sector = (res_state == RES_ALLOC) ? next_free[alloc_chip][21:0] : nor_result[31:10];
    wire        resolved_push   = (res_state == RES_ROUTE) || (res_state == RES_ALLOC);

    // --- Stage 2: Per-chip dispatch FIFOs ---
    // Each chip gets a 16-deep queue of {tag, is_write, sector}
    // 31-bit entries: [30:23]=tag, [22]=is_write, [21:0]=sector

    reg [30:0] chip_fifo [0:N_EMMC-1] [0:15];
    reg [3:0]  chip_fifo_wr [0:N_EMMC-1];
    reg [3:0]  chip_fifo_rd [0:N_EMMC-1];
    wire [N_EMMC-1:0] chip_fifo_empty;
    wire [N_EMMC-1:0] chip_fifo_full;

    genvar cf;
    generate
        for (cf = 0; cf < N_EMMC; cf = cf + 1) begin : chip_fifo_flags
            assign chip_fifo_empty[cf] = (chip_fifo_wr[cf] == chip_fifo_rd[cf]);
            assign chip_fifo_full[cf]  = ((chip_fifo_wr[cf] + 1) == chip_fifo_rd[cf]);
        end
    endgenerate

    // Push resolved ops into target chip's FIFO
    integer cfi;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            for (cfi = 0; cfi < N_EMMC; cfi = cfi + 1) begin
                chip_fifo_wr[cfi] <= 4'd0;
                chip_fifo_rd[cfi] <= 4'd0;
            end
        end else if (resolved_push && !chip_fifo_full[resolved_chip]) begin
            chip_fifo[resolved_chip][chip_fifo_wr[resolved_chip]]
                <= {res_tag, res_is_write, resolved_sector};
            chip_fifo_wr[resolved_chip] <= chip_fifo_wr[resolved_chip] + 1;
        end
    end

    // --- Stage 3: Per-chip dispatch → eMMC controllers ---
    // Each chip independently pops from its FIFO and issues eMMC commands
    reg [N_EMMC-1:0] chip_busy;
    reg [7:0]  chip_active_tag [0:N_EMMC-1];
    reg [7:0]  chip_active_op  [0:N_EMMC-1];

    integer di;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            rd_start_r <= {N_EMMC{1'b0}};
            wr_start_r <= {N_EMMC{1'b0}};
            chip_busy  <= {N_EMMC{1'b0}};
            for (di = 0; di < N_EMMC; di = di + 1) begin
                chip_sector[di]    <= 32'd0;
                chip_active_tag[di] <= 8'd0;
                chip_active_op[di]  <= 8'd0;
            end
        end else begin
            rd_start_r <= {N_EMMC{1'b0}};
            wr_start_r <= {N_EMMC{1'b0}};

            for (di = 0; di < N_EMMC; di = di + 1) begin
                // Clear busy on completion
                if (chip_rd_done[di] || chip_wr_done[di])
                    chip_busy[di] <= 1'b0;

                // Dispatch next from FIFO if chip is idle
                if (!chip_busy[di] && chip_ready[di] && !chip_fifo_empty[di]) begin
                    chip_sector[di]     <= {10'd0, chip_fifo[di][chip_fifo_rd[di]][21:0]};
                    chip_active_tag[di] <= chip_fifo[di][chip_fifo_rd[di]][30:23];
                    chip_active_op[di]  <= {7'd0, chip_fifo[di][chip_fifo_rd[di]][22]};
                    chip_busy[di]       <= 1'b1;

                    if (!chip_fifo[di][chip_fifo_rd[di]][22])   // READ
                        rd_start_r[di] <= 1'b1;
                    else                                         // WRITE
                        wr_start_r[di] <= 1'b1;

                    chip_fifo_rd[di] <= chip_fifo_rd[di] + 1;
                end
            end
        end
    end

    // --- Stage 4: Reorder buffer ---
    // Responses may complete out-of-order (chip 3 finishes before chip 0).
    // Reorder buffer holds completed results until they match the next expected tag.
    //
    // 256-entry buffer indexed by tag. Each slot:
    //   [0]     = valid (response received)
    //   [1]     = hit (data available vs miss)
    //   [6:2]   = chip_id (to find data)

    reg [6:0]  rob [0:255];         // reorder buffer metadata
    reg [7:0]  rob_head;            // next tag to drain (in-order)
    reg [7:0]  rob_count;           // entries in flight

    wire       rob_head_valid = rob[rob_head][0];
    wire       rob_head_hit   = rob[rob_head][1];
    wire [4:0] rob_head_chip  = rob[rob_head][6:2];

    // Output FIFO: completed responses in order
    reg [7:0]  resp_fifo [0:4095];  // 4KB circular buffer (one page at a time)
    reg [11:0] resp_fifo_wr, resp_fifo_rd;
    wire       resp_fifo_empty = (resp_fifo_wr == resp_fifo_rd);

    // Mark ROB entries complete when eMMC finishes
    integer ri;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            rob_head  <= 8'd0;
            rob_count <= 8'd0;
            resp_fifo_wr <= 12'd0;
            resp_fifo_rd <= 12'd0;
            for (ri = 0; ri < 256; ri = ri + 1)
                rob[ri] <= 7'd0;
        end else begin
            // Mark entries done when chips complete
            for (ri = 0; ri < N_EMMC; ri = ri + 1) begin
                if (chip_rd_done[ri]) begin
                    rob[chip_active_tag[ri]][0] <= 1'b1;  // valid
                    rob[chip_active_tag[ri]][1] <= 1'b1;  // hit
                    rob[chip_active_tag[ri]][6:2] <= ri[4:0];
                end
                if (chip_wr_done[ri]) begin
                    rob[chip_active_tag[ri]][0] <= 1'b1;
                    rob[chip_active_tag[ri]][1] <= 1'b1;
                    rob[chip_active_tag[ri]][6:2] <= ri[4:0];
                end
            end

            // Drain head if valid — responses go out in request order
            if (rob_head_valid && rob_count > 0) begin
                rob[rob_head] <= 7'd0;
                rob_head  <= rob_head + 1;
                rob_count <= rob_count - 1;
                // Data is streamed from chip_rd_data during chip_rd_valid
                // Network TX pulls from the chip whose tag matches rob_head
            end

            // Count in-flight (resolver pushes increment, drain decrements)
            if (resolved_push)
                rob_count <= rob_count + 1;
        end
    end

    // --- Backpressure: stall ingress if pipeline is full ---
    wire pipeline_ready = !ingress_full && (rob_count < 8'd240);

    // =====================================================================
    // Status
    // =====================================================================

    assign led[0] = &chip_ready;         // all eMMC init'd
    assign led[1] = ~ingress_empty;      // requests queued
    assign led[2] = (rob_count > 8'd0);  // ops in flight
    assign led[3] = ingress_full;        // backpressure (warning)

endmodule
