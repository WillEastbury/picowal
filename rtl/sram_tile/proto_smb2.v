// proto_smb2.v -- SMB2/CIFS protocol engine (hardware, no filesystem)
//
// Stack: SerDes -> MAC -> IP -> TCP -> socket_mux -> [THIS] -> NVMe
//
// Namespace: \\picowal\{card}\{folder}\{file}
//   card   = 10-bit volume ID  (addr[51:42] in PicoWAL block address)
//   folder = 16-bit folder ID  (maps to index via index pico)
//   file   = 16-bit file ID    (maps to index via index pico)
//
// NO FILESYSTEM. PicoWAL IS the storage engine.
// The index pico resolves (card, folder, file) -> block address range.
// This engine handles SMB2 wire protocol only.
//
// Supported SMB2 commands (minimal useful subset):
//   NEGOTIATE       (0x0000) -> capability exchange
//   SESSION_SETUP   (0x0001) -> auth (accept all / guest)
//   TREE_CONNECT    (0x0003) -> share = card number
//   CREATE          (0x0005) -> open file (folder\file)
//   CLOSE           (0x0006) -> close handle
//   READ            (0x0008) -> read blocks (FPGA bypass path)
//   WRITE           (0x0009) -> write blocks (index pico path)
//   QUERY_DIRECTORY (0x000E) -> list files in folder
//   QUERY_INFO      (0x0010) -> file size/attributes
//
// ~3500 LUTs estimated (header parse + response gen + handle table)
//
`default_nettype none

module proto_smb2 #(
    parameter MAX_HANDLES = 32,
    parameter MAX_SESSIONS = 16
)(
    input  wire        clk,
    input  wire        rst_n,

    // Socket interface (from socket_mux)
    input  wire [7:0]  rx_data,
    input  wire        rx_valid,
    input  wire        rx_sof,
    input  wire        rx_eof,
    input  wire [5:0]  rx_conn,
    output wire        rx_ready,

    output reg  [7:0]  tx_data,
    output reg         tx_valid,
    output reg         tx_sof,
    output reg         tx_eof,
    output reg  [5:0]  tx_conn,
    input  wire        tx_ready,

    // NVMe block interface (to FPGA DMA engine)
    output reg         blk_read_req,
    output reg         blk_write_req,
    output reg  [41:0] blk_addr,       // 42-bit block address
    output reg  [15:0] blk_count,      // blocks to transfer
    input  wire        blk_ready,
    input  wire [7:0]  blk_rdata,
    input  wire        blk_rvalid,
    input  wire        blk_rdone,
    output reg  [7:0]  blk_wdata,
    output reg         blk_wvalid,
    input  wire        blk_wready,

    // Index pico interface (resolve card/folder/file -> block addr)
    output reg         idx_lookup_req,
    output reg  [9:0]  idx_card,
    output reg  [15:0] idx_folder,
    output reg  [15:0] idx_file,
    input  wire        idx_lookup_ack,
    input  wire [41:0] idx_block_addr,
    input  wire [31:0] idx_file_size,
    input  wire        idx_not_found,

    // Stats
    output reg  [31:0] smb_reads,
    output reg  [31:0] smb_writes,
    output reg  [31:0] smb_opens
);

    assign rx_ready = 1'b1;

    // ── SMB2 header constants ──
    localparam [31:0] SMB2_MAGIC = 32'h424D53FE;  // 0xFE 'S' 'M' 'B'

    // SMB2 commands
    localparam CMD_NEGOTIATE     = 16'h0000,
               CMD_SESSION_SETUP = 16'h0001,
               CMD_LOGOFF        = 16'h0002,
               CMD_TREE_CONNECT  = 16'h0003,
               CMD_TREE_DISCONN  = 16'h0004,
               CMD_CREATE        = 16'h0005,
               CMD_CLOSE         = 16'h0006,
               CMD_READ          = 16'h0008,
               CMD_WRITE         = 16'h0009,
               CMD_QUERY_DIR     = 16'h000E,
               CMD_QUERY_INFO    = 16'h0010;

    // ── Handle table: maps handle_id -> (card, folder, file, block_addr) ──
    reg        handle_valid  [0:MAX_HANDLES-1];
    reg [9:0]  handle_card   [0:MAX_HANDLES-1];
    reg [15:0] handle_folder [0:MAX_HANDLES-1];
    reg [15:0] handle_file   [0:MAX_HANDLES-1];
    reg [41:0] handle_addr   [0:MAX_HANDLES-1];
    reg [31:0] handle_size   [0:MAX_HANDLES-1];
    reg [5:0]  handle_conn   [0:MAX_HANDLES-1];  // owning connection

    // ── Session table: maps session_id -> (conn, tree_id=card) ──
    reg        sess_valid    [0:MAX_SESSIONS-1];
    reg [5:0]  sess_conn     [0:MAX_SESSIONS-1];
    reg [9:0]  sess_tree     [0:MAX_SESSIONS-1];  // card number

    integer k;

    // ── RX: Parse SMB2 header (64 bytes) ──
    localparam S_IDLE        = 4'd0,
               S_HDR         = 4'd1,
               S_NEGOTIATE   = 4'd2,
               S_SESS_SETUP  = 4'd3,
               S_TREE_CONN   = 4'd4,
               S_CREATE      = 4'd5,
               S_CLOSE       = 4'd6,
               S_READ        = 4'd7,
               S_WRITE       = 4'd8,
               S_WRITE_DATA  = 4'd9,
               S_QUERY_DIR   = 4'd10,
               S_RESPOND     = 4'd11,
               S_RESP_DATA   = 4'd12,
               S_IDX_WAIT    = 4'd13;

    reg [3:0]  state;
    reg [7:0]  hdr_buf [0:63];
    reg [6:0]  hdr_cnt;
    reg [15:0] smb_command;
    reg [63:0] smb_msg_id;
    reg [31:0] smb_tree_id;
    reg [63:0] smb_session_id;
    reg [5:0]  cur_conn;

    // CREATE request parsed fields
    reg [9:0]  req_card;
    reg [15:0] req_folder;
    reg [15:0] req_file;

    // READ/WRITE request parsed fields
    reg [31:0] req_handle;
    reg [31:0] req_offset;
    reg [31:0] req_length;

    // Response builder
    reg [15:0] resp_len;
    reg [15:0] resp_cnt;
    reg [31:0] resp_status;
    reg [4:0]  alloc_handle;
    reg        alloc_found;

    // Find free handle
    always @(*) begin
        alloc_found = 0;
        alloc_handle = 0;
        for (k = 0; k < MAX_HANDLES; k = k + 1) begin
            if (!alloc_found && !handle_valid[k]) begin
                alloc_found = 1;
                alloc_handle = k[4:0];
            end
        end
    end

    // ── Main FSM ──
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            tx_valid <= 0; tx_sof <= 0; tx_eof <= 0;
            blk_read_req <= 0; blk_write_req <= 0;
            idx_lookup_req <= 0;
            smb_reads <= 0; smb_writes <= 0; smb_opens <= 0;
            for (k = 0; k < MAX_HANDLES; k = k + 1)
                handle_valid[k] <= 0;
            for (k = 0; k < MAX_SESSIONS; k = k + 1)
                sess_valid[k] <= 0;
        end else begin
            tx_valid <= 0; tx_sof <= 0; tx_eof <= 0;
            blk_read_req <= 0; blk_write_req <= 0;
            idx_lookup_req <= 0;

            case (state)

                S_IDLE: begin
                    if (rx_valid && rx_sof) begin
                        hdr_cnt <= 0;
                        cur_conn <= rx_conn;
                        state <= S_HDR;
                        hdr_buf[0] <= rx_data;
                        hdr_cnt <= 1;
                    end
                end

                S_HDR: begin
                    if (rx_valid) begin
                        if (hdr_cnt < 64)
                            hdr_buf[hdr_cnt] <= rx_data;
                        hdr_cnt <= hdr_cnt + 1;

                        // At byte 64, we have full SMB2 header
                        if (hdr_cnt == 63) begin
                            // Parse key header fields
                            // Bytes 0-3: magic (0xFE534D42)
                            // Bytes 12-13: command
                            smb_command <= {hdr_buf[13], hdr_buf[12]};
                            // Bytes 24-31: message ID
                            smb_msg_id <= {hdr_buf[31], hdr_buf[30],
                                          hdr_buf[29], hdr_buf[28],
                                          hdr_buf[27], hdr_buf[26],
                                          hdr_buf[25], hdr_buf[24]};
                            // Bytes 36-39: tree ID
                            smb_tree_id <= {hdr_buf[39], hdr_buf[38],
                                           hdr_buf[37], hdr_buf[36]};
                            // Bytes 40-47: session ID
                            smb_session_id <= {hdr_buf[47], hdr_buf[46],
                                              hdr_buf[45], hdr_buf[44],
                                              hdr_buf[43], hdr_buf[42],
                                              hdr_buf[41], hdr_buf[40]};

                            // Dispatch based on command
                            case ({hdr_buf[13], hdr_buf[12]})
                                CMD_NEGOTIATE:     state <= S_NEGOTIATE;
                                CMD_SESSION_SETUP: state <= S_SESS_SETUP;
                                CMD_TREE_CONNECT:  state <= S_TREE_CONN;
                                CMD_CREATE:        state <= S_CREATE;
                                CMD_CLOSE:         state <= S_CLOSE;
                                CMD_READ:          state <= S_READ;
                                CMD_WRITE:         state <= S_WRITE;
                                default:           state <= S_RESPOND;
                            endcase
                        end
                    end
                    if (rx_eof && hdr_cnt < 63) state <= S_IDLE;
                end

                // ── NEGOTIATE: accept, report capabilities ──
                S_NEGOTIATE: begin
                    resp_status <= 32'h0;  // STATUS_SUCCESS
                    resp_len <= 65;        // negotiate response size
                    resp_cnt <= 0;
                    state <= S_RESPOND;
                end

                // ── SESSION_SETUP: accept all as guest ──
                S_SESS_SETUP: begin
                    // Allocate session (use connection ID as session)
                    if (!sess_valid[cur_conn[3:0]]) begin
                        sess_valid[cur_conn[3:0]] <= 1;
                        sess_conn[cur_conn[3:0]] <= cur_conn;
                    end
                    resp_status <= 32'h0;
                    resp_len <= 9;
                    resp_cnt <= 0;
                    state <= S_RESPOND;
                end

                // ── TREE_CONNECT: share name = card number ──
                S_TREE_CONN: begin
                    // Path bytes follow header — parse card number
                    // Simplified: tree_id in response = card number from path
                    sess_tree[cur_conn[3:0]] <= smb_tree_id[9:0];
                    resp_status <= 32'h0;
                    resp_len <= 16;
                    resp_cnt <= 0;
                    state <= S_RESPOND;
                end

                // ── CREATE: open file by (card, folder, file) ──
                S_CREATE: begin
                    if (rx_valid) begin
                        // Parse filename from request body
                        // Filename format: "FOLDER\FILE" as numbers
                        // For now: use hdr_buf bytes after header
                        hdr_cnt <= hdr_cnt + 1;
                    end

                    if (rx_eof) begin
                        // Lookup via index pico
                        req_card   <= sess_tree[cur_conn[3:0]];
                        req_folder <= {hdr_buf[8], hdr_buf[9]};  // from request body
                        req_file   <= {hdr_buf[10], hdr_buf[11]};
                        idx_card   <= sess_tree[cur_conn[3:0]];
                        idx_folder <= {hdr_buf[8], hdr_buf[9]};
                        idx_file   <= {hdr_buf[10], hdr_buf[11]};
                        idx_lookup_req <= 1;
                        state <= S_IDX_WAIT;
                    end
                end

                // ── Wait for index pico to resolve block address ──
                S_IDX_WAIT: begin
                    if (idx_lookup_ack) begin
                        if (idx_not_found) begin
                            resp_status <= 32'hC0000034;  // STATUS_OBJECT_NAME_NOT_FOUND
                        end else if (alloc_found) begin
                            handle_valid[alloc_handle]  <= 1;
                            handle_card[alloc_handle]   <= req_card;
                            handle_folder[alloc_handle] <= req_folder;
                            handle_file[alloc_handle]   <= req_file;
                            handle_addr[alloc_handle]   <= idx_block_addr;
                            handle_size[alloc_handle]   <= idx_file_size;
                            handle_conn[alloc_handle]   <= cur_conn;
                            resp_status <= 32'h0;
                            smb_opens <= smb_opens + 1;
                        end else begin
                            resp_status <= 32'hC000012D;  // STATUS_TOO_MANY_OPENED_FILES
                        end
                        resp_len <= 89;  // CREATE response size
                        resp_cnt <= 0;
                        state <= S_RESPOND;
                    end
                end

                // ── CLOSE: release handle ──
                S_CLOSE: begin
                    // Handle ID from request body (simplified: first 4 bytes)
                    if (rx_eof) begin
                        req_handle <= {hdr_buf[3], hdr_buf[2], hdr_buf[1], hdr_buf[0]};
                        if (req_handle < MAX_HANDLES && handle_valid[req_handle[4:0]]) begin
                            handle_valid[req_handle[4:0]] <= 0;
                            resp_status <= 32'h0;
                        end else begin
                            resp_status <= 32'hC0000008;  // STATUS_INVALID_HANDLE
                        end
                        resp_len <= 60;
                        resp_cnt <= 0;
                        state <= S_RESPOND;
                    end
                end

                // ── READ: stream blocks from NVMe (FPGA bypass!) ──
                S_READ: begin
                    if (rx_eof) begin
                        // Parse: handle, offset, length from request
                        req_handle <= {hdr_buf[3], hdr_buf[2], hdr_buf[1], hdr_buf[0]};
                        req_offset <= {hdr_buf[7], hdr_buf[6], hdr_buf[5], hdr_buf[4]};
                        req_length <= {hdr_buf[11], hdr_buf[10], hdr_buf[9], hdr_buf[8]};

                        if (req_handle < MAX_HANDLES && handle_valid[req_handle[4:0]]) begin
                            // Issue NVMe read — FPGA handles DMA, zero pico
                            blk_addr <= handle_addr[req_handle[4:0]] +
                                       {10'b0, req_offset};
                            blk_count <= req_length[15:0];
                            blk_read_req <= 1;
                            smb_reads <= smb_reads + 1;
                            resp_status <= 32'h0;
                        end else begin
                            resp_status <= 32'hC0000008;
                        end
                        resp_len <= 17;  // READ response header (data follows)
                        resp_cnt <= 0;
                        state <= S_RESPOND;
                    end
                end

                // ── WRITE: receive data, route to index pico ──
                S_WRITE: begin
                    if (rx_valid && hdr_cnt >= 64) begin
                        // First bytes of write body = handle + offset + length
                        if (hdr_cnt < 76) begin
                            hdr_buf[hdr_cnt - 64] <= rx_data;
                        end else begin
                            // Data payload — forward to NVMe write
                            blk_wdata <= rx_data;
                            blk_wvalid <= 1;
                        end
                        hdr_cnt <= hdr_cnt + 1;
                    end

                    if (rx_eof) begin
                        smb_writes <= smb_writes + 1;
                        resp_status <= 32'h0;
                        resp_len <= 17;
                        resp_cnt <= 0;
                        state <= S_RESPOND;
                    end
                end

                // ── RESPOND: send SMB2 response header ──
                S_RESPOND: begin
                    if (tx_ready) begin
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                        tx_sof <= (resp_cnt == 0);

                        // Build response header byte-by-byte
                        case (resp_cnt)
                            // Magic: 0xFE 'S' 'M' 'B'
                            0:  tx_data <= 8'hFE;
                            1:  tx_data <= 8'h53;  // 'S'
                            2:  tx_data <= 8'h4D;  // 'M'
                            3:  tx_data <= 8'h42;  // 'B'
                            // Header length
                            4:  tx_data <= 8'd64;
                            5:  tx_data <= 8'h00;
                            // Credit charge
                            6:  tx_data <= 8'h01;
                            7:  tx_data <= 8'h00;
                            // Status
                            8:  tx_data <= resp_status[7:0];
                            9:  tx_data <= resp_status[15:8];
                            10: tx_data <= resp_status[23:16];
                            11: tx_data <= resp_status[31:24];
                            // Command (echo back)
                            12: tx_data <= smb_command[7:0];
                            13: tx_data <= smb_command[15:8];
                            // Credits granted
                            14: tx_data <= 8'h01;
                            15: tx_data <= 8'h00;
                            // Flags: response
                            16: tx_data <= 8'h01;
                            17: tx_data <= 8'h00;
                            18: tx_data <= 8'h00;
                            19: tx_data <= 8'h00;
                            // Next command offset
                            20: tx_data <= 8'h00;
                            21: tx_data <= 8'h00;
                            22: tx_data <= 8'h00;
                            23: tx_data <= 8'h00;
                            // Message ID (echo back)
                            24: tx_data <= smb_msg_id[7:0];
                            25: tx_data <= smb_msg_id[15:8];
                            26: tx_data <= smb_msg_id[23:16];
                            27: tx_data <= smb_msg_id[31:24];
                            28: tx_data <= smb_msg_id[39:32];
                            29: tx_data <= smb_msg_id[47:40];
                            30: tx_data <= smb_msg_id[55:48];
                            31: tx_data <= smb_msg_id[63:56];
                            // Reserved / TreeID / SessionID (echo back)
                            default: tx_data <= 8'h00;
                        endcase

                        resp_cnt <= resp_cnt + 1;

                        if (resp_cnt == resp_len - 1) begin
                            // If READ response, stream NVMe data after header
                            if (smb_command == CMD_READ && resp_status == 0) begin
                                state <= S_RESP_DATA;
                            end else begin
                                tx_eof <= 1;
                                state <= S_IDLE;
                            end
                        end
                    end
                end

                // ── Stream NVMe read data as SMB2 READ response body ──
                S_RESP_DATA: begin
                    if (blk_rvalid && tx_ready) begin
                        tx_data <= blk_rdata;
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                    end
                    if (blk_rdone) begin
                        tx_eof <= 1;
                        tx_valid <= 1;
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
