// Full-stack testbench for SATA KV node
// Wires: KV Engine -> SATA Command -> Mock Transport
// Tests the complete data path without PHY/link complexity.
`timescale 1ns / 1ps

module tb_sata_kv_fullstack;

    reg clk, rst_n;

    // Clock: 10ns period
    initial clk = 0;
    always #5 clk = ~clk;

    // =========================================================================
    // KV Engine <-> network side signals (testbench drives these)
    // =========================================================================
    reg         cmd_valid;
    reg  [7:0]  cmd_flags;
    reg  [63:0] cmd_addr;
    wire        cmd_ready;

    reg  [31:0] net_wr_data;
    reg         net_wr_valid;
    reg         net_wr_last;
    wire        net_wr_ready;

    // Response from KV engine
    wire [31:0] resp_data;
    wire        resp_valid;
    wire        resp_last;
    reg         resp_ready;

    wire        resp_start;
    wire [31:0] resp_tag;
    wire [7:0]  resp_flags;

    wire        ack_valid;
    wire [7:0]  ack_byte;

    wire [31:0] reads_completed, writes_completed, errors;

    // =========================================================================
    // KV Engine <-> SATA Command interconnect
    // =========================================================================
    wire        sc_cmd_valid;
    wire        sc_cmd_ready;
    wire        sc_cmd_is_write;
    wire [47:0] sc_cmd_lba;
    wire [15:0] sc_cmd_count;

    wire [31:0] sc_wr_data;
    wire        sc_wr_valid;
    wire        sc_wr_last;
    wire        sc_wr_ready;

    wire [31:0] sc_rd_data;
    wire        sc_rd_valid;
    wire        sc_rd_last;

    wire        sc_cmd_complete;
    wire        sc_cmd_error;
    wire [7:0]  sc_cmd_status;

    // =========================================================================
    // SATA Command <-> Transport interconnect (mock drives the transport side)
    // =========================================================================
    wire        tp_cmd_tx_start;
    wire [7:0]  tp_cmd_tx_command;
    wire [47:0] tp_cmd_tx_lba;
    wire [15:0] tp_cmd_tx_count;
    wire [7:0]  tp_cmd_tx_features;
    wire [7:0]  tp_cmd_tx_device;
    reg         tp_cmd_tx_done;
    reg         tp_cmd_tx_err;

    wire        tp_data_tx_start;
    wire [31:0] tp_data_tx_dword;
    wire        tp_data_tx_valid;
    wire        tp_data_tx_last;
    reg         tp_data_tx_ready;
    reg         tp_data_tx_done;

    reg         tp_rx_reg_fis_valid;
    reg  [7:0]  tp_rx_status;
    reg  [7:0]  tp_rx_error;
    reg         tp_rx_pio_setup_valid;
    reg [15:0]  tp_rx_pio_xfer_count;
    reg  [7:0]  tp_rx_pio_status;
    reg         tp_rx_dma_activate;
    reg  [31:0] tp_rx_data_dword;
    reg         tp_rx_data_valid;
    reg         tp_rx_data_last;
    reg         tp_rx_data_err;

    // Init
    reg         do_init;
    wire        init_done;
    wire [127:0] identify_data_flat;

    // =========================================================================
    // DUT: sata_kv_engine
    // =========================================================================
    sata_kv_engine u_kv (
        .clk             (clk),
        .rst_n           (rst_n),
        .cmd_valid       (cmd_valid),
        .cmd_flags       (cmd_flags),
        .cmd_addr        (cmd_addr),
        .cmd_ready       (cmd_ready),
        .net_wr_data     (net_wr_data),
        .net_wr_valid    (net_wr_valid),
        .net_wr_last     (net_wr_last),
        .net_wr_ready    (net_wr_ready),
        .sata_cmd_valid  (sc_cmd_valid),
        .sata_cmd_ready  (sc_cmd_ready),
        .sata_cmd_is_write(sc_cmd_is_write),
        .sata_cmd_lba    (sc_cmd_lba),
        .sata_cmd_count  (sc_cmd_count),
        .sata_wr_data    (sc_wr_data),
        .sata_wr_valid   (sc_wr_valid),
        .sata_wr_last    (sc_wr_last),
        .sata_wr_ready   (sc_wr_ready),
        .sata_rd_data    (sc_rd_data),
        .sata_rd_valid   (sc_rd_valid),
        .sata_rd_last    (sc_rd_last),
        .sata_cmd_complete(sc_cmd_complete),
        .sata_cmd_error  (sc_cmd_error),
        .sata_cmd_status (sc_cmd_status),
        .resp_data       (resp_data),
        .resp_valid      (resp_valid),
        .resp_last       (resp_last),
        .resp_ready      (resp_ready),
        .resp_start      (resp_start),
        .resp_tag        (resp_tag),
        .resp_flags      (resp_flags),
        .ack_valid       (ack_valid),
        .ack_byte        (ack_byte),
        .reads_completed (reads_completed),
        .writes_completed(writes_completed),
        .errors          (errors)
    );

    // =========================================================================
    // DUT: sata_command
    // =========================================================================
    sata_command u_cmd (
        .clk               (clk),
        .rst_n             (rst_n),
        .user_cmd_valid    (sc_cmd_valid),
        .user_cmd_ready    (sc_cmd_ready),
        .user_cmd_is_write (sc_cmd_is_write),
        .user_cmd_lba      (sc_cmd_lba),
        .user_cmd_count    (sc_cmd_count),
        .user_wr_data      (sc_wr_data),
        .user_wr_valid     (sc_wr_valid),
        .user_wr_last      (sc_wr_last),
        .user_wr_ready     (sc_wr_ready),
        .user_rd_data      (sc_rd_data),
        .user_rd_valid     (sc_rd_valid),
        .user_rd_last      (sc_rd_last),
        .cmd_complete      (sc_cmd_complete),
        .cmd_error         (sc_cmd_error),
        .cmd_status        (sc_cmd_status),
        .do_init           (do_init),
        .init_done         (init_done),
        .identify_data_flat(identify_data_flat),
        .tp_cmd_tx_start   (tp_cmd_tx_start),
        .tp_cmd_tx_command (tp_cmd_tx_command),
        .tp_cmd_tx_lba     (tp_cmd_tx_lba),
        .tp_cmd_tx_count   (tp_cmd_tx_count),
        .tp_cmd_tx_features(tp_cmd_tx_features),
        .tp_cmd_tx_device  (tp_cmd_tx_device),
        .tp_cmd_tx_done    (tp_cmd_tx_done),
        .tp_cmd_tx_err     (tp_cmd_tx_err),
        .tp_data_tx_start  (tp_data_tx_start),
        .tp_data_tx_dword  (tp_data_tx_dword),
        .tp_data_tx_valid  (tp_data_tx_valid),
        .tp_data_tx_last   (tp_data_tx_last),
        .tp_data_tx_ready  (tp_data_tx_ready),
        .tp_data_tx_done   (tp_data_tx_done),
        .tp_rx_reg_fis_valid (tp_rx_reg_fis_valid),
        .tp_rx_status      (tp_rx_status),
        .tp_rx_error       (tp_rx_error),
        .tp_rx_pio_setup_valid(tp_rx_pio_setup_valid),
        .tp_rx_pio_xfer_count(tp_rx_pio_xfer_count),
        .tp_rx_pio_status  (tp_rx_pio_status),
        .tp_rx_dma_activate(tp_rx_dma_activate),
        .tp_rx_data_dword  (tp_rx_data_dword),
        .tp_rx_data_valid  (tp_rx_data_valid),
        .tp_rx_data_last   (tp_rx_data_last),
        .tp_rx_data_err    (tp_rx_data_err)
    );

    // =========================================================================
    // Mock SATA Transport Device
    // =========================================================================
    // Storage: 64 pages x 1024 DWORDs = 256KB
    reg [31:0] storage [0:16383];

    // Mock FSM
    localparam [3:0] M_IDLE        = 4'd0,
                     M_CMD_DONE    = 4'd1,
                     M_READ_DATA   = 4'd2,
                     M_READ_D2H    = 4'd3,
                     M_WRITE_ACT   = 4'd4,
                     M_WRITE_DATA  = 4'd5,
                     M_WRITE_DONE  = 4'd6,
                     M_WRITE_D2H   = 4'd7,
                     M_ID_PIO      = 4'd8,
                     M_ID_DATA     = 4'd9,
                     M_ID_D2H      = 4'd10,
                     M_SF_D2H      = 4'd11;

    reg [3:0]  m_state;
    reg [7:0]  m_cmd;
    reg [47:0] m_lba;
    reg [15:0] m_count;
    reg [15:0] m_dw_idx;
    reg [15:0] m_total_dw;
    reg [1:0]  m_delay;

    // Compute storage base address: lba_page = LBA / 8, base = lba_page * 1024
    wire [13:0] m_storage_base;
    assign m_storage_base = m_lba[9:3] * 1024;

    // Initialize storage to zero
    integer si;
    initial begin
        for (si = 0; si < 16384; si = si + 1)
            storage[si] = 32'd0;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_state            <= M_IDLE;
            m_cmd              <= 8'd0;
            m_lba              <= 48'd0;
            m_count            <= 16'd0;
            m_dw_idx           <= 16'd0;
            m_total_dw         <= 16'd0;
            m_delay            <= 2'd0;
            tp_cmd_tx_done     <= 1'b0;
            tp_cmd_tx_err      <= 1'b0;
            tp_data_tx_ready   <= 1'b0;
            tp_data_tx_done    <= 1'b0;
            tp_rx_reg_fis_valid<= 1'b0;
            tp_rx_status       <= 8'd0;
            tp_rx_error        <= 8'd0;
            tp_rx_pio_setup_valid <= 1'b0;
            tp_rx_pio_xfer_count  <= 16'd0;
            tp_rx_pio_status   <= 8'd0;
            tp_rx_dma_activate <= 1'b0;
            tp_rx_data_dword   <= 32'd0;
            tp_rx_data_valid   <= 1'b0;
            tp_rx_data_last    <= 1'b0;
            tp_rx_data_err     <= 1'b0;
        end else begin
            // Clear single-cycle pulses
            tp_cmd_tx_done      <= 1'b0;
            tp_cmd_tx_err       <= 1'b0;
            tp_data_tx_done     <= 1'b0;
            tp_rx_reg_fis_valid <= 1'b0;
            tp_rx_pio_setup_valid <= 1'b0;
            tp_rx_dma_activate  <= 1'b0;
            tp_rx_data_valid    <= 1'b0;
            tp_rx_data_last     <= 1'b0;
            tp_rx_data_err      <= 1'b0;

            case (m_state)
                M_IDLE: begin
                    tp_data_tx_ready <= 1'b0;
                    if (tp_cmd_tx_start) begin
                        m_cmd      <= tp_cmd_tx_command;
                        m_lba      <= tp_cmd_tx_lba;
                        m_count    <= tp_cmd_tx_count;
                        m_delay    <= 2'd2;
                        m_state    <= M_CMD_DONE;
                    end
                end

                M_CMD_DONE: begin
                    if (m_delay != 2'd0) begin
                        m_delay <= m_delay - 1;
                    end else begin
                        tp_cmd_tx_done <= 1'b1;
                        m_dw_idx       <= 16'd0;
                        if (m_cmd == 8'h25) begin
                            // READ DMA EXT
                            m_total_dw <= m_count * 128;
                            m_state    <= M_READ_DATA;
                        end else if (m_cmd == 8'h35) begin
                            // WRITE DMA EXT
                            m_total_dw <= m_count * 128;
                            m_state    <= M_WRITE_ACT;
                        end else if (m_cmd == 8'hEC) begin
                            // IDENTIFY
                            m_state <= M_ID_PIO;
                        end else if (m_cmd == 8'hEF) begin
                            // SET FEATURES
                            m_state <= M_SF_D2H;
                        end else begin
                            // Unknown command: error
                            tp_cmd_tx_err <= 1'b1;
                            m_state       <= M_IDLE;
                        end
                    end
                end

                // --- READ ---
                M_READ_DATA: begin
                    tp_rx_data_valid <= 1'b1;
                    tp_rx_data_dword <= storage[m_storage_base + m_dw_idx[13:0]];
                    if (m_dw_idx == m_total_dw - 1) begin
                        tp_rx_data_last <= 1'b1;
                        m_state         <= M_READ_D2H;
                    end
                    m_dw_idx <= m_dw_idx + 1;
                end

                M_READ_D2H: begin
                    tp_rx_reg_fis_valid <= 1'b1;
                    tp_rx_status        <= 8'h50;
                    tp_rx_error         <= 8'h00;
                    m_state             <= M_IDLE;
                end

                // --- WRITE ---
                M_WRITE_ACT: begin
                    tp_rx_dma_activate <= 1'b1;
                    tp_data_tx_ready   <= 1'b1;
                    m_state            <= M_WRITE_DATA;
                end

                M_WRITE_DATA: begin
                    tp_data_tx_ready <= 1'b1;
                    if (tp_data_tx_valid) begin
                        storage[m_storage_base + m_dw_idx[13:0]] <= tp_data_tx_dword;
                        m_dw_idx <= m_dw_idx + 1;
                        if (tp_data_tx_last) begin
                            m_state <= M_WRITE_DONE;
                        end
                    end
                end

                M_WRITE_DONE: begin
                    tp_data_tx_ready <= 1'b0;
                    tp_data_tx_done  <= 1'b1;
                    m_state          <= M_WRITE_D2H;
                end

                M_WRITE_D2H: begin
                    tp_rx_reg_fis_valid <= 1'b1;
                    tp_rx_status        <= 8'h50;
                    tp_rx_error         <= 8'h00;
                    m_state             <= M_IDLE;
                end

                // --- IDENTIFY ---
                M_ID_PIO: begin
                    tp_rx_pio_setup_valid <= 1'b1;
                    tp_rx_pio_xfer_count  <= 16'd512;
                    tp_rx_pio_status      <= 8'h58;
                    m_dw_idx              <= 16'd0;
                    m_state               <= M_ID_DATA;
                end

                M_ID_DATA: begin
                    tp_rx_data_valid <= 1'b1;
                    tp_rx_data_dword <= {16'd0, m_dw_idx[15:0]};
                    if (m_dw_idx == 16'd127) begin
                        tp_rx_data_last <= 1'b1;
                        m_state         <= M_ID_D2H;
                    end
                    m_dw_idx <= m_dw_idx + 1;
                end

                M_ID_D2H: begin
                    tp_rx_reg_fis_valid <= 1'b1;
                    tp_rx_status        <= 8'h50;
                    tp_rx_error         <= 8'h00;
                    m_state             <= M_IDLE;
                end

                // --- SET FEATURES ---
                M_SF_D2H: begin
                    tp_rx_reg_fis_valid <= 1'b1;
                    tp_rx_status        <= 8'h50;
                    tp_rx_error         <= 8'h00;
                    m_state             <= M_IDLE;
                end

                default: m_state <= M_IDLE;
            endcase
        end
    end

    // =========================================================================
    // Test infrastructure
    // =========================================================================
    integer test_num;
    integer pass_count;
    integer fail_count;
    integer total_tests;
    integer i;
    integer dw_count;
    integer mismatch;
    reg [31:0] expected;
    reg [31:0] captured_data [0:1023];
    integer timeout_ctr;

    task reset_dut;
        begin
            rst_n      <= 1'b0;
            cmd_valid  <= 1'b0;
            cmd_flags  <= 8'd0;
            cmd_addr   <= 64'd0;
            net_wr_data<= 32'd0;
            net_wr_valid <= 1'b0;
            net_wr_last<= 1'b0;
            resp_ready <= 1'b1;
            do_init    <= 1'b0;
            repeat (5) @(posedge clk);
            rst_n      <= 1'b1;
            repeat (2) @(posedge clk);
        end
    endtask

    // Wait for a condition with timeout
    // Returns 1 on timeout
    reg wait_timeout_flag;

    task wait_for_init_done;
        begin
            timeout_ctr = 0;
            wait_timeout_flag = 1'b0;
            while (init_done !== 1'b1 && timeout_ctr < 5000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
            end
            if (timeout_ctr >= 5000)
                wait_timeout_flag = 1'b1;
        end
    endtask

    task wait_for_ack;
        begin
            timeout_ctr = 0;
            wait_timeout_flag = 1'b0;
            while (ack_valid !== 1'b1 && timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
            end
            if (timeout_ctr >= 50000)
                wait_timeout_flag = 1'b1;
        end
    endtask

    task wait_for_resp_last;
        begin
            timeout_ctr = 0;
            wait_timeout_flag = 1'b0;
            while (timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
                if (resp_valid === 1'b1 && resp_last === 1'b1)
                    timeout_ctr = 50001; // break
            end
            if (timeout_ctr == 50000)
                wait_timeout_flag = 1'b1;
        end
    endtask

    // =========================================================================
    // Main test sequence
    // =========================================================================
    initial begin
        $dumpfile("tb_sata_kv_fullstack.vcd");
        $dumpvars(0, tb_sata_kv_fullstack);

        pass_count  = 0;
        fail_count  = 0;
        total_tests = 5;

        reset_dut;

        // ==============================================================
        // TEST 1: Init sequence (IDENTIFY + SET FEATURES)
        // ==============================================================
        $display("--- Test 1: Init sequence ---");
        do_init <= 1'b1;
        @(posedge clk);

        wait_for_init_done;

        do_init <= 1'b0;
        @(posedge clk);

        if (wait_timeout_flag) begin
            $display("[FAIL] Test 1: init_done timed out");
            fail_count = fail_count + 1;
        end else if (init_done === 1'b1) begin
            $display("[PASS] Test 1: Init sequence completed, init_done asserted");
            pass_count = pass_count + 1;
        end else begin
            $display("[FAIL] Test 1: init_done not asserted");
            fail_count = fail_count + 1;
        end

        // Let command layer return to idle
        repeat (5) @(posedge clk);

        // ==============================================================
        // TEST 2: READ via KV engine
        // ==============================================================
        $display("--- Test 2: READ via KV engine ---");

        // Pre-fill storage with known pattern for LBA 0x800
        // addr=0x100, LBA = 0x100 * 8 = 0x800, page = 0x800/8 = 0x100
        // storage base = 0x100 * 1024 ... but that exceeds 16384.
        // Use smaller address: addr=0x0008, LBA=0x0040, page=0x0040/8=8, base=8*1024=8192
        for (i = 0; i < 1024; i = i + 1)
            storage[8192 + i] = 32'hA000_0000 + i[15:0];

        // Wait for KV engine to be idle
        @(posedge clk);
        while (cmd_ready !== 1'b1) @(posedge clk);

        cmd_valid <= 1'b1;
        cmd_flags <= 8'h00; // READ
        cmd_addr  <= 64'h0000_0000_0000_0008;
        @(posedge clk);
        cmd_valid <= 1'b0;

        // Collect response data
        dw_count = 0;
        mismatch = 0;
        resp_ready <= 1'b1;

        // Wait for resp_start
        timeout_ctr = 0;
        while (resp_start !== 1'b1 && timeout_ctr < 50000) begin
            @(posedge clk);
            timeout_ctr = timeout_ctr + 1;
        end

        if (timeout_ctr >= 50000) begin
            $display("[FAIL] Test 2: resp_start timed out");
            fail_count = fail_count + 1;
        end else begin
            // Collect data until resp_last
            timeout_ctr = 0;
            while (timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
                if (resp_valid === 1'b1 && resp_ready === 1'b1) begin
                    if (dw_count < 1024)
                        captured_data[dw_count] = resp_data;
                    dw_count = dw_count + 1;
                    if (resp_last === 1'b1)
                        timeout_ctr = 50001; // break
                end
            end

            if (timeout_ctr == 50000) begin
                $display("[FAIL] Test 2: resp_last timed out, got %0d DWORDs", dw_count);
                fail_count = fail_count + 1;
            end else begin
                // Verify data
                for (i = 0; i < 1024; i = i + 1) begin
                    expected = 32'hA000_0000 + i[15:0];
                    if (captured_data[i] !== expected)
                        mismatch = mismatch + 1;
                end
                if (dw_count == 1024 && mismatch == 0) begin
                    $display("[PASS] Test 2: READ returned 1024 DWORDs, data correct");
                    pass_count = pass_count + 1;
                end else begin
                    $display("[FAIL] Test 2: dw_count=%0d (exp 1024), mismatches=%0d", dw_count, mismatch);
                    if (mismatch > 0 && mismatch <= 4) begin
                        for (i = 0; i < 1024; i = i + 1) begin
                            expected = 32'hA000_0000 + i[15:0];
                            if (captured_data[i] !== expected)
                                $display("  [%0d] got=%h exp=%h", i, captured_data[i], expected);
                        end
                    end
                    fail_count = fail_count + 1;
                end
            end
        end

        repeat (10) @(posedge clk);

        // ==============================================================
        // TEST 3: WRITE via KV engine
        // ==============================================================
        $display("--- Test 3: WRITE via KV engine ---");

        // Wait for KV engine idle
        while (cmd_ready !== 1'b1) @(posedge clk);

        // addr=0x0010, LBA=0x0080, page=0x80/8=0x10=16, base=16*1024=16384... too big
        // addr=0x0004, LBA=0x0020, page=0x20/8=4, base=4*1024=4096
        cmd_valid <= 1'b1;
        cmd_flags <= 8'h01; // WRITE
        cmd_addr  <= 64'h0000_0000_0000_0004;
        @(posedge clk);
        cmd_valid <= 1'b0;

        // Wait for net_wr_ready
        timeout_ctr = 0;
        while (net_wr_ready !== 1'b1 && timeout_ctr < 5000) begin
            @(posedge clk);
            timeout_ctr = timeout_ctr + 1;
        end

        // Stream 1024 DWORDs of write data
        for (i = 0; i < 1024; i = i + 1) begin
            net_wr_data  <= 32'hB000_0000 + i[15:0];
            net_wr_valid <= 1'b1;
            net_wr_last  <= (i == 1023) ? 1'b1 : 1'b0;
            @(posedge clk);
            while (net_wr_ready !== 1'b1) @(posedge clk);
        end
        net_wr_valid <= 1'b0;
        net_wr_last  <= 1'b0;

        // Wait for ack
        wait_for_ack;

        if (wait_timeout_flag) begin
            $display("[FAIL] Test 3: ack_valid timed out");
            fail_count = fail_count + 1;
        end else if (ack_byte === 8'h00) begin
            // Verify storage
            mismatch = 0;
            for (i = 0; i < 1024; i = i + 1) begin
                expected = 32'hB000_0000 + i[15:0];
                if (storage[4096 + i] !== expected)
                    mismatch = mismatch + 1;
            end
            if (mismatch == 0) begin
                $display("[PASS] Test 3: WRITE completed, ack=0x00, storage verified");
                pass_count = pass_count + 1;
            end else begin
                $display("[FAIL] Test 3: ack ok but %0d storage mismatches", mismatch);
                fail_count = fail_count + 1;
            end
        end else begin
            $display("[FAIL] Test 3: ack_byte=%h (expected 0x00)", ack_byte);
            fail_count = fail_count + 1;
        end

        repeat (10) @(posedge clk);

        // ==============================================================
        // TEST 4: Write-then-Read coherence
        // ==============================================================
        $display("--- Test 4: Write-then-Read coherence ---");

        // Write to addr=0x0006, LBA=0x0030, page=0x30/8=6, base=6*1024=6144
        while (cmd_ready !== 1'b1) @(posedge clk);

        cmd_valid <= 1'b1;
        cmd_flags <= 8'h01; // WRITE
        cmd_addr  <= 64'h0000_0000_0000_0006;
        @(posedge clk);
        cmd_valid <= 1'b0;

        timeout_ctr = 0;
        while (net_wr_ready !== 1'b1 && timeout_ctr < 5000) begin
            @(posedge clk);
            timeout_ctr = timeout_ctr + 1;
        end

        for (i = 0; i < 1024; i = i + 1) begin
            net_wr_data  <= 32'hC0DE_0000 + i[15:0];
            net_wr_valid <= 1'b1;
            net_wr_last  <= (i == 1023) ? 1'b1 : 1'b0;
            @(posedge clk);
            while (net_wr_ready !== 1'b1) @(posedge clk);
        end
        net_wr_valid <= 1'b0;
        net_wr_last  <= 1'b0;

        wait_for_ack;

        if (wait_timeout_flag) begin
            $display("[FAIL] Test 4: write ack timed out");
            fail_count = fail_count + 1;
        end else begin
            // Now read back from same address
            repeat (10) @(posedge clk);
            while (cmd_ready !== 1'b1) @(posedge clk);

            cmd_valid <= 1'b1;
            cmd_flags <= 8'h00; // READ
            cmd_addr  <= 64'h0000_0000_0000_0006;
            @(posedge clk);
            cmd_valid <= 1'b0;

            // Collect response
            dw_count = 0;
            mismatch = 0;
            resp_ready <= 1'b1;

            timeout_ctr = 0;
            while (resp_start !== 1'b1 && timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
            end

            if (timeout_ctr >= 50000) begin
                $display("[FAIL] Test 4: read resp_start timed out");
                fail_count = fail_count + 1;
            end else begin
                timeout_ctr = 0;
                while (timeout_ctr < 50000) begin
                    @(posedge clk);
                    timeout_ctr = timeout_ctr + 1;
                    if (resp_valid === 1'b1 && resp_ready === 1'b1) begin
                        if (dw_count < 1024)
                            captured_data[dw_count] = resp_data;
                        dw_count = dw_count + 1;
                        if (resp_last === 1'b1)
                            timeout_ctr = 50001; // break
                    end
                end

                if (timeout_ctr == 50000) begin
                    $display("[FAIL] Test 4: read resp_last timed out, got %0d DWORDs", dw_count);
                    fail_count = fail_count + 1;
                end else begin
                    for (i = 0; i < 1024; i = i + 1) begin
                        expected = 32'hC0DE_0000 + i[15:0];
                        if (captured_data[i] !== expected)
                            mismatch = mismatch + 1;
                    end
                    if (dw_count == 1024 && mismatch == 0) begin
                        $display("[PASS] Test 4: Write-then-Read coherence verified");
                        pass_count = pass_count + 1;
                    end else begin
                        $display("[FAIL] Test 4: dw_count=%0d, mismatches=%0d", dw_count, mismatch);
                        if (mismatch > 0 && mismatch <= 4) begin
                            for (i = 0; i < 1024; i = i + 1) begin
                                expected = 32'hC0DE_0000 + i[15:0];
                                if (captured_data[i] !== expected)
                                    $display("  [%0d] got=%h exp=%h", i, captured_data[i], expected);
                            end
                        end
                        fail_count = fail_count + 1;
                    end
                end
            end
        end

        repeat (10) @(posedge clk);

        // ==============================================================
        // TEST 5: Multiple operations (3 reads + 2 writes)
        // ==============================================================
        $display("--- Test 5: Multiple operations ---");
        begin : test5_block
            reg test5_ok;
            integer op;
            integer rd_before, wr_before;
            test5_ok = 1'b1;

            rd_before = reads_completed;
            wr_before = writes_completed;

            // Pre-fill 3 pages for reads
            // page 0 (base=0), page 1 (base=1024), page 2 (base=2048)
            for (i = 0; i < 1024; i = i + 1) begin
                storage[0 + i]    = 32'hD100_0000 + i[15:0];
                storage[1024 + i] = 32'hD200_0000 + i[15:0];
                storage[2048 + i] = 32'hD300_0000 + i[15:0];
            end

            // --- Read 1: addr=0x0000, page=0 ---
            while (cmd_ready !== 1'b1) @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= 8'h00;
            cmd_addr  <= 64'h0000_0000_0000_0000;
            @(posedge clk);
            cmd_valid <= 1'b0;

            // Drain response
            timeout_ctr = 0;
            while (timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
                if (resp_valid === 1'b1 && resp_last === 1'b1)
                    timeout_ctr = 50001;
            end
            if (timeout_ctr == 50000) begin
                $display("[FAIL] Test 5: Read 1 timed out");
                test5_ok = 1'b0;
            end

            repeat (5) @(posedge clk);

            // --- Write 1: addr=0x000A, page=10, base=10240 ---
            while (cmd_ready !== 1'b1) @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= 8'h01;
            cmd_addr  <= 64'h0000_0000_0000_000A;
            @(posedge clk);
            cmd_valid <= 1'b0;

            timeout_ctr = 0;
            while (net_wr_ready !== 1'b1 && timeout_ctr < 5000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
            end
            for (i = 0; i < 1024; i = i + 1) begin
                net_wr_data  <= 32'hE100_0000 + i[15:0];
                net_wr_valid <= 1'b1;
                net_wr_last  <= (i == 1023) ? 1'b1 : 1'b0;
                @(posedge clk);
                while (net_wr_ready !== 1'b1) @(posedge clk);
            end
            net_wr_valid <= 1'b0;
            net_wr_last  <= 1'b0;

            wait_for_ack;
            if (wait_timeout_flag) begin
                $display("[FAIL] Test 5: Write 1 ack timed out");
                test5_ok = 1'b0;
            end

            repeat (5) @(posedge clk);

            // --- Read 2: addr=0x0001, page=1 ---
            while (cmd_ready !== 1'b1) @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= 8'h00;
            cmd_addr  <= 64'h0000_0000_0000_0001;
            @(posedge clk);
            cmd_valid <= 1'b0;

            timeout_ctr = 0;
            while (timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
                if (resp_valid === 1'b1 && resp_last === 1'b1)
                    timeout_ctr = 50001;
            end
            if (timeout_ctr == 50000) begin
                $display("[FAIL] Test 5: Read 2 timed out");
                test5_ok = 1'b0;
            end

            repeat (5) @(posedge clk);

            // --- Write 2: addr=0x000B, page=11, base=11264 ---
            while (cmd_ready !== 1'b1) @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= 8'h01;
            cmd_addr  <= 64'h0000_0000_0000_000B;
            @(posedge clk);
            cmd_valid <= 1'b0;

            timeout_ctr = 0;
            while (net_wr_ready !== 1'b1 && timeout_ctr < 5000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
            end
            for (i = 0; i < 1024; i = i + 1) begin
                net_wr_data  <= 32'hE200_0000 + i[15:0];
                net_wr_valid <= 1'b1;
                net_wr_last  <= (i == 1023) ? 1'b1 : 1'b0;
                @(posedge clk);
                while (net_wr_ready !== 1'b1) @(posedge clk);
            end
            net_wr_valid <= 1'b0;
            net_wr_last  <= 1'b0;

            wait_for_ack;
            if (wait_timeout_flag) begin
                $display("[FAIL] Test 5: Write 2 ack timed out");
                test5_ok = 1'b0;
            end

            repeat (5) @(posedge clk);

            // --- Read 3: addr=0x0002, page=2 ---
            while (cmd_ready !== 1'b1) @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= 8'h00;
            cmd_addr  <= 64'h0000_0000_0000_0002;
            @(posedge clk);
            cmd_valid <= 1'b0;

            timeout_ctr = 0;
            while (timeout_ctr < 50000) begin
                @(posedge clk);
                timeout_ctr = timeout_ctr + 1;
                if (resp_valid === 1'b1 && resp_last === 1'b1)
                    timeout_ctr = 50001;
            end
            if (timeout_ctr == 50000) begin
                $display("[FAIL] Test 5: Read 3 timed out");
                test5_ok = 1'b0;
            end

            repeat (5) @(posedge clk);

            // Check stats
            if (reads_completed !== rd_before + 3) begin
                $display("[FAIL] Test 5: reads_completed=%0d (exp %0d)", reads_completed, rd_before + 3);
                test5_ok = 1'b0;
            end
            if (writes_completed !== wr_before + 2) begin
                $display("[FAIL] Test 5: writes_completed=%0d (exp %0d)", writes_completed, wr_before + 2);
                test5_ok = 1'b0;
            end
            if (errors !== 32'd0) begin
                $display("[FAIL] Test 5: errors=%0d (exp 0)", errors);
                test5_ok = 1'b0;
            end

            if (test5_ok) begin
                $display("[PASS] Test 5: Multiple ops (3R+2W) completed, stats correct (reads=%0d, writes=%0d, errors=%0d)",
                         reads_completed, writes_completed, errors);
                pass_count = pass_count + 1;
            end else begin
                $display("[FAIL] Test 5: Multiple operations had errors");
                fail_count = fail_count + 1;
            end
        end

        // ==============================================================
        // Summary
        // ==============================================================
        repeat (10) @(posedge clk);
        $display("====================================");
        $display("Results: %0d/%0d PASSED, %0d FAILED", pass_count, total_tests, fail_count);
        if (fail_count == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");
        $display("====================================");

        $finish;
    end

    // Global timeout
    initial begin
        #5000000;
        $display("[TIMEOUT] Simulation exceeded 5ms limit");
        $finish;
    end

endmodule
