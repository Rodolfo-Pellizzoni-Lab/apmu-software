# EVU smoke test: PC-track milestones on evu_probe() counted by APMU counters 20/21 and logged by the
# sad_profile firmware. Two-pass build like sad_profile: link, extract the addresses of evu_probe, rebuild
# the firmware with them, link again (the program itself never embeds the addresses, so nothing moves).
#
#   make two-pass [fpga=1] [N_CALLS=16]     -> evu_test.riscv
#   make clean
APP := evu_test
XLEN := 64
RISCV_PREFIX ?= riscv64-unknown-elf-

export SW_HOME := $(shell cd .. && pwd)
export HW_HOME ?= $(realpath $(SW_HOME)/../hardware)
include $(shell pwd)/../common.mk

SAD          := ../sad_profile
PMU_FW_DIR   := $(SAD)/pmu_firmware
PMU_TEXT_BIN := $(PMU_FW_DIR)/build/text_section.bin
PMU_DATA_BIN := $(PMU_FW_DIR)/build/data_rodata_bss.bin
PROBE        := evu_probe
N_CALLS      ?= 16
DUMP         := $(APP).dump
ADDRS_H      := auto_func_addrs.h
PYTHON       := python3

EVU_CFLAGS := $(RISCV_FLAGS) -I$(SAD) -DBARE_METAL -DN_CALLS=$(N_CALLS)

define LINK
$(RISCV_GCC) $(EVU_CFLAGS) -T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
	$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
	$(APP).c pmu_text.o pmu_data.o $(SRC) -o $(APP).riscv
endef

# the wrappers .incbin the firmware images from the current directory
define EMBED
cp $(PMU_TEXT_BIN) text_section.bin && $(RISCV_GCC) -c $(SAD)/pmu_text_wrapper.S -o pmu_text.o && \
cp $(PMU_DATA_BIN) data_rodata_bss.bin && $(RISCV_GCC) -c $(SAD)/pmu_data_wrapper.S -o pmu_data.o
endef

.PHONY: two-pass build clean
build: two-pass
	@:

two-pass:
	$(MAKE) -C $(PMU_FW_DIR) clean all generate-bin-files
	$(EMBED)
	$(LINK)
	$(RISCV_PREFIX)objdump -d $(APP).riscv > $(DUMP)
	$(PYTHON) extract_addrs.py $(DUMP) $(PROBE) > $(ADDRS_H)
	_FE=$$(grep 'define FUNC_ENTRY' $(ADDRS_H) | awk '{print $$3}'); \
	_FX=$$(grep 'define FUNC_EXIT'  $(ADDRS_H) | awk '{print $$3}'); \
	echo "[two-pass] $(PROBE): FUNC_ENTRY=$$_FE FUNC_EXIT=$$_FX"; \
	$(MAKE) -C $(PMU_FW_DIR) clean all generate-bin-files FUNC_ENTRY=$$_FE FUNC_EXIT=$$_FX
	$(EMBED)
	$(LINK)
	$(RISCV_PREFIX)objdump -d $(APP).riscv > $(DUMP)
	$(PYTHON) extract_addrs.py $(DUMP) $(PROBE) | diff - $(ADDRS_H) || \
		{ echo "[two-pass] $(PROBE) moved between the two links"; exit 1; }

clean:
	rm -f $(APP).riscv $(APP).dump $(ADDRS_H) *.slm pmu_text.o pmu_data.o text_section.bin data_rodata_bss.bin
	$(MAKE) -C $(PMU_FW_DIR) clean
