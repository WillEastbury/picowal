// tb_query_bypass.v — Testbench for PicoWAL FPGA command router
//
// Tests:
//   1. Data read (addr[52]=0) → bypass to downstream, no IRQ
//   2. Data write (addr[52]=0, flags[0]=1) → bypass + idx_irq for index update
//   3. Query (addr[52]=1) → staged in SRAM FIFO, qry_irq asserted
//   4. Aborted transaction → no state change
//   5. Stats counters track correctly
//
// Key invariant: picos never touch SPI buses or SRAM directly.
// All pico interaction is via FPGA register SPI (tested at interface level).
//
`default_nettype none
`timescale 1ns / 1ps

module tb_query_bypass;
    reg        clk, rst_n;

    // Upstream SPI
    reg        up_spi_sck, up_spi_mosi, up_spi_cs_n;
    wire       up_spi_miso;

    // Downstream SPI
    wire       dn_spi_sck, dn_spi_mosi, dn_spi_cs_n;
    reg        dn_spi_miso;

    // Query pico SPI (pico=master, FPGA=slave)
    reg        qry_spi_sck, qry_spi_mosi, qry_spi_cs_n;
    wire       qry_spi_miso;
    wire       qry_irq;

    // Index pico SPI (pico=master, FPGA=slave)
    reg        idx_spi_sck, idx_spi_mosi, idx_spi_cs_n;
    wire       idx_spi_miso;
    wire       idx_irq;

    // SRAM (FPGA-owned)
    wire [17:0] sram_addr;
    wire [15:0] sram_wdata;
    reg  [15:0] sram_rdata;
    wire        sram_we_n, sram_oe_n, sram_ce_n;

    // Stats
    wire [31:0] bypass_count, query_count, copy_count, index_update_count;

    query_bypass uut (
        .clk(clk), .rst_n(rst_n),
        .up_spi_sck(up_spi_sck), .up_spi_mosi(up_spi_mosi),
        .up_spi_miso(up_spi_miso), .up_spi_cs_n(up_spi_cs_n),
        .dn_spi_sck(dn_spi_sck), .dn_spi_mosi(dn_spi_mosi),
        .dn_spi_miso(dn_spi_miso), .dn_spi_cs_n(dn_spi_cs_n),
        .qry_spi_sck(qry_spi_sck), .qry_spi_mosi(qry_spi_mosi),
        .qry_spi_miso(qry_spi_miso), .qry_spi_cs_n(qry_spi_cs_n),
        .qry_irq(qry_irq),
        .idx_spi_sck(idx_spi_sck), .idx_spi_mosi(idx_spi_mosi),
        .idx_spi_miso(idx_spi_miso), .idx_spi_cs_n(idx_spi_cs_n),
        .idx_irq(idx_irq),
        .sram_addr(sram_addr), .sram_wdata(sram_wdata),
        .sram_rdata(sram_rdata),
        .sram_we_n(sram_we_n), .sram_oe_n(sram_oe_n), .sram_ce_n(sram_ce_n),
        .bypass_count(bypass_count), .query_count(query_count),
        .copy_count(copy_count), .index_update_count(index_update_count)
    );

    // Clock: 100 MHz
    initial clk = 0;
    always #5 clk = ~clk;

    integer pass_count = 0;
    integer fail_count = 0;

    task check(input [255:0] label, input cond);
    begin
        if (cond) begin
            $display("  [PASS] %0s", label);
            pass_count = pass_count + 1;
        end else begin
            $display("  [FAIL] %0s", label);
            fail_count = fail_count + 1;
        end
    end
    endtask

    // Shift 72 bits (9 bytes: flags + addr) via upstream SPI
    task send_cmd(input [7:0] flags, input [63:0] addr);
        reg [71:0] cmd;
        integer i;
    begin
        cmd = {flags, addr};
        up_spi_cs_n = 1'b0;
        #20;
        for (i = 71; i >= 0; i = i - 1) begin
            up_spi_mosi = cmd[i];
            up_spi_sck = 1'b0; #20;
            up_spi_sck = 1'b1; #20;
        end
        up_spi_sck = 1'b0;
        #40;
    end
    endtask

    task finish_cmd;
    begin
        up_spi_cs_n = 1'b1;
        #100;
    end
    endtask

    // Build address: tenant[10:0], index_flag, card[9:0], block[41:0]
    function [63:0] make_addr;
        input [10:0] tenant;
        input        index_flag;
        input [9:0]  card;
        input [41:0] block;
    begin
        make_addr = {tenant, index_flag, card, block};
    end
    endfunction

    initial begin
        $display("=== tb_query_bypass ===");
        rst_n = 0;
        up_spi_sck = 0; up_spi_mosi = 0; up_spi_cs_n = 1;
        dn_spi_miso = 0;
        qry_spi_sck = 0; qry_spi_mosi = 0; qry_spi_cs_n = 1;
        idx_spi_sck = 0; idx_spi_mosi = 0; idx_spi_cs_n = 1;
        sram_rdata = 16'h0;
        #100;
        rst_n = 1;
        #50;

        // ── Test 1: Data READ (addr[52]=0) → bypass, no IRQs ──
        $display("\n--- Test 1: Data read bypass ---");
        send_cmd(8'h00, make_addr(11'd1, 1'b0, 10'd5, 42'h100));
        check("bypass_count=1",  bypass_count == 32'd1);
        check("query_count=0",   query_count  == 32'd0);
        check("idx_irq not set (read)", idx_irq == 1'b0);
        check("dn CS asserted",  dn_spi_cs_n  == 1'b0);
        finish_cmd;
        check("dn CS released",  dn_spi_cs_n  == 1'b1);

        // ── Test 2: Data WRITE (addr[52]=0, flags[0]=1) → bypass + idx_irq ──
        $display("\n--- Test 2: Data write bypass + index notify ---");
        send_cmd(8'h01, make_addr(11'd1, 1'b0, 10'd5, 42'h200));
        check("bypass_count=2",           bypass_count       == 32'd2);
        check("index_update_count=1",     index_update_count == 32'd1);
        finish_cmd;

        // ── Test 3: Query (addr[52]=1) → FIFO + qry_irq ──
        $display("\n--- Test 3: Query → SRAM FIFO ---");
        send_cmd(8'h00, make_addr(11'd1, 1'b1, 10'd5, 42'h100));
        check("query_count=1",  query_count  == 32'd1);
        check("bypass_count=2", bypass_count == 32'd2);
        finish_cmd;
        // After finish + idle settle, SRAM writes are done
        #50;
        check("SRAM writes completed (WE deasserted)", sram_we_n == 1'b1);
        check("qry_irq asserted (FIFO not empty)", qry_irq == 1'b1);

        // ── Test 4: Aborted transaction ──
        $display("\n--- Test 4: Aborted (CS deassert mid-shift) ---");
        up_spi_cs_n = 1'b0;
        #20;
        begin : abort_block
            integer i;
            for (i = 0; i < 32; i = i + 1) begin
                up_spi_mosi = 1'b0;
                up_spi_sck = 1'b0; #20;
                up_spi_sck = 1'b1; #20;
            end
        end
        up_spi_cs_n = 1'b1;
        #100;
        check("bypass unchanged after abort", bypass_count == 32'd2);
        check("query unchanged after abort",  query_count  == 32'd1);

        // ── Test 5: Second query ──
        $display("\n--- Test 5: Second query ---");
        send_cmd(8'h00, make_addr(11'd2, 1'b1, 10'd10, 42'h300));
        check("query_count=2",  query_count  == 32'd2);
        check("bypass_count=2", bypass_count == 32'd2);
        finish_cmd;

        // ── Summary ──
        $display("\n========================================");
        $display(" tb_query_bypass: %0d/%0d PASS", pass_count, pass_count + fail_count);
        if (fail_count > 0)
            $display(" *** %0d FAILURES ***", fail_count);
        $display("========================================");
        $finish;
    end

endmodule
