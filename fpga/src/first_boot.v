`timescale 1ns/1ps
module first_boot (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,         // pulse after SD init complete
    // SD card interface
    output reg         sd_cmd_start,
    output reg  [1:0]  sd_cmd_op,     // 01=read, 10=write
    output reg  [31:0] sd_block_addr,
    output reg  [7:0]  sd_write_data,
    output reg         sd_write_valid,
    input  wire [7:0]  sd_read_data,
    input  wire        sd_read_valid,
    input  wire        sd_cmd_done,
    input  wire        sd_cmd_error,
    output reg         sd_write_req,
    // Boot epoch output
    output reg  [31:0] boot_epoch,
    // Status
    output reg         done,
    output reg         busy
);

    // ---------------------------------------------------------------
    // SD addressing: key = (pack << 22) | card, block = key * 4
    // ---------------------------------------------------------------
    localparam [31:0] SYSCONFIG_BLOCK = ((32'd2 << 22) | 32'd0) << 2; // pack 2, card 0
    localparam [31:0] ADMIN_BLOCK     = ((32'd1 << 22) | 32'd0) << 2; // pack 1, card 0

    localparam [1:0] SD_OP_READ  = 2'b01;
    localparam [1:0] SD_OP_WRITE = 2'b10;

    localparam [15:0] CARD_MAGIC = 16'hCA7D;
    localparam CARD_SIZE = 2048;

    // ---------------------------------------------------------------
    // FSM states
    // ---------------------------------------------------------------
    localparam [3:0] S_IDLE             = 4'd0,
                     S_RD_SYSCONF_CMD   = 4'd1,
                     S_RD_SYSCONF_RECV  = 4'd2,
                     S_CHK_SYSCONF      = 4'd3,
                     S_WR_SYSCONF_CMD   = 4'd4,
                     S_WR_SYSCONF_DATA  = 4'd5,
                     S_WR_SYSCONF_WAIT  = 4'd6,
                     S_RD_ADMIN_CMD     = 4'd7,
                     S_RD_ADMIN_RECV    = 4'd8,
                     S_CHK_ADMIN        = 4'd9,
                     S_WR_ADMIN_CMD     = 4'd10,
                     S_WR_ADMIN_DATA    = 4'd11,
                     S_WR_ADMIN_WAIT    = 4'd12,
                     S_DONE             = 4'd13;

    reg [3:0]  state;
    reg [10:0] byte_cnt;       // 0-2047
    reg [7:0]  sysconf_buf [0:7]; // capture first 8 bytes of sysconfig
    reg [7:0]  admin_magic [0:1]; // capture first 2 bytes of admin
    reg        sysconf_found;
    reg        admin_found;

    // ---------------------------------------------------------------
    // Admin card factory-default template (2048 bytes, zero-padded)
    // ---------------------------------------------------------------
    reg [7:0] admin_template [0:2047];

    integer i;
    initial begin
        for (i = 0; i < 2048; i = i + 1)
            admin_template[i] = 8'h00;
        // Magic (0xCA7D little-endian)
        admin_template[0]  = 8'h7D;
        admin_template[1]  = 8'hCA;
        // Version = 1
        admin_template[2]  = 8'h01;
        // Field 0: username "admin" — ord=0, len=6, pfx=5, "admin"
        admin_template[4]  = 8'h00;
        admin_template[5]  = 8'h06;
        admin_template[6]  = 8'h05;
        admin_template[7]  = 8'h61; // 'a'
        admin_template[8]  = 8'h64; // 'd'
        admin_template[9]  = 8'h6D; // 'm'
        admin_template[10] = 8'h69; // 'i'
        admin_template[11] = 8'h6E; // 'n'
        // Field 1: password hash — ord=1, len=33, pfx=32, [32 zero bytes]
        admin_template[12] = 8'h01;
        admin_template[13] = 8'h21;
        admin_template[14] = 8'h20;
        // bytes 15-46 stay 0x00 (placeholder hash)
        // Field 2: salt — ord=2, len=17, pfx=16, [16 zero bytes]
        admin_template[47] = 8'h02;
        admin_template[48] = 8'h11;
        admin_template[49] = 8'h10;
        // bytes 50-65 stay 0x00 (salt)
        // Field 3: flags — ord=3, len=1, val=0 (enabled)
        admin_template[66] = 8'h03;
        admin_template[67] = 8'h01;
        // byte 68 stays 0x00
        // Field 4: failed attempts — ord=4, len=1, val=0
        admin_template[69] = 8'h04;
        admin_template[70] = 8'h01;
        // byte 71 stays 0x00
        // Field 5: read perms — ord=5, len=3, pfx=2, 0xFFFF (all)
        admin_template[72] = 8'h05;
        admin_template[73] = 8'h03;
        admin_template[74] = 8'h02;
        admin_template[75] = 8'hFF;
        admin_template[76] = 8'hFF;
        // Field 6: write perms — ord=6, len=3, pfx=2, 0xFFFF (all)
        admin_template[77] = 8'h06;
        admin_template[78] = 8'h03;
        admin_template[79] = 8'h02;
        admin_template[80] = 8'hFF;
        admin_template[81] = 8'hFF;
        // Field 7: delete perms — ord=7, len=3, pfx=2, 0xFFFF (all)
        admin_template[82] = 8'h07;
        admin_template[83] = 8'h03;
        admin_template[84] = 8'h02;
        admin_template[85] = 8'hFF;
        admin_template[86] = 8'hFF;
        // bytes 87-2047 stay 0x00
    end

    // ---------------------------------------------------------------
    // Sysconfig card byte generator (for write-back)
    //   [0:1] magic, [2:3] version, [4:7] boot_epoch LE, rest 0
    // ---------------------------------------------------------------
    reg [7:0] sysconf_byte;
    always @(*) begin
        case (byte_cnt)
            11'd0:   sysconf_byte = 8'h7D;
            11'd1:   sysconf_byte = 8'hCA;
            11'd2:   sysconf_byte = 8'h01;
            11'd3:   sysconf_byte = 8'h00;
            11'd4:   sysconf_byte = boot_epoch[7:0];
            11'd5:   sysconf_byte = boot_epoch[15:8];
            11'd6:   sysconf_byte = boot_epoch[23:16];
            11'd7:   sysconf_byte = boot_epoch[31:24];
            default: sysconf_byte = 8'h00;
        endcase
    end

    // ---------------------------------------------------------------
    // Main FSM
    // ---------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= S_IDLE;
            sd_cmd_start   <= 1'b0;
            sd_cmd_op      <= 2'b00;
            sd_block_addr  <= 32'd0;
            sd_write_data  <= 8'd0;
            sd_write_valid <= 1'b0;
            sd_write_req   <= 1'b0;
            boot_epoch     <= 32'd0;
            done           <= 1'b0;
            busy           <= 1'b0;
            byte_cnt       <= 11'd0;
            sysconf_found  <= 1'b0;
            admin_found    <= 1'b0;
        end else begin
            // Defaults — pulses clear each cycle
            sd_cmd_start   <= 1'b0;
            sd_write_valid <= 1'b0;

            case (state)
                // --------------------------------------------------
                S_IDLE: begin
                    done <= 1'b0;
                    if (start) begin
                        busy  <= 1'b1;
                        state <= S_RD_SYSCONF_CMD;
                    end
                end

                // ============ SYSCONFIG READ =======================
                S_RD_SYSCONF_CMD: begin
                    sd_block_addr <= SYSCONFIG_BLOCK;
                    sd_cmd_op     <= SD_OP_READ;
                    sd_cmd_start  <= 1'b1;
                    byte_cnt      <= 11'd0;
                    sysconf_found <= 1'b0;
                    state         <= S_RD_SYSCONF_RECV;
                end

                S_RD_SYSCONF_RECV: begin
                    if (sd_read_valid) begin
                        if (byte_cnt < 11'd8)
                            sysconf_buf[byte_cnt[2:0]] <= sd_read_data;
                        byte_cnt <= byte_cnt + 11'd1;
                    end
                    if (sd_cmd_done)
                        state <= S_CHK_SYSCONF;
                end

                S_CHK_SYSCONF: begin
                    if (!sd_cmd_error &&
                        sysconf_buf[0] == 8'h7D &&
                        sysconf_buf[1] == 8'hCA) begin
                        // Sysconfig exists — extract and increment boot_epoch
                        sysconf_found <= 1'b1;
                        boot_epoch <= {sysconf_buf[7], sysconf_buf[6],
                                       sysconf_buf[5], sysconf_buf[4]} + 32'd1;
                    end else begin
                        // First boot — initialise boot_epoch to 1
                        sysconf_found <= 1'b0;
                        boot_epoch    <= 32'd1;
                    end
                    state <= S_WR_SYSCONF_CMD;
                end

                // ============ SYSCONFIG WRITE ======================
                S_WR_SYSCONF_CMD: begin
                    sd_block_addr <= SYSCONFIG_BLOCK;
                    sd_cmd_op     <= SD_OP_WRITE;
                    sd_cmd_start  <= 1'b1;
                    sd_write_req  <= 1'b1;
                    byte_cnt      <= 11'd0;
                    state         <= S_WR_SYSCONF_DATA;
                end

                S_WR_SYSCONF_DATA: begin
                    sd_write_data  <= sysconf_byte;
                    sd_write_valid <= 1'b1;
                    byte_cnt       <= byte_cnt + 11'd1;
                    if (byte_cnt == (CARD_SIZE - 1)) begin
                        sd_write_req <= 1'b0;
                        state        <= S_WR_SYSCONF_WAIT;
                    end
                end

                S_WR_SYSCONF_WAIT: begin
                    if (sd_cmd_done)
                        state <= S_RD_ADMIN_CMD;
                end

                // ============ ADMIN READ ===========================
                S_RD_ADMIN_CMD: begin
                    sd_block_addr <= ADMIN_BLOCK;
                    sd_cmd_op     <= SD_OP_READ;
                    sd_cmd_start  <= 1'b1;
                    byte_cnt      <= 11'd0;
                    admin_found   <= 1'b0;
                    state         <= S_RD_ADMIN_RECV;
                end

                S_RD_ADMIN_RECV: begin
                    if (sd_read_valid) begin
                        if (byte_cnt < 11'd2)
                            admin_magic[byte_cnt[0]] <= sd_read_data;
                        byte_cnt <= byte_cnt + 11'd1;
                    end
                    if (sd_cmd_done)
                        state <= S_CHK_ADMIN;
                end

                S_CHK_ADMIN: begin
                    if (!sd_cmd_error &&
                        admin_magic[0] == 8'h7D &&
                        admin_magic[1] == 8'hCA) begin
                        // Admin card already exists — skip write
                        admin_found <= 1'b1;
                        state       <= S_DONE;
                    end else begin
                        admin_found <= 1'b0;
                        state       <= S_WR_ADMIN_CMD;
                    end
                end

                // ============ ADMIN WRITE ==========================
                S_WR_ADMIN_CMD: begin
                    sd_block_addr <= ADMIN_BLOCK;
                    sd_cmd_op     <= SD_OP_WRITE;
                    sd_cmd_start  <= 1'b1;
                    sd_write_req  <= 1'b1;
                    byte_cnt      <= 11'd0;
                    state         <= S_WR_ADMIN_DATA;
                end

                S_WR_ADMIN_DATA: begin
                    sd_write_data  <= admin_template[byte_cnt];
                    sd_write_valid <= 1'b1;
                    byte_cnt       <= byte_cnt + 11'd1;
                    if (byte_cnt == (CARD_SIZE - 1)) begin
                        sd_write_req <= 1'b0;
                        state        <= S_WR_ADMIN_WAIT;
                    end
                end

                S_WR_ADMIN_WAIT: begin
                    if (sd_cmd_done)
                        state <= S_DONE;
                end

                // ============ DONE =================================
                S_DONE: begin
                    done <= 1'b1;
                    busy <= 1'b0;
                    state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
