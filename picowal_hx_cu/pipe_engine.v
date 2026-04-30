// pipe_engine.v -- Zero-copy SRAM-to-W5100S DMA engine
// Target: iCE40HX8K (Alchitry Cu)
//
// PIPE instruction: reads card data from SRAM and streams it directly
// to the W5100S TX buffer via SPI. No register involvement — data flows
// through without touching the execution pipeline.
//
// Supports plain PIPE and TEMPLATE mode (with marker substitution).

`default_nettype none

module pipe_engine (
    input  wire        clk,
    input  wire        rst_n,

    // Command interface (from executor)
    input  wire        cmd_start,       // begin PIPE transfer
    input  wire [17:0] card_addr,       // SRAM start address of card
    input  wire [8:0]  card_len,        // card length in 16-bit words (max 256)
    input  wire        template_mode,   // 1 = template substitution active
    input  wire [2:0]  ctx_id,          // which context initiated (for suspend)

    output reg         done,            // transfer complete
    output wire        busy,
    output reg  [2:0]  done_ctx,        // context to wake on completion

    // SRAM interface (directly active SRAM read port)
    output reg         sram_read,
    output reg  [17:0] sram_addr,
    input  wire [15:0] sram_rdata,
    input  wire        sram_done,

    // SPI interface (to W5100S via spi_master)
    output reg         spi_start,
    output reg  [7:0]  spi_txdata,
    output reg         spi_burst,
    input  wire        spi_done,

    // Template: field substitution interface
    output reg         tmpl_field_req,  // request field value
    output reg  [3:0]  tmpl_field_id,   // which field
    input  wire [31:0] tmpl_field_val,  // field value (from schema engine)
    input  wire [3:0]  tmpl_field_len,  // field length in bytes
    input  wire        tmpl_field_ready,

    // Template: FOREACH state
    output reg         tmpl_foreach_start,  // hit 0xFD marker
    output reg  [7:0]  tmpl_foreach_pack,   // which pack to iterate
    input  wire        tmpl_foreach_done,   // FOREACH exhausted (EOF)
    input  wire        tmpl_foreach_next    // next card loaded
);

    // ─── State machine ───────────────────────────────────────────────
    localparam IDLE         = 4'd0;
    localparam SRAM_REQ     = 4'd1;     // request word from SRAM
    localparam SRAM_WAIT    = 4'd2;     // wait for SRAM response
    localparam SPI_HI       = 4'd3;     // send high byte via SPI
    localparam SPI_HI_WAIT  = 4'd4;     // wait for SPI done
    localparam SPI_LO       = 4'd5;     // send low byte via SPI
    localparam SPI_LO_WAIT  = 4'd6;     // wait for SPI done
    localparam TMPL_CHECK   = 4'd7;     // check for template markers
    localparam TMPL_FIELD   = 4'd8;     // substituting a field value
    localparam TMPL_FOREACH = 4'd9;     // handling FOREACH marker
    localparam COMPLETE     = 4'd10;

    reg [3:0]  state;
    reg [8:0]  word_cnt;        // words remaining
    reg [17:0] cur_addr;        // current SRAM address
    reg [15:0] cur_word;        // latched SRAM word
    reg [2:0]  pipe_ctx;        // context that started this pipe
    reg [3:0]  field_byte_idx;  // byte index within field substitution

    assign busy = (state != IDLE);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state    <= IDLE;
            done     <= 1'b0;
            sram_read <= 1'b0;
            spi_start <= 1'b0;
            spi_burst <= 1'b0;
            tmpl_field_req     <= 1'b0;
            tmpl_foreach_start <= 1'b0;
        end else begin
            done      <= 1'b0;
            sram_read <= 1'b0;
            spi_start <= 1'b0;
            tmpl_field_req     <= 1'b0;
            tmpl_foreach_start <= 1'b0;

            case (state)
                IDLE: begin
                    if (cmd_start) begin
                        cur_addr  <= card_addr;
                        word_cnt  <= card_len;
                        pipe_ctx  <= ctx_id;
                        state     <= SRAM_REQ;
                    end
                end

                // ─── Fetch next word from SRAM ───────────────────────
                SRAM_REQ: begin
                    if (word_cnt == 9'd0) begin
                        state <= COMPLETE;
                    end else begin
                        sram_read <= 1'b1;
                        sram_addr <= cur_addr;
                        state     <= SRAM_WAIT;
                    end
                end

                SRAM_WAIT: begin
                    if (sram_done) begin
                        cur_word <= sram_rdata;
                        cur_addr <= cur_addr + 18'd1;
                        word_cnt <= word_cnt - 9'd1;

                        if (template_mode)
                            state <= TMPL_CHECK;
                        else
                            state <= SPI_HI;
                    end
                end

                // ─── Template marker check ───────────────────────────
                TMPL_CHECK: begin
                    if (cur_word[15:8] == 8'hFE) begin
                        // Field substitution marker
                        tmpl_field_id  <= cur_word[3:0];
                        tmpl_field_req <= 1'b1;
                        state          <= TMPL_FIELD;
                        field_byte_idx <= 4'd0;
                    end else if (cur_word[15:8] == 8'hFD) begin
                        // FOREACH begin marker
                        tmpl_foreach_pack  <= cur_word[7:0];
                        tmpl_foreach_start <= 1'b1;
                        state              <= TMPL_FOREACH;
                    end else if (cur_word[15:8] == 8'hFC) begin
                        // FOREACH end marker — handled by external loop controller
                        state <= SRAM_REQ;
                    end else begin
                        // Normal data — send to SPI
                        state <= SPI_HI;
                    end
                end

                // ─── Template field substitution ─────────────────────
                TMPL_FIELD: begin
                    if (tmpl_field_ready) begin
                        // Send field bytes via SPI
                        spi_txdata <= tmpl_field_val[{field_byte_idx, 3'b0} +: 8];
                        spi_start  <= 1'b1;
                        spi_burst  <= (field_byte_idx < tmpl_field_len - 4'd1);
                        state      <= SPI_HI_WAIT;  // reuse wait state

                        if (field_byte_idx >= tmpl_field_len - 4'd1)
                            state <= SRAM_REQ;      // field done, next word
                        else
                            field_byte_idx <= field_byte_idx + 4'd1;
                    end
                end

                // ─── Template FOREACH handling ────────────────────────
                TMPL_FOREACH: begin
                    if (tmpl_foreach_done) begin
                        // Pack exhausted, continue after FOREACH block
                        state <= SRAM_REQ;
                    end else if (tmpl_foreach_next) begin
                        // Next card loaded, continue template body
                        state <= SRAM_REQ;
                    end
                end

                // ─── Send high byte via SPI ──────────────────────────
                SPI_HI: begin
                    spi_txdata <= cur_word[15:8];
                    spi_start  <= 1'b1;
                    spi_burst  <= 1'b1;     // keep CS for low byte
                    state      <= SPI_HI_WAIT;
                end

                SPI_HI_WAIT: begin
                    if (spi_done) begin
                        state <= SPI_LO;
                    end
                end

                // ─── Send low byte via SPI ───────────────────────────
                SPI_LO: begin
                    spi_txdata <= cur_word[7:0];
                    spi_start  <= 1'b1;
                    spi_burst  <= (word_cnt != 9'd0);  // keep CS if more words
                    state      <= SPI_LO_WAIT;
                end

                SPI_LO_WAIT: begin
                    if (spi_done) begin
                        state <= SRAM_REQ;  // next word
                    end
                end

                // ─── Transfer complete ───────────────────────────────
                COMPLETE: begin
                    done     <= 1'b1;
                    done_ctx <= pipe_ctx;
                    state    <= IDLE;
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
