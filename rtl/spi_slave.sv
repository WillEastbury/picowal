// spi_slave.sv — SPI slave interface for RP2354B ↔ ECP5 communication
// Register read/write for control, status, layer descriptors, perf counters
// SPI Mode 0 (CPOL=0, CPHA=0), active-low CS

module spi_slave
  import hydra_infer_pkg::*;
(
  input  logic    clk,         // FPGA system clock
  input  logic    rst_n,

  // ── SPI pins (directly from RP2354B) ───────────────────────
  input  logic    spi_sck,
  input  logic    spi_mosi,
  output logic    spi_miso,
  input  logic    spi_cs_n,

  // ── Register interface (directly drives internal control) ──
  output logic                     reg_wr_en,
  output logic [SPI_ADDR_W-1:0]  reg_addr,
  output logic [31:0]             reg_wr_data,
  input  logic [31:0]             reg_rd_data,
  output logic                     reg_rd_en
);

  // ── CDC: synchronize SPI signals to system clock ───────────
  logic sck_sync  [3];
  logic mosi_sync [2];
  logic cs_sync   [2];

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      sck_sync[0]  <= 1'b0; sck_sync[1]  <= 1'b0; sck_sync[2] <= 1'b0;
      mosi_sync[0] <= 1'b0; mosi_sync[1] <= 1'b0;
      cs_sync[0]   <= 1'b1; cs_sync[1]   <= 1'b1;
    end else begin
      sck_sync[0]  <= spi_sck;   sck_sync[1]  <= sck_sync[0];  sck_sync[2] <= sck_sync[1];
      mosi_sync[0] <= spi_mosi;  mosi_sync[1] <= mosi_sync[0];
      cs_sync[0]   <= spi_cs_n;  cs_sync[1]   <= cs_sync[0];
    end
  end

  logic sck_rise, sck_fall, cs_active;
  assign sck_rise  = sck_sync[1] && !sck_sync[2];
  assign sck_fall  = !sck_sync[1] && sck_sync[2];
  assign cs_active = !cs_sync[1];

  // ── SPI protocol ───────────────────────────────────────────
  // Frame: [1-bit R/W] [15-bit addr] [32-bit data]
  // Total: 48 bits per transaction

  logic [5:0]  bit_count;
  logic [47:0] shift_in;
  logic [31:0] shift_out;
  logic        frame_done;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      bit_count  <= '0;
      shift_in   <= '0;
      shift_out  <= '0;
      frame_done <= 1'b0;
      reg_wr_en  <= 1'b0;
      reg_rd_en  <= 1'b0;
      reg_addr   <= '0;
      reg_wr_data <= '0;
    end else if (!cs_active) begin
      bit_count  <= '0;
      frame_done <= 1'b0;
      reg_wr_en  <= 1'b0;
      reg_rd_en  <= 1'b0;
    end else begin
      reg_wr_en  <= 1'b0;
      reg_rd_en  <= 1'b0;
      frame_done <= 1'b0;

      if (sck_rise) begin
        shift_in  <= {shift_in[46:0], mosi_sync[1]};
        bit_count <= bit_count + 1;

        // After address phase (16 bits), latch read data
        if (bit_count == 15) begin
          reg_addr  <= {shift_in[14:0], mosi_sync[1]};
          reg_rd_en <= ~shift_in[15];  // read if R/W bit = 0
        end

        // Frame complete at 48 bits
        if (bit_count == 47) begin
          frame_done <= 1'b1;
          if (shift_in[47]) begin  // write
            reg_addr    <= shift_in[46:32];
            reg_wr_data <= {shift_in[30:0], mosi_sync[1]};
            reg_wr_en   <= 1'b1;
          end
        end
      end

      // Shift out read data on falling edge
      if (sck_fall && bit_count > 16) begin
        if (bit_count == 17)
          shift_out <= reg_rd_data;
        else
          shift_out <= {shift_out[30:0], 1'b0};
      end
    end
  end

  assign spi_miso = cs_active ? shift_out[31] : 1'bz;

endmodule
