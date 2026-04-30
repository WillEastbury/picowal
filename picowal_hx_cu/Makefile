# Makefile for PicoWAL HX on Alchitry Cu
# Requires: yosys, nextpnr-ice40, icepack, iceprog

DEVICE = hx8k
PACKAGE = ct256
PCF = picowal_hx.pcf
TOP = picowal_hx_top

VERILOG_SRC = picowal_hx_top.v \
              picoscript_decode.v \
              picoscript_alu.v \
              picoscript_branch.v \
              picoscript_dsp.v \
              sram_controller.v \
              spi_master.v \
              card_mapper.v \
              pipe_engine.v \
              irq_controller.v \
              context_scheduler.v

all: $(TOP).bin

$(TOP).json: $(VERILOG_SRC)
	yosys -p "synth_ice40 -top $(TOP) -json $@" $(VERILOG_SRC)

$(TOP).asc: $(TOP).json $(PCF)
	nextpnr-ice40 --$(DEVICE) --package $(PACKAGE) --pcf $(PCF) --json $< --asc $@

$(TOP).bin: $(TOP).asc
	icepack $< $@

prog: $(TOP).bin
	iceprog -S $<

flash: $(TOP).bin
	iceprog $<

timing: $(TOP).asc
	icetime -d $(DEVICE) -t $<

clean:
	rm -f $(TOP).json $(TOP).asc $(TOP).bin

.PHONY: all prog flash timing clean
