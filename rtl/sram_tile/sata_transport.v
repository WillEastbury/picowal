// SATA Transport Layer
// Sits between link layer and command layer.
// Packs/unpacks Frame Information Structures (FIS).
`default_nettype none

module sata_transport (
    input  wire        clk,
    input  wire        rst_n,

    // --- Link layer interface (TX) ---
    output reg  [31:0] link_tx_data,
    output reg         link_tx_valid,
    output reg         link_tx_last,
    input  wire        link_tx_ready,
    output reg         link_tx_start,
    input  wire        link_tx_done,
    input  wire        link_tx_err,

    // --- Link layer interface (RX) ---
    input  wire [31:0] link_rx_data,
    input  wire        link_rx_valid,
    input  wire        link_rx_last,
    input  wire        link_rx_sof,
    input  wire        link_rx_err,

    // --- Command layer interface ---
    // TX command FIS (Register H2D)
    input  wire        cmd_tx_start,
    input  wire [7:0]  cmd_tx_command,
    input  wire [47:0] cmd_tx_lba,
    input  wire [15:0] cmd_tx_count,
    input  wire [7:0]  cmd_tx_features,
    input  wire [7:0]  cmd_tx_device,
    output reg         cmd_tx_done,
    output reg         cmd_tx_err,

    // TX data FIS (for write commands)
    input  wire        data_tx_start,
    input  wire [31:0] data_tx_dword,
    input  wire        data_tx_valid,
    input  wire        data_tx_last,
    output wire        data_tx_ready,
    output reg         data_tx_done,

    // RX responses
    output reg         rx_reg_fis_valid,
    output reg  [7:0]  rx_status,
    output reg  [7:0]  rx_error,

    output reg         rx_pio_setup_valid,
    output reg  [15:0] rx_pio_xfer_count,
    output reg  [7:0]  rx_pio_status,

    output reg         rx_dma_activate,

    // RX data FIS
    output reg  [31:0] rx_data_dword,
    output reg         rx_data_valid,
    output reg         rx_data_last,
    output reg         rx_data_err
);

    // =========================================================================
    // TX FSM
    // =========================================================================
    localparam TX_IDLE      = 3'd0,
               TX_CMD_SEND  = 3'd1,
               TX_CMD_WAIT  = 3'd2,
               TX_DATA_HDR  = 3'd3,
               TX_DATA_SEND = 3'd4,
               TX_DATA_WAIT = 3'd5;

    reg [2:0]  tx_state;
    reg [2:0]  tx_dw_cnt;       // DWORD index within FIS
    reg [31:0] cmd_fis [0:4];   // 5-DWORD register H2D FIS

    // Latch command fields on cmd_tx_start
    reg [7:0]  lat_command;
    reg [47:0] lat_lba;
    reg [15:0] lat_count;
    reg [7:0]  lat_features;
    reg [7:0]  lat_device;

    assign data_tx_ready = (tx_state == TX_DATA_SEND) && link_tx_ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_state       <= TX_IDLE;
            tx_dw_cnt      <= 3'd0;
            link_tx_data   <= 32'd0;
            link_tx_valid  <= 1'b0;
            link_tx_last   <= 1'b0;
            link_tx_start  <= 1'b0;
            cmd_tx_done    <= 1'b0;
            cmd_tx_err     <= 1'b0;
            data_tx_done   <= 1'b0;
        end else begin
            // Pulse signals default low
            link_tx_start <= 1'b0;
            link_tx_valid <= 1'b0;
            link_tx_last  <= 1'b0;
            cmd_tx_done   <= 1'b0;
            cmd_tx_err    <= 1'b0;
            data_tx_done  <= 1'b0;

            case (tx_state)
                TX_IDLE: begin
                    if (cmd_tx_start) begin
                        // Latch command fields
                        lat_command  <= cmd_tx_command;
                        lat_lba      <= cmd_tx_lba;
                        lat_count    <= cmd_tx_count;
                        lat_features <= cmd_tx_features;
                        lat_device   <= cmd_tx_device;
                        tx_dw_cnt    <= 3'd0;
                        link_tx_start <= 1'b1;
                        tx_state     <= TX_CMD_SEND;
                    end else if (data_tx_start) begin
                        tx_dw_cnt     <= 3'd0;
                        link_tx_start <= 1'b1;
                        tx_state      <= TX_DATA_HDR;
                    end
                end

                // ------ Register H2D FIS (0x27) — 5 DWORDs ------
                TX_CMD_SEND: begin
                    if (link_tx_ready) begin
                        link_tx_valid <= 1'b1;
                        case (tx_dw_cnt)
                            3'd0: link_tx_data <= {lat_features, lat_command, 1'b1, 3'b0, 4'b0, 8'h27};
                            3'd1: link_tx_data <= {lat_lba[31:28], lat_device[3:0], lat_lba[23:16], lat_lba[15:8], lat_lba[7:0]};
                            3'd2: link_tx_data <= {8'd0, lat_lba[47:24]};
                            3'd3: link_tx_data <= {8'd0, 8'd0, lat_count};
                                // [15:0]=sector_count, [23:16]=sector_count_exp=0, [31:24]=reserved
                            3'd4: begin
                                link_tx_data <= 32'd0; // reserved
                                link_tx_last <= 1'b1;
                            end
                            default: ;
                        endcase
                        if (tx_dw_cnt == 3'd4) begin
                            tx_state <= TX_CMD_WAIT;
                        end
                        tx_dw_cnt <= tx_dw_cnt + 3'd1;
                    end
                end

                TX_CMD_WAIT: begin
                    if (link_tx_done) begin
                        cmd_tx_done <= 1'b1;
                        tx_state    <= TX_IDLE;
                    end else if (link_tx_err) begin
                        cmd_tx_err <= 1'b1;
                        tx_state   <= TX_IDLE;
                    end
                end

                // ------ Data FIS (0x46) — header + N payload DWORDs ------
                TX_DATA_HDR: begin
                    if (link_tx_ready) begin
                        link_tx_data  <= {24'd0, 8'h46};
                        link_tx_valid <= 1'b1;
                        tx_state      <= TX_DATA_SEND;
                    end
                end

                TX_DATA_SEND: begin
                    if (link_tx_ready && data_tx_valid) begin
                        link_tx_data  <= data_tx_dword;
                        link_tx_valid <= 1'b1;
                        if (data_tx_last) begin
                            link_tx_last <= 1'b1;
                            tx_state     <= TX_DATA_WAIT;
                        end
                    end
                end

                TX_DATA_WAIT: begin
                    if (link_tx_done) begin
                        data_tx_done <= 1'b1;
                        tx_state     <= TX_IDLE;
                    end else if (link_tx_err) begin
                        tx_state <= TX_IDLE;
                    end
                end

                default: tx_state <= TX_IDLE;
            endcase
        end
    end

    // =========================================================================
    // RX FSM
    // =========================================================================
    localparam RX_IDLE      = 3'd0,
               RX_TYPE      = 3'd1,
               RX_REG_D2H   = 3'd2,
               RX_PIO_SETUP = 3'd3,
               RX_DATA      = 3'd4,
               RX_DMA_ACT   = 3'd5,
               RX_DISCARD   = 3'd6;

    reg [2:0]  rx_state;
    reg [2:0]  rx_dw_cnt;
    reg [7:0]  rx_fis_type;

    // Temp storage for multi-DWORD FIS parsing
    reg [7:0]  r_status;
    reg [7:0]  r_error;
    reg [7:0]  r_pio_status;
    reg [15:0] r_pio_xfer_count;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state          <= RX_IDLE;
            rx_dw_cnt         <= 3'd0;
            rx_fis_type       <= 8'd0;
            rx_reg_fis_valid  <= 1'b0;
            rx_status         <= 8'd0;
            rx_error          <= 8'd0;
            rx_pio_setup_valid <= 1'b0;
            rx_pio_xfer_count <= 16'd0;
            rx_pio_status     <= 8'd0;
            rx_dma_activate   <= 1'b0;
            rx_data_dword     <= 32'd0;
            rx_data_valid     <= 1'b0;
            rx_data_last      <= 1'b0;
            rx_data_err       <= 1'b0;
            r_status          <= 8'd0;
            r_error           <= 8'd0;
            r_pio_status      <= 8'd0;
            r_pio_xfer_count  <= 16'd0;
        end else begin
            // Pulse signals default low
            rx_reg_fis_valid   <= 1'b0;
            rx_pio_setup_valid <= 1'b0;
            rx_dma_activate    <= 1'b0;
            rx_data_valid      <= 1'b0;
            rx_data_last       <= 1'b0;
            rx_data_err        <= 1'b0;

            if (link_rx_err) begin
                rx_data_err <= 1'b1;
                rx_state    <= RX_IDLE;
            end else begin
                case (rx_state)
                    RX_IDLE: begin
                        if (link_rx_sof && link_rx_valid) begin
                            rx_fis_type <= link_rx_data[7:0];
                            rx_dw_cnt   <= 3'd1; // DWORD 0 consumed
                            case (link_rx_data[7:0])
                                8'h34: begin // Register D2H
                                    r_status <= link_rx_data[23:16];
                                    r_error  <= link_rx_data[31:24];
                                    if (link_rx_last) begin
                                        // Shouldn't be last at dword 0 for 5-dword FIS, but handle it
                                        rx_reg_fis_valid <= 1'b1;
                                        rx_status <= link_rx_data[23:16];
                                        rx_error  <= link_rx_data[31:24];
                                    end else begin
                                        rx_state <= RX_REG_D2H;
                                    end
                                end
                                8'h5F: begin // PIO Setup
                                    r_pio_status <= link_rx_data[23:16];
                                    r_error      <= link_rx_data[31:24];
                                    if (link_rx_last) begin
                                        rx_pio_setup_valid <= 1'b1;
                                        rx_pio_status      <= link_rx_data[23:16];
                                        rx_pio_xfer_count  <= 16'd0;
                                    end else begin
                                        rx_state <= RX_PIO_SETUP;
                                    end
                                end
                                8'h39: begin // DMA Activate — 1 DWORD only
                                    rx_dma_activate <= 1'b1;
                                    rx_state        <= RX_IDLE;
                                end
                                8'h46: begin // Data FIS
                                    if (link_rx_last) begin
                                        rx_state <= RX_IDLE;
                                    end else begin
                                        rx_state <= RX_DATA;
                                    end
                                end
                                default: begin
                                    if (!link_rx_last)
                                        rx_state <= RX_DISCARD;
                                end
                            endcase
                        end
                    end

                    RX_REG_D2H: begin
                        if (link_rx_valid) begin
                            rx_dw_cnt <= rx_dw_cnt + 3'd1;
                            if (link_rx_last || rx_dw_cnt == 3'd4) begin
                                rx_reg_fis_valid <= 1'b1;
                                rx_status        <= r_status;
                                rx_error         <= r_error;
                                rx_state         <= RX_IDLE;
                            end
                        end
                    end

                    RX_PIO_SETUP: begin
                        if (link_rx_valid) begin
                            rx_dw_cnt <= rx_dw_cnt + 3'd1;
                            // DWORD 4 contains transfer count in [15:0]
                            if (rx_dw_cnt == 3'd4) begin
                                r_pio_xfer_count <= link_rx_data[15:0];
                            end
                            if (link_rx_last || rx_dw_cnt == 3'd4) begin
                                rx_pio_setup_valid <= 1'b1;
                                rx_pio_status      <= r_pio_status;
                                if (rx_dw_cnt == 3'd4)
                                    rx_pio_xfer_count <= link_rx_data[15:0];
                                else
                                    rx_pio_xfer_count <= r_pio_xfer_count;
                                rx_state <= RX_IDLE;
                            end
                        end
                    end

                    RX_DATA: begin
                        if (link_rx_valid) begin
                            rx_data_dword <= link_rx_data;
                            rx_data_valid <= 1'b1;
                            if (link_rx_last) begin
                                rx_data_last <= 1'b1;
                                rx_state     <= RX_IDLE;
                            end
                        end
                    end

                    RX_DMA_ACT: begin
                        // Should not arrive here normally; DMA Activate is 1 DWORD
                        rx_state <= RX_IDLE;
                    end

                    RX_DISCARD: begin
                        if (link_rx_valid && link_rx_last)
                            rx_state <= RX_IDLE;
                    end

                    default: rx_state <= RX_IDLE;
                endcase
            end
        end
    end

endmodule
