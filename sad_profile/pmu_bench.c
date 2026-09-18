/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 University of Waterloo
 */

/*
 * PMU computeSAD Single-Function Profiler — Solo vs Interference
 *
 * Runs disparity benchmark twice:
 *   Pass 1: No interference (cores 1-3 idle)
 *   Pass 2: Write-memory interference from cores 1-3
 *
 * Each pass uses a fresh PMU firmware instance with 1 PC milestone slot
 * toggling between computeSAD entry/exit. Results printed side by side.
 */

#include "encoding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "pmu_test_func.c"
#include "fat16.h"
#include "fat_file.h"
#include "sdvbs_common.h"
#include "sdvb/benchmarks/disparity/src/c/disparity_top.h"
#include "sdvb/benchmarks/disparity/src/c/disparity.h"   /* exposes padarray4 symbol */

/* Use the linker's address of padarray4 instead of a hardcoded literal.
   This avoids a build oscillation where the literal influenced immediate-
   field encoding sizes and shifted the function by 4 bytes each rebuild. */
#define DYNAMIC_PROFILE_FUNC_ENTRY  ((uint32_t)(uintptr_t)&padarray4)
/* Offset to first ret inside padarray4 — stable across builds because the
   function body doesn't reference these literals. Computed from objdump:
   first ret is 0x114 bytes past the function entry. */
#define DYNAMIC_PROFILE_FUNC_EXIT   (DYNAMIC_PROFILE_FUNC_ENTRY + 0x114)
#include "iopmp.h"

// PLIC interrupt configuration for PMU
#define PMU_IRQ_BASE    156
#define PMU_IRQ_CORE(n) (PMU_IRQ_BASE + (n))
#define PLIC_CONTEXT_CORE(n) ((n) * 2)

#define NUM_CORES 4

// Cacheable memory base addresses per core
#define CORE0_DATA_BASE   0x81000000
#define CORE1_DATA_BASE   0x81100000
#define CORE2_DATA_BASE   0x81200000
#define CORE3_DATA_BASE   0x81300000
#define CACHE_LINE_SIZE   64

// DSPM communication addresses (must match firmware)
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)
#define DSPM_CMD_ADDR           (DSPM_BASE_ADDR + 0x80)
#define DSPM_CORES_READY_ADDR   (DSPM_BASE_ADDR + 0x100)

// Per-call milestone log from firmware
#define DSPM_MILESTONE_LOG_ADDR (DSPM_BASE_ADDR + 0x2000)
#define NUM_METRICS             9
#define MILESTONE_RECORD_WORDS  9
#define MAX_LOG_RECORDS         1000

// Status and command codes (must match firmware)
#define STATUS_STARTED          0xBB000001
#define STATUS_SENDING_IRQS     0xBB000010
#define STATUS_ALL_IRQS_SENT    0xBB000099
#define CMD_TRIGGER_INTERRUPT   0x000000FF

// External symbols for PMU firmware
extern unsigned char _binary_text_section_bin_start[];
extern unsigned char _binary_text_section_bin_end[];
extern unsigned char _binary_data_rodata_bss_bin_start[];
extern unsigned char _binary_data_rodata_bss_bin_end[];

// Per-core state set by interrupt handler
volatile uint32_t g_paused[NUM_CORES] = {0xDEAD0001, 0xDEAD0001, 0xDEAD0001, 0xDEAD0001};
volatile uint32_t counter_val[NUM_CORES] = {0xDEAD0001, 0xDEAD0001, 0xDEAD0001, 0xDEAD0001};
volatile uint32_t g_interrupt_count[NUM_CORES] = {0xDEAD0002, 0xDEAD0002, 0xDEAD0002, 0xDEAD0002};
volatile uint32_t g_work_iterations[NUM_CORES] = {0xDEAD0003, 0xDEAD0003, 0xDEAD0003, 0xDEAD0003};
volatile uint64_t g_saved_mepc[NUM_CORES] = {0xDEAD0007, 0xDEAD0007, 0xDEAD0007, 0xDEAD0007};
volatile uint32_t g_core_done[NUM_CORES] = {0, 0, 0, 0};
volatile uint32_t g_interference_go = 0xDEAD0008;  // 0=idle, 1=interfere, 2=stop (reinit in main)

// Debug state
volatile uint64_t g_mcause = 0xDEAD0004;
volatile uint64_t g_mepc = 0xDEAD0005;
volatile uint32_t g_claimed_irq = 0xDEAD0006;

#define MAX_IRQ_LOG 5000
volatile uint32_t g_irq_log_cycle[NUM_CORES][MAX_IRQ_LOG];
volatile uint8_t  g_irq_log_type[NUM_CORES][MAX_IRQ_LOG];
volatile uint32_t g_core0_start_mcycle = 0;

static uint32_t* const core_data_base[NUM_CORES] = {
    (uint32_t*)CORE0_DATA_BASE, (uint32_t*)CORE1_DATA_BASE,
    (uint32_t*)CORE2_DATA_BASE, (uint32_t*)CORE3_DATA_BASE
};

// Write interference: 256KB stride writes through LLC into main memory
void write_interference_mem(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    for (uint32_t i = 0; i < 8192; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
    g_work_iterations[core]++;
}

// Enables the LLC for 0x8000_0000 - 0xA000_0000
void enable_llc(void) {
    write_32b(0x1C + 0x1A106000, 0xA0000000);
}

// Partition 16-way LLC: 4 ways per core
void partition_cache_4cores(void) {
    write_32b(0x10401040, 0xF0FF0FFF);
    write_32b(0x10401044, 0xFF0FFFF0);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

void prime_cache(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < 4096; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
}

void reset_counters(uint32_t core) {
    uint32_t offsets[4] = {4, 8, 12, 16};
    for (uint32_t g = 0; g < 4; g++) {
        write_32b(COUNTER_BASE_ADDR + (offsets[g] + core) * COUNTER_BUNDLE_SIZE, 0x00000000);
    }
    asm volatile ("fence iorw, iorw" ::: "memory");
}

void my_sleep() {
    uint32_t sleep = 100000;
    for (volatile uint32_t i = 0; i < sleep; i++) {
        asm volatile ("fence");
        asm volatile ("addi x1, x1, 1");
        asm volatile ("fence");
    }
}

void end_test(uint32_t mhartid) {
    printf("Exiting: %0d.\r\n", mhartid);
}

uint64_t read_pmu_timer(void) {
    uint32_t timer_low = read_32b(TIMER_ADDR);
    uint32_t timer_high = read_32b(TIMER_ADDR + 4);
    return ((uint64_t)timer_high << 32) | timer_low;
}

// *********************************************************************
// Saved results from each pass
// *********************************************************************
typedef struct {
    uint32_t call_count;
    uint32_t cyc_disp;
    uint32_t tot_cyc, tot_l1, tot_stall;
    uint32_t tot_llcrd, tot_llcwr, tot_memrd, tot_memwr;
    uint32_t tot_rlat, tot_wlat;
    uint32_t pmu_instret, pmu_mcycle;
} pass_results_t;

void read_pass_results(pass_results_t *r, uint32_t cyc_disp) {
    r->cyc_disp   = cyc_disp;
    r->call_count = read_32b(DSPM_MILESTONE_LOG_ADDR + 0x00);

    uint32_t base = DSPM_MILESTONE_LOG_ADDR + 0x08;
    r->tot_cyc   = read_32b(base + 0);
    r->tot_l1    = read_32b(base + 4);
    r->tot_stall = read_32b(base + 8);
    r->tot_llcrd = read_32b(base + 12);
    r->tot_llcwr = read_32b(base + 16);
    r->tot_memrd = read_32b(base + 20);
    r->tot_memwr = read_32b(base + 24);
    r->tot_rlat  = read_32b(base + 28);
    r->tot_wlat  = read_32b(base + 32);
    r->pmu_instret = read_32b(DSPM_BASE_ADDR + 0x1F00);
    r->pmu_mcycle  = read_32b(DSPM_BASE_ADDR + 0x1F04);
}

// *********************************************************************
// WFI Loop - execution redirects here when paused
// *********************************************************************
void __attribute__((naked, aligned(4))) wfi_loop(void) {
    asm volatile (
        "1: wfi\n\t"
        "   j 1b\n\t"
    );
}

// *********************************************************************
// Interrupt Handler
// *********************************************************************
void __attribute__((naked, aligned(4))) handle_trap(void) {
    asm volatile (
        "addi sp, sp, -128\n\t"
        "sd ra,  0(sp)\n\t"
        "sd t0,  8(sp)\n\t"
        "sd t1, 16(sp)\n\t"
        "sd t2, 24(sp)\n\t"
        "sd a0, 32(sp)\n\t"
        "sd a1, 40(sp)\n\t"
        "sd a2, 48(sp)\n\t"
        "sd a3, 56(sp)\n\t"
        "sd a4, 64(sp)\n\t"
        "sd a5, 72(sp)\n\t"
        "sd a6, 80(sp)\n\t"
        "sd a7, 88(sp)\n\t"
        "sd t3, 96(sp)\n\t"
        "sd t4, 104(sp)\n\t"
        "sd t5, 112(sp)\n\t"
        "sd t6, 120(sp)\n\t"
        "jal ra, handle_trap_c\n\t"
        "ld ra,  0(sp)\n\t"
        "ld t0,  8(sp)\n\t"
        "ld t1, 16(sp)\n\t"
        "ld t2, 24(sp)\n\t"
        "ld a0, 32(sp)\n\t"
        "ld a1, 40(sp)\n\t"
        "ld a2, 48(sp)\n\t"
        "ld a3, 56(sp)\n\t"
        "ld a4, 64(sp)\n\t"
        "ld a5, 72(sp)\n\t"
        "ld a6, 80(sp)\n\t"
        "ld a7, 88(sp)\n\t"
        "ld t3, 96(sp)\n\t"
        "ld t4, 104(sp)\n\t"
        "ld t5, 112(sp)\n\t"
        "ld t6, 120(sp)\n\t"
        "addi sp, sp, 128\n\t"
        "mret\n\t"
    );
}

void handle_trap_c(void) {
    uint32_t mhartid;
    asm volatile ("csrr %0, mhartid" : "=r"(mhartid));

    uint64_t counter_addr = COUNTER_BASE_ADDR + mhartid * COUNTER_BUNDLE_SIZE;
    uint32_t CVA_ACK_MASK = 0x00100000;
    uint32_t PMU_PR_MASK =  0x01000000;

    uint64_t mcause, mepc;
    asm volatile ("csrr %0, mcause" : "=r"(mcause));
    asm volatile ("csrr %0, mepc" : "=r"(mepc));

    g_mcause = mcause;
    g_mepc = mepc;

    uint64_t is_interrupt = mcause >> 63;
    uint64_t code = mcause & 0x7FF;

    if (is_interrupt && code == 11) {
        uint32_t plic_context = PLIC_CONTEXT_CORE(mhartid);
        uint32_t claimed = plic_claim_msg(plic_context);
        g_claimed_irq = claimed;

        uint32_t expected_irq = PMU_IRQ_CORE(mhartid);
        if (claimed == expected_irq) {
            uint32_t irq_idx = g_interrupt_count[mhartid];
            g_interrupt_count[mhartid]++;

            uint32_t cyc;
            asm volatile ("csrr %0, mcycle" : "=r"(cyc));
            if (irq_idx < MAX_IRQ_LOG) {
                g_irq_log_cycle[mhartid][irq_idx] = cyc;
                g_irq_log_type[mhartid][irq_idx] = g_paused[mhartid] ? 1 : 0;
            }

            counter_val[mhartid] = read_32b(counter_addr);

            if ((counter_val[mhartid] & PMU_PR_MASK) == 0 && (counter_val[mhartid] & CVA_ACK_MASK) == 0) {
                g_saved_mepc[mhartid] = mepc;
                asm volatile ("csrw mepc, %0" :: "r"((uint64_t)&wfi_loop));
                g_paused[mhartid] = 1;
                write_32b(counter_addr, CVA_ACK_MASK);
            } else if ((counter_val[mhartid] & PMU_PR_MASK) == PMU_PR_MASK) {
                asm volatile ("csrw mepc, %0" :: "r"(g_saved_mepc[mhartid]));
                g_paused[mhartid] = 0;
                write_32b(counter_addr, CVA_ACK_MASK);
            }

            plic_complete_msg(plic_context, claimed);
        } else if (claimed != 0) {
            plic_complete_msg(plic_context, claimed);
        }
    }
}

// *********************************************************************
// Per-core setup: trap vector, PLIC, and PMU counter
// *********************************************************************
void setup_core_interrupt(uint32_t core_id) {
    uint32_t plic_context = PLIC_CONTEXT_CORE(core_id);
    uint32_t irq_id = PMU_IRQ_CORE(core_id);

    uintptr_t mtvec_val = (uintptr_t)&handle_trap;
    asm volatile ("csrw mtvec, %0" :: "r"(mtvec_val));

    plic_set_priority(irq_id, 7);
    plic_set_ie(plic_context, irq_id, 1);
    plic_set_thresh(plic_context, 6);

    write_32b(COUNTER_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE, 0x00000000);
    asm volatile ("fence iorw, iorw" ::: "memory");

    uint32_t stale_irq;
    do {
        stale_irq = plic_claim_msg(plic_context);
        if (stale_irq != 0)
            plic_complete_msg(plic_context, stale_irq);
    } while (stale_irq != 0);

    uint64_t event_info_addr = EVENT_INFO_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE;
    uint64_t counter_addr = COUNTER_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE;
    write_32b(event_info_addr, OVERFLOW_EN);
    write_32b(counter_addr, 0x00100000);
    asm volatile ("fence iorw, iorw" ::: "memory");

    do {
        stale_irq = plic_claim_msg(plic_context);
        if (stale_irq != 0)
            plic_complete_msg(plic_context, stale_irq);
    } while (stale_irq != 0);

    uint64_t mie;
    asm volatile ("csrr %0, mie" : "=r"(mie));
    mie |= (1 << 11);
    asm volatile ("csrw mie, %0" :: "r"(mie));

    uint64_t mstatus;
    asm volatile ("csrr %0, mstatus" : "=r"(mstatus));
    mstatus |= (1 << 3);
    asm volatile ("csrw mstatus, %0" :: "r"(mstatus));
}

void thread_entry(int cid, int nc) {
    return;
}

static void copy_words(void *dst, const void *src, uint32_t size) {
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (uint32_t i = 0; i < (size + 3) / 4; i++)
        d[i] = s[i];
}

// *********************************************************************
// Helper: load firmware, start PMU, wait for ready
// *********************************************************************
void load_and_start_pmu(void) {
    uint32_t text_size = _binary_text_section_bin_end - _binary_text_section_bin_start;
    uint32_t data_size = _binary_data_rodata_bss_bin_end - _binary_data_rodata_bss_bin_start;

    // Halt PMU
    write_32b(PMC_STATUS_ADDR, 1);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Free-running timer
    write_32b(PERIOD_ADDR, 0xFFFFFFFF);
    write_32b(PERIOD_ADDR + 4, 0xFFFFFFFF);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Clear DSPM communication area
    for (uint32_t i = 0; i < 0x200; i += 4) {
        write_32b(DSPM_BASE_ADDR + i, 0);
    }

    // Load firmware with 32-bit word stores only. The ISPM/DSPM SRAMs index by byte address and
    // ignore byte strobes, so the byte stores memcpy uses for a tail that is not 8-byte aligned
    // (this image is 2292 bytes) land in the wrong entries: the last instruction of the firmware,
    // the polling loop's back-edge, became `j .` and no call was ever logged.
    copy_words((void*)ISPM_BASE_ADDR, _binary_text_section_bin_start, text_size);
    copy_words((void*)(DSPM_BASE_ADDR + 0x200), _binary_data_rodata_bss_bin_start, data_size);
    asm volatile ("fence iorw, iorw" ::: "memory");
    {
        const volatile uint32_t *d = (const volatile uint32_t *)ISPM_BASE_ADDR;
        const uint32_t *src = (const uint32_t *)_binary_text_section_bin_start;
        uint32_t bad = 0;
        for (uint32_t i = 0; i < (text_size + 3) / 4; i++)
            if (d[i] != src[i]) bad++;
        printf("    ISPM read-back: %u of %u words differ from the image\r\n", bad, (text_size + 3) / 4);
    }

    // Boot PMU
    write_32b(PMC_BOOT_ADDR, ISPM_BASE_ADDR);
    write_32b(PMC_STATUS_ADDR, 0);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Wait for PMU to start
    uint32_t timeout = 100000;
    while (read_32b(DSPM_STATUS_ADDR) != STATUS_STARTED && timeout > 0) {
        timeout--;
        asm volatile ("nop");
    }
    if (timeout == 0) {
        printf("    TIMEOUT waiting for PMU (status=0x%08x)\r\n", read_32b(DSPM_STATUS_ADDR));
        while(1) asm volatile ("wfi");
    }
}

// *********************************************************************
// Helper: configure EVU counters and milestone
// *********************************************************************
void configure_evu_and_milestone(void) {
    printf("    [EVU] enter configure_evu_and_milestone\r\n");
    // EVU event IDs for Core 0 — milestone code MUST go in EID2 slot (offset +8).
    // Empirically (see pmu_evu_c1_track): EVENTID2 reg fires e_id[1] when matched,
    // so the counter filter EV1 (0x1F0022) catches it. EID0/EID1/EID3 disabled
    // because we no longer need the L1D/branch/stall measurements interfering.
    printf("    [EVU] EID0 <- 0x00 (disabled)\r\n");
    write_32b(EVENTID_C0_BASE_ADDR,      0x00);
    printf("    [EVU] EID1 <- 0x00 (disabled)\r\n");
    write_32b(EVENTID_C0_BASE_ADDR + 4,  0x00);
    printf("    [EVU] EID2 <- 0x17 (PC milestone hit) [drives e_id[1]]\r\n");
    write_32b(EVENTID_C0_BASE_ADDR + 8,  0x17);
    printf("    [EVU] EID3 <- 0x00 (disabled)\r\n");
    write_32b(EVENTID_C0_BASE_ADDR + 12, 0x00);
    printf("    [EVU] all 4 EIDs written\r\n");

    // PC Milestones — PCTRACK0/PCTRACK1 are 64-bit registers. On RV64 we MUST
    // write both halves: low 32b at offset+0, high 32b at offset+4. Leaving the
    // high half at boot-junk means the 64-bit equality check never matches and
    // milestone hits NEVER fire — this was the silent killer of all prior debug
    // attempts.
    printf("    [EVU] PC_MIL[0].lo <- 0x%08x (FUNC_ENTRY via &padarray4)\r\n",
           DYNAMIC_PROFILE_FUNC_ENTRY);
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 0,  DYNAMIC_PROFILE_FUNC_ENTRY);
    printf("    [EVU] PC_MIL[0].hi <- 0x00000000\r\n");
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 4,  0x00000000);

    printf("    [EVU] PC_MIL[1].lo <- 0x%08x (FUNC_EXIT = entry+0x114)\r\n",
           DYNAMIC_PROFILE_FUNC_EXIT);
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 8,  DYNAMIC_PROFILE_FUNC_EXIT);
    printf("    [EVU] PC_MIL[1].hi <- 0x00000000\r\n");
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 12, 0x00000000);

    printf("    [EVU] PC_MIL[2] <- 0 (both halves)\r\n");
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 16, 0x00000000);
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 20, 0x00000000);

    printf("    [EVU] PC_MIL[3] <- 0 (both halves)\r\n");
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 24, 0x00000000);
    write_32b(PC_MILESTONE_C0_BASE_ADDR + 28, 0x00000000);
    asm volatile ("fence iorw, iorw" ::: "memory");
    printf("    [EVU] PC milestone slots done (64-bit writes)\r\n");

    // Working pattern from pmu_evu_c1_track (verified end-to-end May 22):
    //   - EventSelCfg = EV1 for Core 0 = 0x001F0022 (port=1, EvVal=2, EvMask=2)
    //     → accepts packets where e_id bit 1 is set, which happens when EID2's
    //       event (0x17, PC-milestone-hit) fires
    //   - EventInfoCfg = (1<<23) | (n<<5) | n
    //       = ALU-Add of single bit e_info[n]
    //       = counter += 1 each time PCTRACKn matches
    //
    // PCTRACK match bits ARE at e_info[3:0] (one per slot) — my earlier
    // "[18:15] slice" reading was wrong; that was the val_l position in the
    // EventInfoCfg register, NOT the position of PCTRACK bits in e_info.
    #define EVU_C0_EV1_SEL    0x001F0022UL
    #define EVU_INFO_CFG(n)   ((1UL << 23) | ((uint32_t)(n) << 5) | (uint32_t)(n))

    printf("    [EVU] cnt20.sel <- 0x%08x  cnt20.info <- 0x%08x  [PCTRACK0/entry]\r\n",
           EVU_C0_EV1_SEL, EVU_INFO_CFG(0));
    write_32b(EVENT_SEL_BASE_ADDR  + 20 * COUNTER_BUNDLE_SIZE, EVU_C0_EV1_SEL);
    write_32b(EVENT_INFO_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE, EVU_INFO_CFG(0));
    write_32b(COUNTER_BASE_ADDR    + 20 * COUNTER_BUNDLE_SIZE, 0);

    printf("    [EVU] cnt21.sel <- 0x%08x  cnt21.info <- 0x%08x  [PCTRACK1/exit]\r\n",
           EVU_C0_EV1_SEL, EVU_INFO_CFG(1));
    write_32b(EVENT_SEL_BASE_ADDR  + 21 * COUNTER_BUNDLE_SIZE, EVU_C0_EV1_SEL);
    write_32b(EVENT_INFO_BASE_ADDR + 21 * COUNTER_BUNDLE_SIZE, EVU_INFO_CFG(1));
    write_32b(COUNTER_BASE_ADDR    + 21 * COUNTER_BUNDLE_SIZE, 0);

    // Counter 22: L1D miss (EV0)
    printf("    [EVU] cnt22.sel <- EV0\r\n");
    write_32b(EVENT_SEL_BASE_ADDR  + 22 * COUNTER_BUNDLE_SIZE, PERF_COUNTER_EVU_C0_EV0);
    printf("    [EVU] cnt22.info <- 0\r\n");
    write_32b(EVENT_INFO_BASE_ADDR + 22 * COUNTER_BUNDLE_SIZE, 0);
    printf("    [EVU] cnt22.cnt <- 0 (cnt_wr)\r\n");
    write_32b(COUNTER_BASE_ADDR    + 22 * COUNTER_BUNDLE_SIZE, 0);
    printf("    [EVU] cnt22 done\r\n");

    // Counter 23: pipeline stalls (EV2)
    printf("    [EVU] cnt23.sel <- EV2\r\n");
    write_32b(EVENT_SEL_BASE_ADDR  + 23 * COUNTER_BUNDLE_SIZE, PERF_COUNTER_EVU_C0_EV2);
    printf("    [EVU] cnt23.info <- 0\r\n");
    write_32b(EVENT_INFO_BASE_ADDR + 23 * COUNTER_BUNDLE_SIZE, 0);
    printf("    [EVU] cnt23.cnt <- 0 (cnt_wr)\r\n");
    write_32b(COUNTER_BASE_ADDR    + 23 * COUNTER_BUNDLE_SIZE, 0);
    printf("    [EVU] cnt23 done\r\n");
    printf("    [EVU] exit configure_evu_and_milestone\r\n");
}

// *********************************************************************
// Helper: run one profiling pass
// Returns cycle count from PMU timer
// *********************************************************************
uint32_t run_profiling_pass(void) {
    // Signal PMU to start
    write_32b(DSPM_CMD_ADDR, CMD_TRIGGER_INTERRUPT);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Wait for firmware to be ready
    while (read_32b(DSPM_STATUS_ADDR) != STATUS_ALL_IRQS_SENT) {
        asm volatile ("nop");
    }
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Run disparity benchmark
    uint64_t t0, t1;
    char *a[] = { "disp", "DISP" };
    t0 = read_pmu_timer();
    sdvb_disparity(2, a);
    t1 = read_pmu_timer();

    // Halt PMU
    write_32b(PMC_STATUS_ADDR, 1);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Brief delay for PMU to finish last log write
    for (volatile uint32_t i = 0; i < 1000; i++) asm volatile ("nop");

    return (uint32_t)(t1 - t0);
}

// *********************************************************************
// Helper: print one pass's per-call records
// *********************************************************************
void print_per_call_records(uint32_t call_count) {
    uint32_t print_count = call_count;
    if (print_count > MAX_LOG_RECORDS) print_count = MAX_LOG_RECORDS;

    printf("\r\n    --- Per-Call Records (first %u) ---\r\n", print_count);
    printf("    %-6s %10s %10s %10s %10s %10s %10s %10s %10s %10s\r\n",
           "Call#", "Cycles", "Branch", "L1DAcc", "LLC_RD", "LLC_WR", "MEM_RD", "MEM_WR", "RdLat", "WrLat");

    for (uint32_t i = 0; i < print_count; i++) {
        uint32_t rec = DSPM_MILESTONE_LOG_ADDR + 0x80 + i * MILESTONE_RECORD_WORDS * 4;
        printf("    %-6u %10u %10u %10u %10u %10u %10u %10u %10u %10u\r\n",
               i,
               read_32b(rec + 0),  read_32b(rec + 4),
               read_32b(rec + 8),  read_32b(rec + 12),
               read_32b(rec + 16), read_32b(rec + 20),
               read_32b(rec + 24), read_32b(rec + 28),
               read_32b(rec + 32));
    }
}

// *********************************************************************
// Main Function
// *********************************************************************
int main(int argc, char const *argv[]) {

    uint32_t mhartid;
    asm volatile ("csrr %0, 0xF14\n" : "=r" (mhartid));

    // *******************************************************************
    // Core 0 — two-pass computeSAD profiling (solo then interference)
    // *******************************************************************
    if (mhartid == 0) {
        #ifdef FPGA_EMULATION
        // Match pmu_sdvb_disp/disp_main.c: only program LCR for 8N1, preserve
        // the divisor set by whoever loaded us (bootloader / OpenOCD).
        // Reprogramming DLL/DLM with a wrong test_freq silences the UART.
        *(volatile uint8_t *)(0x4000000CUL) = 0x83;   // DLAB=1, 8N1
        asm volatile ("fence iorw, iorw" ::: "memory");
        *(volatile uint8_t *)(0x4000000CUL) = 0x03;   // DLAB=0, 8N1
        asm volatile ("fence iorw, iorw" ::: "memory");
        #else
        set_flls();
        uart_set_cfg(0, (50000000 / 115200) >> 4);
        #endif

        printf("\r\n");
        printf("========================================\r\n");
        printf("computeSAD Profiler: Solo vs Interference\r\n");
        printf("========================================\r\n");

        // Reinitialize globals
        for (uint32_t i = 0; i < NUM_CORES; i++) {
            g_paused[i] = 0;
            g_interrupt_count[i] = 0;
            g_work_iterations[i] = 0;
            g_saved_mepc[i] = 0;
            g_core_done[i] = 0;
            for (uint32_t j = 0; j < MAX_IRQ_LOG; j++) {
                g_irq_log_cycle[i][j] = 0;
                g_irq_log_type[i][j] = 0;
            }
        }
        g_mcause = 0;
        g_mepc = 0;
        g_claimed_irq = 0;
        g_interference_go = 0;

        // ************************************************************************
        // Load PMU Firmware (first instance)
        // ************************************************************************
        printf("[1] Loading PMU firmware...\r\n");
        load_and_start_pmu();
        printf("    PMU started!\r\n");

        // ************************************************************************
        // Trap, PLIC, Counter setup
        // ************************************************************************
        printf("[2] Configuring interrupts and counters...\r\n");

        for (uint32_t core = 0; core < NUM_CORES; core++) {
            plic_set_priority(PMU_IRQ_CORE(core), 1);
        }

        setup_core_interrupt(0);

        // Signal other cores to set up interrupts
        write_32b(DSPM_CORES_READY_ADDR, 0xCAFE0001);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Wait for cores 1-3
        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint32_t to = 1000000;
            while (read_32b(DSPM_BASE_ADDR + 0x24 + core * 0x4) != 1 && to > 0) {
                to--;
                asm volatile ("nop");
            }
        }
        printf("    All cores ready!\r\n");

        // ************************************************************************
        // LLC + counter configuration
        // ************************************************************************
        enable_llc();
        partition_cache_4cores();
        printf("    LLC enabled + partitioned (4 ways/core)\r\n");

        // cnt 4-7: LLC_RD, 8-11: LLC_WR, 12-15: MEM_RD, 16-19: MEM_WR
        uint32_t llc_rd_events[NUM_CORES] = {
            LLC_RD_REQ_CORE_0, LLC_RD_REQ_CORE_1,
            LLC_RD_REQ_CORE_2, LLC_RD_REQ_CORE_3
        };
        uint32_t llc_wr_events[NUM_CORES] = {
            LLC_WR_REQ_CORE_0, LLC_WR_REQ_CORE_1,
            LLC_WR_REQ_CORE_2, LLC_WR_REQ_CORE_3
        };
        uint32_t mem_rd_events[NUM_CORES] = {
            MEM_RD_REQ_CORE_0, MEM_RD_REQ_CORE_1,
            MEM_RD_REQ_CORE_2, MEM_RD_REQ_CORE_3
        };
        uint32_t mem_wr_events[NUM_CORES] = {
            MEM_WR_REQ_CORE_0, MEM_WR_REQ_CORE_1,
            MEM_WR_REQ_CORE_2, MEM_WR_REQ_CORE_3
        };
        uint32_t *event_groups[4] = {llc_rd_events, llc_wr_events, mem_rd_events, mem_wr_events};
        uint32_t group_base[4] = {4, 8, 12, 16};

        printf("    [LLC] start cnt-group loop (cnt 4..19)\r\n");
        for (uint32_t g = 0; g < 4; g++) {
            printf("    [LLC] group g=%u (base cnt=%u)\r\n", g, group_base[g]);
            for (uint32_t core = 0; core < NUM_CORES; core++) {
                uint32_t cnt_idx = group_base[g] + core;
                printf("    [LLC]   cnt%u.sel <- 0x%08x\r\n", cnt_idx, event_groups[g][core]);
                write_32b(EVENT_SEL_BASE_ADDR  + cnt_idx * COUNTER_BUNDLE_SIZE, event_groups[g][core]);
                printf("    [LLC]   cnt%u.info <- 0\r\n", cnt_idx);
                write_32b(EVENT_INFO_BASE_ADDR + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
                printf("    [LLC]   cnt%u.cnt <- 0 (cnt_wr)\r\n", cnt_idx);
                write_32b(COUNTER_BASE_ADDR    + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
            }
        }
        printf("    [LLC] cnt-group loop done\r\n");

        // Counter 27: MEM read response latency (core 0)
        printf("    [LAT] cnt27.sel <- MEM_RD_RES_CORE_0\r\n");
        write_32b(EVENT_SEL_BASE_ADDR  + 27 * COUNTER_BUNDLE_SIZE, MEM_RD_RES_CORE_0);
        printf("    [LAT] cnt27.info <- ADD_RESP_LAT (0x%x)\r\n", ADD_RESP_LAT);
        write_32b(EVENT_INFO_BASE_ADDR + 27 * COUNTER_BUNDLE_SIZE, ADD_RESP_LAT);
        printf("    [LAT] cnt27.cnt <- 0 (cnt_wr)\r\n");
        write_32b(COUNTER_BASE_ADDR    + 27 * COUNTER_BUNDLE_SIZE, 0);
        printf("    [LAT] cnt27 done\r\n");

        // Counter 28: MEM write response latency (core 0)
        printf("    [LAT] cnt28.sel <- MEM_WR_RES_CORE_0\r\n");
        write_32b(EVENT_SEL_BASE_ADDR  + 28 * COUNTER_BUNDLE_SIZE, MEM_WR_RES_CORE_0);
        printf("    [LAT] cnt28.info <- ADD_RESP_LAT\r\n");
        write_32b(EVENT_INFO_BASE_ADDR + 28 * COUNTER_BUNDLE_SIZE, ADD_RESP_LAT);
        printf("    [LAT] cnt28.cnt <- 0 (cnt_wr)\r\n");
        write_32b(COUNTER_BASE_ADDR    + 28 * COUNTER_BUNDLE_SIZE, 0);
        printf("    [LAT] cnt28 done\r\n");

        printf("    [CALL] -> configure_evu_and_milestone()\r\n");
        configure_evu_and_milestone();
        printf("    [CALL] <- configure_evu_and_milestone()\r\n");
        printf("    Milestone: entry=0x%08x, exit=0x%08x (linker symbols)\r\n",
               DYNAMIC_PROFILE_FUNC_ENTRY, DYNAMIC_PROFILE_FUNC_EXIT);

        // ************************************************************************
        // Mount FAT16 filesystem
        // ************************************************************************
        FAT16 vol;
        printf("[3] Mounting FAT16...\r\n");
        if (fat16_mount(&vol) != 0) {
            printf("    FAIL: fat16_mount\r\n");
            while (1) asm volatile ("wfi");
        }
        printf("    FAT mounted\r\n");
        fat_file_init(&vol);

        // ************************************************************************
        // PASS 1: No interference
        // ************************************************************************
        printf("\r\n========================================\r\n");
        printf("PASS 1: Solo (no interference)\r\n");
        printf("========================================\r\n");
        uart_wait_tx_done();

        // Prime caches, reset counters
        for (uint32_t c = 0; c < NUM_CORES; c++) prime_cache(c);
        for (uint32_t c = 0; c < NUM_CORES; c++) reset_counters(c);

        // Run pass 1 (cores 1-3 are idle, g_interference_go == 0)
        uint32_t cyc1 = run_profiling_pass();

        // Read results
        pass_results_t solo;
        read_pass_results(&solo, cyc1);

        printf("    Calls: %u, PMU timer: %u\r\n", solo.call_count, solo.cyc_disp);
        print_per_call_records(solo.call_count);

        // ************************************************************************
        // PHASE-1 RAW COUNTER DUMP (diagnostic — confirms milestone wiring)
        // ************************************************************************
        printf("\r\n    ---- Phase-1 raw counter dump ----\r\n");
        printf("    %-22s %-12s %s\r\n", "Counter", "Actual", "Expected if working");
        // Counter 20: milestone slot 0 hits  (THE one we care about)
        printf("    %-22s 0x%08x   should be NON-ZERO if padarray4 fired (==Calls)\r\n",
               "cnt20 MILESTONE_0", read_32b(COUNTER_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE));
        // Counter 22: EV0 -> EID0 = 0x02 (L1D miss)
        printf("    %-22s 0x%08x   non-zero (L1D misses occur during 7M cycles)\r\n",
               "cnt22 EV0/EID0=L1Dmiss", read_32b(COUNTER_BASE_ADDR + 22 * COUNTER_BUNDLE_SIZE));
        // Counter 23: EV2 -> EID2 = 0x16 (stalls)
        printf("    %-22s 0x%08x   non-zero (stalls happen)\r\n",
               "cnt23 EV2/EID2=stalls", read_32b(COUNTER_BASE_ADDR + 23 * COUNTER_BUNDLE_SIZE));
        // Counter 26: firmware breadcrumb — last value the Ibex wrote
        printf("    %-22s 0x%08x   2=before-loop, 10+N=after entry, 100+N=after exit\r\n",
               "cnt26 BREADCRUMB", read_32b(COUNTER_BASE_ADDR + 26 * COUNTER_BUNDLE_SIZE));
        // PC milestone slot 0 — should have been reprogrammed by firmware to FUNC_EXIT if any hit
        printf("    %-22s 0x%08x   %s\r\n",
               "PC_MIL_SLOT0.lo",
               read_32b(PC_MILESTONE_C0_BASE_ADDR + 0),
               "want = FUNC_ENTRY low 32b");
        printf("    %-22s 0x%08x   %s\r\n",
               "PC_MIL_SLOT0.hi",
               read_32b(PC_MILESTONE_C0_BASE_ADDR + 4),
               "want = 0x00000000 (was uninit junk before fix)");
        printf("    %-22s 0x%08x   %s\r\n",
               "PC_MIL_SLOT1.lo",
               read_32b(PC_MILESTONE_C0_BASE_ADDR + 8),
               "want = FUNC_EXIT low 32b");
        printf("    %-22s 0x%08x   %s\r\n",
               "PC_MIL_SLOT1.hi",
               read_32b(PC_MILESTONE_C0_BASE_ADDR + 12),
               "want = 0x00000000");
        // EVENTID readbacks
        printf("    EID0=0x%08x  EID1=0x%08x  EID2=0x%08x  EID3=0x%08x\r\n",
               read_32b(EVENTID_C0_BASE_ADDR),
               read_32b(EVENTID_C0_BASE_ADDR + 4),
               read_32b(EVENTID_C0_BASE_ADDR + 8),
               read_32b(EVENTID_C0_BASE_ADDR + 12));
        printf("\r\n    ---- Milestone counters (EV1, ALU-Add e_info[n]) ----\r\n");
        printf("    Wiring matches working pmu_evu_c1_track: EID2=0x17 + EV1 + ALU-Add\r\n");
        printf("    %-34s %-12s %s\r\n", "Counter", "Actual (masked)", "Expected");
        printf("    %-34s %u           %s\r\n",
               "cnt20 (PCTRACK0 / FUNC_ENTRY)",
               read_32b(COUNTER_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFFu,
               "= number of padarray4 calls");
        printf("    %-34s %u           %s\r\n",
               "cnt21 (PCTRACK1 / FUNC_EXIT)",
               read_32b(COUNTER_BASE_ADDR + 21 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFFu,
               "= same (entry+exit pair per call)");
        printf("    Sanity: cnt20.sel=0x%08x  cnt20.info=0x%08x\r\n",
               read_32b(EVENT_SEL_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE),
               read_32b(EVENT_INFO_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE));
        printf("            cnt21.sel=0x%08x  cnt21.info=0x%08x\r\n",
               read_32b(EVENT_SEL_BASE_ADDR + 21 * COUNTER_BUNDLE_SIZE),
               read_32b(EVENT_INFO_BASE_ADDR + 21 * COUNTER_BUNDLE_SIZE));
        printf("    ---- end of dump ----\r\n");
        uart_wait_tx_done();

        // ************************************************************************
        // Reset PMU for pass 2
        // ************************************************************************
        printf("\r\n[4] Reloading PMU firmware for pass 2...\r\n");
        uart_wait_tx_done();

        load_and_start_pmu();
        configure_evu_and_milestone();

        // Reset all perf counters
        for (uint32_t c = 0; c < NUM_CORES; c++) reset_counters(c);

        printf("    PMU restarted!\r\n");

        // ************************************************************************
        // PASS 2: With write-memory interference from cores 1-3
        // ************************************************************************
        printf("\r\n========================================\r\n");
        printf("PASS 2: With write-mem interference (cores 1-3)\r\n");
        printf("========================================\r\n");
        uart_wait_tx_done();

        // Prime caches again
        for (uint32_t c = 0; c < NUM_CORES; c++) prime_cache(c);
        for (uint32_t c = 0; c < NUM_CORES; c++) reset_counters(c);

        // Signal cores 1-3 to start write interference
        g_interference_go = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Brief delay for interference to ramp up
        for (volatile uint32_t i = 0; i < 1000; i++) asm volatile ("nop");

        // Run pass 2
        uint32_t cyc2 = run_profiling_pass();

        // Stop interference
        g_interference_go = 2;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Read results
        pass_results_t intrf;
        read_pass_results(&intrf, cyc2);

        printf("    Calls: %u, PMU timer: %u\r\n", intrf.call_count, intrf.cyc_disp);
        print_per_call_records(intrf.call_count);

        // ************************************************************************
        // Side-by-side comparison
        // ************************************************************************
        printf("\r\n========================================\r\n");
        printf("COMPARISON: Solo vs Interference\r\n");
        printf("========================================\r\n");

        printf("    %-12s %10s %10s %10s\r\n", "", "Solo", "Interf", "Delta%");

        #define CMP(label, s, i) do { \
            int32_t pct = (s) > 0 ? (int32_t)(((int64_t)(i) - (int64_t)(s)) * 100 / (int64_t)(s)) : 0; \
            printf("    %-12s %10u %10u   %c%d%%\r\n", label, (s), (i), pct < 0 ? '-' : '+', pct < 0 ? -pct : pct); \
        } while(0)

        CMP("PMU Cycles",  solo.cyc_disp,  intrf.cyc_disp);
        CMP("Calls",       solo.call_count, intrf.call_count);

        printf("\r\n    --- Totals ---\r\n");
        printf("    %-12s %10s %10s %10s\r\n", "", "Solo", "Interf", "Delta%");
        CMP("Cycles",   solo.tot_cyc,   intrf.tot_cyc);
        CMP("Branch",   solo.tot_l1,    intrf.tot_l1);
        CMP("L1DAcc",   solo.tot_stall, intrf.tot_stall);
        CMP("LLC_RD",   solo.tot_llcrd, intrf.tot_llcrd);
        CMP("LLC_WR",   solo.tot_llcwr, intrf.tot_llcwr);
        CMP("MEM_RD",   solo.tot_memrd, intrf.tot_memrd);
        CMP("MEM_WR",   solo.tot_memwr, intrf.tot_memwr);
        CMP("RdLat",    solo.tot_rlat,  intrf.tot_rlat);
        CMP("WrLat",    solo.tot_wlat,  intrf.tot_wlat);

        if (solo.call_count > 0 && intrf.call_count > 0) {
            printf("\r\n    --- Averages (per call) ---\r\n");
            printf("    %-12s %10s %10s %10s\r\n", "", "Solo", "Interf", "Delta%");
            CMP("AvgCycles", solo.tot_cyc/solo.call_count,   intrf.tot_cyc/intrf.call_count);
            CMP("AvgBranch", solo.tot_l1/solo.call_count,    intrf.tot_l1/intrf.call_count);
            CMP("AvgL1DAcc", solo.tot_stall/solo.call_count, intrf.tot_stall/intrf.call_count);
            CMP("AvgLLCrd",  solo.tot_llcrd/solo.call_count, intrf.tot_llcrd/intrf.call_count);
            CMP("AvgLLCwr",  solo.tot_llcwr/solo.call_count, intrf.tot_llcwr/intrf.call_count);
            CMP("AvgMEMrd",  solo.tot_memrd/solo.call_count, intrf.tot_memrd/intrf.call_count);
            CMP("AvgMEMwr",  solo.tot_memwr/solo.call_count, intrf.tot_memwr/intrf.call_count);
            CMP("AvgRdLat",  solo.tot_rlat/solo.call_count,  intrf.tot_rlat/intrf.call_count);
            CMP("AvgWrLat",  solo.tot_wlat/solo.call_count,  intrf.tot_wlat/intrf.call_count);
        }

        #undef CMP

        // PMU instret comparison
        printf("\r\n    --- PMU Core Efficiency ---\r\n");
        printf("    %-12s %12s %12s\r\n", "", "Solo", "Interf");
        printf("    %-12s %12u %12u\r\n", "PMU instret", solo.pmu_instret, intrf.pmu_instret);
        printf("    %-12s %12u %12u\r\n", "PMU mcycle",  solo.pmu_mcycle,  intrf.pmu_mcycle);
        if (solo.pmu_mcycle > 0 && intrf.pmu_mcycle > 0)
            printf("    %-12s %11u.%02u %11u.%02u\r\n", "PMU IPC",
                   solo.pmu_instret / solo.pmu_mcycle,
                   (solo.pmu_instret * 100 / solo.pmu_mcycle) % 100,
                   intrf.pmu_instret / intrf.pmu_mcycle,
                   (intrf.pmu_instret * 100 / intrf.pmu_mcycle) % 100);

        // Global counters
        printf("\r\n    --- Global PMU Counters (pass 2) ---\r\n");
        for (uint32_t core = 0; core < NUM_CORES; core++) {
            uint32_t llc_rd = read_32b(COUNTER_BASE_ADDR + (4 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            uint32_t llc_wr = read_32b(COUNTER_BASE_ADDR + (8 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            uint32_t mem_rd = read_32b(COUNTER_BASE_ADDR + (12 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            uint32_t mem_wr = read_32b(COUNTER_BASE_ADDR + (16 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            printf("    Core %u: LLC_RD=%u, LLC_WR=%u, MEM_RD=%u, MEM_WR=%u\r\n",
                   core, llc_rd, llc_wr, mem_rd, mem_wr);
        }

        // Signal cores 1-3 to exit
        g_core_done[0] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        printf("\r\n    Test complete.\r\n");
        uart_wait_tx_done();

    // *******************************************************************
    // Cores 1-3: idle during pass 1, write interference during pass 2
    // *******************************************************************
    } else if (mhartid == 1 || mhartid == 2 || mhartid == 3) {

        while (read_32b(DSPM_CORES_READY_ADDR) == 0xCAFE0001) {
            asm volatile ("nop");
        }
        while (read_32b(DSPM_CORES_READY_ADDR) != 0xCAFE0001) {
            asm volatile ("nop");
        }

        setup_core_interrupt(mhartid);

        write_32b(DSPM_BASE_ADDR + 0x24 + mhartid * 0x4, 1);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Wait for interference signal (0=idle, 1=interfere, 2=stop)
        // During pass 1: g_interference_go == 0, so we just spin
        // During pass 2: g_interference_go == 1, we run interference
        // After pass 2:  g_interference_go == 2, we stop
        while (!g_core_done[0]) {
            if (g_interference_go == 1) {
                write_interference_mem(mhartid);
            } else {
                asm volatile ("nop");
            }
        }

        // Clean up
        asm volatile ("csrw mie, zero");
        asm volatile ("csrc mstatus, %0" :: "r"(1 << 3));
        write_32b(COUNTER_BASE_ADDR + mhartid * COUNTER_BUNDLE_SIZE, 0x00000000);
        asm volatile ("fence iorw, iorw" ::: "memory");

        uint32_t plic_ctx = PLIC_CONTEXT_CORE(mhartid);
        uint32_t stale;
        do {
            stale = plic_claim_msg(plic_ctx);
            if (stale != 0) plic_complete_msg(plic_ctx, stale);
        } while (stale != 0);

        g_core_done[mhartid] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        while(1) asm volatile ("wfi");
    } else {
        end_test(mhartid);
        uart_wait_tx_done();
        while(1) asm volatile ("wfi");
    }

    return 0;
}
