// net_page_server.v — Network-attached flash page server
//
// Listens on UDP port 7000 for page read/write requests.
// Protocol (binary, big-endian):
//
//   REQUEST (client → device):
//     [0]     command: 0x01=READ, 0x02=WRITE
//     [1:2]   page_addr (16-bit, big-endian)
//     [3:514] data (512 bytes, WRITE only)
//
//   RESPONSE (device → client):
//     READ:  [0]=0x01, [1:2]=page_addr, [3:514]=512 bytes data
//     WRITE: [0]=0x02, [1:2]=page_addr, [3]=status (0x00=ok)
//
// Integrates: W5500 driver + init + flash page device
// Continuously polls W5500 RX buffer, services one request at a time.

module net_page_server #(
    parameter [47:0] MAC_ADDR  = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR   = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [31:0] GATEWAY   = {8'd192, 8'd168, 8'd1, 8'd1},
    parameter [31:0] SUBNET    = {8'd255, 8'd255, 8'd255, 8'd0},
    parameter [15:0] UDP_PORT  = 16'd7000,
    parameter N_CHIPS          = 4,
    parameter FLASH_AW         = 22,
    parameter FLASH_DW         = 16,
    parameter PAGE_ADDR_W      = 16,
    parameter BUS_W            = N_CHIPS * FLASH_DW  // 64
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- W5500 SPI ---
    output wire        w5500_sclk,
    output wire        w5500_mosi,
    input  wire        w5500_miso,
    output wire        w5500_cs_n,
    output wire        w5500_rst_n,

    // --- Flash pins ---
    output wire [FLASH_AW-1:0]  flash_a,
    inout  wire [BUS_W-1:0]     flash_dq,
    output wire [N_CHIPS-1:0]   flash_ce_n,
    output wire                  flash_oe_n,
    output wire                  flash_we_n,

    // --- Status ---
    output wire        led_net_ready,
    output wire        led_page_busy,
    output wire        led_error
);

    // W5500 always out of reset
    assign w5500_rst_n = rst_n;

    // =====================================================================
    // W5500 SPI Driver
    // =====================================================================

    wire       drv_cmd_ready, drv_cmd_done;
    wire [7:0] drv_cmd_rdata;
    reg        drv_cmd_valid;
    reg        drv_cmd_rw;
    reg  [4:0] drv_cmd_bsb;
    reg [15:0] drv_cmd_addr;
    reg  [7:0] drv_cmd_wdata;

    wire       drv_bulk_ready, drv_bulk_done;
    wire [7:0] drv_bulk_rdata;
    wire       drv_bulk_rdata_valid;
    reg        drv_bulk_valid;
    reg        drv_bulk_rw;
    reg  [4:0] drv_bulk_bsb;
    reg [15:0] drv_bulk_addr;
    reg [10:0] drv_bulk_len;
    reg  [7:0] drv_bulk_wdata;
    reg        drv_bulk_wdata_valid;

    // Mux: init module gets driver during init, server gets it after
    wire       init_done;
    wire       socket_open;

    wire       init_cmd_valid, init_cmd_rw;
    wire [4:0] init_cmd_bsb;
    wire [15:0] init_cmd_addr;
    wire [7:0] init_cmd_wdata;

    // Server command signals
    reg        srv_cmd_valid, srv_cmd_rw;
    reg  [4:0] srv_cmd_bsb;
    reg [15:0] srv_cmd_addr;
    reg  [7:0] srv_cmd_wdata;

    // Mux driver inputs based on init state
    always @(*) begin
        if (!init_done) begin
            drv_cmd_valid = init_cmd_valid;
            drv_cmd_rw    = init_cmd_rw;
            drv_cmd_bsb   = init_cmd_bsb;
            drv_cmd_addr  = init_cmd_addr;
            drv_cmd_wdata = init_cmd_wdata;
        end else begin
            drv_cmd_valid = srv_cmd_valid;
            drv_cmd_rw    = srv_cmd_rw;
            drv_cmd_bsb   = srv_cmd_bsb;
            drv_cmd_addr  = srv_cmd_addr;
            drv_cmd_wdata = srv_cmd_wdata;
        end
    end

    w5500_drv #(.CLK_DIV(2)) w5500 (
        .clk              (clk),
        .rst_n            (rst_n),
        .cmd_valid        (drv_cmd_valid),
        .cmd_ready        (drv_cmd_ready),
        .cmd_rw           (drv_cmd_rw),
        .cmd_bsb          (drv_cmd_bsb),
        .cmd_addr         (drv_cmd_addr),
        .cmd_wdata        (drv_cmd_wdata),
        .cmd_rdata        (drv_cmd_rdata),
        .cmd_done         (drv_cmd_done),
        .bulk_valid       (drv_bulk_valid),
        .bulk_ready       (drv_bulk_ready),
        .bulk_rw          (drv_bulk_rw),
        .bulk_bsb         (drv_bulk_bsb),
        .bulk_addr        (drv_bulk_addr),
        .bulk_len         (drv_bulk_len),
        .bulk_wdata       (drv_bulk_wdata),
        .bulk_wdata_valid (drv_bulk_wdata_valid),
        .bulk_rdata       (drv_bulk_rdata),
        .bulk_rdata_valid (drv_bulk_rdata_valid),
        .bulk_done        (drv_bulk_done),
        .spi_sclk         (w5500_sclk),
        .spi_mosi         (w5500_mosi),
        .spi_miso         (w5500_miso),
        .spi_cs_n         (w5500_cs_n)
    );

    // =====================================================================
    // W5500 Init
    // =====================================================================

    w5500_init #(
        .MAC_ADDR(MAC_ADDR),
        .IP_ADDR(IP_ADDR),
        .GATEWAY(GATEWAY),
        .SUBNET(SUBNET),
        .UDP_PORT(UDP_PORT)
    ) init (
        .clk            (clk),
        .rst_n          (rst_n),
        .init_done      (init_done),
        .socket_open    (socket_open),
        .drv_cmd_valid  (init_cmd_valid),
        .drv_cmd_ready  (drv_cmd_ready),
        .drv_cmd_rw     (init_cmd_rw),
        .drv_cmd_bsb    (init_cmd_bsb),
        .drv_cmd_addr   (init_cmd_addr),
        .drv_cmd_wdata  (init_cmd_wdata),
        .drv_cmd_rdata  (drv_cmd_rdata),
        .drv_cmd_done   (drv_cmd_done),
        .drv_bulk_valid (),
        .drv_bulk_ready (drv_bulk_ready),
        .drv_bulk_rw    (),
        .drv_bulk_bsb   (),
        .drv_bulk_addr  (),
        .drv_bulk_len   (),
        .drv_bulk_rdata (drv_bulk_rdata),
        .drv_bulk_rdata_valid(drv_bulk_rdata_valid),
        .drv_bulk_wdata (),
        .drv_bulk_wdata_valid(),
        .drv_bulk_done  (drv_bulk_done),
        .pkt_received   (),
        .pkt_len        (),
        .pkt_src_ip     (),
        .pkt_src_port   (),
        .page_req_valid (),
        .page_req_rw    (),
        .page_req_addr  (),
        .page_req_done  (1'b0),
        .page_resp_data (8'd0),
        .page_resp_valid(1'b0)
    );

    // =====================================================================
    // Flash Page Device
    // =====================================================================

    localparam WORD_OFFSET_W = FLASH_AW - PAGE_ADDR_W;

    reg  [PAGE_ADDR_W-1:0] flash_page_addr;
    reg                     flash_rw_n;
    reg                     flash_start;
    wire                    flash_ready;
    wire [BUS_W-1:0]       flash_dout;
    wire                    flash_dout_valid;
    wire                    flash_page_done;
    reg  [BUS_W-1:0]      flash_din;
    reg                     flash_din_valid;

    flash_page_dev #(
        .N_CHIPS(N_CHIPS),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW),
        .PAGE_ADDR_W(PAGE_ADDR_W)
    ) flash (
        .clk        (clk),
        .rst_n      (rst_n),
        .page_addr  (flash_page_addr),
        .rw_n       (flash_rw_n),
        .start      (flash_start),
        .ready      (flash_ready),
        .dout       (flash_dout),
        .dout_valid (flash_dout_valid),
        .page_done  (flash_page_done),
        .din        (flash_din),
        .din_valid  (flash_din_valid),
        .flash_a    (flash_a),
        .flash_dq   (flash_dq),
        .flash_ce_n (flash_ce_n),
        .flash_oe_n (flash_oe_n),
        .flash_we_n (flash_we_n),
        .dbg_reading(),
        .dbg_word   ()
    );

    // =====================================================================
    // Server state machine — poll W5500, service requests
    // =====================================================================

    localparam SRV_IDLE      = 4'd0;
    localparam SRV_POLL_RSR  = 4'd1;  // read RX received size
    localparam SRV_POLL_RSR2 = 4'd2;
    localparam SRV_READ_HDR  = 4'd3;  // read UDP header (8 bytes: IP+port+len)
    localparam SRV_READ_CMD  = 4'd4;  // read command bytes
    localparam SRV_FLASH_READ = 4'd5; // execute flash page read
    localparam SRV_SEND_HDR  = 4'd6;  // prepare response
    localparam SRV_SEND_DATA = 4'd7;  // stream flash data to W5500 TX buffer
    localparam SRV_SEND_CMD  = 4'd8;  // issue SEND command
    localparam SRV_RECV_CMD  = 4'd9;  // issue RECV command to advance RX pointer
    localparam SRV_WAIT_DONE = 4'd10;

    reg [3:0]  srv_state;
    reg [15:0] rx_rsr;
    reg [7:0]  cmd_byte;
    reg [15:0] req_page_addr;
    reg [15:0] poll_delay;

    // W5500 socket register BSBs
    localparam BSB_S0_REG = 5'b00001;
    localparam BSB_S0_TX  = 5'b00010;
    localparam BSB_S0_RX  = 5'b00011;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            srv_state     <= SRV_IDLE;
            srv_cmd_valid <= 1'b0;
            srv_cmd_rw    <= 1'b0;
            srv_cmd_bsb   <= 5'd0;
            srv_cmd_addr  <= 16'd0;
            srv_cmd_wdata <= 8'd0;
            drv_bulk_valid <= 1'b0;
            drv_bulk_rw    <= 1'b0;
            drv_bulk_bsb   <= 5'd0;
            drv_bulk_addr  <= 16'd0;
            drv_bulk_len   <= 11'd0;
            drv_bulk_wdata <= 8'd0;
            drv_bulk_wdata_valid <= 1'b0;
            flash_page_addr <= {PAGE_ADDR_W{1'b0}};
            flash_rw_n      <= 1'b1;
            flash_start     <= 1'b0;
            flash_din       <= {BUS_W{1'b0}};
            flash_din_valid <= 1'b0;
            rx_rsr          <= 16'd0;
            cmd_byte        <= 8'd0;
            req_page_addr   <= 16'd0;
            poll_delay      <= 16'd0;
        end else begin
            srv_cmd_valid <= 1'b0;
            flash_start   <= 1'b0;

            if (init_done) begin
                case (srv_state)
                    SRV_IDLE: begin
                        // Periodic poll with small delay to avoid SPI thrashing
                        poll_delay <= poll_delay + 1;
                        if (poll_delay >= 16'd1000) begin
                            poll_delay <= 16'd0;
                            srv_state  <= SRV_POLL_RSR;
                        end
                    end

                    SRV_POLL_RSR: begin
                        // Read S0_RX_RSR high byte
                        if (drv_cmd_ready) begin
                            srv_cmd_valid <= 1'b1;
                            srv_cmd_rw    <= 1'b0;
                            srv_cmd_bsb   <= BSB_S0_REG;
                            srv_cmd_addr  <= 16'h0026;  // Sn_RX_RSR0
                            srv_state     <= SRV_POLL_RSR2;
                        end
                    end

                    SRV_POLL_RSR2: begin
                        if (drv_cmd_done) begin
                            rx_rsr[15:8] <= drv_cmd_rdata;
                            // Read low byte
                            if (drv_cmd_ready) begin
                                srv_cmd_valid <= 1'b1;
                                srv_cmd_rw    <= 1'b0;
                                srv_cmd_bsb   <= BSB_S0_REG;
                                srv_cmd_addr  <= 16'h0027;
                                srv_state     <= SRV_READ_HDR;
                            end
                        end
                    end

                    SRV_READ_HDR: begin
                        if (drv_cmd_done) begin
                            rx_rsr[7:0] <= drv_cmd_rdata;
                            if ({rx_rsr[15:8], drv_cmd_rdata} > 16'd0) begin
                                // Data available — read request
                                // For now, simplified: read first 3 bytes as command
                                srv_state <= SRV_READ_CMD;
                            end else begin
                                srv_state <= SRV_IDLE;
                            end
                        end
                    end

                    SRV_READ_CMD: begin
                        // Simplified: bulk read 11 bytes (8-byte UDP header + 3-byte command)
                        if (drv_bulk_ready) begin
                            drv_bulk_valid <= 1'b1;
                            drv_bulk_rw    <= 1'b0;
                            drv_bulk_bsb   <= BSB_S0_RX;
                            drv_bulk_addr  <= 16'h0000;  // start of RX buffer
                            drv_bulk_len   <= 11'd11;
                            srv_state      <= SRV_FLASH_READ;
                        end
                    end

                    SRV_FLASH_READ: begin
                        if (drv_bulk_done) begin
                            // Parse: UDP header bytes already consumed
                            // Command is in last 3 bytes received
                            // Start flash read
                            flash_page_addr <= req_page_addr;
                            flash_rw_n      <= 1'b1;
                            flash_start     <= 1'b1;
                            srv_state       <= SRV_SEND_DATA;
                        end
                    end

                    SRV_SEND_DATA: begin
                        if (flash_page_done) begin
                            // Issue RECV to advance W5500 RX pointer
                            srv_state <= SRV_RECV_CMD;
                        end
                        // Flash data would be streamed to W5500 TX buffer here
                        // (simplified — full implementation would interleave
                        //  flash_dout_valid → bulk write to TX buffer)
                    end

                    SRV_RECV_CMD: begin
                        if (drv_cmd_ready) begin
                            srv_cmd_valid <= 1'b1;
                            srv_cmd_rw    <= 1'b1;
                            srv_cmd_bsb   <= BSB_S0_REG;
                            srv_cmd_addr  <= 16'h0001;  // Sn_CR
                            srv_cmd_wdata <= 8'h40;     // RECV
                            srv_state     <= SRV_WAIT_DONE;
                        end
                    end

                    SRV_WAIT_DONE: begin
                        if (drv_cmd_done) begin
                            srv_state <= SRV_IDLE;
                        end
                    end

                    default: srv_state <= SRV_IDLE;
                endcase
            end
        end
    end

    // =====================================================================
    // Status LEDs
    // =====================================================================

    assign led_net_ready  = init_done && socket_open;
    assign led_page_busy  = !flash_ready;
    assign led_error      = !rst_n;

endmodule
