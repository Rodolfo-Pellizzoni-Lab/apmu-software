APP := pmu_bench
XLEN:=64
RISCV_PREFIX ?= riscv64-unknown-elf-

export SW_HOME := $(shell cd .. && pwd)
export HW_HOME ?= $(realpath $(SW_HOME)/../hardware)
include $(shell pwd)/../common.mk

# PMU firmware paths (RV32 Ibex core firmware embedded into the CVA6 binary)
PMU_FW_DIR   := pmu_firmware
PMU_BUILD_DIR := $(PMU_FW_DIR)/build
PMU_TEXT_BIN  := $(PMU_BUILD_DIR)/text_section.bin
PMU_DATA_BIN  := $(PMU_BUILD_DIR)/data_rodata_bss.bin

OBJCOPY ?= $(RISCV_PREFIX)objcopy

SDVB_DIR    := sdvb
DISP_DIR    := $(SDVB_DIR)/benchmarks/disparity/src/c
COMMON_DIR  := $(SDVB_DIR)/common/c

# Disparity benchmark sources
DISP_BENCH_SRC := \
	$(DISP_DIR)/script_disparity.c \
	$(DISP_DIR)/computeSAD.c \
	$(DISP_DIR)/correlateSAD_2D.c \
	$(DISP_DIR)/finalSAD.c \
	$(DISP_DIR)/findDisparity.c \
	$(DISP_DIR)/getDisparity.c \
	$(DISP_DIR)/integralImage2D2D.c \
	$(DISP_DIR)/padarray2.c \
	$(DISP_DIR)/padarray4.c

# SDVB common sources
DISP_COMMON_SRC := \
	$(COMMON_DIR)/readImage.c \
	$(COMMON_DIR)/iMallocHandle.c \
	$(COMMON_DIR)/fMallocHandle.c \
	$(COMMON_DIR)/uiMallocHandle.c \
	$(COMMON_DIR)/iFreeHandle.c \
	$(COMMON_DIR)/fFreeHandle.c \
	$(COMMON_DIR)/uiFreeHandle.c \
	$(COMMON_DIR)/iSetArray.c \
	$(COMMON_DIR)/fSetArray.c \
	$(COMMON_DIR)/selfCheck.c \
	$(COMMON_DIR)/photonStartTiming.c \
	$(COMMON_DIR)/photonEndTiming.c \
	$(COMMON_DIR)/photonReportTiming.c \
	$(COMMON_DIR)/photonPrintTiming.c

# Our bare-metal support
BARE_SRC := fat16.c fat_file.c bare_malloc.c

# Common include flags
BENCH_INCLUDES_BASE := \
	-I$(shell pwd) \
	-I$(shell pwd)/$(COMMON_DIR)

DISP_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(DISP_DIR)

# ==============================================================================
# PMU Firmware build + embedding
# ==============================================================================

.PHONY: pmu_firmware
pmu_firmware:
	$(MAKE) -C $(PMU_FW_DIR) all generate-bin-files

pmu_text.o: pmu_firmware
	cp $(PMU_TEXT_BIN) text_section.bin
	$(RISCV_GCC) -c pmu_text_wrapper.S -o $@

pmu_data.o: pmu_firmware
	cp $(PMU_DATA_BIN) data_rodata_bss.bin
	$(RISCV_GCC) -c pmu_data_wrapper.S -o $@

# ==============================================================================
# computeSAD profiler: disparity on core 0, no interference
# ==============================================================================
fat_image.o:
	$(RISCV_GCC) -c fat_image_wrapper.S -o $@

# Override common.mk's build recipe so "all: build" works correctly.
build: pmu_bench
	@:

pmu_bench: pmu_text.o pmu_data.o fat_image.o
	$(RISCV_GCC) $(RISCV_FLAGS) $(DISP_INCLUDES) \
		-Driscv -Dsim -DBARE_METAL \
		-T test_sim.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c $(BARE_SRC) \
		pmu_text.o pmu_data.o fat_image.o \
		$(DISP_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o pmu_bench.riscv

# ==============================================================================
# all: transparent two-pass build + SLM copy to hardware/
# ==============================================================================
DUMP     := pmu_bench.dump
ADDRS_H  := auto_func_addrs.h
PYTHON   := python3

.PHONY: all
all: two-pass
	$(SW_HOME)/elf_to_slm.py --binary=pmu_bench.riscv --vectors=hyperram0.slm
	cp hyperram*.slm $(HW_HOME)/
	cp pmu_bench.riscv $(HW_HOME)/

$(DUMP): pmu_bench.riscv
	$(RISCV_PREFIX)objdump -d $< > $@

$(ADDRS_H): $(DUMP)
	$(PYTHON) extract_addrs.py $(DUMP) > $(ADDRS_H)

gen-addrs: $(ADDRS_H)

two-pass:
	$(MAKE) pmu_bench
	$(RISCV_PREFIX)objdump -d pmu_bench.riscv > $(DUMP)
	$(PYTHON) extract_addrs.py $(DUMP) > $(ADDRS_H)
	_FE=$$(grep 'define FUNC_ENTRY' $(ADDRS_H) | awk '{print $$3}'); \
	_FX=$$(grep 'define FUNC_EXIT'  $(ADDRS_H) | awk '{print $$3}'); \
	echo "[two-pass] FUNC_ENTRY=$$_FE FUNC_EXIT=$$_FX"; \
	$(MAKE) -C $(PMU_FW_DIR) clean all generate-bin-files \
		FUNC_ENTRY=$$_FE FUNC_EXIT=$$_FX && \
	cp $(PMU_FW_DIR)/build/text_section.bin text_section.bin && \
	$(RISCV_GCC) -c pmu_text_wrapper.S -o pmu_text.o && \
	cp $(PMU_FW_DIR)/build/data_rodata_bss.bin data_rodata_bss.bin && \
	$(RISCV_GCC) -c pmu_data_wrapper.S -o pmu_data.o && \
	$(RISCV_GCC) $(RISCV_FLAGS) $(DISP_INCLUDES) \
		-Driscv -Dsim -DBARE_METAL \
		-T test_sim.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c $(BARE_SRC) \
		pmu_text.o pmu_data.o fat_image.o \
		$(DISP_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o pmu_bench.riscv

clean:
	rm -f $(APP).riscv disp_test.riscv pmu_bench.riscv $(DUMP) $(ADDRS_H) *.slm
	$(MAKE) -C $(PMU_FW_DIR) clean
	rm -f pmu_text.o pmu_data.o fat_image.o text_section.bin data_rodata_bss.bin
