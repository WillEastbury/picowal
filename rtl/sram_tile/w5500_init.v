// w5500_init.v — W5500 initialization and UDP socket manager
//
// Configures W5500 common registers (MAC, IP, gateway, subnet)
// and opens a UDP socket on a configurable port.
//
// After init, provides a simple request/response interface:
//   - Polls for incoming UDP packets
//   - Presents received data to the page controller
//   - Sends response data back via UDP
//
// W5500 register addresses (key ones):
//   Common: GAR=0x0001, SUBR=0x0005, SHAR=0x0009, SIPR=0x000F
//   Socket0: Sn_MR=0x0000, Sn_CR=0x0001, Sn_IR=0x0002, Sn_SR=0x0003
//            Sn_PORT=0x0004, Sn_TX_WR=0x0024, Sn_RX_RSR=0x0026
//            Sn_RX_RD=0x0028
//
// BSB mapping: 00000=common, 00001=S0_REG, 00010=S0_TX, 00011=S0_RX

module w5500_init #(
    parameter [47:0] MAC_ADDR  = 48'h02_00_00_00_00_01,  // locally administered
    parameter [31:0] IP_ADDR   = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [31:0] GATEWAY   = {8'd192, 8'd168, 8'd1, 8'd1},
    parameter [31:0] SUBNET    = {8'd255, 8'd255, 8'd255, 8'd0},
    parameter [15:0] UDP_PORT  = 16'd7000
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Status ---
    output reg         init_done,
    output reg         socket_open,

    // --- W5500 driver command interface ---
    output reg         drv_cmd_valid,
    input  wire        drv_cmd_ready,
    output reg         drv_cmd_rw,
    output reg  [4:0]  drv_cmd_bsb,
    output reg  [15:0] drv_cmd_addr,
    output reg  [7:0]  drv_cmd_wdata,
    input  wire [7:0]  drv_cmd_rdata,
    input  wire        drv_cmd_done,

    // --- Incoming packet notification ---
    output reg         pkt_received,
    output reg  [15:0] pkt_len,
    output reg  [31:0] pkt_src_ip,
    output reg  [15:0] pkt_src_port,

    // --- Bulk interface passthrough (for data transfer) ---
    output reg         drv_bulk_valid,
    input  wire        drv_bulk_ready,
    output reg         drv_bulk_rw,
    output reg  [4:0]  drv_bulk_bsb,
    output reg  [15:0] drv_bulk_addr,
    output reg  [10:0] drv_bulk_len,
    input  wire [7:0]  drv_bulk_rdata,
    input  wire        drv_bulk_rdata_valid,
    output reg  [7:0]  drv_bulk_wdata,
    output reg         drv_bulk_wdata_valid,
    input  wire        drv_bulk_done,

    // --- Page request interface ---
    output reg         page_req_valid,
    output reg         page_req_rw,       // 1=read, 0=write
    output reg  [15:0] page_req_addr,
    input  wire        page_req_done,
    input  wire [7:0]  page_resp_data,
    input  wire        page_resp_valid
);

    // =====================================================================
    // W5500 register addresses
    // =====================================================================

    // Common registers (BSB=00000)
    localparam REG_GAR0  = 16'h0001;   // Gateway
    localparam REG_SUBR0 = 16'h0005;   // Subnet mask
    localparam REG_SHAR0 = 16'h0009;   // MAC address
    localparam REG_SIPR0 = 16'h000F;   // IP address

    // Socket 0 registers (BSB=00001)
    localparam REG_S0_MR     = 16'h0000;
    localparam REG_S0_CR     = 16'h0001;
    localparam REG_S0_IR     = 16'h0002;
    localparam REG_S0_SR     = 16'h0003;
    localparam REG_S0_PORT0  = 16'h0004;
    localparam REG_S0_RXRSR0 = 16'h0026;
    localparam REG_S0_RXRD0  = 16'h0028;
    localparam REG_S0_TXWR0  = 16'h0024;

    // BSB values
    localparam BSB_COMMON = 5'b00000;
    localparam BSB_S0_REG = 5'b00001;
    localparam BSB_S0_TX  = 5'b00010;
    localparam BSB_S0_RX  = 5'b00011;

    // Socket commands
    localparam CMD_OPEN  = 8'h01;
    localparam CMD_CLOSE = 8'h10;
    localparam CMD_SEND  = 8'h20;
    localparam CMD_RECV  = 8'h40;

    // Socket modes
    localparam MODE_UDP  = 8'h02;

    // Socket status
    localparam SOCK_UDP  = 8'h22;

    // =====================================================================
    // Init sequence table: {bsb, addr, data}
    // =====================================================================

    localparam INIT_LEN = 20;

    reg [7:0] init_data [0:INIT_LEN-1];
    reg [15:0] init_addr [0:INIT_LEN-1];
    reg [4:0] init_bsb [0:INIT_LEN-1];

    initial begin
        // Gateway (4 bytes)
        init_bsb[0]=BSB_COMMON; init_addr[0]=REG_GAR0;   init_data[0]=GATEWAY[31:24];
        init_bsb[1]=BSB_COMMON; init_addr[1]=REG_GAR0+1; init_data[1]=GATEWAY[23:16];
        init_bsb[2]=BSB_COMMON; init_addr[2]=REG_GAR0+2; init_data[2]=GATEWAY[15:8];
        init_bsb[3]=BSB_COMMON; init_addr[3]=REG_GAR0+3; init_data[3]=GATEWAY[7:0];
        // Subnet (4 bytes)
        init_bsb[4]=BSB_COMMON; init_addr[4]=REG_SUBR0;   init_data[4]=SUBNET[31:24];
        init_bsb[5]=BSB_COMMON; init_addr[5]=REG_SUBR0+1; init_data[5]=SUBNET[23:16];
        init_bsb[6]=BSB_COMMON; init_addr[6]=REG_SUBR0+2; init_data[6]=SUBNET[15:8];
        init_bsb[7]=BSB_COMMON; init_addr[7]=REG_SUBR0+3; init_data[7]=SUBNET[7:0];
        // MAC (6 bytes)
        init_bsb[8] =BSB_COMMON; init_addr[8] =REG_SHAR0;   init_data[8] =MAC_ADDR[47:40];
        init_bsb[9] =BSB_COMMON; init_addr[9] =REG_SHAR0+1; init_data[9] =MAC_ADDR[39:32];
        init_bsb[10]=BSB_COMMON; init_addr[10]=REG_SHAR0+2; init_data[10]=MAC_ADDR[31:24];
        init_bsb[11]=BSB_COMMON; init_addr[11]=REG_SHAR0+3; init_data[11]=MAC_ADDR[23:16];
        init_bsb[12]=BSB_COMMON; init_addr[12]=REG_SHAR0+4; init_data[12]=MAC_ADDR[15:8];
        init_bsb[13]=BSB_COMMON; init_addr[13]=REG_SHAR0+5; init_data[13]=MAC_ADDR[7:0];
        // IP (4 bytes)
        init_bsb[14]=BSB_COMMON; init_addr[14]=REG_SIPR0;   init_data[14]=IP_ADDR[31:24];
        init_bsb[15]=BSB_COMMON; init_addr[15]=REG_SIPR0+1; init_data[15]=IP_ADDR[23:16];
        init_bsb[16]=BSB_COMMON; init_addr[16]=REG_SIPR0+2; init_data[16]=IP_ADDR[15:8];
        init_bsb[17]=BSB_COMMON; init_addr[17]=REG_SIPR0+3; init_data[17]=IP_ADDR[7:0];
        // Socket 0: mode=UDP, port
        init_bsb[18]=BSB_S0_REG; init_addr[18]=REG_S0_MR;    init_data[18]=MODE_UDP;
        init_bsb[19]=BSB_S0_REG; init_addr[19]=REG_S0_PORT0; init_data[19]=UDP_PORT[15:8];
    end

    // =====================================================================
    // State machine
    // =====================================================================

    localparam ST_RESET     = 4'd0;
    localparam ST_INIT_WRITE = 4'd1;
    localparam ST_INIT_WAIT = 4'd2;
    localparam ST_PORT_LO   = 4'd3;
    localparam ST_OPEN_CMD  = 4'd4;
    localparam ST_OPEN_WAIT = 4'd5;
    localparam ST_CHECK_SR  = 4'd6;
    localparam ST_POLL_RX   = 4'd7;
    localparam ST_READ_RSR  = 4'd8;
    localparam ST_IDLE      = 4'd9;

    reg [3:0]  state;
    reg [4:0]  init_idx;
    reg [15:0] delay_cnt;
    reg [7:0]  rsr_hi, rsr_lo;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= ST_RESET;
            init_done       <= 1'b0;
            socket_open     <= 1'b0;
            drv_cmd_valid   <= 1'b0;
            drv_cmd_rw      <= 1'b0;
            drv_cmd_bsb     <= 5'd0;
            drv_cmd_addr    <= 16'd0;
            drv_cmd_wdata   <= 8'd0;
            drv_bulk_valid  <= 1'b0;
            drv_bulk_rw     <= 1'b0;
            drv_bulk_bsb    <= 5'd0;
            drv_bulk_addr   <= 16'd0;
            drv_bulk_len    <= 11'd0;
            drv_bulk_wdata  <= 8'd0;
            drv_bulk_wdata_valid <= 1'b0;
            pkt_received    <= 1'b0;
            pkt_len         <= 16'd0;
            pkt_src_ip      <= 32'd0;
            pkt_src_port    <= 16'd0;
            page_req_valid  <= 1'b0;
            page_req_rw     <= 1'b0;
            page_req_addr   <= 16'd0;
            init_idx        <= 5'd0;
            delay_cnt       <= 16'd0;
            rsr_hi          <= 8'd0;
            rsr_lo          <= 8'd0;
        end else begin
            drv_cmd_valid  <= 1'b0;
            pkt_received   <= 1'b0;
            page_req_valid <= 1'b0;

            case (state)
                ST_RESET: begin
                    delay_cnt <= delay_cnt + 1;
                    if (delay_cnt >= 16'd1000) begin  // ~33μs power-up delay
                        init_idx  <= 5'd0;
                        state     <= ST_INIT_WRITE;
                    end
                end

                ST_INIT_WRITE: begin
                    if (drv_cmd_ready) begin
                        drv_cmd_valid <= 1'b1;
                        drv_cmd_rw    <= 1'b1;  // write
                        drv_cmd_bsb   <= init_bsb[init_idx];
                        drv_cmd_addr  <= init_addr[init_idx];
                        drv_cmd_wdata <= init_data[init_idx];
                        state         <= ST_INIT_WAIT;
                    end
                end

                ST_INIT_WAIT: begin
                    if (drv_cmd_done) begin
                        if (init_idx == INIT_LEN - 1) begin
                            state <= ST_PORT_LO;
                        end else begin
                            init_idx <= init_idx + 1;
                            state    <= ST_INIT_WRITE;
                        end
                    end
                end

                ST_PORT_LO: begin
                    // Write port low byte
                    if (drv_cmd_ready) begin
                        drv_cmd_valid <= 1'b1;
                        drv_cmd_rw    <= 1'b1;
                        drv_cmd_bsb   <= BSB_S0_REG;
                        drv_cmd_addr  <= REG_S0_PORT0 + 1;
                        drv_cmd_wdata <= UDP_PORT[7:0];
                        state         <= ST_OPEN_CMD;
                    end
                end

                ST_OPEN_CMD: begin
                    if (drv_cmd_done && drv_cmd_ready) begin
                        // Send OPEN command
                        drv_cmd_valid <= 1'b1;
                        drv_cmd_rw    <= 1'b1;
                        drv_cmd_bsb   <= BSB_S0_REG;
                        drv_cmd_addr  <= REG_S0_CR;
                        drv_cmd_wdata <= CMD_OPEN;
                        state         <= ST_OPEN_WAIT;
                    end
                end

                ST_OPEN_WAIT: begin
                    if (drv_cmd_done) begin
                        state <= ST_CHECK_SR;
                    end
                end

                ST_CHECK_SR: begin
                    // Read socket status
                    if (drv_cmd_ready) begin
                        drv_cmd_valid <= 1'b1;
                        drv_cmd_rw    <= 1'b0;  // read
                        drv_cmd_bsb   <= BSB_S0_REG;
                        drv_cmd_addr  <= REG_S0_SR;
                        state         <= ST_POLL_RX;
                    end
                end

                ST_POLL_RX: begin
                    if (drv_cmd_done) begin
                        if (drv_cmd_rdata == SOCK_UDP) begin
                            socket_open <= 1'b1;
                            init_done   <= 1'b1;
                            state       <= ST_IDLE;
                        end else begin
                            // Retry
                            delay_cnt <= 16'd0;
                            state     <= ST_CHECK_SR;
                        end
                    end
                end

                ST_IDLE: begin
                    // Main loop: poll RX received size
                    // Higher-level module handles packet processing
                    state <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
