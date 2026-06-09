`timescale 1ns/1ps
module status_counters (
    input  wire        clk,
    input  wire        rst_n,
    // Events to count
    input  wire        evt_read,
    input  wire        evt_write,
    input  wire        evt_delete,
    input  wire        evt_list,
    input  wire        evt_mget,
    input  wire        evt_login_ok,
    input  wire        evt_login_fail,
    input  wire        evt_auth_fail,
    input  wire        evt_sd_error,
    // Tick counter
    input  wire [31:0] tick_counter,
    // Read interface (for /status response)
    input  wire        read_start,
    output reg  [7:0]  data_out,
    output reg         data_valid,
    output reg         data_done
);

    // ---------------------------------------------------------------
    // 32-bit event counters (always incrementing)
    // ---------------------------------------------------------------
    reg [31:0] cnt_reads;
    reg [31:0] cnt_writes;
    reg [31:0] cnt_deletes;
    reg [31:0] cnt_lists;
    reg [31:0] cnt_mgets;
    reg [31:0] cnt_logins;
    reg [31:0] cnt_fails;
    reg [31:0] cnt_auth_err;
    reg [31:0] cnt_sd_err;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cnt_reads    <= 32'd0;
            cnt_writes   <= 32'd0;
            cnt_deletes  <= 32'd0;
            cnt_lists    <= 32'd0;
            cnt_mgets    <= 32'd0;
            cnt_logins   <= 32'd0;
            cnt_fails    <= 32'd0;
            cnt_auth_err <= 32'd0;
            cnt_sd_err   <= 32'd0;
        end else begin
            if (evt_read)       cnt_reads    <= cnt_reads    + 32'd1;
            if (evt_write)      cnt_writes   <= cnt_writes   + 32'd1;
            if (evt_delete)     cnt_deletes  <= cnt_deletes  + 32'd1;
            if (evt_list)       cnt_lists    <= cnt_lists    + 32'd1;
            if (evt_mget)       cnt_mgets    <= cnt_mgets    + 32'd1;
            if (evt_login_ok)   cnt_logins   <= cnt_logins   + 32'd1;
            if (evt_login_fail) cnt_fails    <= cnt_fails    + 32'd1;
            if (evt_auth_fail)  cnt_auth_err <= cnt_auth_err + 32'd1;
            if (evt_sd_error)   cnt_sd_err   <= cnt_sd_err   + 32'd1;
        end
    end

    // ---------------------------------------------------------------
    // Snapshot registers (latched on read_start)
    // ---------------------------------------------------------------
    reg [31:0] snap [0:9];

    // ---------------------------------------------------------------
    // FSM states
    // ---------------------------------------------------------------
    localparam [3:0] S_IDLE      = 4'd0,
                     S_SNAPSHOT  = 4'd1,
                     S_LABEL     = 4'd2,
                     S_CONV_INIT = 4'd3,
                     S_CONV_SUB  = 4'd4,
                     S_SKIP_ZERO = 4'd5,
                     S_EMIT_DIG  = 4'd6,
                     S_NEWLINE   = 4'd7,
                     S_NEXT      = 4'd8,
                     S_FINISH    = 4'd9;

    reg [3:0]  state;
    reg [3:0]  ctr_idx;       // which counter 0-9
    reg [6:0]  label_pos;     // flat position in label ROM
    reg [6:0]  label_end;     // one past last byte of current label
    reg [31:0] conv_val;      // value being converted
    reg [3:0]  dig_idx;       // power-of-10 index (0 = 10^9 .. 9 = 10^0)
    reg [3:0]  cur_dig;       // current digit accumulator
    reg [3:0]  dig_buf [0:9]; // converted decimal digits
    reg [3:0]  emit_idx;      // digit emit index

    // ---------------------------------------------------------------
    // Label ROM — flat character lookup (69 bytes total)
    //  0: "reads:"    6: "writes:"   13: "deletes:"  21: "lists:"
    // 27: "mgets:"   33: "logins:"   40: "fails:"    46: "auth_err:"
    // 55: "sd_err:"  62: "uptime:"
    // ---------------------------------------------------------------
    function [7:0] label_char;
        input [6:0] idx;
        case (idx)
            7'd0:  label_char = 8'h72; // r
            7'd1:  label_char = 8'h65; // e
            7'd2:  label_char = 8'h61; // a
            7'd3:  label_char = 8'h64; // d
            7'd4:  label_char = 8'h73; // s
            7'd5:  label_char = 8'h3A; // :
            7'd6:  label_char = 8'h77; // w
            7'd7:  label_char = 8'h72; // r
            7'd8:  label_char = 8'h69; // i
            7'd9:  label_char = 8'h74; // t
            7'd10: label_char = 8'h65; // e
            7'd11: label_char = 8'h73; // s
            7'd12: label_char = 8'h3A; // :
            7'd13: label_char = 8'h64; // d
            7'd14: label_char = 8'h65; // e
            7'd15: label_char = 8'h6C; // l
            7'd16: label_char = 8'h65; // e
            7'd17: label_char = 8'h74; // t
            7'd18: label_char = 8'h65; // e
            7'd19: label_char = 8'h73; // s
            7'd20: label_char = 8'h3A; // :
            7'd21: label_char = 8'h6C; // l
            7'd22: label_char = 8'h69; // i
            7'd23: label_char = 8'h73; // s
            7'd24: label_char = 8'h74; // t
            7'd25: label_char = 8'h73; // s
            7'd26: label_char = 8'h3A; // :
            7'd27: label_char = 8'h6D; // m
            7'd28: label_char = 8'h67; // g
            7'd29: label_char = 8'h65; // e
            7'd30: label_char = 8'h74; // t
            7'd31: label_char = 8'h73; // s
            7'd32: label_char = 8'h3A; // :
            7'd33: label_char = 8'h6C; // l
            7'd34: label_char = 8'h6F; // o
            7'd35: label_char = 8'h67; // g
            7'd36: label_char = 8'h69; // i
            7'd37: label_char = 8'h6E; // n
            7'd38: label_char = 8'h73; // s
            7'd39: label_char = 8'h3A; // :
            7'd40: label_char = 8'h66; // f
            7'd41: label_char = 8'h61; // a
            7'd42: label_char = 8'h69; // i
            7'd43: label_char = 8'h6C; // l
            7'd44: label_char = 8'h73; // s
            7'd45: label_char = 8'h3A; // :
            7'd46: label_char = 8'h61; // a
            7'd47: label_char = 8'h75; // u
            7'd48: label_char = 8'h74; // t
            7'd49: label_char = 8'h68; // h
            7'd50: label_char = 8'h5F; // _
            7'd51: label_char = 8'h65; // e
            7'd52: label_char = 8'h72; // r
            7'd53: label_char = 8'h72; // r
            7'd54: label_char = 8'h3A; // :
            7'd55: label_char = 8'h73; // s
            7'd56: label_char = 8'h64; // d
            7'd57: label_char = 8'h5F; // _
            7'd58: label_char = 8'h65; // e
            7'd59: label_char = 8'h72; // r
            7'd60: label_char = 8'h72; // r
            7'd61: label_char = 8'h3A; // :
            7'd62: label_char = 8'h75; // u
            7'd63: label_char = 8'h70; // p
            7'd64: label_char = 8'h74; // t
            7'd65: label_char = 8'h69; // i
            7'd66: label_char = 8'h6D; // m
            7'd67: label_char = 8'h65; // e
            7'd68: label_char = 8'h3A; // :
            default: label_char = 8'h00;
        endcase
    endfunction

    // Label start offset per counter index
    function [6:0] label_start;
        input [3:0] idx;
        case (idx)
            4'd0: label_start = 7'd0;
            4'd1: label_start = 7'd6;
            4'd2: label_start = 7'd13;
            4'd3: label_start = 7'd21;
            4'd4: label_start = 7'd27;
            4'd5: label_start = 7'd33;
            4'd6: label_start = 7'd40;
            4'd7: label_start = 7'd46;
            4'd8: label_start = 7'd55;
            4'd9: label_start = 7'd62;
            default: label_start = 7'd0;
        endcase
    endfunction

    // Label length per counter index
    function [3:0] label_len;
        input [3:0] idx;
        case (idx)
            4'd0: label_len = 4'd6;  // reads:
            4'd1: label_len = 4'd7;  // writes:
            4'd2: label_len = 4'd8;  // deletes:
            4'd3: label_len = 4'd6;  // lists:
            4'd4: label_len = 4'd6;  // mgets:
            4'd5: label_len = 4'd7;  // logins:
            4'd6: label_len = 4'd6;  // fails:
            4'd7: label_len = 4'd9;  // auth_err:
            4'd8: label_len = 4'd7;  // sd_err:
            4'd9: label_len = 4'd7;  // uptime:
            default: label_len = 4'd0;
        endcase
    endfunction

    // ---------------------------------------------------------------
    // Powers of 10 for repeated-subtraction decimal conversion
    // ---------------------------------------------------------------
    function [31:0] pow10;
        input [3:0] idx;
        case (idx)
            4'd0: pow10 = 32'd1000000000;
            4'd1: pow10 = 32'd100000000;
            4'd2: pow10 = 32'd10000000;
            4'd3: pow10 = 32'd1000000;
            4'd4: pow10 = 32'd100000;
            4'd5: pow10 = 32'd10000;
            4'd6: pow10 = 32'd1000;
            4'd7: pow10 = 32'd100;
            4'd8: pow10 = 32'd10;
            4'd9: pow10 = 32'd1;
            default: pow10 = 32'd1;
        endcase
    endfunction

    // ---------------------------------------------------------------
    // Main FSM
    // ---------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            data_out   <= 8'd0;
            data_valid <= 1'b0;
            data_done  <= 1'b0;
            ctr_idx    <= 4'd0;
            label_pos  <= 7'd0;
            label_end  <= 7'd0;
            conv_val   <= 32'd0;
            dig_idx    <= 4'd0;
            cur_dig    <= 4'd0;
            emit_idx   <= 4'd0;
        end else begin
            // Defaults
            data_valid <= 1'b0;
            data_done  <= 1'b0;

            case (state)
                // --------------------------------------------------
                S_IDLE: begin
                    if (read_start) begin
                        state <= S_SNAPSHOT;
                    end
                end

                // Latch all counter values atomically
                S_SNAPSHOT: begin
                    snap[0] <= cnt_reads;
                    snap[1] <= cnt_writes;
                    snap[2] <= cnt_deletes;
                    snap[3] <= cnt_lists;
                    snap[4] <= cnt_mgets;
                    snap[5] <= cnt_logins;
                    snap[6] <= cnt_fails;
                    snap[7] <= cnt_auth_err;
                    snap[8] <= cnt_sd_err;
                    snap[9] <= tick_counter;
                    ctr_idx  <= 4'd0;
                    state    <= S_LABEL;
                    label_pos <= label_start(4'd0);
                    label_end <= label_start(4'd0) + {3'd0, label_len(4'd0)};
                end

                // Stream label bytes
                S_LABEL: begin
                    data_out   <= label_char(label_pos);
                    data_valid <= 1'b1;
                    label_pos  <= label_pos + 7'd1;
                    if (label_pos + 7'd1 == label_end)
                        state <= S_CONV_INIT;
                end

                // Prepare decimal conversion
                S_CONV_INIT: begin
                    conv_val <= snap[ctr_idx];
                    dig_idx  <= 4'd0;
                    cur_dig  <= 4'd0;
                    state    <= S_CONV_SUB;
                end

                // Repeated subtraction: one subtract or one digit-advance per cycle
                S_CONV_SUB: begin
                    if (conv_val >= pow10(dig_idx)) begin
                        conv_val <= conv_val - pow10(dig_idx);
                        cur_dig  <= cur_dig + 4'd1;
                    end else begin
                        dig_buf[dig_idx] <= cur_dig;
                        cur_dig <= 4'd0;
                        if (dig_idx == 4'd9) begin
                            emit_idx <= 4'd0;
                            state    <= S_SKIP_ZERO;
                        end else begin
                            dig_idx <= dig_idx + 4'd1;
                        end
                    end
                end

                // Skip leading zeros (keep at least the units digit)
                S_SKIP_ZERO: begin
                    if (dig_buf[emit_idx] != 4'd0 || emit_idx == 4'd9)
                        state <= S_EMIT_DIG;
                    else
                        emit_idx <= emit_idx + 4'd1;
                end

                // Emit ASCII decimal digits
                S_EMIT_DIG: begin
                    data_out   <= 8'h30 + {4'd0, dig_buf[emit_idx]};
                    data_valid <= 1'b1;
                    if (emit_idx == 4'd9)
                        state <= S_NEWLINE;
                    else
                        emit_idx <= emit_idx + 4'd1;
                end

                // Emit newline, advance to next counter
                S_NEWLINE: begin
                    data_out   <= 8'h0A;
                    data_valid <= 1'b1;
                    state      <= S_NEXT;
                end

                // Move to next counter or finish
                S_NEXT: begin
                    if (ctr_idx == 4'd9) begin
                        state <= S_FINISH;
                    end else begin
                        ctr_idx   <= ctr_idx + 4'd1;
                        label_pos <= label_start(ctr_idx + 4'd1);
                        label_end <= label_start(ctr_idx + 4'd1)
                                     + {3'd0, label_len(ctr_idx + 4'd1)};
                        state     <= S_LABEL;
                    end
                end

                // Signal completion
                S_FINISH: begin
                    data_done <= 1'b1;
                    state     <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
