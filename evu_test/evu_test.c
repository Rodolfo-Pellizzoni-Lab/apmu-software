/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 University of Waterloo
 */

/*
 * EVU smoke test (core 0 only, self-checking).
 *
 * Arms the core-0 EVU PC-track milestones on the entry and the first `ret` of evu_probe(),
 * routes the milestone hits to APMU counters 20 (entry) and 21 (exit) exactly as sad_profile
 * does, calls evu_probe() N_CALLS times and checks that both counters and the firmware's
 * per-call log (sad_profile/pmu_firmware, which samples the two counters and records the
 * cycles between entry and exit) all agree with N_CALLS.
 *
 * Two-pass build: the first link gives the addresses of evu_probe, the firmware is rebuilt
 * with them (it programs the PCTRACK registers, which only latch writes from the APMU side)
 * and the program is linked again. The program never embeds the addresses itself, so the
 * second link cannot move evu_probe; it checks the PCTRACK read-back against &evu_probe.
 *
 * Exit code 0 = PASS, 1 = FAIL (written to tohost, so the RTL simulation reports it).
 */

#include "encoding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "pmu_test_func.c"      /* ../sad_profile: read_32b/write_32b + APMU/EVU defines */

#ifndef N_CALLS
#define N_CALLS 16
#endif

/* DSPM protocol and log layout shared with sad_profile/pmu_firmware/main.c */
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)
#define DSPM_CMD_ADDR           (DSPM_BASE_ADDR + 0x80)
#define DSPM_LOG_BASE           (DSPM_BASE_ADDR + 0x2000)
#define DSPM_LOG_CALL_COUNT     (DSPM_LOG_BASE + 0x00)
#define DSPM_LOG_TOTALS         (DSPM_LOG_BASE + 0x08)
#define DSPM_LOG_RECORDS        (DSPM_LOG_BASE + 0x80)
#define RECORD_WORDS            9
#define STATUS_STARTED          0xBB000001
#define STATUS_ALL_IRQS_SENT    0xBB000099
#define CMD_TRIGGER_INTERRUPT   0x000000FF

#define CNT_MILESTONE_ENTRY     20
#define CNT_MILESTONE_EXIT      21
#define EVU_C0_EV1_SEL          0x001F0022UL
#define EVU_INFO_CFG(n)         ((1UL << 23) | ((uint32_t)(n) << 5) | (uint32_t)(n))

extern char _binary_text_section_bin_start[], _binary_text_section_bin_end[];
extern char _binary_data_rodata_bss_bin_start[], _binary_data_rodata_bss_bin_end[];

volatile uint32_t g_sink;

/* The profiled function: one basic loop, one return, so its first `ret` is its only exit. */
__attribute__((noinline, noclone))
uint32_t evu_probe(uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += i * 3 + (s >> 2);
    g_sink = s;
    return s;
}

static void copy_words(void *dst, const void *src, uint32_t size) {
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (uint32_t i = 0; i < (size + 3) / 4; i++)
        d[i] = s[i];
}

static uint32_t verify_words(const void *dst, const void *src, uint32_t size) {
    const volatile uint32_t *d = (const volatile uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < (size + 3) / 4; i++)
        if (d[i] != s[i]) {
            if (bad < 4) printf("    ISPM word %u: image 0x%08x, memory 0x%08x\r\n", i, s[i], d[i]);
            bad++;
        }
    return bad;
}

static int load_and_start_pmu(void) {
    uint32_t text_size = _binary_text_section_bin_end - _binary_text_section_bin_start;
    uint32_t data_size = _binary_data_rodata_bss_bin_end - _binary_data_rodata_bss_bin_start;

    write_32b(PMC_STATUS_ADDR, 1);                       /* halt the APMU core */
    asm volatile ("fence iorw, iorw" ::: "memory");
    write_32b(PERIOD_ADDR, 0xFFFFFFFF);                   /* free-running timer */
    write_32b(PERIOD_ADDR + 4, 0xFFFFFFFF);
    asm volatile ("fence iorw, iorw" ::: "memory");
    for (uint32_t i = 0; i < 0x200; i += 4)
        write_32b(DSPM_BASE_ADDR + i, 0);
    /* Word copies only: the ISPM/DSPM SRAMs index by byte address and ignore byte strobes, so the
       sub-word stores memcpy uses for a tail that is not 8-byte aligned land in the wrong entries
       (an image of 2292 bytes lost the upper 3 bytes of its last instruction). */
    copy_words((void *)ISPM_BASE_ADDR, _binary_text_section_bin_start, text_size);
    copy_words((void *)(DSPM_BASE_ADDR + 0x200), _binary_data_rodata_bss_bin_start, data_size);
    asm volatile ("fence iorw, iorw" ::: "memory");
    uint32_t bad = verify_words((void *)ISPM_BASE_ADDR, _binary_text_section_bin_start, text_size);
    printf("    ISPM read-back: %u of %u words differ from the image\r\n", bad, (text_size + 3) / 4);
    write_32b(PMC_BOOT_ADDR, ISPM_BASE_ADDR);
    write_32b(PMC_STATUS_ADDR, 0);
    asm volatile ("fence iorw, iorw" ::: "memory");

    uint32_t timeout = 100000;
    while (read_32b(DSPM_STATUS_ADDR) != STATUS_STARTED && timeout > 0) {
        timeout--;
        asm volatile ("nop");
    }
    printf("    firmware text=%u data=%u bytes, status=0x%08x\r\n",
           text_size, data_size, read_32b(DSPM_STATUS_ADDR));
    return timeout == 0 ? -1 : 0;
}

static void configure_evu(void) {
    /* EID2 = 0x17 (PC milestone hit) drives e_id[1]; the other event IDs stay off */
    write_32b(EVENTID_C0_BASE_ADDR + 0,  0x00);
    write_32b(EVENTID_C0_BASE_ADDR + 4,  0x00);
    write_32b(EVENTID_C0_BASE_ADDR + 8,  0x17);
    write_32b(EVENTID_C0_BASE_ADDR + 12, 0x00);
    asm volatile ("fence iorw, iorw" ::: "memory");
    /* counter n counts packets with e_id bit 1 set whose e_info[slot] is set: one per PCTRACK<slot> match */
    write_32b(EVENT_SEL_BASE_ADDR  + CNT_MILESTONE_ENTRY * COUNTER_BUNDLE_SIZE, EVU_C0_EV1_SEL);
    write_32b(EVENT_INFO_BASE_ADDR + CNT_MILESTONE_ENTRY * COUNTER_BUNDLE_SIZE, EVU_INFO_CFG(0));
    write_32b(COUNTER_BASE_ADDR    + CNT_MILESTONE_ENTRY * COUNTER_BUNDLE_SIZE, 0);
    write_32b(EVENT_SEL_BASE_ADDR  + CNT_MILESTONE_EXIT  * COUNTER_BUNDLE_SIZE, EVU_C0_EV1_SEL);
    write_32b(EVENT_INFO_BASE_ADDR + CNT_MILESTONE_EXIT  * COUNTER_BUNDLE_SIZE, EVU_INFO_CFG(1));
    write_32b(COUNTER_BASE_ADDR    + CNT_MILESTONE_EXIT  * COUNTER_BUNDLE_SIZE, 0);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

int main(int argc, char const *argv[]) {
    uint32_t mhartid;
    asm volatile ("csrr %0, 0xF14\n" : "=r" (mhartid));
    if (mhartid != 0) {
        while (1) asm volatile ("wfi");
    }

    #ifdef FPGA_EMULATION
    *(volatile uint8_t *)(0x4000000CUL) = 0x83;   /* 8N1, keep the loader's divisor */
    asm volatile ("fence iorw, iorw" ::: "memory");
    *(volatile uint8_t *)(0x4000000CUL) = 0x03;
    asm volatile ("fence iorw, iorw" ::: "memory");
    #else
    set_flls();
    uart_set_cfg(0, (50000000 / 115200) >> 4);
    #endif

    int fails = 0;
    printf("\r\n=== EVU PC-milestone smoke test: %u calls of evu_probe ===\r\n", (unsigned)N_CALLS);

    printf("[1] Loading and starting the APMU firmware\r\n");
    if (load_and_start_pmu() != 0) {
        printf("    FAIL: firmware did not report STATUS_STARTED\r\n");
        uart_wait_tx_done();
        return 1;
    }

    printf("[2] EVU configuration\r\n");
    configure_evu();
    uint32_t want_entry = (uint32_t)(uintptr_t)&evu_probe;
    uint32_t track0_lo = read_32b(PC_MILESTONE_C0_BASE_ADDR + 0);
    uint32_t track0_hi = read_32b(PC_MILESTONE_C0_BASE_ADDR + 4);
    uint32_t track1_lo = read_32b(PC_MILESTONE_C0_BASE_ADDR + 8);
    uint32_t track1_hi = read_32b(PC_MILESTONE_C0_BASE_ADDR + 12);
    printf("    &evu_probe = 0x%08x\r\n", want_entry);
    printf("    PCTRACK0 (firmware) = 0x%08x%08x  PCTRACK1 (firmware) = 0x%08x%08x\r\n",
           track0_hi, track0_lo, track1_hi, track1_lo);
    if (track0_lo != want_entry || track0_hi != 0) {
        printf("    FAIL: PCTRACK0 is not the entry of evu_probe (rebuild with `make two-pass`)\r\n");
        fails++;
    }
    if (track1_lo <= want_entry || track1_lo - want_entry > 0x200 || track1_hi != 0) {
        printf("    FAIL: PCTRACK1 is not inside evu_probe\r\n");
        fails++;
    }

    printf("[3] Running %u calls\r\n", (unsigned)N_CALLS);
    write_32b(DSPM_CMD_ADDR, CMD_TRIGGER_INTERRUPT);
    asm volatile ("fence iorw, iorw" ::: "memory");
    while (read_32b(DSPM_STATUS_ADDR) != STATUS_ALL_IRQS_SENT)
        asm volatile ("nop");
    uint32_t before_entry = read_32b(COUNTER_BASE_ADDR + CNT_MILESTONE_ENTRY * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
    uint32_t before_exit  = read_32b(COUNTER_BASE_ADDR + CNT_MILESTONE_EXIT  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
    printf("    firmware breadcrumb (cnt26) before the calls: %u (2 = polling loop entered)\r\n",
           read_32b(COUNTER_BASE_ADDR + 26 * COUNTER_BUNDLE_SIZE));

    uint32_t acc = 0;
    for (uint32_t i = 0; i < N_CALLS; i++)
        acc += evu_probe(64 + 16 * i);
    for (volatile uint32_t i = 0; i < 2000; i++) asm volatile ("nop");   /* let the firmware log the last exit */

    uint32_t cnt_entry  = read_32b(COUNTER_BASE_ADDR + CNT_MILESTONE_ENTRY * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
    uint32_t cnt_exit   = read_32b(COUNTER_BASE_ADDR + CNT_MILESTONE_EXIT  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
    uint32_t call_count = read_32b(DSPM_LOG_CALL_COUNT);
    uint32_t tot_cycles = read_32b(DSPM_LOG_TOTALS + 0);
    uint32_t breadcrumb = read_32b(COUNTER_BASE_ADDR + 26 * COUNTER_BUNDLE_SIZE);
    uint32_t log_state  = read_32b(DSPM_LOG_BASE + 0x04);
    write_32b(PMC_STATUS_ADDR, 1);                       /* halt the APMU core */

    printf("[4] Results (acc=%u)\r\n", acc);
    printf("    firmware breadcrumb (cnt26) after the calls: %u (10+n = entry seen, 100+n = exit seen), log state %u\r\n",
           breadcrumb, log_state);
    printf("    counter 20 (entry hits): %u before, %u after\r\n", before_entry, cnt_entry);
    printf("    counter 21 (exit hits):  %u before, %u after\r\n", before_exit, cnt_exit);
    printf("    firmware log: %u calls, %u cycles inside evu_probe in total\r\n", call_count, tot_cycles);
    printf("    call  cycles  l1dmiss  stalls  llc_rd  llc_wr  mem_rd  mem_wr\r\n");
    uint32_t zero_records = 0;
    for (uint32_t i = 0; i < call_count && i < N_CALLS; i++) {
        uint32_t rec = DSPM_LOG_RECORDS + i * RECORD_WORDS * 4;
        uint32_t cyc = read_32b(rec + 0);
        if (cyc == 0) zero_records++;
        printf("    %4u  %6u  %7u  %6u  %6u  %6u  %6u  %6u\r\n", i, cyc, read_32b(rec + 4),
               read_32b(rec + 8), read_32b(rec + 12), read_32b(rec + 16), read_32b(rec + 20), read_32b(rec + 24));
    }

    if (before_entry != 0 || before_exit != 0) { printf("    FAIL: counters not zero before the calls\r\n"); fails++; }
    if (cnt_entry != N_CALLS)  { printf("    FAIL: entry milestone counted %u, expected %u\r\n", cnt_entry, (unsigned)N_CALLS); fails++; }
    if (cnt_exit != N_CALLS)   { printf("    FAIL: exit milestone counted %u, expected %u\r\n", cnt_exit, (unsigned)N_CALLS); fails++; }
    if (call_count != N_CALLS) { printf("    FAIL: firmware logged %u calls, expected %u\r\n", call_count, (unsigned)N_CALLS); fails++; }
    if (zero_records)          { printf("    FAIL: %u per-call records with 0 cycles\r\n", zero_records); fails++; }

    printf("=== EVU TEST %s (%d failure%s) ===\r\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    uart_wait_tx_done();
    return fails ? 1 : 0;
}
