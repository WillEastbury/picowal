# PicoWAL FPGA Build — iCE40 HX8K (Alchitry Cu)
# Requires: yosys, nextpnr-ice40, icepack, iceprog

DEVICE  = hx8k
PACKAGE = cb132
PCF     = constraints/alchitry_cu.pcf
TOP     = top

SRCS = src/pll.v \
       src/reset_sync.v \
       src/spi_master.v \
       src/crc.v \
       src/sd_init.v \
       src/sd_read.v \
       src/sd_write.v \
       src/sd_spi.v \
       src/w5500_spi.v \
       src/w5500_ctrl.v \
       src/http_parser.v \
       src/siphash.v \
       src/auth.v \
       src/merge_engine.v \
       src/kv_engine.v \
       src/response_builder.v \
       src/ui_rom.v \
       src/first_boot.v \
       src/status_counters.v \
       src/top.v

BUILD_DIR = build

.PHONY: all synth pnr pack program clean

all: $(BUILD_DIR)/picowal.bin

$(BUILD_DIR)/picowal.json: $(SRCS)
	@mkdir -p $(BUILD_DIR)
	yosys -p "synth_ice40 -top $(TOP) -json $@" $(SRCS)

$(BUILD_DIR)/picowal.asc: $(BUILD_DIR)/picowal.json $(PCF)
	nextpnr-ice40 --$(DEVICE) --package $(PACKAGE) --pcf $(PCF) --json $< --asc $@

$(BUILD_DIR)/picowal.bin: $(BUILD_DIR)/picowal.asc
	icepack $< $@

synth: $(BUILD_DIR)/picowal.json
pnr: $(BUILD_DIR)/picowal.asc
pack: $(BUILD_DIR)/picowal.bin

program: $(BUILD_DIR)/picowal.bin
	iceprog $<

clean:
	rm -rf $(BUILD_DIR)
