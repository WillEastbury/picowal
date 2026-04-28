// udp_page_tx.v — UDP Page Fragmenter
//
// Splits a 4KB page into 3 UDP packets that fit standard 1500-byte MTU:
//   Packet 0: 8-byte app header + 1400 bytes data = 1408 payload
//   Packet 1: 8-byte app header + 1400 bytes data = 1408 payload
//   Packet 2: 8-byte app header + 1296 bytes data = 1304 payload
//   Total: 1400 + 1400 + 1296 = 4096 bytes
//
// App header (8 bytes):
//   [7:0]   sequence (0, 1, 2)
//   [15:8]  total_fragments (3)
//   [23:16] flags (0x00 for data, 0xFF for error/miss)
//   [31:24] reserved
//   [63:32] request_tag (echoed from request for matching)
//
// Each UDP payload = 8 app header + up to 1400 data = max 1408
// UDP+IP+Eth overhead = 14+20+8 = 42 bytes
// Max frame: 42 + 1408 = 1450 bytes — fits MTU

module udp_page_tx #(
    parameter PAGE_SIZE  = 4096,
    parameter FRAG_DATA  = 1400,    // data bytes per fragment
    parameter N_FRAGS    = 3,       // ceil(4096/1400) = 3
    parameter LAST_FRAG  = 1296    // 4096 - 2*1400
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Page buffer read port ---
    output reg  [11:0] buf_addr,    // 0-4095
    input  wire [7:0]  buf_data,    // byte from page buffer

    // --- Control ---
    input  wire        start,       // pulse: begin fragmenting
    input  wire [31:0] req_tag,     // echoed in app header
    input  wire [7:0]  resp_flags,  // 0x00=data, 0xFF=error
    output reg         done,        // all fragments sent
    output reg         busy,

    // --- Frame output (byte stream to MAC) ---
    output reg  [7:0]  tx_data,
    output reg         tx_valid,
    output reg         tx_sof,      // start of frame
    output reg         tx_eof,      // end of frame
    input  wire        tx_ready     // MAC can accept
);

    localparam S_IDLE    = 3'd0;
    localparam S_HEADER  = 3'd1;    // emit app header (8 bytes)
    localparam S_DATA    = 3'd2;    // emit page data
    localparam S_NEXT    = 3'd3;    // advance to next fragment
    localparam S_DONE    = 3'd4;

    reg [2:0]  state;
    reg [1:0]  frag_idx;            // 0, 1, 2
    reg [10:0] byte_cnt;            // within current fragment
    reg [2:0]  hdr_cnt;             // header byte counter
    reg [11:0] page_offset;         // cumulative offset into page

    wire [10:0] frag_len = (frag_idx == N_FRAGS - 1) ? LAST_FRAG[10:0] : FRAG_DATA[10:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            frag_idx    <= 2'd0;
            byte_cnt    <= 11'd0;
            hdr_cnt     <= 3'd0;
            page_offset <= 12'd0;
            buf_addr    <= 12'd0;
            tx_data     <= 8'd0;
            tx_valid    <= 1'b0;
            tx_sof      <= 1'b0;
            tx_eof      <= 1'b0;
            done        <= 1'b0;
            busy        <= 1'b0;
        end else begin
            tx_valid <= 1'b0;
            tx_sof   <= 1'b0;
            tx_eof   <= 1'b0;
            done     <= 1'b0;

            case (state)
                S_IDLE: begin
                    busy <= 1'b0;
                    if (start) begin
                        frag_idx    <= 2'd0;
                        page_offset <= 12'd0;
                        busy        <= 1'b1;
                        state       <= S_HEADER;
                        hdr_cnt     <= 3'd0;
                    end
                end

                S_HEADER: begin
                    if (tx_ready) begin
                        tx_valid <= 1'b1;
                        if (hdr_cnt == 3'd0) tx_sof <= 1'b1;

                        case (hdr_cnt)
                            3'd0: tx_data <= {6'd0, frag_idx};          // sequence
                            3'd1: tx_data <= N_FRAGS[7:0];              // total
                            3'd2: tx_data <= resp_flags;                // flags
                            3'd3: tx_data <= 8'd0;                      // reserved
                            3'd4: tx_data <= req_tag[31:24];            // tag bytes
                            3'd5: tx_data <= req_tag[23:16];
                            3'd6: tx_data <= req_tag[15:8];
                            3'd7: tx_data <= req_tag[7:0];
                            default: tx_data <= 8'd0;
                        endcase

                        hdr_cnt <= hdr_cnt + 1;
                        if (hdr_cnt == 3'd7) begin
                            byte_cnt <= 11'd0;
                            buf_addr <= page_offset;
                            state    <= S_DATA;
                        end
                    end
                end

                S_DATA: begin
                    if (tx_ready) begin
                        tx_data  <= buf_data;
                        tx_valid <= 1'b1;
                        byte_cnt <= byte_cnt + 1;
                        buf_addr <= page_offset + byte_cnt[11:0] + 1;

                        if (byte_cnt == frag_len - 1) begin
                            tx_eof <= 1'b1;
                            state  <= S_NEXT;
                        end
                    end
                end

                S_NEXT: begin
                    page_offset <= page_offset + {1'b0, frag_len};
                    frag_idx    <= frag_idx + 1;

                    if (frag_idx == N_FRAGS - 1) begin
                        state <= S_DONE;
                    end else begin
                        hdr_cnt <= 3'd0;
                        state   <= S_HEADER;
                    end
                end

                S_DONE: begin
                    done  <= 1'b1;
                    busy  <= 1'b0;
                    state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
