APP := fat_test
XLEN:=64

export SW_HOME := $(shell cd .. && pwd)
include $(shell pwd)/../common.mk

# PMU firmware paths (RV32 Ibex core firmware embedded into the CVA6 binary)
PMU_FW_DIR   := pmu_firmware
PMU_BUILD_DIR := $(PMU_FW_DIR)/build
PMU_TEXT_BIN  := $(PMU_BUILD_DIR)/text_section.bin
PMU_DATA_BIN  := $(PMU_BUILD_DIR)/data_rodata_bss.bin

OBJCOPY ?= riscv64-unknown-elf-objcopy

SDVB_DIR    := sdvb
DISP_DIR    := $(SDVB_DIR)/benchmarks/disparity/src/c
MSER_DIR    := $(SDVB_DIR)/benchmarks/mser/src/c
SIFT_DIR    := $(SDVB_DIR)/benchmarks/sift/src/c
STITCH_DIR  := $(SDVB_DIR)/benchmarks/stitch/src/c
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

# MSER benchmark sources
MSER_BENCH_SRC := \
	$(MSER_DIR)/script_mser.c \
	$(MSER_DIR)/mser.c

# SIFT benchmark sources
SIFT_BENCH_SRC := \
	$(SIFT_DIR)/script_sift.c \
	$(SIFT_DIR)/sift.c \
	$(SIFT_DIR)/diffss.c \
	$(SIFT_DIR)/doubleSize.c \
	$(SIFT_DIR)/filterBoundaryPoints.c \
	$(SIFT_DIR)/gaussianss.c \
	$(SIFT_DIR)/halveSize.c \
	$(SIFT_DIR)/imsmooth.c \
	$(SIFT_DIR)/siftlocalmax.c \
	$(SIFT_DIR)/siftrefinemx.c

# SDVB common sources used by disparity and MSER
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

# STITCH benchmark sources
STITCH_BENCH_SRC := \
	$(STITCH_DIR)/script_stitch.c \
	$(STITCH_DIR)/dist2.c \
	$(STITCH_DIR)/extractFeatures.c \
	$(STITCH_DIR)/getANMS.c \
	$(STITCH_DIR)/harris.c \
	$(STITCH_DIR)/matchFeatures.c \
	$(STITCH_DIR)/maxWindow.c \
	$(STITCH_DIR)/supress.c

# Additional common sources needed by SIFT
SIFT_EXTRA_SRC := \
	$(COMMON_DIR)/fSelfCheck.c \
	$(COMMON_DIR)/fDeepCopy.c \
	$(COMMON_DIR)/fHorzcat.c \
	$(COMMON_DIR)/fiDeepCopy.c \
	$(COMMON_DIR)/ifDeepCopy.c \
	$(COMMON_DIR)/iReshape.c \
	$(COMMON_DIR)/isMinus.c \
	$(COMMON_DIR)/isPlus.c \
	$(COMMON_DIR)/iVertcat.c \
	$(COMMON_DIR)/fTranspose.c

# Additional common sources needed by STITCH (beyond DISP_COMMON_SRC)
STITCH_EXTRA_SRC := \
	$(COMMON_DIR)/fSelfCheck.c \
	$(COMMON_DIR)/fDeepCopy.c \
	$(COMMON_DIR)/fiDeepCopy.c \
	$(COMMON_DIR)/fTranspose.c \
	$(COMMON_DIR)/ffConv2.c \
	$(COMMON_DIR)/fDivide.c \
	$(COMMON_DIR)/fTimes.c \
	$(COMMON_DIR)/fMinus.c \
	$(COMMON_DIR)/fPlus.c \
	$(COMMON_DIR)/ffDivide.c \
	$(COMMON_DIR)/fFind3.c \
	$(COMMON_DIR)/fMtimes.c \
	$(COMMON_DIR)/ifMtimes.c \
	$(COMMON_DIR)/fSort.c \
	$(COMMON_DIR)/fSortIndices.c \
	$(COMMON_DIR)/ffVertcat.c

# Union of all extra sources needed by all 4 benchmarks (SIFT_EXTRA ∪ STITCH_EXTRA)
ALL_EXTRA_SRC := \
	$(COMMON_DIR)/fSelfCheck.c \
	$(COMMON_DIR)/fDeepCopy.c \
	$(COMMON_DIR)/fHorzcat.c \
	$(COMMON_DIR)/fiDeepCopy.c \
	$(COMMON_DIR)/ifDeepCopy.c \
	$(COMMON_DIR)/iReshape.c \
	$(COMMON_DIR)/isMinus.c \
	$(COMMON_DIR)/isPlus.c \
	$(COMMON_DIR)/iVertcat.c \
	$(COMMON_DIR)/fTranspose.c \
	$(COMMON_DIR)/ffConv2.c \
	$(COMMON_DIR)/fDivide.c \
	$(COMMON_DIR)/fTimes.c \
	$(COMMON_DIR)/fMinus.c \
	$(COMMON_DIR)/fPlus.c \
	$(COMMON_DIR)/ffDivide.c \
	$(COMMON_DIR)/fFind3.c \
	$(COMMON_DIR)/fMtimes.c \
	$(COMMON_DIR)/ifMtimes.c \
	$(COMMON_DIR)/fSort.c \
	$(COMMON_DIR)/fSortIndices.c \
	$(COMMON_DIR)/ffVertcat.c

# Our bare-metal support
BARE_SRC := fat16.c fat_file.c bare_malloc.c

# Common include flags (pmu_sdvb/ first so our headers shadow SDVB ones)
BENCH_INCLUDES_BASE := \
	-I$(shell pwd) \
	-I$(shell pwd)/$(COMMON_DIR)

# Include: pmu_sdvb/ FIRST so our sdvbs_common.h shadows the SDVB one
DISP_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(DISP_DIR)

MSER_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(MSER_DIR)

SIFT_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(SIFT_DIR)

STITCH_INCLUDES := \
	$(BENCH_INCLUDES_BASE) \
	-I$(shell pwd)/$(STITCH_DIR)

build:
	$(RISCV_GCC) $(RISCV_FLAGS) -T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		$(APP).c fat16.c $(SRC) \
		-o $(APP).riscv

disp:
	$(RISCV_GCC) $(RISCV_FLAGS) $(DISP_INCLUDES) \
		-Driscv -Dsim -DCHECK -DBARE_METAL \
		-T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		disp_main.c $(BARE_SRC) \
		$(DISP_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o disp_test.riscv

# Full parameters (WIN_SZ=8, SHIFT=64) matching expected_C.txt in VGA data dir
disp_full:
	$(RISCV_GCC) $(RISCV_FLAGS) $(DISP_INCLUDES) \
		-Driscv -DCHECK -DBARE_METAL \
		-T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		disp_main.c $(BARE_SRC) \
		$(DISP_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o disp_test.riscv

mser:
	$(RISCV_GCC) $(RISCV_FLAGS) $(MSER_INCLUDES) \
		-Driscv -Dsim -DCHECK -DBARE_METAL \
		-T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		mser_main.c $(BARE_SRC) \
		$(MSER_BENCH_SRC) $(DISP_COMMON_SRC) \
		$(SRC) \
		-o mser_test.riscv

sift:
	$(RISCV_GCC) $(RISCV_FLAGS) $(SIFT_INCLUDES) \
		-Driscv -Dsim -DCHECK -DBARE_METAL \
		-T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		sift_main.c $(BARE_SRC) bare_math.c \
		$(SIFT_BENCH_SRC) $(DISP_COMMON_SRC) $(SIFT_EXTRA_SRC) \
		$(SRC) \
		-o sift_test.riscv

stitch:
	$(RISCV_GCC) $(RISCV_FLAGS) $(STITCH_INCLUDES) \
		-Driscv -Dsim -DCHECK -DBARE_METAL \
		-T $(inc_dir)/test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		stitch_main.c $(BARE_SRC) \
		$(STITCH_BENCH_SRC) $(DISP_COMMON_SRC) $(STITCH_EXTRA_SRC) \
		$(SRC) \
		-o stitch_test.riscv

# ==============================================================================
# PMU Firmware build + embedding (mirrors pmu_mempol pattern)
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

fat_image.o:
	$(RISCV_GCC) -c fat_image_wrapper.S -o $@

# ==============================================================================
# Run all 4 SDVB benchmarks on core 0 under PMU regulation (cores 1-3 interference).
# Firmware is rebuilt automatically before linking.
# ==============================================================================
pmu_bench: pmu_text.o pmu_data.o
	$(RISCV_GCC) $(RISCV_FLAGS) $(BENCH_INCLUDES_BASE) \
		-Driscv -Dsim -DBARE_METAL \
		-T test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c bare_malloc.c bare_math.c \
		pmu_text.o pmu_data.o \
		$(SRC) \
		-o pmu_bench.riscv

# VGA build: full disparity params (WIN_SZ=8, SHIFT=64) by omitting -Dsim
pmu_bench_vga: pmu_text.o pmu_data.o fat_image.o
	$(RISCV_GCC) $(RISCV_FLAGS) $(BENCH_INCLUDES_BASE) \
		-I$(shell pwd)/$(DISP_DIR) \
		-I$(shell pwd)/$(MSER_DIR) \
		-I$(shell pwd)/$(SIFT_DIR) \
		-I$(shell pwd)/$(STITCH_DIR) \
		-Driscv -DBARE_METAL \
		-T test.ld $(RISCV_LINK_OPTS) $(cc-elf-y) \
		$(inc_dir)/crt.S $(inc_dir)/syscalls.c -L $(inc_dir) \
		pmu_bench.c $(BARE_SRC) bare_math.c \
		pmu_text.o pmu_data.o fat_image.o \
		$(DISP_BENCH_SRC) $(MSER_BENCH_SRC) $(SIFT_BENCH_SRC) $(STITCH_BENCH_SRC) \
		$(DISP_COMMON_SRC) $(ALL_EXTRA_SRC) \
		$(SRC) \
		-o pmu_bench.riscv

clean:
	rm -f $(APP).riscv disp_test.riscv mser_test.riscv sift_test.riscv stitch_test.riscv pmu_bench.riscv $(APP).dump *.slm
	$(MAKE) -C $(PMU_FW_DIR) clean
	rm -f pmu_text.o pmu_data.o fat_image.o text_section.bin data_rodata_bss.bin
