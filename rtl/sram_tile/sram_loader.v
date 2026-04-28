// sram_loader.v — Loads SRAM tables from SD card byte stream
//
// Takes byte stream from spi_sd_reader and writes 16-bit words
// to SRAMs via the bus controllers' load interfaces.
//
// SD card layout (raw sectors):
//   Sector 0:     Header — magic, N chips, entries per chip
//   Sector 1+:    Weight data, 2 bytes per entry (little-endian)
//                  Chips are stored sequentially: chip0 full, chip1 full, ...
//
// Each chip has up to 65536 entries × 2 bytes = 128KB = 256 sectors

module sram_loader #(
    parameter N_CHIPS   = 16,       // total chips across both buses
    parameter ADDR_W    = 16,       // entries per chip = 2^ADDR_W
    parameter DATA_W    = 16
)(
    input  wire               clk,
    input  wire               rst_n,

    // --- Control ---
    input  wire               start_load,    // pulse to begin
    output reg                load_done,
    output reg                load_error,
    output reg  [3:0]         load_chip_idx, // current chip being loaded

    // --- SD card reader interface ---
    output reg                sd_read_cmd,
    output reg  [31:0]        sd_sector,
    input  wire               sd_data_valid,
    input  wire [7:0]         sd_data_byte,
    input  wire               sd_read_done,
    input  wire               sd_card_ready,

    // --- SRAM write interface (active during load) ---
    output reg  [2:0]         sram_chip_sel,  // chip within bus (0-7)
    output reg                sram_bus_sel,    // which bus (0 or 1)
    output reg  [ADDR_W-1:0] sram_addr,
    output reg  [DATA_W-1:0] sram_data,
    output reg                sram_we
);

    localparam ENTRIES_PER_CHIP = (1 << ADDR_W);
    localparam BYTES_PER_CHIP  = ENTRIES_PER_CHIP * 2;
    localparam SECTORS_PER_CHIP = BYTES_PER_CHIP / 512;
    localparam MAGIC = 32'h5352414D;  // "SRAM"

    localparam S_IDLE       = 3'd0;
    localparam S_READ_HDR   = 3'd1;
    localparam S_LOAD_DATA  = 3'd2;
    localparam S_NEXT_SEC   = 3'd3;
    localparam S_NEXT_CHIP  = 3'd4;
    localparam S_DONE       = 3'd5;
    localparam S_ERROR      = 3'd6;

    reg [2:0]  state;
    reg [31:0] sector_cnt;
    reg [15:0] byte_cnt;         // bytes within current sector
    reg        byte_phase;       // 0=low byte, 1=high byte
    reg [7:0]  lo_byte;
    reg [ADDR_W-1:0] word_addr; // address within current chip
    reg [15:0] sector_in_chip;  // sector index within current chip

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state         <= S_IDLE;
            load_done     <= 1'b0;
            load_error    <= 1'b0;
            load_chip_idx <= 4'd0;
            sd_read_cmd   <= 1'b0;
            sd_sector     <= 32'd0;
            sram_chip_sel <= 3'd0;
            sram_bus_sel  <= 1'b0;
            sram_addr     <= {ADDR_W{1'b0}};
            sram_data     <= {DATA_W{1'b0}};
            sram_we       <= 1'b0;
            sector_cnt    <= 32'd0;
            byte_cnt      <= 16'd0;
            byte_phase    <= 1'b0;
            lo_byte       <= 8'd0;
            word_addr     <= {ADDR_W{1'b0}};
            sector_in_chip <= 16'd0;
        end else begin
            sd_read_cmd <= 1'b0;
            sram_we     <= 1'b0;

            case (state)
                S_IDLE: begin
                    load_done  <= 1'b0;
                    load_error <= 1'b0;
                    if (start_load && sd_card_ready) begin
                        // Skip header sector, start loading data from sector 1
                        sd_sector     <= 32'd1;
                        sd_read_cmd   <= 1'b1;
                        load_chip_idx <= 4'd0;
                        sram_chip_sel <= 3'd0;
                        sram_bus_sel  <= 1'b0;
                        word_addr     <= {ADDR_W{1'b0}};
                        byte_phase    <= 1'b0;
                        byte_cnt      <= 16'd0;
                        sector_in_chip <= 16'd0;
                        state         <= S_LOAD_DATA;
                    end
                end

                S_LOAD_DATA: begin
                    if (sd_data_valid) begin
                        byte_cnt <= byte_cnt + 1;

                        if (!byte_phase) begin
                            lo_byte    <= sd_data_byte;
                            byte_phase <= 1'b1;
                        end else begin
                            // Got both bytes — write word to SRAM
                            sram_data  <= {sd_data_byte, lo_byte};
                            sram_addr  <= word_addr;
                            sram_we    <= 1'b1;
                            word_addr  <= word_addr + 1;
                            byte_phase <= 1'b0;
                        end
                    end

                    if (sd_read_done) begin
                        sector_in_chip <= sector_in_chip + 1;

                        if (sector_in_chip + 1 >= SECTORS_PER_CHIP) begin
                            state <= S_NEXT_CHIP;
                        end else begin
                            state <= S_NEXT_SEC;
                        end
                    end
                end

                S_NEXT_SEC: begin
                    sd_sector   <= sd_sector + 1;
                    sd_read_cmd <= 1'b1;
                    byte_cnt    <= 16'd0;
                    state       <= S_LOAD_DATA;
                end

                S_NEXT_CHIP: begin
                    if (load_chip_idx == N_CHIPS - 1) begin
                        state <= S_DONE;
                    end else begin
                        load_chip_idx  <= load_chip_idx + 1;
                        // Map chip index to bus + chip_sel
                        sram_bus_sel   <= (load_chip_idx + 1) >= (N_CHIPS / 2);
                        sram_chip_sel  <= (load_chip_idx + 1) % (N_CHIPS / 2);
                        word_addr      <= {ADDR_W{1'b0}};
                        byte_phase     <= 1'b0;
                        byte_cnt       <= 16'd0;
                        sector_in_chip <= 16'd0;
                        sd_sector      <= sd_sector + 1;
                        sd_read_cmd    <= 1'b1;
                        state          <= S_LOAD_DATA;
                    end
                end

                S_DONE: begin
                    load_done <= 1'b1;
                    state     <= S_IDLE;
                end

                S_ERROR: begin
                    load_error <= 1'b1;
                end
            endcase
        end
    end

endmodule
