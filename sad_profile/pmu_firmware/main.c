/*
 * PMU Firmware - computeSAD Single-Function Profiler
 *
 * Uses 1 PC milestone slot (slot 0), reprogrammed each hit:
 *   Hit while waiting for entry -> reprogram slot to exit addr
 *   Hit while inside function   -> log stats, reprogram slot to entry addr
 *
 * Two modes (toggle USE_WFP):
 *   USE_WFP=1: sleep via cnt.wfp, wake only on milestone hit
 *   USE_WFP=0: busy-poll cnt_rd(CNT_MILESTONE_0) every cycle
 *
 * DSPM log layout (at DSPM_BASE_ADDR + 0x2000):
 *   +0x00: call_count       (completed entry->exit pairs)
 *   +0x04: state            (0=waiting_entry, 1=inside_func)
 *   +0x08: totals[9]        (9 metrics x 4B = 36 bytes)
 *   +0x80: per-call records (9 words = 36 bytes each)
 *
 * Instret tracking at DSPM_BASE_ADDR + 0x1F00:
 *   +0x00: total PMU instret
 *   +0x04: total PMU mcycle
 */

#include <stddef.h>
#include <stdint.h>
#include "include/pmu_hw_desc.h"

// *** Toggle this to compare polling vs wfp ***
#define USE_WFP  1

// DSPM communication addresses (must match CVA6 pmu_bench.c)
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)
#define DSPM_CMD_ADDR           (DSPM_BASE_ADDR + 0x80)

// DSPM log layout
#define DSPM_LOG_BASE           (DSPM_BASE_ADDR + 0x2000)
#define DSPM_LOG_CALL_COUNT     (DSPM_LOG_BASE + 0x00)
#define DSPM_LOG_STATE          (DSPM_LOG_BASE + 0x04)
#define DSPM_LOG_TOTALS         (DSPM_LOG_BASE + 0x08)
#define DSPM_LOG_RECORDS        (DSPM_LOG_BASE + 0x80)
#define RECORD_WORDS            9
#define MAX_LOG_RECORDS         1000

// Instret tracking area
#define DSPM_INSTRET_BASE       (DSPM_BASE_ADDR + 0x1F00)
#define DSPM_PMU_INSTRET        (DSPM_INSTRET_BASE + 0x00)
#define DSPM_PMU_MCYCLE         (DSPM_INSTRET_BASE + 0x04)

// Status codes
#define STATUS_STARTED          0xBB000001
#define STATUS_SENDING_IRQS     0xBB000010
#define STATUS_ALL_IRQS_SENT    0xBB000099

// Command codes
#define CMD_NONE                0x00000000
#define CMD_TRIGGER_INTERRUPT   0x000000FF

// Memory access macros
#define WRITE_MEM(addr, value)  (*((volatile uint32_t*)(addr)) = (value))
#define READ_MEM(addr)          (*((volatile uint32_t*)(addr)))

// Counter indices (must match CVA6 counter config)
#define CNT_IRQ_BASE       0
#define CNT_LLC_RD_C0      4
#define CNT_LLC_WR_C0      8
#define CNT_MEM_RD_C0      12
#define CNT_MEM_WR_C0      16
// Canonical spec pattern: PCTRACK0=FUNC_ENTRY and PCTRACK1=FUNC_EXIT armed
// simultaneously, EID3=0x17 (milestone code). cnt20 fires only on ENTRY hits,
// cnt21 only on EXIT hits — no slot reprogramming required, no race.
#define CNT_MILESTONE_0      20   // ENTRY hits  (EV3 + EventInfoCfg=0x00808C60)
#define CNT_MILESTONE_EXIT   21   // EXIT  hits  (EV3 + EventInfoCfg=0x00810C60)
#define CNT_L1_MISS          22   // L1-D misses (EVU EV0)
#define CNT_STALLS           23   // pipeline stalls (EVU EV2)
#define CNT_TOTAL_CYCLES   25   // firmware writes total mcycle here
#define CNT_BREADCRUMB     26   // debug breadcrumb
#define CNT_LLC_RD_LAT     27   // LLC read response latency (core 0)
#define CNT_LLC_WR_LAT     28   // LLC write response latency (core 0)

// Milestone hardware register (slot 0)
#define PC_MILESTONE_SLOT0      0x10606010

// Target function addresses — UPDATE after rebuilding pmu_bench.riscv
#ifndef FUNC_ENTRY
#define FUNC_ENTRY              0x80004EEE  // padarray4 entry
#endif
#ifndef FUNC_EXIT
#define FUNC_EXIT               0x80005002  // padarray4 first ret
#endif

static inline void cnt_wr(uint32_t counter_idx, uint32_t value) {
    asm volatile(
        "mv a5, %0\n\t"
        "mv a4, %1\n\t"
        ".word 0x00e79007\n\t"
        :
        : "r" (counter_idx), "r" (value)
        : "a4", "a5"
    );
}

static inline uint32_t cnt_rd(uint32_t counter_idx) {
    uint32_t value;
    asm volatile(
        "mv a5, %1\n\t"
        ".word 0x00078707\n\t"
        "mv %0, a4\n\t"
        : "=r" (value)
        : "r" (counter_idx)
        : "a4", "a5"
    );
    return value;
}

/*
 * APMU counter values via plain AXI READ_MEM (same path CVA6 uses).
 * The custom cnt.rd instruction (above) doesn't appear to see milestone
 * counter updates from the Ibex side — the polling loop using cnt_rd
 * never observed cnt20/cnt21 transitioning despite CVA6 reading 8.
 * Direct memory-mapped access avoids that and matches what works on CVA6.
 *
 * Layout (from CVA6-side pmu_evu_c1_track):
 *   PMU base       = 0x10405000
 *   counter base   = 0x10407000  (= PMU base + 0x2000)
 *   bundle stride  = 0x1000      (counter N at base + N*0x1000)
 *   counter value  at offset 0 within bundle
 */
#define APMU_COUNTER_B_BASE   0x10407000UL
#define APMU_COUNTER_STRIDE   0x1000UL
#define cnt_val(n)  READ_MEM(APMU_COUNTER_B_BASE + (uint32_t)(n) * APMU_COUNTER_STRIDE)

/*
 * Wait-for-Pending: sleep until any counter in bitmap has its pending bit set.
 * Encoding: 0x00072887 (rd=x17=woke_bitmap, rs1=x14=monitor_bitmap)
 */
static inline uint32_t cnt_wfp(uint32_t counter_bitmap) {
    uint32_t woke_bitmap;
    asm volatile(
        "mv x14, %1\n\t"
        ".word 0x00072887\n\t"
        "mv %0, x17\n\t"
        : "=r" (woke_bitmap)
        : "r" (counter_bitmap)
        : "x14", "x17"
    );
    return woke_bitmap;
}

int main() {
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");

    // Program PC milestone slots from the Ibex side. In this RTL, PCTRACK
    // comparator only latches writes from the APMU-internal path; CVA6's
    // AXI writes land in a read-back-able register but don't reach the
    // comparator. (Discovered by comparing against pmu_evu_simple.)
    WRITE_MEM(0x10606010 + 0,  FUNC_ENTRY);   // PCTRACK0 = padarray4 entry
    WRITE_MEM(0x10606010 + 8,  FUNC_EXIT);    // PCTRACK1 = padarray4 first ret
    WRITE_MEM(0x10606010 + 16, 0);            // PCTRACK2 unused
    WRITE_MEM(0x10606010 + 24, 0);            // PCTRACK3 unused

    WRITE_MEM(0x10429000, 0xC4A0E440);

    // Enable mcycle + minstret counters
    asm volatile ("csrw 0x320, zero");

    // Clear command
    WRITE_MEM(DSPM_CMD_ADDR, CMD_NONE);

    // Clear overflow hardware for IRQ counters at boot
    for (uint32_t c = 0; c < 4; c++)
        cnt_wr(CNT_IRQ_BASE + c, 0x00000000);

    // Clear DSPM log area
    for (uint32_t i = 0; i < 0x80 + MAX_LOG_RECORDS * RECORD_WORDS * 4; i += 4)
        WRITE_MEM(DSPM_LOG_BASE + i, 0);

    // Clear instret tracking
    WRITE_MEM(DSPM_PMU_INSTRET, 0);
    WRITE_MEM(DSPM_PMU_MCYCLE, 0);

    // Signal PMU started
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_STARTED);

    // Wait for command from CVA6
    while (READ_MEM(DSPM_CMD_ADDR) != CMD_TRIGGER_INTERRUPT)
        asm volatile ("nop");

    // Clear command
    WRITE_MEM(DSPM_CMD_ADDR, CMD_NONE);

    // Keep status handshake compatible
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_SENDING_IRQS);
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_ALL_IRQS_SENT);

    cnt_wr(CNT_BREADCRUMB, 1);

    // ================================================================
    // computeSAD profiler — single milestone slot 0
    // ================================================================

    uint32_t call_count = 0;

    uint32_t tot_cycles = 0, tot_l1miss = 0, tot_stalls = 0;
    uint32_t tot_llcrd = 0, tot_llcwr = 0;
    uint32_t tot_memrd = 0, tot_memwr = 0;
    uint32_t tot_rlat = 0, tot_wlat = 0;

    // Clear stat counters
    cnt_wr(CNT_LLC_RD_C0, 0);
    cnt_wr(CNT_LLC_WR_C0, 0);
    cnt_wr(CNT_MEM_RD_C0, 0);
    cnt_wr(CNT_MEM_WR_C0, 0);
    cnt_wr(CNT_L1_MISS, 0);
    cnt_wr(CNT_STALLS, 0);
    cnt_wr(CNT_LLC_RD_LAT, 0);
    cnt_wr(CNT_LLC_WR_LAT, 0);

    // POLLING design: PCTRACK0 (FUNC_ENTRY) -> cnt20, PCTRACK1 (FUNC_EXIT) -> cnt21.
    // Both armed simultaneously by CVA6 in configure_evu_and_milestone. The
    // firmware NEVER resets cnt20/cnt21 or touches the PCTRACK regs — it just
    // samples the counters in a tight loop and records mcycle on every
    // transition. Same approach for LLC/MEM counters: snapshot at entry,
    // delta at exit. Eliminates the previous cnt_wfp + slot reprogramming
    // race that limited capture to 1 of 8 calls.
    cnt_wr(CNT_MILESTONE_0,    0);
    cnt_wr(CNT_MILESTONE_EXIT, 0);

    uint32_t prev_entry = 0, prev_exit = 0;
    uint32_t mcycle_entry = 0;

    /* Per-interval snapshot baselines */
    uint32_t llcrd_snap = 0, llcwr_snap = 0;
    uint32_t memrd_snap = 0, memwr_snap = 0;
    uint32_t l1miss_snap = 0, stalls_snap = 0;
    uint32_t llcrd_lat_snap = 0, llcwr_lat_snap = 0;

    uint32_t cyc_start;
    asm volatile ("csrr %0, mcycle" : "=r"(cyc_start));

    uint32_t instret_start;
    asm volatile ("csrr %0, minstret" : "=r"(instret_start));

    cnt_wr(CNT_BREADCRUMB, 2);

    while (1) {
        /* ---- pure polling: sample cnt20/cnt21 every iteration via AXI ---- */
        uint32_t cur_entry = cnt_val(CNT_MILESTONE_0)    & 0x7FFFFFFF;
        uint32_t cur_exit  = cnt_val(CNT_MILESTONE_EXIT) & 0x7FFFFFFF;

        if (cur_entry != prev_entry) {
            /* ---- ENTRY tick: snapshot mcycle + interval counter baselines ---- */
            asm volatile ("csrr %0, mcycle" : "=r"(mcycle_entry));
            llcrd_snap     = cnt_val(CNT_LLC_RD_C0)  & 0x7FFFFFFF;
            llcwr_snap     = cnt_val(CNT_LLC_WR_C0)  & 0x7FFFFFFF;
            memrd_snap     = cnt_val(CNT_MEM_RD_C0)  & 0x7FFFFFFF;
            memwr_snap     = cnt_val(CNT_MEM_WR_C0)  & 0x7FFFFFFF;
            l1miss_snap    = cnt_val(CNT_L1_MISS)    & 0x7FFFFFFF;
            stalls_snap    = cnt_val(CNT_STALLS)     & 0x7FFFFFFF;
            llcrd_lat_snap = cnt_val(CNT_LLC_RD_LAT) & 0x7FFFFFFF;
            llcwr_lat_snap = cnt_val(CNT_LLC_WR_LAT) & 0x7FFFFFFF;
            cnt_wr(CNT_BREADCRUMB, 10 + call_count);
            WRITE_MEM(DSPM_LOG_STATE, 1);
            prev_entry = cur_entry;
        }

        if (cur_exit != prev_exit) {
            /* ---- EXIT tick: compute deltas, write log record ---- */
            uint32_t mcycle_exit;
            asm volatile ("csrr %0, mcycle" : "=r"(mcycle_exit));

            uint32_t delta_cyc = mcycle_exit - mcycle_entry;
            uint32_t llc_rd    = (cnt_val(CNT_LLC_RD_C0)  & 0x7FFFFFFF) - llcrd_snap;
            uint32_t llc_wr    = (cnt_val(CNT_LLC_WR_C0)  & 0x7FFFFFFF) - llcwr_snap;
            uint32_t mem_rd    = (cnt_val(CNT_MEM_RD_C0)  & 0x7FFFFFFF) - memrd_snap;
            uint32_t mem_wr    = (cnt_val(CNT_MEM_WR_C0)  & 0x7FFFFFFF) - memwr_snap;
            uint32_t l1_miss   = (cnt_val(CNT_L1_MISS)    & 0x7FFFFFFF) - l1miss_snap;
            uint32_t stalls    = (cnt_val(CNT_STALLS)     & 0x7FFFFFFF) - stalls_snap;
            uint32_t llcrd_lat = (cnt_val(CNT_LLC_RD_LAT) & 0x7FFFFFFF) - llcrd_lat_snap;
            uint32_t llcwr_lat = (cnt_val(CNT_LLC_WR_LAT) & 0x7FFFFFFF) - llcwr_lat_snap;

            tot_cycles += delta_cyc;
            tot_l1miss += l1_miss;
            tot_stalls += stalls;
            tot_llcrd  += llc_rd;
            tot_llcwr  += llc_wr;
            tot_memrd  += mem_rd;
            tot_memwr  += mem_wr;
            tot_rlat   += llcrd_lat;
            tot_wlat   += llcwr_lat;

            WRITE_MEM(DSPM_LOG_TOTALS + 0,  tot_cycles);
            WRITE_MEM(DSPM_LOG_TOTALS + 4,  tot_l1miss);
            WRITE_MEM(DSPM_LOG_TOTALS + 8,  tot_stalls);
            WRITE_MEM(DSPM_LOG_TOTALS + 12, tot_llcrd);
            WRITE_MEM(DSPM_LOG_TOTALS + 16, tot_llcwr);
            WRITE_MEM(DSPM_LOG_TOTALS + 20, tot_memrd);
            WRITE_MEM(DSPM_LOG_TOTALS + 24, tot_memwr);
            WRITE_MEM(DSPM_LOG_TOTALS + 28, tot_rlat);
            WRITE_MEM(DSPM_LOG_TOTALS + 32, tot_wlat);

            if (call_count < MAX_LOG_RECORDS) {
                uint32_t rec = DSPM_LOG_RECORDS + call_count * RECORD_WORDS * 4;
                WRITE_MEM(rec + 0,  delta_cyc);
                WRITE_MEM(rec + 4,  l1_miss);
                WRITE_MEM(rec + 8,  stalls);
                WRITE_MEM(rec + 12, llc_rd);
                WRITE_MEM(rec + 16, llc_wr);
                WRITE_MEM(rec + 20, mem_rd);
                WRITE_MEM(rec + 24, mem_wr);
                WRITE_MEM(rec + 28, llcrd_lat);
                WRITE_MEM(rec + 32, llcwr_lat);
            }

            call_count++;
            WRITE_MEM(DSPM_LOG_CALL_COUNT, call_count);
            cnt_wr(CNT_BREADCRUMB, 100 + call_count);
            WRITE_MEM(DSPM_LOG_STATE, 0);
            prev_exit = cur_exit;
        }
    }
}
