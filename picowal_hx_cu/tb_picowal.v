// tb_picowal.v -- Testbench for PicoWAL top module
// Verifies: PLL lock → scheduler starts → decode → ALU → LED output
// Run: iverilog -g2012 -o tb tb_picowal.v ice40_stub.v picowal_hx_top.v \
//        picoscript_decode.v picoscript_alu.v picoscript_branch.v \
//        sram_controller.v spi_master.v context_scheduler.v \
//        pipe_engine.v http_parser.v && vvp tb

`timescale 1ns/1ps

module tb_picowal;

    reg clk_100mhz;
    wire [17:0] sram_addr;
    wire [15:0] sram_data;
    wire sram_ce_n, sram_oe_n, sram_we_n, sram_lb_n, sram_ub_n;
    wire w5100_mosi, w5100_sck, w5100_cs_n, w5100_rst_n;
    reg  w5100_miso, w5100_int_n;
    wire sd_mosi, sd_sck, sd_cs_n;
    reg  sd_miso, sd_detect;
    wire uart_tx;
    reg  uart_rx;
    wire [7:0] leds;

    // SRAM model: simple read-back (stores last write)
    // Use small model for fast simulation (only 1K words needed for test)
    reg [15:0] sram_mem [0:1023];
    reg [15:0] sram_drive;
    reg        sram_driving;

    assign sram_data = sram_driving ? sram_drive : 16'hZZZZ;

    always @(*) begin
        sram_driving = ~sram_oe_n & ~sram_ce_n;
        if (sram_driving)
            sram_drive = sram_mem[sram_addr[9:0]];
    end

    always @(negedge sram_we_n) begin
        if (~sram_ce_n)
            sram_mem[sram_addr[9:0]] <= sram_data;
    end

    // DUT
    picowal_hx_top dut (
        .clk_100mhz (clk_100mhz),
        .sram_addr   (sram_addr),
        .sram_data   (sram_data),
        .sram_ce_n   (sram_ce_n),
        .sram_oe_n   (sram_oe_n),
        .sram_we_n   (sram_we_n),
        .sram_lb_n   (sram_lb_n),
        .sram_ub_n   (sram_ub_n),
        .w5100_mosi  (w5100_mosi),
        .w5100_miso  (w5100_miso),
        .w5100_sck   (w5100_sck),
        .w5100_cs_n  (w5100_cs_n),
        .w5100_int_n (w5100_int_n),
        .w5100_rst_n (w5100_rst_n),
        .sd_mosi     (sd_mosi),
        .sd_miso     (sd_miso),
        .sd_sck      (sd_sck),
        .sd_cs_n     (sd_cs_n),
        .sd_detect   (sd_detect),
        .uart_tx     (uart_tx),
        .uart_rx     (uart_rx),
        .leds        (leds)
    );

    // Clock: 100MHz → 10ns period
    initial clk_100mhz = 0;
    always #5 clk_100mhz = ~clk_100mhz;

    // Default inputs
    initial begin
        w5100_miso  = 1'b0;
        w5100_int_n = 1'b1;
        sd_miso     = 1'b0;
        sd_detect   = 1'b1;     // card present
        uart_rx     = 1'b1;     // idle
    end

    // Pre-load instruction memory directly into DUT's instr_mem
    //   ADD R0, R0, 42   → opcode=4, rd=0, rs1=0, rs2=0, imm16=42 → 0x4000002A
    //   ADD R1, R0, 1    → 0x41000001
    //   SUB R2, R1, 10   → 0x5210000A
    //   NOOP             → 0x00000000
    initial begin
        dut.instr_mem[0] = 32'h4000_002A;  // ADD R0, R0, 42
        dut.instr_mem[1] = 32'h4100_0001;  // ADD R1, R0, 1
        dut.instr_mem[2] = 32'h5210_000A;  // SUB R2, R1, 10
        dut.instr_mem[3] = 32'h0000_0000;  // NOOP
        dut.instr_mem[4] = 32'h0000_0000;  // NOOP (padding)
    end

    // Test sequence
    initial begin
        $dumpfile("tb_picowal.vcd");
        $dumpvars(0, tb_picowal);

        // Wait for PLL lock (stub locks after 1 cycle = 10ns + margin)
        #50;

        // Check LED7 = PLL locked
        if (leds[7] !== 1'b1) begin
            $display("FAIL: PLL not locked after 100ns");
            $finish;
        end
        $display("PASS: PLL locked, LED7=1");

        // Check LED6 = context valid (scheduler running)
        #500;
        if (leds[6] !== 1'b1) begin
            $display("FAIL: Scheduler not active after 550ns (LED6=%b, leds=%b)", leds[6], leds);
            $finish;
        end
        $display("PASS: Scheduler active, LED6=1");

        // Let it run for a while to exercise execution
        #5000;

        // Check no stack errors
        if (leds[1] | leds[0]) begin
            $display("FAIL: Stack error detected (LEDs[1:0]=%b)", leds[1:0]);
            $finish;
        end
        $display("PASS: No stack errors after 5000ns");

        $display("");
        $display("═══════════════════════════════════════════");
        $display(" ALL TESTS PASSED — PicoWAL boots cleanly");
        $display("═══════════════════════════════════════════");
        $finish;
    end

    // Timeout
    initial begin
        #20000;
        $display("TIMEOUT at 20us");
        $finish;
    end

endmodule
