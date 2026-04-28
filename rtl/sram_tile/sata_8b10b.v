// SATA 8b/10b Encoder and Decoder
// ROM/table-driven approach using 5b/6b and 3b/4b sub-tables per the standard.

// ============================================================================
// 8b/10b Encoder
// ============================================================================
module sata_8b10b_enc (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  data_in,
    input  wire        k_in,        // 1 = control symbol
    input  wire        valid_in,
    output reg  [9:0]  data_out,
    output reg         valid_out
);

    reg rd; // running disparity: 0 = RD-, 1 = RD+

    // 5b/6b sub-encoder: returns {abcdei} for given RD
    function [5:0] enc_5b6b;
        input [4:0] d;
        input       k;
        input       cur_rd;
        reg [5:0] cn, cp; // code for RD-, RD+
        begin
            // K28 has unique 5b/6b code
            if (k && d == 5'd28) begin
                cn = 6'b001111; cp = 6'b110000;
            end else begin
                case (d)
                    5'd0:  begin cn = 6'b100111; cp = 6'b011000; end
                    5'd1:  begin cn = 6'b011101; cp = 6'b100010; end
                    5'd2:  begin cn = 6'b101101; cp = 6'b010010; end
                    5'd3:  begin cn = 6'b110001; cp = 6'b110001; end
                    5'd4:  begin cn = 6'b110101; cp = 6'b001010; end
                    5'd5:  begin cn = 6'b101001; cp = 6'b101001; end
                    5'd6:  begin cn = 6'b011001; cp = 6'b011001; end
                    5'd7:  begin cn = 6'b111000; cp = 6'b000111; end
                    5'd8:  begin cn = 6'b111001; cp = 6'b000110; end
                    5'd9:  begin cn = 6'b100101; cp = 6'b100101; end
                    5'd10: begin cn = 6'b010101; cp = 6'b010101; end
                    5'd11: begin cn = 6'b110100; cp = 6'b110100; end
                    5'd12: begin cn = 6'b001101; cp = 6'b001101; end
                    5'd13: begin cn = 6'b101100; cp = 6'b101100; end
                    5'd14: begin cn = 6'b011100; cp = 6'b011100; end
                    5'd15: begin cn = 6'b010111; cp = 6'b101000; end
                    5'd16: begin cn = 6'b011011; cp = 6'b100100; end
                    5'd17: begin cn = 6'b100011; cp = 6'b100011; end
                    5'd18: begin cn = 6'b010011; cp = 6'b010011; end
                    5'd19: begin cn = 6'b110010; cp = 6'b110010; end
                    5'd20: begin cn = 6'b001011; cp = 6'b001011; end
                    5'd21: begin cn = 6'b101010; cp = 6'b101010; end
                    5'd22: begin cn = 6'b011010; cp = 6'b011010; end
                    5'd23: begin cn = 6'b111010; cp = 6'b000101; end
                    5'd24: begin cn = 6'b110011; cp = 6'b001100; end
                    5'd25: begin cn = 6'b100110; cp = 6'b100110; end
                    5'd26: begin cn = 6'b010110; cp = 6'b010110; end
                    5'd27: begin cn = 6'b110110; cp = 6'b001001; end
                    5'd28: begin cn = 6'b001110; cp = 6'b001110; end
                    5'd29: begin cn = 6'b101110; cp = 6'b010001; end
                    5'd30: begin cn = 6'b011110; cp = 6'b100001; end
                    5'd31: begin cn = 6'b101011; cp = 6'b010100; end
                    default: begin cn = 6'b000000; cp = 6'b000000; end
                endcase
            end
            enc_5b6b = cur_rd ? cp : cn;
        end
    endfunction

    // 3b/4b sub-encoder: returns {fghj} for given intermediate RD
    function [3:0] enc_3b4b;
        input [2:0] d;
        input       k;
        input       mid_rd;
        input [4:0] x; // 5-bit input value for alternate D.x.7 check
        reg [3:0] cn, cp;
        reg use_alt;
        begin
            if (k) begin
                case (d)
                    3'd0: begin cn = 4'b1011; cp = 4'b0100; end
                    3'd1: begin cn = 4'b0110; cp = 4'b1001; end
                    3'd2: begin cn = 4'b1010; cp = 4'b0101; end
                    3'd3: begin cn = 4'b1100; cp = 4'b0011; end
                    3'd4: begin cn = 4'b1101; cp = 4'b0010; end
                    3'd5: begin cn = 4'b0101; cp = 4'b1010; end
                    3'd6: begin cn = 4'b1001; cp = 4'b0110; end
                    3'd7: begin cn = 4'b0111; cp = 4'b1000; end
                    default: begin cn = 4'b0000; cp = 4'b0000; end
                endcase
            end else begin
                case (d)
                    3'd0: begin cn = 4'b1011; cp = 4'b0100; end
                    3'd1: begin cn = 4'b1001; cp = 4'b1001; end
                    3'd2: begin cn = 4'b0101; cp = 4'b0101; end
                    3'd3: begin cn = 4'b1100; cp = 4'b0011; end
                    3'd4: begin cn = 4'b1101; cp = 4'b0010; end
                    3'd5: begin cn = 4'b1010; cp = 4'b1010; end
                    3'd6: begin cn = 4'b0110; cp = 4'b0110; end
                    3'd7: begin
                        // Alternate D.x.A7 to avoid comma-like patterns
                        use_alt = 1'b0;
                        if (!mid_rd && (x == 5'd17 || x == 5'd18 || x == 5'd20))
                            use_alt = 1'b1;
                        if (mid_rd && (x == 5'd11 || x == 5'd13 || x == 5'd14))
                            use_alt = 1'b1;
                        if (use_alt) begin
                            cn = 4'b0111; cp = 4'b1000;
                        end else begin
                            cn = 4'b1110; cp = 4'b0001;
                        end
                    end
                    default: begin cn = 4'b0000; cp = 4'b0000; end
                endcase
            end
            enc_3b4b = mid_rd ? cp : cn;
        end
    endfunction

    // Compute new RD from sub-block ones count
    wire [4:0] din5 = data_in[4:0];
    wire [2:0] din3 = data_in[7:5];

    wire [5:0] code6 = enc_5b6b(din5, k_in, rd);

    wire [2:0] ones6 = code6[0] + code6[1] + code6[2] + code6[3] + code6[4] + code6[5];
    wire rd_mid = (ones6 > 3'd3) ? 1'b1 : (ones6 < 3'd3) ? 1'b0 : rd;

    wire [3:0] code4 = enc_3b4b(din3, k_in, rd_mid, din5);

    wire [2:0] ones4 = code4[0] + code4[1] + code4[2] + code4[3];
    wire rd_next = (ones4 > 3'd2) ? 1'b1 : (ones4 < 3'd2) ? 1'b0 : rd_mid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd        <= 1'b0;
            data_out  <= 10'd0;
            valid_out <= 1'b0;
        end else begin
            valid_out <= valid_in;
            if (valid_in) begin
                data_out <= {code6, code4};
                rd       <= rd_next;
            end
        end
    end

endmodule

// ============================================================================
// 8b/10b Decoder
// ============================================================================
module sata_8b10b_dec (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [9:0]  data_in,
    input  wire        valid_in,
    output reg  [7:0]  data_out,
    output reg         k_out,
    output reg         err_out,
    output reg         valid_out
);

    reg rd; // running disparity: 0 = RD-, 1 = RD+

    // 6b/5b decode: returns {valid, k28, value[4:0]}
    function [6:0] dec_6b5b;
        input [5:0] c;
        begin
            case (c)
                6'b100111: dec_6b5b = {1'b1, 1'b0, 5'd0};
                6'b011000: dec_6b5b = {1'b1, 1'b0, 5'd0};
                6'b011101: dec_6b5b = {1'b1, 1'b0, 5'd1};
                6'b100010: dec_6b5b = {1'b1, 1'b0, 5'd1};
                6'b101101: dec_6b5b = {1'b1, 1'b0, 5'd2};
                6'b010010: dec_6b5b = {1'b1, 1'b0, 5'd2};
                6'b110001: dec_6b5b = {1'b1, 1'b0, 5'd3};
                6'b110101: dec_6b5b = {1'b1, 1'b0, 5'd4};
                6'b001010: dec_6b5b = {1'b1, 1'b0, 5'd4};
                6'b101001: dec_6b5b = {1'b1, 1'b0, 5'd5};
                6'b011001: dec_6b5b = {1'b1, 1'b0, 5'd6};
                6'b111000: dec_6b5b = {1'b1, 1'b0, 5'd7};
                6'b000111: dec_6b5b = {1'b1, 1'b0, 5'd7};
                6'b111001: dec_6b5b = {1'b1, 1'b0, 5'd8};
                6'b000110: dec_6b5b = {1'b1, 1'b0, 5'd8};
                6'b100101: dec_6b5b = {1'b1, 1'b0, 5'd9};
                6'b010101: dec_6b5b = {1'b1, 1'b0, 5'd10};
                6'b110100: dec_6b5b = {1'b1, 1'b0, 5'd11};
                6'b001101: dec_6b5b = {1'b1, 1'b0, 5'd12};
                6'b101100: dec_6b5b = {1'b1, 1'b0, 5'd13};
                6'b011100: dec_6b5b = {1'b1, 1'b0, 5'd14};
                6'b010111: dec_6b5b = {1'b1, 1'b0, 5'd15};
                6'b101000: dec_6b5b = {1'b1, 1'b0, 5'd15};
                6'b011011: dec_6b5b = {1'b1, 1'b0, 5'd16};
                6'b100100: dec_6b5b = {1'b1, 1'b0, 5'd16};
                6'b100011: dec_6b5b = {1'b1, 1'b0, 5'd17};
                6'b010011: dec_6b5b = {1'b1, 1'b0, 5'd18};
                6'b110010: dec_6b5b = {1'b1, 1'b0, 5'd19};
                6'b001011: dec_6b5b = {1'b1, 1'b0, 5'd20};
                6'b101010: dec_6b5b = {1'b1, 1'b0, 5'd21};
                6'b011010: dec_6b5b = {1'b1, 1'b0, 5'd22};
                6'b111010: dec_6b5b = {1'b1, 1'b0, 5'd23};
                6'b000101: dec_6b5b = {1'b1, 1'b0, 5'd23};
                6'b110011: dec_6b5b = {1'b1, 1'b0, 5'd24};
                6'b001100: dec_6b5b = {1'b1, 1'b0, 5'd24};
                6'b100110: dec_6b5b = {1'b1, 1'b0, 5'd25};
                6'b010110: dec_6b5b = {1'b1, 1'b0, 5'd26};
                6'b110110: dec_6b5b = {1'b1, 1'b0, 5'd27};
                6'b001001: dec_6b5b = {1'b1, 1'b0, 5'd27};
                6'b001110: dec_6b5b = {1'b1, 1'b0, 5'd28};
                6'b101110: dec_6b5b = {1'b1, 1'b0, 5'd29};
                6'b010001: dec_6b5b = {1'b1, 1'b0, 5'd29};
                6'b011110: dec_6b5b = {1'b1, 1'b0, 5'd30};
                6'b100001: dec_6b5b = {1'b1, 1'b0, 5'd30};
                6'b101011: dec_6b5b = {1'b1, 1'b0, 5'd31};
                6'b010100: dec_6b5b = {1'b1, 1'b0, 5'd31};
                // K28
                6'b001111: dec_6b5b = {1'b1, 1'b1, 5'd28};
                6'b110000: dec_6b5b = {1'b1, 1'b1, 5'd28};
                default:   dec_6b5b = {1'b0, 1'b0, 5'd0};
            endcase
        end
    endfunction

    // 4b/3b data decode: returns {valid, value[2:0]}
    function [3:0] dec_4b3b_d;
        input [3:0] c;
        begin
            case (c)
                4'b0100: dec_4b3b_d = {1'b1, 3'd0};
                4'b1011: dec_4b3b_d = {1'b1, 3'd0};
                4'b1001: dec_4b3b_d = {1'b1, 3'd1};
                4'b0101: dec_4b3b_d = {1'b1, 3'd2};
                4'b0011: dec_4b3b_d = {1'b1, 3'd3};
                4'b1100: dec_4b3b_d = {1'b1, 3'd3};
                4'b0010: dec_4b3b_d = {1'b1, 3'd4};
                4'b1101: dec_4b3b_d = {1'b1, 3'd4};
                4'b1010: dec_4b3b_d = {1'b1, 3'd5};
                4'b0110: dec_4b3b_d = {1'b1, 3'd6};
                4'b0001: dec_4b3b_d = {1'b1, 3'd7};
                4'b1110: dec_4b3b_d = {1'b1, 3'd7};
                4'b0111: dec_4b3b_d = {1'b1, 3'd7};
                4'b1000: dec_4b3b_d = {1'b1, 3'd7};
                default: dec_4b3b_d = {1'b0, 3'd0};
            endcase
        end
    endfunction

    // 4b/3b K28 decode: needs intermediate RD to disambiguate K28.1/6, K28.2/5
    function [3:0] dec_4b3b_k;
        input [3:0] c;
        input       mid_rd;
        begin
            if (!mid_rd) begin
                case (c)
                    4'b1011: dec_4b3b_k = {1'b1, 3'd0};
                    4'b0110: dec_4b3b_k = {1'b1, 3'd1};
                    4'b1010: dec_4b3b_k = {1'b1, 3'd2};
                    4'b1100: dec_4b3b_k = {1'b1, 3'd3};
                    4'b1101: dec_4b3b_k = {1'b1, 3'd4};
                    4'b0101: dec_4b3b_k = {1'b1, 3'd5};
                    4'b1001: dec_4b3b_k = {1'b1, 3'd6};
                    4'b0111: dec_4b3b_k = {1'b1, 3'd7};
                    default: dec_4b3b_k = {1'b0, 3'd0};
                endcase
            end else begin
                case (c)
                    4'b0100: dec_4b3b_k = {1'b1, 3'd0};
                    4'b1001: dec_4b3b_k = {1'b1, 3'd1};
                    4'b0101: dec_4b3b_k = {1'b1, 3'd2};
                    4'b0011: dec_4b3b_k = {1'b1, 3'd3};
                    4'b0010: dec_4b3b_k = {1'b1, 3'd4};
                    4'b1010: dec_4b3b_k = {1'b1, 3'd5};
                    4'b0110: dec_4b3b_k = {1'b1, 3'd6};
                    4'b1000: dec_4b3b_k = {1'b1, 3'd7};
                    default: dec_4b3b_k = {1'b0, 3'd0};
                endcase
            end
        end
    endfunction

    wire [5:0] code6 = data_in[9:4];
    wire [3:0] code4 = data_in[3:0];

    wire [6:0] r6    = dec_6b5b(code6);
    wire       v6    = r6[6];
    wire       is_k28 = r6[5];
    wire [4:0] val5  = r6[4:0];

    // Intermediate RD after 6-bit sub-block
    wire [2:0] ones6 = code6[0] + code6[1] + code6[2] + code6[3] + code6[4] + code6[5];
    wire rd_mid = (ones6 > 3'd3) ? 1'b1 : (ones6 < 3'd3) ? 1'b0 : rd;

    // Data path decode
    wire [3:0] r4d   = dec_4b3b_d(code4);
    wire       v4d   = r4d[3];
    wire [2:0] val3d = r4d[2:0];

    // K28 path decode
    wire [3:0] r4k   = dec_4b3b_k(code4, rd_mid);
    wire       v4k   = r4k[3];
    wire [2:0] val3k = r4k[2:0];

    // K.x.7 detection (x != 28): for x in {23,27,29,30} with alternate-7 4b code
    wire is_kx7 = !is_k28 &&
                  (val5 == 5'd23 || val5 == 5'd27 || val5 == 5'd29 || val5 == 5'd30) &&
                  (code4 == 4'b0111 || code4 == 4'b1000);

    // Output RD after full symbol
    wire [2:0] ones4 = code4[0] + code4[1] + code4[2] + code4[3];
    wire rd_next = (ones4 > 3'd2) ? 1'b1 : (ones4 < 3'd2) ? 1'b0 : rd_mid;

    // Disparity error detection
    wire disp6_pos = (ones6 > 3'd3); // 6b sub-block has positive disparity
    wire disp6_neg = (ones6 < 3'd3);
    wire disp4_pos = (ones4 > 3'd2);
    wire disp4_neg = (ones4 < 3'd2);
    wire rd_err6 = (disp6_pos && rd) || (disp6_neg && !rd);
    wire rd_err4 = (disp4_pos && rd_mid) || (disp4_neg && !rd_mid);

    // Select decoded values
    wire       use_k28_path = is_k28;
    wire       is_k = is_k28 || is_kx7;
    wire       v4   = use_k28_path ? v4k : v4d;
    wire [2:0] val3 = use_k28_path ? val3k : val3d;
    wire       symbol_err = !v6 || !v4;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd        <= 1'b0;
            data_out  <= 8'd0;
            k_out     <= 1'b0;
            err_out   <= 1'b0;
            valid_out <= 1'b0;
        end else begin
            valid_out <= valid_in;
            if (valid_in) begin
                data_out <= {val3, val5};
                k_out    <= is_k;
                err_out  <= symbol_err || rd_err6 || rd_err4;
                rd       <= rd_next;
            end
        end
    end

endmodule
