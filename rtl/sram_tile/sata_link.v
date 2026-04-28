// SATA Link Layer
// Sits between PHY (32-bit DWORD + charisk) and Transport layer.
// Handles frame TX/RX with CRC, scrambling, and HOLD flow control.
// Internally instantiates sata_crc32 and sata_scrambler (one pair TX, one RX).

module sata_link (
    input  wire        clk,
    input  wire        rst_n,

    // --- PHY interface (32-bit) ---
    output reg  [31:0] phy_tx_data,
    output reg  [3:0]  phy_tx_isk,
    input  wire [31:0] phy_rx_data,
    input  wire [3:0]  phy_rx_isk,
    input  wire        phy_ready,

    // --- Transport layer interface ---
    // TX: transport pushes FIS DWORDs
    input  wire [31:0] tx_data,
    input  wire        tx_valid,
    input  wire        tx_last,
    output wire        tx_ready,
    input  wire        tx_start,
    output reg         tx_done,
    output reg         tx_err,

    // RX: link pushes FIS DWORDs to transport
    output reg  [31:0] rx_data,
    output reg         rx_valid,
    output reg         rx_last,
    output reg         rx_sof,
    output reg         rx_err
);

    // =========================================================================
    // SATA Primitives (32-bit, byte 0 = LSB = K28.5)
    // Dx.y byte value = {y[2:0], x[4:0]}
    // K28.5 = 8'hBC
    // =========================================================================
    localparam [31:0] PRIM_ALIGN = {8'h7B, 8'h4A, 8'h4A, 8'hBC}; // K28.5 D10.2 D10.2 D27.3
    localparam [31:0] PRIM_SYNC  = {8'h95, 8'h95, 8'h95, 8'hBC}; // K28.5 D21.4 D21.4 D21.4
    localparam [31:0] PRIM_SOF   = {8'h17, 8'hB5, 8'hB5, 8'hBC}; // K28.5 D21.5 D21.5 D23.0
    localparam [31:0] PRIM_EOF   = {8'hD5, 8'hD5, 8'hD5, 8'hBC}; // K28.5 D21.6 D21.6 D21.6
    localparam [31:0] PRIM_ROK   = {8'h89, 8'h95, 8'h95, 8'hBC}; // K28.5 D21.4 D21.4 D9.4
    localparam [31:0] PRIM_RERR  = {8'h19, 8'h95, 8'h95, 8'hBC}; // K28.5 D21.4 D21.4 D25.0
    localparam [31:0] PRIM_HOLD  = {8'hB5, 8'h95, 8'h95, 8'hBC}; // K28.5 D21.4 D21.4 D21.5
    localparam [31:0] PRIM_HOLDA = {8'h95, 8'h95, 8'h95, 8'hBC}; // K28.5 D21.4 D21.4 D21.4
    localparam [31:0] PRIM_XRDY  = {8'h37, 8'h37, 8'h37, 8'hBC}; // K28.5 D23.1 D23.1 D23.1
    localparam [31:0] PRIM_RRDY  = {8'hB5, 8'h4A, 8'h4A, 8'hBC}; // K28.5 D10.2 D10.2 D21.5
    localparam [31:0] PRIM_WTRM  = {8'h18, 8'h18, 8'h18, 8'hBC}; // K28.5 D24.0 D24.0 D24.0

    // =========================================================================
    // State encodings
    // =========================================================================
    localparam [3:0] TX_IDLE = 4'd0,
                     TX_XRDY = 4'd1,
                     TX_SOF  = 4'd2,
                     TX_DATA = 4'd3,
                     TX_CRC  = 4'd4,
                     TX_EOF  = 4'd5,
                     TX_WTRM = 4'd6;

    localparam [3:0] RX_IDLE   = 4'd0,
                     RX_RRDY   = 4'd1,
                     RX_DATA   = 4'd2,
                     RX_CHECK  = 4'd3,
                     RX_STATUS = 4'd4;

    reg [3:0] tx_state, rx_state;

    // =========================================================================
    // Primitive detection (combinational)
    // =========================================================================
    wire rx_is_prim   = (phy_rx_isk == 4'b0001);
    wire rx_is_data   = (phy_rx_isk == 4'b0000);
    wire rx_prim_sync = rx_is_prim && (phy_rx_data == PRIM_SYNC);
    wire rx_prim_sof  = rx_is_prim && (phy_rx_data == PRIM_SOF);
    wire rx_prim_eof  = rx_is_prim && (phy_rx_data == PRIM_EOF);
    wire rx_prim_rok  = rx_is_prim && (phy_rx_data == PRIM_ROK);
    wire rx_prim_rerr = rx_is_prim && (phy_rx_data == PRIM_RERR);
    wire rx_prim_hold = rx_is_prim && (phy_rx_data == PRIM_HOLD);
    wire rx_prim_xrdy = rx_is_prim && (phy_rx_data == PRIM_XRDY);
    wire rx_prim_rrdy = rx_is_prim && (phy_rx_data == PRIM_RRDY);
    wire rx_prim_wtrm = rx_is_prim && (phy_rx_data == PRIM_WTRM);

    // =========================================================================
    // TX CRC and Scrambler
    // =========================================================================
    wire        tx_crc_init;
    wire [31:0] tx_crc_data_in;
    wire        tx_crc_valid;
    wire [31:0] tx_crc_out;

    wire        tx_scr_init;
    wire [31:0] tx_scr_data_in;
    wire        tx_scr_valid;
    wire [31:0] tx_scr_data_out;

    wire rx_hold_det = rx_prim_hold && (tx_state == TX_DATA);
    wire tx_data_xfer = (tx_state == TX_DATA) && tx_valid && !rx_hold_det;

    assign tx_crc_init    = (tx_state == TX_SOF);
    assign tx_crc_data_in = tx_data;
    assign tx_crc_valid   = tx_data_xfer;

    assign tx_scr_init    = (tx_state == TX_SOF);
    assign tx_scr_data_in = (tx_state == TX_CRC) ? tx_crc_out : tx_data;
    assign tx_scr_valid   = tx_data_xfer || (tx_state == TX_CRC);

    sata_crc32 tx_crc_inst (
        .clk     (clk),
        .rst_n   (rst_n),
        .init    (tx_crc_init),
        .data_in (tx_crc_data_in),
        .valid   (tx_crc_valid),
        .crc_out (tx_crc_out)
    );

    sata_scrambler tx_scr_inst (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (tx_scr_init),
        .data_in  (tx_scr_data_in),
        .valid    (tx_scr_valid),
        .data_out (tx_scr_data_out)
    );

    // =========================================================================
    // RX CRC and Scrambler
    // =========================================================================
    wire        rx_crc_init;
    wire [31:0] rx_crc_data_in;
    wire        rx_crc_valid;
    wire [31:0] rx_crc_out;

    wire        rx_scr_init;
    wire [31:0] rx_scr_data_in;
    wire        rx_scr_valid;
    wire [31:0] rx_scr_data_out;

    // RX pipeline: 2-stage buffer to strip CRC DWORD
    reg [31:0] rx_buf_a;     // older DWORD (data to output)
    reg [31:0] rx_buf_b;     // newer DWORD
    reg [2:0]  rx_buf_cnt;   // DWORDs buffered so far
    reg [31:0] rx_expected_crc;
    reg        rx_crc_good;

    wire rx_sof_detect  = rx_is_prim && (phy_rx_data == PRIM_SOF);
    wire rx_eof_detect  = (rx_state == RX_DATA) && rx_is_prim && (phy_rx_data == PRIM_EOF);
    wire rx_data_dword  = (rx_state == RX_DATA) && rx_is_data;

    assign rx_scr_init    = (rx_state == RX_RRDY) && rx_sof_detect;
    assign rx_scr_data_in = phy_rx_data;
    assign rx_scr_valid   = rx_data_dword;

    // CRC fed from pipeline buf_a (oldest buffered DWORD)
    // Feed when: outputting a data DWORD during normal flow, or at EOF for last data DWORD
    wire rx_pipe_output = rx_data_dword && (rx_buf_cnt >= 3'd2);
    wire rx_eof_output  = rx_eof_detect && (rx_buf_cnt >= 3'd2);

    assign rx_crc_init    = (rx_state == RX_RRDY) && rx_sof_detect;
    assign rx_crc_data_in = rx_buf_a;
    assign rx_crc_valid   = rx_pipe_output || rx_eof_output;

    sata_crc32 rx_crc_inst (
        .clk     (clk),
        .rst_n   (rst_n),
        .init    (rx_crc_init),
        .data_in (rx_crc_data_in),
        .valid   (rx_crc_valid),
        .crc_out (rx_crc_out)
    );

    sata_scrambler rx_scr_inst (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (rx_scr_init),
        .data_in  (rx_scr_data_in),
        .valid    (rx_scr_valid),
        .data_out (rx_scr_data_out)
    );

    // =========================================================================
    // TX Ready
    // =========================================================================
    assign tx_ready = (tx_state == TX_DATA) && !rx_hold_det;

    // =========================================================================
    // TX State Machine + PHY TX mux
    // =========================================================================
    // Timeout counter for WTRM
    reg [15:0] wtrm_cnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_state    <= TX_IDLE;
            tx_done     <= 1'b0;
            tx_err      <= 1'b0;
            wtrm_cnt    <= 16'd0;
        end else begin
            tx_done <= 1'b0;
            tx_err  <= 1'b0;

            case (tx_state)
                TX_IDLE: begin
                    if (tx_start && phy_ready)
                        tx_state <= TX_XRDY;
                end

                TX_XRDY: begin
                    if (rx_prim_rrdy)
                        tx_state <= TX_SOF;
                end

                TX_SOF: begin
                    tx_state <= TX_DATA;
                end

                TX_DATA: begin
                    if (!rx_hold_det && tx_valid && tx_last)
                        tx_state <= TX_CRC;
                end

                TX_CRC: begin
                    tx_state <= TX_EOF;
                end

                TX_EOF: begin
                    tx_state <= TX_WTRM;
                    wtrm_cnt <= 16'd0;
                end

                TX_WTRM: begin
                    wtrm_cnt <= wtrm_cnt + 1'b1;
                    if (rx_prim_rok) begin
                        tx_done  <= 1'b1;
                        tx_state <= TX_IDLE;
                    end else if (rx_prim_rerr) begin
                        tx_err   <= 1'b1;
                        tx_state <= TX_IDLE;
                    end else if (wtrm_cnt >= 16'd1000) begin
                        tx_err   <= 1'b1;
                        tx_state <= TX_IDLE;
                    end
                end

                default: tx_state <= TX_IDLE;
            endcase
        end
    end

    // =========================================================================
    // RX State Machine
    // =========================================================================
    reg [3:0]  rx_status_cnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state      <= RX_IDLE;
            rx_buf_a      <= 32'd0;
            rx_buf_b      <= 32'd0;
            rx_buf_cnt    <= 3'd0;
            rx_expected_crc <= 32'd0;
            rx_crc_good   <= 1'b0;
            rx_data       <= 32'd0;
            rx_valid      <= 1'b0;
            rx_last       <= 1'b0;
            rx_sof        <= 1'b0;
            rx_err        <= 1'b0;
            rx_status_cnt <= 4'd0;
        end else begin
            rx_valid <= 1'b0;
            rx_last  <= 1'b0;
            rx_sof   <= 1'b0;
            rx_err   <= 1'b0;

            case (rx_state)
                RX_IDLE: begin
                    rx_buf_cnt <= 3'd0;
                    if (rx_prim_xrdy && (tx_state == TX_IDLE))
                        rx_state <= RX_RRDY;
                end

                RX_RRDY: begin
                    if (rx_sof_detect) begin
                        rx_state   <= RX_DATA;
                        rx_buf_cnt <= 3'd0;
                        rx_sof     <= 1'b1;
                    end
                end

                RX_DATA: begin
                    if (rx_eof_detect) begin
                        // EOF: output last data DWORD from buf_a, keep buf_b as CRC
                        if (rx_buf_cnt >= 3'd2) begin
                            rx_data  <= rx_buf_a;
                            rx_valid <= 1'b1;
                            rx_last  <= 1'b1;
                            rx_expected_crc <= rx_buf_b;
                        end
                        rx_state <= RX_CHECK;
                    end else if (rx_data_dword) begin
                        // Shift pipeline
                        rx_buf_b <= rx_scr_data_out;
                        rx_buf_a <= rx_buf_b;
                        if (rx_buf_cnt < 3'd7)
                            rx_buf_cnt <= rx_buf_cnt + 1'b1;
                        // Output buf_a when pipeline is full (2+ DWORDs already buffered)
                        if (rx_buf_cnt >= 3'd2) begin
                            rx_data  <= rx_buf_a;
                            rx_valid <= 1'b1;
                        end
                    end
                    // Primitives other than EOF during data: ignore (HOLDA, SYNC, etc.)
                end

                RX_CHECK: begin
                    // CRC was fed on the EOF cycle; result available now
                    rx_crc_good <= (rx_crc_out == rx_expected_crc);
                    rx_state    <= RX_STATUS;
                    rx_status_cnt <= 4'd0;
                    if (rx_crc_out != rx_expected_crc)
                        rx_err <= 1'b1;
                end

                RX_STATUS: begin
                    rx_status_cnt <= rx_status_cnt + 1'b1;
                    if (rx_status_cnt >= 4'd5)
                        rx_state <= RX_IDLE;
                end

                default: rx_state <= RX_IDLE;
            endcase
        end
    end

    // =========================================================================
    // PHY TX Output Mux
    // State-aligned: each state outputs its own primitive/data.
    // TX state machine has priority when active; RX controls when TX is idle.
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            phy_tx_data <= PRIM_SYNC;
            phy_tx_isk  <= 4'b0001;
        end else if (tx_state != TX_IDLE) begin
            // TX state machine controls PHY TX
            case (tx_state)
                TX_XRDY: begin
                    phy_tx_data <= PRIM_XRDY;
                    phy_tx_isk  <= 4'b0001;
                end
                TX_SOF: begin
                    phy_tx_data <= PRIM_SOF;
                    phy_tx_isk  <= 4'b0001;
                end
                TX_DATA: begin
                    if (rx_hold_det) begin
                        phy_tx_data <= PRIM_HOLDA;
                        phy_tx_isk  <= 4'b0001;
                    end else if (tx_valid) begin
                        phy_tx_data <= tx_scr_data_out;
                        phy_tx_isk  <= 4'b0000;
                    end else begin
                        phy_tx_data <= PRIM_HOLD;
                        phy_tx_isk  <= 4'b0001;
                    end
                end
                TX_CRC: begin
                    phy_tx_data <= tx_scr_data_out;
                    phy_tx_isk  <= 4'b0000;
                end
                TX_EOF: begin
                    phy_tx_data <= PRIM_EOF;
                    phy_tx_isk  <= 4'b0001;
                end
                TX_WTRM: begin
                    phy_tx_data <= PRIM_WTRM;
                    phy_tx_isk  <= 4'b0001;
                end
                default: begin
                    phy_tx_data <= PRIM_SYNC;
                    phy_tx_isk  <= 4'b0001;
                end
            endcase
        end else begin
            // RX state machine controls PHY TX (TX is idle)
            case (rx_state)
                RX_RRDY: begin
                    phy_tx_data <= PRIM_RRDY;
                    phy_tx_isk  <= 4'b0001;
                end
                RX_STATUS: begin
                    if (rx_crc_good) begin
                        phy_tx_data <= PRIM_ROK;
                        phy_tx_isk  <= 4'b0001;
                    end else begin
                        phy_tx_data <= PRIM_RERR;
                        phy_tx_isk  <= 4'b0001;
                    end
                end
                default: begin
                    phy_tx_data <= PRIM_SYNC;
                    phy_tx_isk  <= 4'b0001;
                end
            endcase
        end
    end

endmodule
