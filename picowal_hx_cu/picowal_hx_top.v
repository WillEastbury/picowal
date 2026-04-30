// picowal_hx_top.v -- PicoWAL on Alchitry Cu (iCE40HX8K)
// Fully integrated top module: all submodules wired together.
// Synthesise with: yosys -p "synth_ice40 -top picowal_hx_top -json out.json"

`default_nettype none

module picowal_hx_top (
    input  wire        clk_100mhz,    // Cu onboard 100MHz oscillator

    // SRAM (Header A + B)
    output wire [17:0] sram_addr,
    inout  wire [15:0] sram_data,
    output wire        sram_ce_n,
    output wire        sram_oe_n,
    output wire        sram_we_n,
    output wire        sram_lb_n,
    output wire        sram_ub_n,

    // W5100S SPI (Header C)
    output wire        w5100_mosi,
    input  wire        w5100_miso,
    output wire        w5100_sck,
    output wire        w5100_cs_n,
    input  wire        w5100_int_n,
    output wire        w5100_rst_n,

    // SD Card SPI (Header C)
    output wire        sd_mosi,
    input  wire        sd_miso,
    output wire        sd_sck,
    output wire        sd_cs_n,
    input  wire        sd_detect,

    // UART debug (Header D)
    output wire        uart_tx,
    input  wire        uart_rx,

    // Onboard LEDs (active high on Cu)
    output wire [7:0]  leds
);

    // ═══════════════════════════════════════════════════════════════════
    // Clock generation (100MHz → 48MHz via PLL)
    // ═══════════════════════════════════════════════════════════════════
    wire clk;
    wire pll_locked;

    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0100),         // ref div = 5 → 20MHz
        .DIVF(7'b0010011),      // fb div = 20 → 400MHz VCO
        .DIVQ(3'b011),          // out div = 8 → 50MHz (close to 48)
        .FILTER_RANGE(3'b010)
    ) pll_inst (
        .REFERENCECLK(clk_100mhz),
        .PLLOUTCORE(clk),
        .LOCK(pll_locked),
        .RESETB(1'b1),
        .BYPASS(1'b0)
    );

    wire rst_n = pll_locked;

    // ═══════════════════════════════════════════════════════════════════
    // Register file (8 contexts × 16 registers × 32 bits)
    // Dual-port BRAM: read port A (rs1), read port B (rs2), write port
    // ═══════════════════════════════════════════════════════════════════
    reg  [31:0] regfile [0:127];    // 8 ctx × 16 regs = 128 entries
    initial begin : rf_init
        integer i;
        for (i = 0; i < 128; i = i + 1) regfile[i] = 32'd0;
    end
    wire [6:0]  rf_raddr1 = {ctx_id, decode_rs1};
    wire [6:0]  rf_raddr2 = {ctx_id, decode_rs2};
    wire [6:0]  rf_waddr  = {ctx_id, decode_rd};
    reg  [31:0] rf_rdata1, rf_rdata2;
    wire [31:0] rf_wdata;
    wire        rf_we;

    always @(posedge clk) begin
        rf_rdata1 <= regfile[rf_raddr1];
        rf_rdata2 <= regfile[rf_raddr2];
        if (rf_we)
            regfile[rf_waddr] <= rf_wdata;
    end

    // ═══════════════════════════════════════════════════════════════════
    // Instruction memory (card buffer: 128 instructions × 32-bit)
    // Loaded from SRAM when PC card changes
    // ═══════════════════════════════════════════════════════════════════
    reg  [31:0] instr_mem [0:127];
    wire [6:0]  instr_addr;
    wire [31:0] instruction = instr_mem[instr_addr];
    wire        instr_valid;

    // ═══════════════════════════════════════════════════════════════════
    // Context scheduler
    // ═══════════════════════════════════════════════════════════════════
    wire [2:0]  ctx_id;
    wire        ctx_valid;
    wire        ctx_done_cycle;
    wire        cmd_wait, cmd_raise, cmd_fork, cmd_join;
    wire [3:0]  wait_mask, raise_channel;
    wire [2:0]  raise_target, fork_count;
    wire        http_new_conn;
    wire [2:0]  http_ctx_id;

    context_scheduler sched_inst (
        .clk            (clk),
        .rst_n          (rst_n),
        .active_ctx     (ctx_id),
        .ctx_valid      (ctx_valid),
        .ctx_done_cycle (ctx_done_cycle),
        .cmd_wait       (cmd_wait),
        .wait_mask      (wait_mask),
        .cmd_raise      (cmd_raise),
        .raise_channel  (raise_channel),
        .raise_target   (raise_target),
        .cmd_fork       (cmd_fork),
        .fork_count     (fork_count),
        .cmd_join       (cmd_join),
        .http_new_conn  (http_new_conn),
        .http_ctx_id    (http_ctx_id),
        .ctx_state_0    (),
        .ctx_state_1    (),
        .ctx_state_2    (),
        .ctx_state_3    (),
        .ctx_state_4    (),
        .ctx_state_5    (),
        .ctx_state_6    (),
        .ctx_state_7    ()
    );

    // ═══════════════════════════════════════════════════════════════════
    // Instruction decoder
    // ═══════════════════════════════════════════════════════════════════
    wire [3:0]  decode_opcode, decode_rd, decode_rs1, decode_rs2;
    wire [15:0] decode_imm16;
    wire        is_noop, is_load, is_save, is_pipe;
    wire        is_add, is_sub, is_mul, is_div, is_inc;
    wire        is_jump, is_branch, is_call, is_return;
    wire        is_wait, is_raise, is_dsp;
    wire        is_http_ctrl, is_field, is_foreach, is_template;
    wire        is_hw_for, is_switch, is_fork, is_join, is_card_jump;
    wire        sel_alu, sel_mul, sel_div, sel_mem, sel_pipe;
    wire        sel_flow, sel_sched, sel_dsp, sel_http;
    wire        decode_rf_we;
    wire [3:0]  dsp_subop;

    picoscript_decode decode_inst (
        .instruction    (instruction),
        .valid          (instr_valid),
        .opcode         (decode_opcode),
        .rd             (decode_rd),
        .rs1            (decode_rs1),
        .rs2            (decode_rs2),
        .imm16          (decode_imm16),
        .is_noop        (is_noop),
        .is_load        (is_load),
        .is_save        (is_save),
        .is_pipe        (is_pipe),
        .is_add         (is_add),
        .is_sub         (is_sub),
        .is_mul         (is_mul),
        .is_div         (is_div),
        .is_inc         (is_inc),
        .is_jump        (is_jump),
        .is_branch      (is_branch),
        .is_call        (is_call),
        .is_return      (is_return),
        .is_wait        (is_wait),
        .is_raise       (is_raise),
        .is_dsp         (is_dsp),
        .is_http_ctrl   (is_http_ctrl),
        .is_field       (is_field),
        .is_foreach     (is_foreach),
        .is_template    (is_template),
        .is_hw_for      (is_hw_for),
        .is_switch      (is_switch),
        .is_fork        (is_fork),
        .is_join        (is_join),
        .is_card_jump   (is_card_jump),
        .sel_alu        (sel_alu),
        .sel_mul        (sel_mul),
        .sel_div        (sel_div),
        .sel_mem        (sel_mem),
        .sel_pipe       (sel_pipe),
        .sel_flow       (sel_flow),
        .sel_sched      (sel_sched),
        .sel_dsp        (sel_dsp),
        .sel_http       (sel_http),
        .br_eq          (),
        .br_ne          (),
        .br_lt          (),
        .br_gt          (),
        .br_le          (),
        .br_ge          (),
        .br_z           (),
        .br_nz          (),
        .br_eof         (),
        .br_err         (),
        .dsp_subop      (dsp_subop),
        .rf_we          (decode_rf_we),
        .rf_waddr       (),
        .rf_raddr1      (),
        .rf_raddr2      ()
    );

    // ═══════════════════════════════════════════════════════════════════
    // ALU
    // ═══════════════════════════════════════════════════════════════════
    wire [31:0] alu_result;
    wire        alu_done, alu_busy;
    wire        flag_z, flag_n, flag_c;

    // B operand: register value or zero-extended immediate
    wire [31:0] alu_b = (decode_rs2[0]) ? rf_rdata2 : {16'd0, decode_imm16};

    picoscript_alu alu_inst (
        .clk        (clk),
        .rst_n      (rst_n),
        .op_add     (is_add),
        .op_sub     (is_sub),
        .op_inc     (is_inc),
        .op_mul     (is_mul),
        .op_div     (is_div),
        .a          (rf_rdata1),
        .b          (alu_b),
        .start      (alu_start_pulse),
        .result     (alu_result),
        .done       (alu_done),
        .flag_z     (flag_z),
        .flag_n     (flag_n),
        .flag_c     (flag_c),
        .busy       (alu_busy)
    );

    // ═══════════════════════════════════════════════════════════════════
    // Branch unit + program counter
    // ═══════════════════════════════════════════════════════════════════
    wire [15:0] pc_card;
    wire [6:0]  pc_ip;
    wire        pc_update, need_card_load;
    wire        stack_overflow, stack_underflow;
    wire        for_done;

    // FOR counter (stored in register R14 by convention)
    wire [31:0] for_counter = rf_rdata1;
    wire [31:0] for_limit   = rf_rdata2;

    picoscript_branch branch_inst (
        .clk            (clk),
        .rst_n          (rst_n),
        .ctx_id         (ctx_id),
        .advance        (ctx_done_cycle),
        .is_jump        (is_jump),
        .is_branch      (is_branch),
        .is_call        (is_call),
        .is_return      (is_return),
        .is_hw_for      (is_hw_for),
        .is_switch      (is_switch),
        .is_card_jump   (is_card_jump),
        .imm16          (decode_imm16),
        .rs1            (decode_rs1),
        .rs2            (decode_rs2),
        .flag_z         (flag_z),
        .flag_n         (flag_n),
        .flag_c         (flag_c),
        .flag_eof       (1'b0),         // TODO: from FOREACH engine
        .flag_err       (1'b0),         // TODO: error accumulator
        .reg_val        (rf_rdata1),
        .for_counter    (for_counter),
        .for_limit      (for_limit),
        .for_done       (for_done),
        .pc_card        (pc_card),
        .pc_ip          (pc_ip),
        .pc_update      (pc_update),
        .need_card_load (need_card_load),
        .stack_overflow (stack_overflow),
        .stack_underflow(stack_underflow)
    );

    assign instr_addr = pc_ip;

    // ═══════════════════════════════════════════════════════════════════
    // SRAM controller
    // ═══════════════════════════════════════════════════════════════════
    wire        sram_cmd_read, sram_cmd_write;
    wire [17:0] sram_cmd_addr;
    wire [15:0] sram_cmd_wdata;
    wire        sram_burst;
    wire [15:0] sram_rdata;
    wire        sram_done, sram_busy;

    sram_controller sram_inst (
        .clk        (clk),
        .rst_n      (rst_n),
        .cmd_read   (sram_cmd_read),
        .cmd_write  (sram_cmd_write),
        .cmd_addr   (sram_cmd_addr),
        .cmd_wdata  (sram_cmd_wdata),
        .cmd_burst  (sram_burst),
        .rdata      (sram_rdata),
        .done       (sram_done),
        .busy       (sram_busy),
        .sram_addr  (sram_addr),
        .sram_data  (sram_data),
        .sram_ce_n  (sram_ce_n),
        .sram_oe_n  (sram_oe_n),
        .sram_we_n  (sram_we_n),
        .sram_lb_n  (sram_lb_n),
        .sram_ub_n  (sram_ub_n)
    );

    // ═══════════════════════════════════════════════════════════════════
    // SPI masters (W5100S + SD card — separate instances)
    // ═══════════════════════════════════════════════════════════════════
    wire        w5100_spi_start, w5100_spi_done, w5100_spi_busy;
    wire [7:0]  w5100_spi_txdata, w5100_spi_rxdata;
    wire        w5100_spi_burst;

    spi_master w5100_spi_inst (
        .clk        (clk),
        .rst_n      (rst_n),
        .cmd_start  (w5100_spi_start),
        .cmd_txdata (w5100_spi_txdata),
        .cmd_cs_sel (1'b0),             // always W5100S
        .cmd_burst  (w5100_spi_burst),
        .cmd_clkdiv (3'd1),             // 48MHz / 4 = 12MHz SPI
        .rx_data    (w5100_spi_rxdata),
        .done       (w5100_spi_done),
        .busy       (w5100_spi_busy),
        .spi_sck    (w5100_sck),
        .spi_mosi   (w5100_mosi),
        .spi_miso   (w5100_miso),
        .cs_w5100_n (w5100_cs_n),
        .cs_sd_n    ()                  // unused on this instance
    );

    wire        sd_spi_start, sd_spi_done, sd_spi_busy;
    wire [7:0]  sd_spi_txdata, sd_spi_rxdata;
    wire        sd_spi_burst;

    spi_master sd_spi_inst (
        .clk        (clk),
        .rst_n      (rst_n),
        .cmd_start  (sd_spi_start),
        .cmd_txdata (sd_spi_txdata),
        .cmd_cs_sel (1'b1),             // always SD
        .cmd_burst  (sd_spi_burst),
        .cmd_clkdiv (3'd0),             // 48MHz / 2 = 24MHz SPI
        .rx_data    (sd_spi_rxdata),
        .done       (sd_spi_done),
        .busy       (sd_spi_busy),
        .spi_sck    (sd_sck),
        .spi_mosi   (sd_mosi),
        .spi_miso   (sd_miso),
        .cs_w5100_n (),                 // unused on this instance
        .cs_sd_n    (sd_cs_n)
    );

    // W5100S reset: hold low for first few cycles after PLL lock
    // In synthesis: 4_800_000 (~100ms). In sim: much shorter.
    reg [22:0] rst_counter;
    reg        w5100_rst_done;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rst_counter   <= 23'd0;
            w5100_rst_done <= 1'b0;
        end else if (!w5100_rst_done) begin
            rst_counter <= rst_counter + 23'd1;
            if (rst_counter[7])  // 128 cycles in sim; replace with [22] for synth
                w5100_rst_done <= 1'b1;
        end
    end
    assign w5100_rst_n = w5100_rst_done;

    // ═══════════════════════════════════════════════════════════════════
    // PIPE engine (SRAM → W5100S DMA)
    // ═══════════════════════════════════════════════════════════════════
    wire        pipe_start, pipe_done, pipe_busy;
    wire [17:0] pipe_sram_addr;
    wire        pipe_sram_read;
    wire        pipe_spi_start;
    wire [7:0]  pipe_spi_txdata;
    wire        pipe_spi_burst;
    wire [2:0]  pipe_done_ctx;

    pipe_engine pipe_inst (
        .clk            (clk),
        .rst_n          (rst_n),
        .cmd_start      (pipe_start),
        .card_addr      (sram_cmd_addr),
        .card_len       (9'd256),           // full card = 512 bytes = 256 words
        .template_mode  (is_template),
        .ctx_id         (ctx_id),
        .done           (pipe_done),
        .busy           (pipe_busy),
        .done_ctx       (pipe_done_ctx),
        .sram_read      (pipe_sram_read),
        .sram_addr      (pipe_sram_addr),
        .sram_rdata     (sram_rdata),
        .sram_done      (sram_done),
        .spi_start      (pipe_spi_start),
        .spi_txdata     (pipe_spi_txdata),
        .spi_burst      (pipe_spi_burst),
        .spi_done       (w5100_spi_done),
        .tmpl_field_req (),
        .tmpl_field_id  (),
        .tmpl_field_val (32'd0),
        .tmpl_field_len (4'd0),
        .tmpl_field_ready(1'b0),
        .tmpl_foreach_start(),
        .tmpl_foreach_pack (),
        .tmpl_foreach_done (1'b0),
        .tmpl_foreach_next (1'b0)
    );

    // ═══════════════════════════════════════════════════════════════════
    // HTTP parser (byte stream from W5100S → parsed request)
    // ═══════════════════════════════════════════════════════════════════
    wire [1:0]  req_method;
    wire [3:0]  req_tenant;
    wire [7:0]  req_pack;
    wire [15:0] req_card;
    wire        req_is_list, req_valid;
    wire [1:0]  req_socket;
    wire        irq_request_ready;
    wire [2:0]  irq_target_ctx;

    http_parser http_inst (
        .clk            (clk),
        .rst_n          (rst_n),
        .rx_byte        (w5100_spi_rxdata),
        .rx_valid       (w5100_spi_done),   // each SPI byte = 1 HTTP byte
        .socket_id      (2'd0),             // TODO: socket mux
        .req_method     (req_method),
        .req_tenant     (req_tenant),
        .req_pack       (req_pack),
        .req_card       (req_card),
        .req_is_list    (req_is_list),
        .req_valid      (req_valid),
        .req_socket     (req_socket),
        .body_length    (),
        .body_start     (),
        .irq_request_ready(irq_request_ready),
        .irq_target_ctx (irq_target_ctx)
    );

    // Connect HTTP parser → scheduler
    assign http_new_conn = irq_request_ready;
    assign http_ctx_id   = irq_target_ctx;

    // ═══════════════════════════════════════════════════════════════════
    // Execution control — 3-stage FSM: FETCH → READ → EXECUTE
    // Stage 0 (FETCH): PC addresses instr_mem, decoder outputs valid combinatorially
    // Stage 1 (READ): Register file reads latch (1-cycle BRAM latency)
    // Stage 2 (EXECUTE): ALU/branch/etc operate on latched register data
    // ═══════════════════════════════════════════════════════════════════

    reg  loading_card;
    initial loading_card = 1'b0;

    localparam EX_FETCH   = 2'd0;
    localparam EX_READ    = 2'd1;
    localparam EX_EXECUTE = 2'd2;
    localparam EX_RETIRE  = 2'd3;

    reg [1:0] exec_state;
    initial exec_state = EX_FETCH;

    // instr_valid gates the decoder — high during READ and EXECUTE
    assign instr_valid = ctx_valid & ~loading_card & ~alu_busy & ~pipe_busy
                         & (exec_state == EX_READ || exec_state == EX_EXECUTE);

    // ALU start only fires once at the beginning of EXECUTE stage
    reg alu_started;
    initial alu_started = 1'b0;
    wire alu_start_pulse = (sel_alu | sel_mul | sel_div) & (exec_state == EX_EXECUTE) & ~alu_started;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            alu_started <= 1'b0;
        else if (exec_state == EX_EXECUTE && (sel_alu | sel_mul | sel_div))
            alu_started <= 1'b1;
        else if (exec_state != EX_EXECUTE)
            alu_started <= 1'b0;
    end

    // Cycle complete: instruction finished this cycle
    wire exec_complete = (exec_state == EX_EXECUTE) & (
        ((sel_alu | sel_mul | sel_div) & alu_done) |
        sel_flow |
        sel_sched |
        sel_http |
        (is_noop & ~is_http_ctrl) |
        (sel_mem & sram_done) |
        sel_pipe
    );

    assign ctx_done_cycle = exec_complete;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            exec_state <= EX_FETCH;
        end else begin
            case (exec_state)
                EX_FETCH: begin
                    // Wait for valid context
                    if (ctx_valid & ~loading_card)
                        exec_state <= EX_READ;
                end
                EX_READ: begin
                    // Register file outputs will be valid next cycle
                    exec_state <= EX_EXECUTE;
                end
                EX_EXECUTE: begin
                    if (exec_complete)
                        exec_state <= EX_RETIRE;
                    // Multi-cycle ops (MUL/DIV/PIPE): stay here until done
                end
                EX_RETIRE: begin
                    // Wait for PC update from branch unit, then fetch next
                    if (pc_update)
                        exec_state <= EX_FETCH;
                end
            endcase
        end
    end

    // WAIT/RAISE/FORK/JOIN signals to scheduler (only during EXECUTE)
    wire in_execute = (exec_state == EX_EXECUTE);
    assign cmd_wait     = is_wait & ~is_join & in_execute;
    assign cmd_raise    = is_raise & ~is_fork & in_execute;
    assign cmd_fork     = is_fork & in_execute;
    assign cmd_join     = is_join & in_execute;
    assign wait_mask    = decode_imm16[3:0];
    assign raise_channel = decode_imm16[3:0];
    assign raise_target = decode_rs1[2:0];
    assign fork_count   = decode_imm16[2:0];

    // PIPE start signal (only during EXECUTE)
    assign pipe_start = is_pipe & in_execute & ~alu_started;

    // SRAM arbitration (PIPE gets priority when active)
    assign sram_cmd_read  = pipe_busy ? pipe_sram_read : (is_load & in_execute & ~alu_started);
    assign sram_cmd_write = is_save & in_execute & ~pipe_busy & ~alu_started;
    assign sram_cmd_addr  = pipe_busy ? pipe_sram_addr : {2'b00, decode_imm16};
    assign sram_cmd_wdata = rf_rdata1[15:0];
    assign sram_burst     = pipe_busy;

    // W5100S SPI arbitration (PIPE gets priority)
    assign w5100_spi_start  = pipe_busy ? pipe_spi_start  : 1'b0;
    assign w5100_spi_txdata = pipe_busy ? pipe_spi_txdata : 8'd0;
    assign w5100_spi_burst  = pipe_busy ? pipe_spi_burst  : 1'b0;

    // Register file write-back
    assign rf_we    = alu_done | (is_load & sram_done & ~pipe_busy);
    assign rf_wdata = alu_done ? alu_result : {16'd0, sram_rdata};

    // ═══════════════════════════════════════════════════════════════════
    // LED status display
    // ═══════════════════════════════════════════════════════════════════
    assign leds = {
        pll_locked,         // LED7: PLL locked
        ctx_valid,          // LED6: executing
        pipe_busy,          // LED5: PIPE active
        sram_busy,          // LED4: SRAM access
        w5100_spi_busy,     // LED3: W5100S SPI active
        sd_spi_busy,        // LED2: SD SPI active
        stack_overflow,     // LED1: error (stack overflow)
        stack_underflow     // LED0: error (stack underflow)
    };

    // ═══════════════════════════════════════════════════════════════════
    // UART debug (stub — TODO: implement peek/poke/step/run monitor)
    // ═══════════════════════════════════════════════════════════════════
    assign uart_tx = 1'b1;  // idle high (no transmission)

endmodule

