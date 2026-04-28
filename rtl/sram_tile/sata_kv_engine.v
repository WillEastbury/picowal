// SATA KV Engine
// Bridges network commands (9-byte: flags+address) to SATA READ/WRITE DMA.
// Double-buffered 4KB page BRAM for concurrent network and SATA I/O.
// Direct LBA mode: lba = cmd_addr[41:0] << 3 (8 sectors per 4KB page).
`default_nettype none

module sata_kv_engine (
    input  wire        clk,
    input  wire        rst_n,

    // --- Network ingress (from command parser) ---
    input  wire        cmd_valid,
    input  wire [7:0]  cmd_flags,      // bit 0: 0=READ, 1=WRITE; bit 1: reserved (QUERY)
    input  wire [63:0] cmd_addr,       // [63:53]=tenant, [52]=INDEX flag, [51:42]=card, [41:0]=block
    output wire        cmd_ready,

    // Write data from network (4KB page as DWORDs)
    input  wire [31:0] net_wr_data,
    input  wire        net_wr_valid,
    input  wire        net_wr_last,
    output wire        net_wr_ready,

    // --- SATA command layer ---
    output reg         sata_cmd_valid,
    input  wire        sata_cmd_ready,
    output reg         sata_cmd_is_write,
    output reg  [47:0] sata_cmd_lba,
    output reg  [15:0] sata_cmd_count,

    output reg  [31:0] sata_wr_data,
    output reg         sata_wr_valid,
    output reg         sata_wr_last,
    input  wire        sata_wr_ready,

    input  wire [31:0] sata_rd_data,
    input  wire        sata_rd_valid,
    input  wire        sata_rd_last,

    input  wire        sata_cmd_complete,
    input  wire        sata_cmd_error,
    input  wire [7:0]  sata_cmd_status,

    // --- Response output (to TX fragmenter) ---
    output reg  [31:0] resp_data,
    output reg         resp_valid,
    output reg         resp_last,
    input  wire        resp_ready,

    output reg         resp_start,
    output reg  [31:0] resp_tag,
    output reg  [7:0]  resp_flags,

    // Write ACK
    output reg         ack_valid,
    output reg  [7:0]  ack_byte,

    // --- Stats ---
    output reg  [31:0] reads_completed,
    output reg  [31:0] writes_completed,
    output reg  [31:0] errors
);

    // =====================================================================
    // State machine
    // =====================================================================
    localparam [3:0] S_IDLE           = 4'd0,
                     S_SATA_READ_CMD  = 4'd1,
                     S_SATA_READ_DATA = 4'd2,
                     S_NET_TX_DATA    = 4'd3,
                     S_NET_RX_DATA    = 4'd4,
                     S_SATA_WRITE_CMD = 4'd5,
                     S_SATA_WRITE_DATA= 4'd6,
                     S_SEND_ACK       = 4'd7,
                     S_WAIT_COMPLETE  = 4'd8,
                     S_COMPLETE       = 4'd9;

    reg [3:0] state, state_next;

    // =====================================================================
    // Double-buffer BRAM (2 × 1024×32)
    // =====================================================================
    reg [31:0] buf_mem_0 [0:1023];
    reg [31:0] buf_mem_1 [0:1023];

    reg        active_buf;  // which buffer is being filled/drained
    reg [10:0] buf_wr_ptr;  // write pointer (0..1023)
    reg [10:0] buf_rd_ptr;  // read pointer (0..1023)

    // BRAM read data (combinational for zero-latency access)
    wire [31:0] buf_rd_data = (active_buf == 1'b0)
                              ? buf_mem_0[buf_rd_ptr[9:0]]
                              : buf_mem_1[buf_rd_ptr[9:0]];

    // =====================================================================
    // Latched command
    // =====================================================================
    reg [7:0]  lat_flags;
    reg [63:0] lat_addr;
    wire       lat_is_write = lat_flags[0];

    // Timeout counter
    reg [20:0] timeout_cnt;
    wire       timeout = timeout_cnt[20];  // ~1M cycles

    // Error flag for current command
    reg        cmd_err_flag;

    // =====================================================================
    // cmd_ready / net_wr_ready
    // =====================================================================
    assign cmd_ready    = (state == S_IDLE);
    assign net_wr_ready = (state == S_NET_RX_DATA);

    // =====================================================================
    // State machine (sequential)
    // =====================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state <= S_IDLE;
        else
            state <= state_next;
    end

    // =====================================================================
    // State transitions (combinational)
    // =====================================================================
    always @(*) begin
        state_next = state;
        case (state)
            S_IDLE: begin
                if (cmd_valid)
                    state_next = cmd_flags[0] ? S_NET_RX_DATA : S_SATA_READ_CMD;
            end

            S_SATA_READ_CMD: begin
                if (sata_cmd_valid && sata_cmd_ready)
                    state_next = S_SATA_READ_DATA;
            end

            S_SATA_READ_DATA: begin
                if (sata_cmd_error || timeout)
                    state_next = S_NET_TX_DATA;
                else if (sata_cmd_complete)
                    state_next = S_NET_TX_DATA;
            end

            S_NET_TX_DATA: begin
                if (resp_valid && resp_ready && resp_last)
                    state_next = S_COMPLETE;
            end

            S_NET_RX_DATA: begin
                if (net_wr_valid && (buf_wr_ptr == 11'd1023 || net_wr_last))
                    state_next = S_SATA_WRITE_CMD;
            end

            S_SATA_WRITE_CMD: begin
                if (sata_cmd_valid && sata_cmd_ready)
                    state_next = S_SATA_WRITE_DATA;
            end

            S_SATA_WRITE_DATA: begin
                if (cmd_err_flag || timeout)
                    state_next = S_SEND_ACK;
                else if (sata_cmd_complete)
                    state_next = S_SEND_ACK;
                else if (sata_wr_valid && sata_wr_ready && sata_wr_last)
                    state_next = S_WAIT_COMPLETE;
            end

            S_WAIT_COMPLETE: begin
                if (sata_cmd_complete || sata_cmd_error || timeout)
                    state_next = S_SEND_ACK;
            end

            S_SEND_ACK: begin
                // Single-cycle ACK
                state_next = S_COMPLETE;
            end

            S_COMPLETE: begin
                state_next = S_IDLE;
            end

            default: state_next = S_IDLE;
        endcase
    end

    // =====================================================================
    // Datapath
    // =====================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active_buf       <= 1'b0;
            buf_wr_ptr       <= 11'd0;
            buf_rd_ptr       <= 11'd0;
            lat_flags        <= 8'd0;
            lat_addr         <= 64'd0;
            timeout_cnt      <= 21'd0;
            cmd_err_flag     <= 1'b0;
            sata_cmd_valid   <= 1'b0;
            sata_cmd_is_write<= 1'b0;
            sata_cmd_lba     <= 48'd0;
            sata_cmd_count   <= 16'd0;
            sata_wr_data     <= 32'd0;
            sata_wr_valid    <= 1'b0;
            sata_wr_last     <= 1'b0;
            resp_data        <= 32'd0;
            resp_valid       <= 1'b0;
            resp_last        <= 1'b0;
            resp_start       <= 1'b0;
            resp_tag         <= 32'd0;
            resp_flags       <= 8'd0;
            ack_valid        <= 1'b0;
            ack_byte         <= 8'd0;
            reads_completed  <= 32'd0;
            writes_completed <= 32'd0;
            errors           <= 32'd0;
        end else begin
            // Defaults: clear single-cycle pulses
            resp_start     <= 1'b0;
            ack_valid      <= 1'b0;

            // Clear sata_cmd_valid after handshake
            if (sata_cmd_valid && sata_cmd_ready)
                sata_cmd_valid <= 1'b0;

            // Clear resp_valid after handshake
            if (resp_valid && resp_ready)
                resp_valid <= 1'b0;

            // Clear sata_wr_valid after handshake
            if (sata_wr_valid && sata_wr_ready)
                sata_wr_valid <= 1'b0;

            // Timeout counter in states that wait for SATA
            if (state == S_SATA_READ_DATA || state == S_SATA_WRITE_DATA || state == S_WAIT_COMPLETE)
                timeout_cnt <= timeout_cnt + 1;

            // Latch error from SATA
            if (sata_cmd_error)
                cmd_err_flag <= 1'b1;

            case (state)
                S_IDLE: begin
                    if (cmd_valid) begin
                        lat_flags    <= cmd_flags;
                        lat_addr     <= cmd_addr;
                        buf_wr_ptr   <= 11'd0;
                        buf_rd_ptr   <= 11'd0;
                        timeout_cnt  <= 21'd0;
                        cmd_err_flag <= 1'b0;
                    end
                end

                S_SATA_READ_CMD: begin
                    if (!sata_cmd_valid) begin
                        sata_cmd_valid    <= 1'b1;
                        sata_cmd_is_write <= 1'b0;
                        sata_cmd_lba      <= {3'b000, lat_addr[41:0], 3'b000};
                        sata_cmd_count    <= 16'd8;
                        // Signal response start
                        resp_start <= 1'b1;
                        resp_tag   <= lat_addr[31:0];
                        resp_flags <= 8'h00;
                    end
                end

                S_SATA_READ_DATA: begin
                    if (sata_cmd_error || timeout) begin
                        cmd_err_flag <= 1'b1;
                        resp_flags   <= 8'hFF;
                        // Reset pointers for TX phase
                        buf_rd_ptr   <= 11'd0;
                    end else if (sata_rd_valid) begin
                        // Write incoming data to buffer
                        if (active_buf == 1'b0)
                            buf_mem_0[buf_wr_ptr[9:0]] <= sata_rd_data;
                        else
                            buf_mem_1[buf_wr_ptr[9:0]] <= sata_rd_data;
                        buf_wr_ptr <= buf_wr_ptr + 1;
                        if (sata_rd_last || buf_wr_ptr == 11'd1023) begin
                            buf_rd_ptr <= 11'd0;
                        end
                    end
                    if (sata_cmd_complete && !sata_cmd_error) begin
                        buf_rd_ptr <= 11'd0;
                    end
                end

                S_NET_TX_DATA: begin
                    if (cmd_err_flag) begin
                        // Send single error DWORD
                        if (!resp_valid) begin
                            resp_data  <= 32'hDEAD_BEEF;
                            resp_valid <= 1'b1;
                            resp_last  <= 1'b1;
                            resp_flags <= 8'hFF;
                        end
                    end else begin
                        // Stream buffer to response
                        if (!resp_valid || resp_ready) begin
                            resp_data  <= buf_rd_data;
                            resp_valid <= 1'b1;
                            resp_last  <= (buf_rd_ptr == 11'd1023);
                            if (!resp_valid || resp_ready)
                                buf_rd_ptr <= buf_rd_ptr + 1;
                        end
                    end
                end

                S_NET_RX_DATA: begin
                    if (net_wr_valid) begin
                        if (active_buf == 1'b0)
                            buf_mem_0[buf_wr_ptr[9:0]] <= net_wr_data;
                        else
                            buf_mem_1[buf_wr_ptr[9:0]] <= net_wr_data;
                        buf_wr_ptr <= buf_wr_ptr + 1;
                    end
                end

                S_SATA_WRITE_CMD: begin
                    if (!sata_cmd_valid) begin
                        sata_cmd_valid    <= 1'b1;
                        sata_cmd_is_write <= 1'b1;
                        sata_cmd_lba      <= {3'b000, lat_addr[41:0], 3'b000};
                        sata_cmd_count    <= 16'd8;
                        buf_rd_ptr        <= 11'd0;
                    end
                end

                S_SATA_WRITE_DATA: begin
                    if (!cmd_err_flag && !timeout) begin
                        if (!sata_wr_valid || sata_wr_ready) begin
                            sata_wr_data  <= buf_rd_data;
                            sata_wr_valid <= 1'b1;
                            sata_wr_last  <= (buf_rd_ptr == 11'd1023);
                            buf_rd_ptr    <= buf_rd_ptr + 1;
                        end
                    end
                end

                S_WAIT_COMPLETE: begin
                    if (sata_cmd_error)
                        cmd_err_flag <= 1'b1;
                end

                S_SEND_ACK: begin
                    ack_valid <= 1'b1;
                    ack_byte  <= cmd_err_flag ? 8'hFF : 8'h00;
                end

                S_COMPLETE: begin
                    // Update stats
                    if (cmd_err_flag)
                        errors <= errors + 1;
                    else if (lat_is_write)
                        writes_completed <= writes_completed + 1;
                    else
                        reads_completed <= reads_completed + 1;
                    // Swap buffers
                    active_buf <= ~active_buf;
                end
            endcase
        end
    end

endmodule
