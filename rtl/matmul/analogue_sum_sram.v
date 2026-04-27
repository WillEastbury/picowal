// analogue_sum_sram.v — Analogue accumulation via SRAM current summing
//
// THE IDEA:
//   Multiple SRAMs output products simultaneously on shared data tracks.
//   Each SRAM's data pins drive through a resistor to a summing bus.
//   The VOLTAGE on the summing bus = proportional to SUM of all outputs.
//   Physics does the addition. No adder tree. No FPGA gates for accumulate.
//
// ═══════════════════════════════════════════════════════════════════
//                      PHYSICAL WIRING (PCB)
// ═══════════════════════════════════════════════════════════════════
//
//  SRAM0.D[7:0] ──[R-2R DAC]──►─┐
//  SRAM1.D[7:0] ──[R-2R DAC]──►─┤
//  SRAM2.D[7:0] ──[R-2R DAC]──►─┼── SUMMING BUS ── FPGA ADC input
//  SRAM3.D[7:0] ──[R-2R DAC]──►─┤        │
//  SRAM7.D[7:0] ──[R-2R DAC]──►─┘    (voltage ∝ Σ products)
//
//  Each R-2R ladder converts 8-bit SRAM output to analogue current.
//  Currents sum at the bus node (Kirchhoff's current law).
//  Single ADC read = dot product. No digital addition.
//
// ═══════════════════════════════════════════════════════════════════
//                      WHY THIS WORKS
// ═══════════════════════════════════════════════════════════════════
//
//  SRAM output buffers (unlike FPGA GPIO):
//    - VOH ≥ 2.4V, VOL ≤ 0.4V (well-defined, spec'd at load)
//    - Proper CMOS totem-pole drivers, not weak fabric I/O
//    - Consistent across chips at same VCC/temperature
//    - IS61WV25616BLL output drive: 8mA per pin
//
//  R-2R ladder (0.1% resistors):
//    - 8-bit DAC accuracy: ±0.5 LSB with 0.1% tolerance
//    - 10K base R, E96 series, £0.01 each
//    - 16 resistors per DAC × 8 SRAMs = 128 resistors = £1.28
//
//  Summing accuracy:
//    - 8 contributions × 8-bit each = effective 11-bit sum
//    - Analogue noise floor: ~6-7 effective bits with 0.1% R
//    - Sufficient for INT8 inference (especially with retraining)
//
// ═══════════════════════════════════════════════════════════════════
//                      ADC OPTIONS
// ═══════════════════════════════════════════════════════════════════
//
//  Option A: External flash ADC (fastest)
//    - 8× LM361 comparators + resistor ladder = 3-bit flash ADC
//    - Or AD7822: 8-bit, 2MSPS, parallel output, SOIC-20, $3
//    - Converts in 400ns. One ADC per summing bus.
//
//  Option B: FPGA comparator trick (cheapest)
//    - iCE40HX input pins have defined VIH/VIL thresholds
//    - Programmable via input register (Schmitt trigger)
//    - Feed summing bus through resistor divider to N FPGA pins
//    - Each pin set to different threshold = thermometer ADC
//    - ~3-4 effective bits. Free (no extra parts).
//
//  Option C: SAR ADC on RP2354B (simplest)
//    - RP2354B has 4× ADC channels, 12-bit, 500KSPS
//    - Feed summing bus directly to ADC pin
//    - 2μs conversion time. Limits throughput but zero extra parts.
//    - Good for prototyping, not for max speed.
//
//  Option D: Sigma-delta in FPGA (clever)
//    - Use one FPGA output pin + RC filter as feedback DAC
//    - Compare with summing bus via analogue comparator
//    - Bit-bang sigma-delta ADC in FPGA fabric
//    - ~8-10 bit resolution, ~1MSPS
//    - Needs 1× comparator IC ($0.10)
//
// ═══════════════════════════════════════════════════════════════════
//                      FULL ARCHITECTURE
// ═══════════════════════════════════════════════════════════════════
//
//  BROADCAST PHASE:
//    FPGA drives shared address bus → all 8 SRAMs read same address
//    SRAM A[7:0] = input activation
//    SRAM A[17:8] = each chip has DIFFERENT weight (chip select via A[17:10])
//
//  SUM PHASE:
//    All 8 SRAM outputs drive through R-2R DACs to summing bus
//    Voltage settles in ~50ns (RC time constant)
//    ADC samples the voltage → digital dot product result
//
//  LOOPBACK:
//    Digital result → FPGA → next address cycle (accumulate or next layer)
//
//  For 8-element dot product:
//    1 broadcast cycle + 1 settle + 1 ADC = ~100ns total
//    But ALL 8 multiplies + the sum happen simultaneously!
//    No sequential accumulation. Fully parallel.
//
// ═══════════════════════════════════════════════════════════════════
//
//  THROUGHPUT COMPARISON:
//    Digital adder tree:  1 dot per clock @ 100MHz = 100M dot/s
//    Analogue sum bus:    1 dot per ~100ns = 10M dot/s (slower!)
//    BUT: analogue needs ZERO FPGA LUTs for accumulation
//    AND: can run MORE parallel buses (no LUT budget limit)
//
//  With 8 parallel summing buses (8 ADC channels):
//    8 × 10M = 80M dot/s — approaching digital speed
//    With zero adder tree LUTs. The PCB is the computer.

module analogue_sum_sram #(
    parameter N_SRAM    = 8,     // SRAMs per summing group
    parameter N_BUSES   = 1,     // parallel summing buses
    parameter ADC_BITS  = 8      // ADC resolution
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- Address routing (FPGA → SRAM pins) ----
    output reg  [17:0] sram_addr [0:N_SRAM-1],
    output reg  [N_SRAM-1:0] sram_ce_n,
    output reg  [N_SRAM-1:0] sram_oe_n,

    // ---- Analogue interface (active from PCB summing bus → ADC → FPGA) ----
    // The physical summing happens on the PCB. FPGA sees digital ADC result.
    input  wire [ADC_BITS-1:0] adc_result [0:N_BUSES-1],
    input  wire [N_BUSES-1:0]  adc_valid,

    // ---- Control ----
    input  wire [7:0]  x_in [0:N_SRAM-1],    // input vector
    input  wire [9:0]  row_sel [0:N_SRAM-1],  // weight row per SRAM
    input  wire        start,

    // ---- ADC trigger ----
    output reg         adc_trigger,   // tell ADC to sample (after settle time)

    // ---- Result ----
    output reg  [ADC_BITS-1:0] dot_result,
    output reg                 dot_valid
);

    localparam S_IDLE    = 3'd0,
               S_DRIVE   = 3'd1,
               S_SETTLE  = 3'd2,
               S_SAMPLE  = 3'd3,
               S_CAPTURE = 3'd4;

    reg [2:0] state;
    reg [3:0] settle_count;

    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            sram_ce_n   <= {N_SRAM{1'b1}};
            sram_oe_n   <= {N_SRAM{1'b1}};
            adc_trigger <= 0;
            dot_valid   <= 0;
        end else begin
            dot_valid   <= 0;
            adc_trigger <= 0;

            case (state)

            S_IDLE: begin
                if (start) begin
                    // Drive addresses to all SRAMs simultaneously
                    for (i = 0; i < N_SRAM; i = i + 1) begin
                        sram_addr[i] <= {row_sel[i][1:0], row_sel[i][9:2], x_in[i]};
                        sram_ce_n[i] <= 0;
                        sram_oe_n[i] <= 0;
                    end
                    state <= S_DRIVE;
                end
            end

            S_DRIVE: begin
                // Wait one cycle for address to propagate
                settle_count <= 0;
                state <= S_SETTLE;
            end

            S_SETTLE: begin
                // Wait for SRAM outputs + R-2R + RC to settle
                // SRAM: 10ns, R-2R settle: ~20ns, RC: ~20ns = ~50ns total
                // At 100MHz: 5 cycles
                if (settle_count >= 4'd5) begin
                    adc_trigger <= 1;
                    state <= S_SAMPLE;
                end else begin
                    settle_count <= settle_count + 1;
                end
            end

            S_SAMPLE: begin
                // Wait for ADC conversion
                if (adc_valid[0]) begin
                    dot_result <= adc_result[0];
                    dot_valid  <= 1;
                    // Deassert SRAM
                    sram_ce_n <= {N_SRAM{1'b1}};
                    sram_oe_n <= {N_SRAM{1'b1}};
                    state     <= S_IDLE;
                end
            end

            endcase
        end
    end

endmodule
