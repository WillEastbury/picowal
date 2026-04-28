// SATA Command Layer
// Sits on top of sata_transport.v. Translates user READ/WRITE/IDENTIFY/SET_FEATURES
// requests into ATA command sequences. QD1 (single command in-flight).
`default_nettype none

module sata_command (
    input  wire        clk,
    input  wire        rst_n,

    // --- User interface (from KV engine) ---
    input  wire        user_cmd_valid,
    output wire        user_cmd_ready,
    input  wire        user_cmd_is_write,
    input  wire [47:0] user_cmd_lba,
    input  wire [15:0] user_cmd_count,

    // User write data (streaming DWORDs for write commands)
    input  wire [31:0] user_wr_data,
    input  wire        user_wr_valid,
    input  wire        user_wr_last,
    output wire        user_wr_ready,

    // User read data (streaming DWORDs from read commands)
    output reg  [31:0] user_rd_data,
    output reg         user_rd_valid,
    output reg         user_rd_last,

    // Completion
    output reg         cmd_complete,
    output reg         cmd_error,
    output reg  [7:0]  cmd_status,

    // Init sequence
    input  wire        do_init,
    output reg         init_done,
    output reg [127:0] identify_data_flat,  // first 4 DWORDs packed

    // --- Transport layer interface ---
    output reg         tp_cmd_tx_start,
    output reg  [7:0]  tp_cmd_tx_command,
    output reg  [47:0] tp_cmd_tx_lba,
    output reg  [15:0] tp_cmd_tx_count,
    output reg  [7:0]  tp_cmd_tx_features,
    output reg  [7:0]  tp_cmd_tx_device,
    input  wire        tp_cmd_tx_done,
    input  wire        tp_cmd_tx_err,

    output reg         tp_data_tx_start,
    output reg  [31:0] tp_data_tx_dword,
    output reg         tp_data_tx_valid,
    output reg         tp_data_tx_last,
    input  wire        tp_data_tx_ready,
    input  wire        tp_data_tx_done,

    input  wire        tp_rx_reg_fis_valid,
    input  wire [7:0]  tp_rx_status,
    input  wire [7:0]  tp_rx_error,
    input  wire        tp_rx_pio_setup_valid,
    input  wire [15:0] tp_rx_pio_xfer_count,
    input  wire [7:0]  tp_rx_pio_status,
    input  wire        tp_rx_dma_activate,
    input  wire [31:0] tp_rx_data_dword,
    input  wire        tp_rx_data_valid,
    input  wire        tp_rx_data_last,
    input  wire        tp_rx_data_err
);

    // ATA command codes
    localparam [7:0] ATA_READ_DMA_EXT  = 8'h25,
                     ATA_WRITE_DMA_EXT = 8'h35,
                     ATA_IDENTIFY       = 8'hEC,
                     ATA_SET_FEATURES   = 8'hEF;

    // ATA status bits
    localparam ATA_STATUS_ERR = 0,
               ATA_STATUS_BSY = 7;

    // FSM states
    localparam [3:0] S_IDLE           = 4'd0,
                     S_INIT_IDENTIFY  = 4'd1,
                     S_INIT_ID_PIO    = 4'd2,
                     S_INIT_ID_DATA   = 4'd3,
                     S_INIT_ID_D2H    = 4'd4,
                     S_INIT_SETFEAT   = 4'd5,
                     S_INIT_SF_D2H    = 4'd6,
                     S_CMD_SEND       = 4'd7,
                     S_READ_DATA      = 4'd8,
                     S_WRITE_WAIT_ACT = 4'd9,
                     S_WRITE_DATA     = 4'd10,
                     S_WAIT_D2H       = 4'd11,
                     S_COMPLETE        = 4'd12;

    reg [3:0] state;

    // Latched command
    reg        lat_is_write;
    reg [47:0] lat_lba;
    reg [15:0] lat_count;

    // IDENTIFY data collection counter
    reg [7:0] id_dw_cnt;

    // Latched error flag (asserted with cmd_complete in S_COMPLETE)
    reg       lat_error;

    assign user_cmd_ready = (state == S_IDLE) && !do_init;
    assign user_wr_ready  = (state == S_WRITE_DATA) && tp_data_tx_ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= S_IDLE;
            tp_cmd_tx_start    <= 1'b0;
            tp_cmd_tx_command  <= 8'd0;
            tp_cmd_tx_lba      <= 48'd0;
            tp_cmd_tx_count    <= 16'd0;
            tp_cmd_tx_features <= 8'd0;
            tp_cmd_tx_device   <= 8'd0;
            tp_data_tx_start   <= 1'b0;
            tp_data_tx_dword   <= 32'd0;
            tp_data_tx_valid   <= 1'b0;
            tp_data_tx_last    <= 1'b0;
            user_rd_data       <= 32'd0;
            user_rd_valid      <= 1'b0;
            user_rd_last       <= 1'b0;
            cmd_complete       <= 1'b0;
            cmd_error          <= 1'b0;
            cmd_status         <= 8'd0;
            init_done          <= 1'b0;
            identify_data_flat <= 128'd0;
            lat_is_write       <= 1'b0;
            lat_lba            <= 48'd0;
            lat_count          <= 16'd0;
            lat_error          <= 1'b0;
            id_dw_cnt          <= 8'd0;
        end else begin
            // Defaults: pulse signals clear each cycle
            tp_cmd_tx_start  <= 1'b0;
            tp_data_tx_start <= 1'b0;
            tp_data_tx_valid <= 1'b0;
            tp_data_tx_last  <= 1'b0;
            user_rd_valid    <= 1'b0;
            user_rd_last     <= 1'b0;
            cmd_complete     <= 1'b0;
            cmd_error        <= 1'b0;

            case (state)
                // =============================================================
                S_IDLE: begin
                    if (do_init && !init_done) begin
                        // Start IDENTIFY DEVICE
                        tp_cmd_tx_start    <= 1'b1;
                        tp_cmd_tx_command  <= ATA_IDENTIFY;
                        tp_cmd_tx_lba      <= 48'd0;
                        tp_cmd_tx_count    <= 16'd0;
                        tp_cmd_tx_features <= 8'd0;
                        tp_cmd_tx_device   <= 8'hE0;  // LBA mode
                        id_dw_cnt          <= 8'd0;
                        lat_error          <= 1'b0;
                        state              <= S_INIT_IDENTIFY;
                    end else if (user_cmd_valid) begin
                        lat_is_write <= user_cmd_is_write;
                        lat_lba      <= user_cmd_lba;
                        lat_count    <= user_cmd_count;
                        lat_error    <= 1'b0;
                        // Send command FIS
                        tp_cmd_tx_start    <= 1'b1;
                        tp_cmd_tx_command  <= user_cmd_is_write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
                        tp_cmd_tx_lba      <= user_cmd_lba;
                        tp_cmd_tx_count    <= user_cmd_count;
                        tp_cmd_tx_features <= 8'd0;
                        tp_cmd_tx_device   <= 8'hE0;
                        state              <= S_CMD_SEND;
                    end
                end

                // =============================================================
                // INIT: IDENTIFY DEVICE (PIO data-in)
                // =============================================================
                S_INIT_IDENTIFY: begin
                    // Wait for transport to finish sending H2D FIS
                    if (tp_cmd_tx_done) begin
                        state <= S_INIT_ID_PIO;
                    end else if (tp_cmd_tx_err) begin
                        lat_error  <= 1'b1;
                        cmd_status <= 8'hFF;
                        state      <= S_IDLE;
                    end
                end

                S_INIT_ID_PIO: begin
                    // Wait for PIO Setup FIS from device
                    if (tp_rx_pio_setup_valid) begin
                        state <= S_INIT_ID_DATA;
                    end
                end

                S_INIT_ID_DATA: begin
                    // Collect 128 DWORDs (512 bytes) of identify data
                    if (tp_rx_data_valid) begin
                        // Capture first 4 DWORDs
                        if (id_dw_cnt < 8'd4) begin
                            case (id_dw_cnt[1:0])
                                2'd0: identify_data_flat[127:96] <= tp_rx_data_dword;
                                2'd1: identify_data_flat[95:64]  <= tp_rx_data_dword;
                                2'd2: identify_data_flat[63:32]  <= tp_rx_data_dword;
                                2'd3: identify_data_flat[31:0]   <= tp_rx_data_dword;
                            endcase
                        end
                        id_dw_cnt <= id_dw_cnt + 8'd1;
                        if (tp_rx_data_last) begin
                            state <= S_INIT_ID_D2H;
                        end
                    end
                    if (tp_rx_data_err) begin
                        lat_error  <= 1'b1;
                        cmd_status <= 8'hFF;
                        state      <= S_IDLE;
                    end
                end

                S_INIT_ID_D2H: begin
                    // Wait for Register D2H FIS (completion)
                    if (tp_rx_reg_fis_valid) begin
                        if (tp_rx_status[ATA_STATUS_ERR]) begin
                            lat_error  <= 1'b1;
                            cmd_status <= tp_rx_status;
                            state      <= S_IDLE;
                        end else begin
                            // IDENTIFY done, now send SET FEATURES
                            tp_cmd_tx_start    <= 1'b1;
                            tp_cmd_tx_command  <= ATA_SET_FEATURES;
                            tp_cmd_tx_lba      <= 48'd0;
                            tp_cmd_tx_count    <= 16'd0;
                            tp_cmd_tx_features <= 8'h03; // Set transfer mode
                            tp_cmd_tx_device   <= 8'hE0;
                            state              <= S_INIT_SETFEAT;
                        end
                    end
                end

                // =============================================================
                // INIT: SET FEATURES (non-data)
                // =============================================================
                S_INIT_SETFEAT: begin
                    if (tp_cmd_tx_done) begin
                        state <= S_INIT_SF_D2H;
                    end else if (tp_cmd_tx_err) begin
                        lat_error  <= 1'b1;
                        cmd_status <= 8'hFF;
                        state      <= S_IDLE;
                    end
                end

                S_INIT_SF_D2H: begin
                    if (tp_rx_reg_fis_valid) begin
                        init_done  <= 1'b1;
                        cmd_status <= tp_rx_status;
                        if (tp_rx_status[ATA_STATUS_ERR]) begin
                            lat_error <= 1'b1;
                        end
                        state <= S_COMPLETE;
                    end
                end

                // =============================================================
                // Normal R/W: wait for H2D FIS send to complete
                // =============================================================
                S_CMD_SEND: begin
                    if (tp_cmd_tx_done) begin
                        if (lat_is_write)
                            state <= S_WRITE_WAIT_ACT;
                        else
                            state <= S_READ_DATA;
                    end else if (tp_cmd_tx_err) begin
                        lat_error  <= 1'b1;
                        cmd_status <= 8'hFF;
                        state      <= S_COMPLETE;
                    end
                end

                // =============================================================
                // READ DMA EXT: receive Data FIS
                // =============================================================
                S_READ_DATA: begin
                    if (tp_rx_data_valid) begin
                        user_rd_data  <= tp_rx_data_dword;
                        user_rd_valid <= 1'b1;
                        user_rd_last  <= tp_rx_data_last;
                        if (tp_rx_data_last) begin
                            state <= S_WAIT_D2H;
                        end
                    end
                    if (tp_rx_data_err) begin
                        lat_error  <= 1'b1;
                        cmd_status <= 8'hFF;
                        state      <= S_COMPLETE;
                    end
                end

                // =============================================================
                // WRITE DMA EXT: wait DMA Activate then send Data FIS
                // =============================================================
                S_WRITE_WAIT_ACT: begin
                    if (tp_rx_dma_activate) begin
                        tp_data_tx_start <= 1'b1;
                        state            <= S_WRITE_DATA;
                    end
                end

                S_WRITE_DATA: begin
                    // Stream user write data through to transport
                    if (user_wr_valid && tp_data_tx_ready) begin
                        tp_data_tx_valid <= 1'b1;
                        tp_data_tx_dword <= user_wr_data;
                        tp_data_tx_last  <= user_wr_last;
                    end
                    if (tp_data_tx_done) begin
                        state <= S_WAIT_D2H;
                    end
                end

                // =============================================================
                // Wait for Register D2H FIS (completion status)
                // =============================================================
                S_WAIT_D2H: begin
                    if (tp_rx_reg_fis_valid) begin
                        cmd_status <= tp_rx_status;
                        if (tp_rx_status[ATA_STATUS_ERR]) begin
                            lat_error <= 1'b1;
                        end
                        state <= S_COMPLETE;
                    end
                end

                // =============================================================
                // Signal completion and return to idle
                // =============================================================
                S_COMPLETE: begin
                    cmd_complete <= 1'b1;
                    cmd_error    <= lat_error;
                    lat_error    <= 1'b0;
                    state        <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
