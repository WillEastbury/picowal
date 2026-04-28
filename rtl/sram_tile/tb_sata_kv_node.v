// tb_sata_kv_node.v — Integration testbench for sata_kv_node
// Verifies: compilation, reset sequencing, SATA PHY link-up, LED status

`timescale 1ns / 1ps

module tb_sata_kv_node;

    reg         clk_12m;
    wire        rgmii_txc;
    wire [3:0]  rgmii_txd;
    wire        rgmii_tx_ctl;
    wire        phy_rst_n;
    wire        mdc;
    wire        mdio;
    wire        sata_txp, sata_txn;
    wire [21:0] nor_addr;
    wire [15:0] nor_dq;
    wire        nor_ce_n, nor_oe_n, nor_we_n;
    wire        led_link, led_sata_rdy, led_activity, led_error;

    // 50 MHz clock (20 ns period)
    initial clk_12m = 0;
    always #10 clk_12m = ~clk_12m;

    sata_kv_node #(
        .MAC_ADDR (48'h02_00_00_00_00_01),
        .IP_ADDR  ({8'd192, 8'd168, 8'd1, 8'd100})
    ) dut (
        .clk_12m      (clk_12m),
        .rgmii_txc    (rgmii_txc),
        .rgmii_txd    (rgmii_txd),
        .rgmii_tx_ctl (rgmii_tx_ctl),
        .rgmii_rxc    (1'b0),
        .rgmii_rxd    (4'd0),
        .rgmii_rx_ctl (1'b0),
        .phy_rst_n    (phy_rst_n),
        .mdc          (mdc),
        .mdio         (mdio),
        .sata_txp     (sata_txp),
        .sata_txn     (sata_txn),
        .sata_rxp     (1'b0),
        .sata_rxn     (1'b1),
        .nor_addr     (nor_addr),
        .nor_dq       (nor_dq),
        .nor_ce_n     (nor_ce_n),
        .nor_oe_n     (nor_oe_n),
        .nor_we_n     (nor_we_n),
        .led_link     (led_link),
        .led_sata_rdy (led_sata_rdy),
        .led_activity (led_activity),
        .led_error    (led_error)
    );

    // ------------------------------------------------------------------
    // Test infrastructure
    // ------------------------------------------------------------------

    integer pass_count = 0;
    integer fail_count = 0;

    task check;
        input cond;
        input [8*80-1:0] msg;
        begin
            if (cond) begin
                $display("  PASS: %0s", msg);
                pass_count = pass_count + 1;
            end else begin
                $display("  FAIL: %0s", msg);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // ------------------------------------------------------------------
    // Main test sequence
    // ------------------------------------------------------------------

    initial begin
        $display("=== tb_sata_kv_node: integration tests ===");
        $display("");

        // ---- Test 1: Reset sequence ----
        #100;  // 5 clock cycles
        check(phy_rst_n === 1'b1,   "phy_rst_n deasserts after PLL lock");
        check(dut.rst_n === 1'b1,   "internal rst_n deasserts after PLL lock");

        // ---- Test 2: SATA PHY link-up via OOB ----
        // OOB_SHORT=1: ~200 cycles to complete OOB + a few more for ALIGN
        // Budget 2000 cycles (40 us) to be safe
        begin : wait_phy_ready
            integer i;
            for (i = 0; i < 2000; i = i + 1) begin
                @(posedge clk_12m);
                if (led_sata_rdy === 1'b1) disable wait_phy_ready;
            end
        end

        check(led_sata_rdy === 1'b1, "led_sata_rdy asserts (PHY OOB complete)");

        // ---- Test 3: LED sanity ----
        check(led_error === 1'b0,    "led_error is low at startup");

        // ---- Summary ----
        $display("");
        if (fail_count == 0)
            $display("ALL %0d TESTS PASSED", pass_count);
        else
            $display("FAILED: %0d of %0d tests failed",
                     fail_count, pass_count + fail_count);

        $finish;
    end

    // Timeout watchdog
    initial begin
        #200000;
        $display("TIMEOUT after 200 us");
        $finish;
    end

endmodule
