/*
 * PMU Interrupt Test with PLIC
 *
 * Test flow:
 *   1. CVA6 loads PMU firmware and starts PMU core
 *   2. CVA6 sets up PLIC for PMU interrupt and trap handler
 *   3. CVA6 configures PMU counter with OVERFLOW_EN
 *   4. CVA6 signals PMU via DSPM and does work in a loop
 *   5. PMU firmware waits for timeout, then triggers interrupt
 *   6. CVA6 trap handler fires and sets flag
 *   7. CVA6 main loop detects flag and reports success
 */

#include "encoding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "pmu_test_func.c"
// #include "fat16.h"      // not needed for LLC/MEM stages
// #include "fat_file.h"   // not needed for LLC/MEM stages
#include "sdvbs_common.h"
#include "sdvb/benchmarks/disparity/src/c/disparity_top.h"
#include "sdvb/benchmarks/stitch/src/c/stitch_top.h"
#include "sdvb/benchmarks/sift/src/c/sift_top.h"
#include "sdvb/benchmarks/mser/src/c/mser_top.h"
#include "iopmp.h"


// PLIC interrupt configuration for PMU (per-core)
// IRQ IDs: 156 = counter 0 (core 0), 157 = counter 1 (core 1), etc.
#define PMU_IRQ_BASE    156
#define PMU_IRQ_CORE(n) (PMU_IRQ_BASE + (n)) //mhartid is n
// PLIC contexts: M-mode contexts are hart * 2 (assuming M/S mode per hart)
#define PLIC_CONTEXT_CORE(n) ((n) * 2)

// Number of cores
#define NUM_CORES 4

// Cacheable memory base addresses per core (in LLC range 0x80000000-0xA0000000)
#define CORE0_DATA_BASE   0x81000000
#define CORE1_DATA_BASE   0x81100000
#define CORE2_DATA_BASE   0x81200000
#define CORE3_DATA_BASE   0x81300000
#define MEM_BASE          0x83100000
#define WORK_ARRAY_SIZE   256  // uint32_t elements per work iteration
#define NUM_ITERATIONS    100  // iterations of core_work for timing measurement

// Enables the LLC for 0x8000_0000 - 0xA000_0000
void enable_llc(void) {
    write_32b(0x1C + 0x1A106000, 0x83000000);
}

#define CACHE_LINE_SIZE 64

// Partition 16-way LLC: 4 ways per core
void partition_cache_4cores(void) {
    write_32b(0x10401040, 0xF0FF0FFF);
    write_32b(0x10401044, 0xFF0FFFF0);
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// DSPM communication addresses (must match firmware)
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)
#define DSPM_CMD_ADDR           (DSPM_BASE_ADDR + 0x80)
#define DSPM_CORES_READY_ADDR   (DSPM_BASE_ADDR + 0x100)  // Core 0 sets when setup done
#define DSPM_POLL_CYCLES_ADDR   (DSPM_BASE_ADDR + 0x180)  // PMU writes avg poll loop cycles here
#define DSPM_V_TRACE_ADDR       (DSPM_BASE_ADDR + 0x500)  // V trace buffer (must match firmware)
#define V_SAMPLE_INTERVAL       10
#define MAX_V_SAMPLES           400

// DSPM mempol log (must match firmware)
#define DSPM_LOG_BASE           (DSPM_BASE_ADDR + 0x2000)
#define DSPM_LOG_MAX_ENTRIES    10000

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
// Note: Use non-zero init to force .data section, then reinit in main()
volatile uint32_t g_paused[NUM_CORES] = {0xDEAD0001, 0xDEAD0001, 0xDEAD0001, 0xDEAD0001};
volatile uint32_t counter_val[NUM_CORES] = {0xDEAD0001, 0xDEAD0001, 0xDEAD0001, 0xDEAD0001};
volatile uint32_t g_interrupt_count[NUM_CORES] = {0xDEAD0002, 0xDEAD0002, 0xDEAD0002, 0xDEAD0002};
volatile uint32_t g_work_iterations[NUM_CORES] = {0xDEAD0003, 0xDEAD0003, 0xDEAD0003, 0xDEAD0003};
volatile uint64_t g_saved_mepc[NUM_CORES] = {0xDEAD0007, 0xDEAD0007, 0xDEAD0007, 0xDEAD0007};
volatile uint32_t g_core_done[NUM_CORES] = {0, 0, 0, 0};  // Set when core finishes work
volatile uint32_t g_interference_go = 0;  // Core 0 sets this to start interference on cores 1-3

// Debug state (shared)
volatile uint64_t g_mcause = 0xDEAD0004;
volatile uint64_t g_mepc = 0xDEAD0005;
volatile uint32_t g_claimed_irq = 0xDEAD0006;

// Interrupt timing distribution: record mcycle and type (pause/resume) for each IRQ
#define MAX_IRQ_LOG 5000
volatile uint32_t g_irq_log_cycle[NUM_CORES][MAX_IRQ_LOG];  // mcycle timestamp
volatile uint8_t  g_irq_log_type[NUM_CORES][MAX_IRQ_LOG];   // 0=pause, 1=resume
volatile uint32_t g_core0_start_mcycle = 0;  // mcycle when core 0 starts work

// UART delay helper
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

// Reset PMU counter values for a specific core (cnt 0-3: IRQ, 4-7: LLCr,
// 8-11: LLCw, 12-15: MEMr, 16-19: MEMw)
void reset_counters(uint32_t core) {
    // Only reset performance counters (4-7: LLCr, 8-11: LLCw, 12-15: MEMr, 16-19: MEMw).
    // Do NOT reset counter 0-3 (IRQ/ACK channel) — clearing it breaks the
    // PMU handshake protocol (CVA_ACK_MASK gets wiped).
    uint32_t offsets[4] = {4, 8, 12, 16};
    for (uint32_t g = 0; g < 4; g++) {
        write_32b(COUNTER_BASE_ADDR + (offsets[g] + core) * COUNTER_BUNDLE_SIZE, 0x00000000);
    }
    asm volatile ("fence iorw, iorw" ::: "memory");
}

// Read and print all PMU counter values for each core
// cnt 0-3: interrupt overflow, cnt 4-7: LLCr, cnt 8-11: LLCw,
// cnt 12-15: MEMr, cnt 16-19: MEMw
void print_all_counters(void) {
    printf("\r\n========================================\r\n");
    printf("PMU Counter Summary (per core)\r\n");
    printf("========================================\r\n");
    for (uint32_t core = 0; core < NUM_CORES; core++) {
        uint32_t irq_cnt  = read_32b(COUNTER_BASE_ADDR + core        * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t llc_rd   = read_32b(COUNTER_BASE_ADDR + (4  + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t llc_wr   = read_32b(COUNTER_BASE_ADDR + (8  + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t mem_rd   = read_32b(COUNTER_BASE_ADDR + (12 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t mem_wr   = read_32b(COUNTER_BASE_ADDR + (16 + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
        uint32_t L2mem = read_32b(DSPM_BASE_ADDR + 0x1000);
        printf("Core %u: IRQ_CNT=%u, LLC_RD=%u, LLC_WR=%u, MEM_RD=%u, MEM_WR=%u\r\n",
               core, irq_cnt, llc_rd, llc_wr, mem_rd, mem_wr);

        printf("READ FROM L2SPACE: x%x \r\n", L2mem);
    }
    printf("========================================\r\n");
}

// *********************************************************************
// Per-core work functions - each core accesses cacheable memory
// *********************************************************************

static uint32_t* const core_data_base[NUM_CORES] = {
    (uint32_t*)CORE0_DATA_BASE, (uint32_t*)CORE1_DATA_BASE,
    (uint32_t*)CORE2_DATA_BASE, (uint32_t*)CORE3_DATA_BASE
};

// 256KB / 64B cache line = 4096 cache lines per work iteration
#define WORK_READ_LINES   4096
#define LLC_READ_LINES    256    // 16KB — fits in per-core LLC partition (warm hits)
#define DRAM_READ_LINES   4096   // 256KB — exceeds LLC, forces DRAM
#define STAGE_ITERATIONS  10
#define TOTAL_STAGE_ITERS (STAGE_ITERATIONS * 3)

uint64_t read_pmu_timer(void) {
    uint64_t timer_val;
    uint32_t timer_low = read_32b(TIMER_ADDR);
    uint32_t timer_high = read_32b(TIMER_ADDR + 4);
    timer_val = ((uint64_t)timer_high << 32) | timer_low;
    return timer_val;
}

void prime_cache(uint32_t core){
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
}

void read_interference_cache(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < 4096; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    g_work_iterations[core]++;

}

void read_interference_mem(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < 8192; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    g_work_iterations[core]++;

}

void write_interference_cache(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    for (uint32_t i = 0; i < 4096; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
    g_work_iterations[core]++;
}

void write_interference_mem(uint32_t core) {
    volatile uint64_t *data = (volatile uint64_t*)core_data_base[core];
    for (uint32_t i = 0; i < 8192; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
    g_work_iterations[core]++;
}


void core0_work(void) {
    volatile uint64_t *data = (volatile uint64_t*)CORE0_DATA_BASE;
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    //g_work_iterations[0]++;
}

void core1_work(void) {
    volatile uint64_t *data = (volatile uint64_t*)CORE1_DATA_BASE;
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
       dummy =  data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    g_work_iterations[1]++;
}

void core2_work(void) {
    volatile uint64_t *data = (volatile uint64_t*)CORE2_DATA_BASE;
    volatile uint64_t dummy = 0; 
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    g_work_iterations[2]++;
}

void core3_work(void) {
    volatile uint64_t *data = (volatile uint64_t*)CORE3_DATA_BASE;
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < WORK_READ_LINES; i++)
        dummy = data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))];
    g_work_iterations[3]++;
}

// Array of work functions indexed by core ID
typedef void (*work_func_t)(void);
work_func_t core_work[NUM_CORES] = {core0_work, core1_work, core2_work, core3_work};

// Stage 1 & 3: small LLC-resident region — all hits, no DRAM traffic
void llc_stage_work(void) {
    volatile uint64_t *data = (volatile uint64_t *)CORE0_DATA_BASE;
    volatile uint64_t dummy = 0;
    for (uint32_t i = 0; i < LLC_READ_LINES*3; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
    (void)dummy;
}

// Stage 2: large region exceeding LLC partition — forces DRAM hits
void dram_stage_work(void) {
    volatile uint64_t *data = (volatile uint64_t *)
    MEM_BASE;  // Use separate base to ensure we exceed LLC and hit DRAM
    for (uint32_t i = 0; i < LLC_READ_LINES*3; i++)
        data[i * (CACHE_LINE_SIZE / sizeof(uint64_t))] = i;
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
// Interrupt Handler - called from trap vector
// crt.S does "jal x0, handle_trap" which doesn't save context or use mret.
// We need to save/restore registers ourselves and use mret.
// *********************************************************************
void __attribute__((naked, aligned(4))) handle_trap(void) {
    asm volatile (
        // Save caller-saved registers to stack
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

        // Call the actual handler
        "jal ra, handle_trap_c\n\t"

        // Restore registers
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

        // Return from trap
        "mret\n\t"
    );
}

// Actual C handler called from the naked wrapper
void handle_trap_c(void) {
    // Get current hart ID
    uint32_t mhartid;
    asm volatile ("csrr %0, mhartid" : "=r"(mhartid));

    //get counter OF address
    uint64_t counter_addr = COUNTER_BASE_ADDR + mhartid * COUNTER_BUNDLE_SIZE;
    uint32_t CVA_ACK_MASK = 0x00100000; //set to 1 when CVA does requested task
    uint32_t PMU_PR_MASK =  0x01000000; //1 = running, 0 = paused


    // Read mcause and mepc directly from CSR
    uint64_t mcause, mepc;
    asm volatile ("csrr %0, mcause" : "=r"(mcause));
    asm volatile ("csrr %0, mepc" : "=r"(mepc));

    g_mcause = mcause;
    g_mepc = mepc;

    // Check if this is an external interrupt (mcause bit 63 set + code 11)
    uint64_t is_interrupt = mcause >> 63;
    uint64_t code = mcause & 0x7FF;

    if (is_interrupt && code == 11) {
        // Machine external interrupt - claim from PLIC for this core's context
        uint32_t plic_context = PLIC_CONTEXT_CORE(mhartid);
        uint32_t claimed = plic_claim_msg(plic_context);
        g_claimed_irq = claimed;

        // Check if this is a PMU interrupt (IRQs 156-159 for cores 0-3)
        // Each core should only handle its own IRQ (IRQ = 156 + mhartid)
        uint32_t expected_irq = PMU_IRQ_CORE(mhartid);
        if (claimed == expected_irq) {
            // PMU interrupt for this core - toggle pause state
            uint32_t irq_idx = g_interrupt_count[mhartid];
            g_interrupt_count[mhartid]++;

            // Record timestamp and type for this interrupt
            uint32_t cyc;
            asm volatile ("csrr %0, mcycle" : "=r"(cyc));
            if (irq_idx < MAX_IRQ_LOG) {
                g_irq_log_cycle[mhartid][irq_idx] = cyc;
                g_irq_log_type[mhartid][irq_idx] = g_paused[mhartid] ? 1 : 0;  // 0=pause, 1=resume
            }
        
            //get latest counter value
            counter_val[mhartid] = read_32b(counter_addr);

            // PMU_PR_MASK=0 means pause command (PMU wrote 0x40000000)
            if ((counter_val[mhartid] & PMU_PR_MASK) == 0 && (counter_val[mhartid] & CVA_ACK_MASK) == 0) {

                g_saved_mepc[mhartid] = mepc;
                asm volatile ("csrw mepc, %0" :: "r"((uint64_t)&wfi_loop));
                g_paused[mhartid] = 1;
                write_32b(counter_addr, CVA_ACK_MASK); // ACK the pause command

            }

            // PMU_PR_MASK=1 means resume command (PMU wrote 0x41000000)
            else if ((counter_val[mhartid] & PMU_PR_MASK) == PMU_PR_MASK) {

                asm volatile ("csrw mepc, %0" :: "r"(g_saved_mepc[mhartid]));
                g_paused[mhartid] = 0;
                write_32b(counter_addr, CVA_ACK_MASK); // ACK the resume command

            }

            // if (!g_paused[mhartid]) {
            //     // PAUSE: Save where we were, redirect to WFI loop
            //     g_saved_mepc[mhartid] = mepc;
            //     asm volatile ("csrw mepc, %0" :: "r"((uint64_t)&wfi_loop));
            //     g_paused[mhartid] = 1;
            // } 
            
            
            // else {
            //     // RESUME: Restore original mepc to continue where we left off
            //     asm volatile ("csrw mepc, %0" :: "r"(g_saved_mepc[mhartid]));
            //     g_paused[mhartid] = 0;
            // }

            

            // Clear the counter overflow bit for this core's counter
            //write_32b(counter_addr, 0x00000000);

            // Complete the interrupt
            plic_complete_msg(plic_context, claimed);

        } else if (claimed != 0) {
            // Some other interrupt - complete it anyway
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

    // Set up mtvec to point directly to our handler (direct mode)
    uintptr_t mtvec_val = (uintptr_t)&handle_trap;
    asm volatile ("csrw mtvec, %0" :: "r"(mtvec_val));

    // Set priority and threshold FIRST so stale interrupts can be claimed
    // (priority must be > threshold for plic_claim_msg to return the IRQ)
    plic_set_priority(irq_id, 7);
    plic_set_ie(plic_context, irq_id, 1);
    plic_set_thresh(plic_context, 6);

    // Clear counter overflow BEFORE draining PLIC, so the overflow source
    // doesn't re-trigger immediately after we drain
    write_32b(COUNTER_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE, 0x00000000);
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Drain stale PLIC interrupts (priority=7 > threshold=6, claim works now)
    uint32_t stale_irq;
    do {
        stale_irq = plic_claim_msg(plic_context);
        if (stale_irq != 0)
            plic_complete_msg(plic_context, stale_irq);
    } while (stale_irq != 0);

    // Configure PMU counter for this core with overflow enabled
    uint64_t event_info_addr = EVENT_INFO_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE;
    uint64_t counter_addr = COUNTER_BASE_ADDR + core_id * COUNTER_BUNDLE_SIZE;
    write_32b(event_info_addr, OVERFLOW_EN);
    write_32b(counter_addr, 0x00100000);  // Init with CVA_ACK_MASK so PMU can send first command
    asm volatile ("fence iorw, iorw" ::: "memory");

    // Second drain: enabling OVERFLOW_EN above may re-trigger a stale overflow
    // that was latched in hardware (write_32b may not clear the hw overflow flag)
    do {
        stale_irq = plic_claim_msg(plic_context);
        if (stale_irq != 0)
            plic_complete_msg(plic_context, stale_irq);
    } while (stale_irq != 0);

    // Enable MIE LAST - after all stale interrupts are drained
    uint64_t mie;
    asm volatile ("csrr %0, mie" : "=r"(mie));
    mie |= (1 << 11);
    asm volatile ("csrw mie, %0" :: "r"(mie));

    uint64_t mstatus;
    asm volatile ("csrr %0, mstatus" : "=r"(mstatus));
    mstatus |= (1 << 3);
    asm volatile ("csrw mstatus, %0" :: "r"(mstatus));
}

// *********************************************************************
// Thread Entry - Override weak version to let all cores proceed to main()
// *********************************************************************
void thread_entry(int cid, int nc) {
    // Return immediately - all cores will proceed to main()
    // where we handle per-core logic with mhartid checks
    return;
}

// *********************************************************************
// Main Function
// *********************************************************************
int main(int argc, char const *argv[]) {

    uint32_t mhartid;
    asm volatile (
        "csrr %0, 0xF14\n"
        : "=r" (mhartid)
    );

    // *******************************************************************
    // Core 0
    // *******************************************************************
    if (mhartid == 0) {
        #ifdef FPGA_EMULATION
        // Match hello_culsans: only program LCR for 8N1, preserve the divisor
        // that was set by whoever loaded us (bootloader / OpenOCD). Reprogramming
        // DLL/DLM with a wrong test_freq assumption silences the UART.
        *(volatile uint8_t *)(0x4000000CUL) = 0x83;    // DLAB=1, 8N1 (byte write!)
        asm volatile ("fence iorw, iorw" ::: "memory");
        *(volatile uint8_t *)(0x4000000CUL) = 0x03;    // DLAB=0, 8N1 (byte write!)
        asm volatile ("fence iorw, iorw" ::: "memory");
        #else
        set_flls();
        uart_set_cfg(0, (50000000 / 115200) >> 4);
        #endif

        printf("\r\n");
        printf("========================================\r\n");
        printf("PMU Interrupt Test with PLIC\r\n");
        printf("========================================\r\n");

        // Explicitly reinitialize globals (in case BSS not cleared)
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

        printf("Testing PMU overflow interrupt\r\n");
        printf("========================================\r\n\r\n");

        // ************************************************************************
        // Load PMU Firmware
        // ************************************************************************
        printf("[1] Loading PMU firmware...\r\n");

        uint32_t text_size = _binary_text_section_bin_end - _binary_text_section_bin_start;
        uint32_t data_size = _binary_data_rodata_bss_bin_end - _binary_data_rodata_bss_bin_start;

        // Halt PMU
        write_32b(PMC_STATUS_ADDR, 1);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Configure PMU timer period to max so it free-runs
        write_32b(PERIOD_ADDR, 0xFFFFFFFF);
        write_32b(PERIOD_ADDR + 4, 0xFFFFFFFF);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Clear DSPM communication area
        for (uint32_t i = 0; i < 0x200; i += 4) {
            write_32b(DSPM_BASE_ADDR + i, 0);
        }

        // Load firmware
        if (text_size > 0 && text_size < 4096) {
            memcpy((void*)ISPM_BASE_ADDR, _binary_text_section_bin_start, text_size);
            memcpy((void*)(DSPM_BASE_ADDR + 0x200), _binary_data_rodata_bss_bin_start, data_size);
            asm volatile ("fence iorw, iorw" ::: "memory");
            printf("    Firmware loaded (text=%u, data=%u bytes)\r\n", text_size, data_size);
        } else {
            printf("    ERROR: Invalid firmware size (text=%u)\r\n", text_size);
            while(1) asm volatile ("wfi");
        }

        uint16_t  md = 0;
        uintptr_t start_raddr = 0x1C000000;

        // Configure IOPMP
        enable_iopmp();
        mdcfg_entry_config(5, 0);
        srcmd_entry_config(&md, 1, 0, 0);

        // Put 1 TOR entry allowing reads and writes
        //set_entry_off(start_raddr, ACCESS_NONE, 0);
        set_entry_tor(start_raddr + 64, ACCESS_READ | ACCESS_WRITE, 1);


        // Start PMU
        write_32b(PMC_BOOT_ADDR, ISPM_BASE_ADDR);
        write_32b(PMC_STATUS_ADDR, 0);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Wait for PMU to start
        printf("[2] Waiting for PMU to start...\r\n");
        uint32_t timeout = 100000;
        while (read_32b(DSPM_STATUS_ADDR) != STATUS_STARTED && timeout > 0) {
            timeout--;
            asm volatile ("nop");
        }
        if (timeout == 0) {
            printf("    TIMEOUT waiting for PMU (status=0x%08x)\r\n", read_32b(DSPM_STATUS_ADDR));
            while(1) asm volatile ("wfi");
        }
        printf("    PMU started!\r\n");

        // ************************************************************************
        // Trap Vector, PLIC, and PMU Counter Configuration for all cores
        // ************************************************************************
        printf("[3] Configuring trap vector, PLIC, and PMU counters for all cores...\r\n");

        // Print stale counter state from previous run (before any setup)
        printf("    Pre-setup counter state:\r\n");
        for (uint32_t core = 0; core < NUM_CORES; core++) {
            uint32_t cnt_val = read_32b(COUNTER_BASE_ADDR + core * COUNTER_BUNDLE_SIZE);
            printf("    Core %u: cnt=0x%08x\r\n", core, cnt_val);
        }
        uart_wait_tx_done();

        for (uint32_t core = 0; core < NUM_CORES; core++) {
            // Set PLIC priority for each core's IRQ
            plic_set_priority(PMU_IRQ_CORE(core), 1);
            printf("    Core %u: IRQ %u, context %u, counter at 0x%08x\r\n",
                   core, PMU_IRQ_CORE(core), PLIC_CONTEXT_CORE(core),
                   (uint32_t)(COUNTER_BASE_ADDR + core * COUNTER_BUNDLE_SIZE));
        }

        // Core 0 sets up its own interrupt handling
        setup_core_interrupt(0);
        printf("    Core 0 interrupt handling configured\r\n");

        // Signal other cores that setup is complete - they can now configure themselves
        write_32b(DSPM_CORES_READY_ADDR, 0xCAFE0001);
        asm volatile ("fence iorw, iorw" ::: "memory");
        printf("    Signaled other cores to start setup\r\n");

        // Wait for all other cores to finish their setup
        printf("    Waiting for cores 1-3 to complete setup...\r\n");
        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint32_t timeout = 1000000;
            while (read_32b(DSPM_BASE_ADDR + 0x24 + core * 0x4) != 1 && timeout > 0) {
                timeout--;
                asm volatile ("nop");
            }
            if (timeout == 0) {
                printf("    WARNING: Core %u did not signal ready\r\n", core);
            }
        }
        printf("    All cores ready!\r\n");

        // ************************************************************************
        // Enable LLC and configure counters 4-7 for LLC_RD_REQ per core
        // ************************************************************************
        enable_llc();
        partition_cache_4cores();
        printf("    LLC enabled + partitioned (4 ways/core, 16-way)\r\n");

        // cnt 0-3 send interrupts to cores 0-3
        // cnt 4-7 -> LLCr
        // cnt 8-11 -> LLCw
        // cnt 12-15 -> MEMr
        // cnt 16-19 -> MEMw
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


        for (uint32_t g = 0; g < 4; g++) {
            for (uint32_t core = 0; core < NUM_CORES; core++) {
                uint32_t cnt_idx = group_base[g] + core;
                write_32b(EVENT_SEL_BASE_ADDR  + cnt_idx * COUNTER_BUNDLE_SIZE, event_groups[g][core]);
                write_32b(EVENT_INFO_BASE_ADDR + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
                write_32b(COUNTER_BASE_ADDR    + cnt_idx * COUNTER_BUNDLE_SIZE, 0);
            }
        }
        printf("    Counters 4-7: LLC_RD, 8-11: LLC_WR, 12-15: MEM_RD, 16-19: MEM_WR per core\r\n");

        // FAT16 mount removed — not needed for LLC/MEM stages

        // ************************************************************************
        // Setup phase: prime caches and start interference BEFORE PMU policing
        // ************************************************************************
        printf("[4] Priming caches and starting interference...\r\n");
        uart_wait_tx_done();

        // Prime caches BEFORE PMU starts (avoids policing during setup)
        prime_cache(1);
        prime_cache(2);
        prime_cache(3);
        prime_cache(0);

        // Reset performance counters so PMU starts with clean measurements
        for (uint32_t core = 0; core < NUM_CORES; core++)
            reset_counters(core);

        // Signal cores 1-3 to start interference work
        g_interference_go = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Brief delay for cores 1-3 to begin their work loops
        for (volatile uint32_t i = 0; i < 1000; i++) asm volatile ("nop"); //SIM CHANGE

        // NOW signal PMU to start policing (all setup done, clean counters)
        printf("    Signaling PMU to start policing...\r\n");
        uart_wait_tx_done();
        write_32b(DSPM_CMD_ADDR, CMD_TRIGGER_INTERRUPT);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Record mcycle baseline for interrupt timing CSV
        asm volatile ("csrr %0, mcycle" : "=r"(g_core0_start_mcycle));
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Buffers for deferred CSV print — filled during work, printed after 15s dump
        uint32_t buf_llcrd[TOTAL_STAGE_ITERS], buf_llcwr[TOTAL_STAGE_ITERS];
        uint32_t buf_memrd[TOTAL_STAGE_ITERS], buf_memwr[TOTAL_STAGE_ITERS];
        uint32_t buf_irqs[TOTAL_STAGE_ITERS],  buf_cyc[TOTAL_STAGE_ITERS];
        uint8_t  buf_stage[TOTAL_STAGE_ITERS]; // 0=LLC, 1=DRAM, 2=LLC2

        uint32_t global_iter = 0;
        uint64_t t_s1, t_s1_end, t_s2, t_s2_end, t_s3, t_s3_end;

        // Stage 1: LLC — warm hits, mempol idle
        t_s1 = read_pmu_timer();
        for (uint32_t i = 0; i < STAGE_ITERATIONS; i++, global_iter++) {
            reset_counters(0);
            uint64_t t0 = read_pmu_timer();
            llc_stage_work();
            uint64_t t1 = read_pmu_timer();
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_irqs[global_iter]  = g_interrupt_count[0];
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 0;
        }
        t_s1_end = read_pmu_timer();

        // Stage 2: DRAM — exceeds LLC, MEM_RD spikes, mempol regulates
        t_s2 = read_pmu_timer();
        for (uint32_t i = 0; i < STAGE_ITERATIONS; i++, global_iter++) {
            reset_counters(0);
            uint64_t t0 = read_pmu_timer();
            dram_stage_work();
            uint64_t t1 = read_pmu_timer();
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_irqs[global_iter]  = g_interrupt_count[0];
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 1;
        }
        t_s2_end = read_pmu_timer();

        // Stage 3: LLC — back to warm hits, regulation subsides
        t_s3 = read_pmu_timer();
        for (uint32_t i = 0; i < STAGE_ITERATIONS; i++, global_iter++) {
            reset_counters(0);
            uint64_t t0 = read_pmu_timer();
            llc_stage_work();
            uint64_t t1 = read_pmu_timer();
            buf_llcrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 4  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_llcwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 8  * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memrd[global_iter] = read_32b(COUNTER_BASE_ADDR + 12 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_memwr[global_iter] = read_32b(COUNTER_BASE_ADDR + 16 * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
            buf_irqs[global_iter]  = g_interrupt_count[0];
            buf_cyc[global_iter]   = (uint32_t)(t1 - t0);
            buf_stage[global_iter] = 2;
        }
        t_s3_end = read_pmu_timer();


        // Core 0 is done - signal completion
        g_core_done[0] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        printf("\r\n[5a] Core 0 work done. Paused state: C1=%u C2=%u C3=%u\r\n",
               g_paused[1], g_paused[2], g_paused[3]);
        uart_wait_tx_done();

        // Halt PMU so it stops sending interrupts
        write_32b(PMC_STATUS_ADDR, 1);
        asm volatile ("fence iorw, iorw" ::: "memory");
        printf("[5b] PMU halted\r\n");
        uart_wait_tx_done();

        // Resume any paused cores so they can see g_core_done and exit
        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint64_t core_counter_addr = COUNTER_BASE_ADDR + core * COUNTER_BUNDLE_SIZE;
            uint32_t cnt_before = read_32b(core_counter_addr);
            if (g_paused[core]) {
                write_32b(core_counter_addr, 0x41000000);  // overflow + PMU_PR_MASK = resume
                printf("[5c] Core %u: PAUSED, cnt=0x%08x -> wrote resume\r\n", core, cnt_before);
            } else {
                printf("[5c] Core %u: WORKING, cnt=0x%08x\r\n", core, cnt_before);
            }
            uart_wait_tx_done();
        }

        // ************************************************************************
        // Wait for ALL cores to finish before reporting results
        // ************************************************************************
        printf("[5d] Waiting for cores 1-3 to signal done...\r\n");
        uart_wait_tx_done();

        for (uint32_t core = 1; core < NUM_CORES; core++) {
            uint32_t timeout = 1000; // _CHANGED FOR SIM, SHOULD BE HIGHER
            while (!g_core_done[core] && timeout > 0) {
                timeout--;
                asm volatile ("nop");
            }
            if (timeout == 0) {
                printf("    WARNING: Core %u did not finish (paused=%u, cnt=0x%08x)\r\n",
                       core, g_paused[core],
                       read_32b(COUNTER_BASE_ADDR + core * COUNTER_BUNDLE_SIZE));
            } else {
                printf("    Core %u done\r\n", core);
            }
            uart_wait_tx_done();
        }


        printf("\r\n[6] All cores finished!\r\n");
        printf("    Per-core results:\r\n");
        for (uint32_t core = 0; core < NUM_CORES; core++) {
            printf("    Core %u: interrupts=%u, work_iters=%u, state=%s\r\n",
                   core, g_interrupt_count[core], g_work_iterations[core],
                   g_paused[core] ? "PAUSED" : "WORKING");
        }
        printf("    Last mcause: 0x%08x%08x\r\n", (uint32_t)(g_mcause >> 32), (uint32_t)g_mcause);
        printf("    PMU status: 0x%08x\r\n", read_32b(DSPM_STATUS_ADDR));

        printf("\r\n--- Stage timing summary ---\r\n");
        printf("  Stage 1 (LLC):  %u cycles total, %u avg/iter\r\n",
               (uint32_t)(t_s1_end - t_s1), (uint32_t)(t_s1_end - t_s1) / STAGE_ITERATIONS);
        printf("  Stage 2 (DRAM): %u cycles total, %u avg/iter  [mempol active]\r\n",
               (uint32_t)(t_s2_end - t_s2), (uint32_t)(t_s2_end - t_s2) / STAGE_ITERATIONS);
        printf("  Stage 3 (LLC):  %u cycles total, %u avg/iter\r\n",
               (uint32_t)(t_s3_end - t_s3), (uint32_t)(t_s3_end - t_s3) / STAGE_ITERATIONS);

        // Wait for PMU to complete calibration iteration
        for (volatile uint32_t i = 0; i < 1000; i++) asm volatile ("nop");

        // cnt20=poll iterations, cnt21-24=IRQs sent to cores 0-3
        uint32_t poll_iters = read_32b(COUNTER_BASE_ADDR + 20 * COUNTER_BUNDLE_SIZE);
        uint32_t irqs_c0    = read_32b(COUNTER_BASE_ADDR + 21 * COUNTER_BUNDLE_SIZE);
        uint32_t irqs_c1    = read_32b(COUNTER_BASE_ADDR + 22 * COUNTER_BUNDLE_SIZE);
        uint32_t irqs_c2    = read_32b(COUNTER_BASE_ADDR + 23 * COUNTER_BUNDLE_SIZE);
        uint32_t irqs_c3    = read_32b(COUNTER_BASE_ADDR + 24 * COUNTER_BUNDLE_SIZE);
        uint32_t pmu_total_cyc    = read_32b(COUNTER_BASE_ADDR + 25 * COUNTER_BUNDLE_SIZE);
        uint32_t breadcrumb       = read_32b(COUNTER_BASE_ADDR + 26 * COUNTER_BUNDLE_SIZE);
        uint32_t max_work_cycles  = read_32b(COUNTER_BASE_ADDR + 27 * COUNTER_BUNDLE_SIZE);
        printf("    PMU debug breadcrumb: %u\r\n", breadcrumb);
        printf("    PMU poll iterations: %u\r\n", poll_iters);
        printf("    PMU total cycles: %u\r\n", pmu_total_cyc);
        if (poll_iters > 0)
            printf("    PMU avg cycles/poll loop: %u\r\n", pmu_total_cyc / poll_iters);
        printf("    PMU max work cycles (before padding): %u\r\n", max_work_cycles);
        printf("    PMU IRQs sent: Core0=%u, Core1=%u, Core2=%u, Core3=%u\r\n",
               irqs_c0, irqs_c1, irqs_c2, irqs_c3);
        printf("    CVA6 IRQs recv: Core0=%u, Core1=%u, Core2=%u, Core3=%u\r\n",
               g_interrupt_count[0], g_interrupt_count[1], g_interrupt_count[2], g_interrupt_count[3]);

        uint32_t throttled_c0 = read_32b(COUNTER_BASE_ADDR + 28 * COUNTER_BUNDLE_SIZE);
        uint32_t throttled_c1 = read_32b(COUNTER_BASE_ADDR + 29 * COUNTER_BUNDLE_SIZE);
        uint32_t throttled_c2 = read_32b(COUNTER_BASE_ADDR + 30 * COUNTER_BUNDLE_SIZE);
        uint32_t throttled_c3 = read_32b(COUNTER_BASE_ADDR + 31 * COUNTER_BUNDLE_SIZE);
        printf("    Throttled state: Core0=%u, Core1=%u, Core2=%u, Core3=%u\r\n",
               throttled_c0, throttled_c1, throttled_c2, throttled_c3);

        print_all_counters();

        if (0) { // Interrupt tracking CSV dump
        printf("Going to dump in 15s... \r\n");

        //Clock speed is 25MHz, so 10s = 250 million cycles, which should be enough to capture the logged interrupts
        for (volatile uint32_t i = 0; i < 250000000/10; i++) asm volatile ("nop");

        uart_wait_tx_done();
            printf("\r\ntype,core,mcycle\r\n");
            // Merge-sort across cores using per-core indices
            uint32_t ci[NUM_CORES] = {0, 0, 0, 0};
            uint32_t counts[NUM_CORES];
            for (uint32_t c = 0; c < NUM_CORES; c++) {
                counts[c] = g_interrupt_count[c];
                if (counts[c] > MAX_IRQ_LOG) counts[c] = MAX_IRQ_LOG;
            }
            while (1) {
                // Find core with smallest mcycle among remaining entries
                uint32_t best_core = NUM_CORES;
                uint32_t best_cyc = 0xFFFFFFFF;
                for (uint32_t c = 0; c < NUM_CORES; c++) {
                    if (ci[c] < counts[c] && g_irq_log_cycle[c][ci[c]] < best_cyc) {
                        best_cyc = g_irq_log_cycle[c][ci[c]];
                        best_core = c;
                    }
                }
                if (best_core == NUM_CORES) break;
                if (best_cyc >= g_core0_start_mcycle) {
                    printf("%s,%u,%u\r\n",
                           g_irq_log_type[best_core][ci[best_core]] ? "R" : "P",
                           best_core,
                           best_cyc);
                }
                ci[best_core]++;
            }
            printf("\r\n");
        } // end if (0)

        // Dump buffered CVA6 per-stage iterations
        static const char * const stage_names[] = {"LLC", "DRAM", "LLC2"};
        printf("\r\niter,stage,LLC_RD,LLC_WR,MEM_RD,MEM_WR,IRQs,cycles\r\n");
        for (uint32_t r = 0; r < TOTAL_STAGE_ITERS; r++) {
            printf("%u,%s,%u,%u,%u,%u,%u,%u\r\n",
                r, stage_names[buf_stage[r]],
                buf_llcrd[r], buf_llcwr[r], buf_memrd[r], buf_memwr[r],
                buf_irqs[r], buf_cyc[r]);
            uart_wait_tx_done();
        }

        // Dump V trace (sampled every 10 iterations)
        // uint32_t num_samples = poll_iters / V_SAMPLE_INTERVAL;
        // if (num_samples > MAX_V_SAMPLES) num_samples = MAX_V_SAMPLES;
        // printf("\r\nV trace (%u samples, every %u iters):\r\n", num_samples, V_SAMPLE_INTERVAL);
        // printf("iter,V_c1,V_c2,V_c3\r\n");
        // for (uint32_t s = 0; s < num_samples; s++) {
        //     int32_t v1 = (int32_t)read_32b(DSPM_V_TRACE_ADDR + (s * 3 + 0) * 4);
        //     int32_t v2 = (int32_t)read_32b(DSPM_V_TRACE_ADDR + (s * 3 + 1) * 4);
        //     int32_t v3 = (int32_t)read_32b(DSPM_V_TRACE_ADDR + (s * 3 + 2) * 4);
        //     printf("%u,%d,%d,%d\r\n", (s + 1) * V_SAMPLE_INTERVAL, v1, v2, v3);
        // }

        // Dump DSPM mempol log as CSV
        printf("Dumping mempol log in 15s...\r\n");
        uart_wait_tx_done();
        for (volatile uint32_t i = 0; i < 100000000; i++) asm volatile ("nop");

        uint32_t log_entries = read_32b(DSPM_LOG_BASE);
        if (log_entries > DSPM_LOG_MAX_ENTRIES) log_entries = DSPM_LOG_MAX_ENTRIES;
        printf("\r\niter,llc_rd,llc_wr,mem_rd,mem_wr,decision,token\r\n");
        for (uint32_t i = 0; i < log_entries; i++) {
            uint32_t w0 = read_32b(DSPM_LOG_BASE + 4 + i * 12);
            uint32_t w1 = read_32b(DSPM_LOG_BASE + 4 + i * 12 + 4);
            //uint32_t w2 = read_32b(DSPM_LOG_BASE + 4 + i * 12 + 8);
            uint32_t lr = (w0 >> 23) & 0x1FF;
            uint32_t lw = (w0 >> 14) & 0x1FF;
            uint32_t mr = (w0 >> 7) & 0x7F;
            uint32_t mw = w0 & 0x7F;
            uint32_t dec = (w1 >> 30) & 0x3;
            int32_t tok = (int32_t)((w1 & 0x3FFFFFFF) | ((w1 & 0x20000000) ? 0xC0000000 : 0));
            const char *dec_str = (dec == 1) ? "H" : (dec == 2) ? "R" : "-";
            printf("%u,%u,%u,%u,%u,%s,%d\r\n", i, lr, lw, mr, mw, dec_str, tok);
            if ((i & 0xFF) == 0xFF) uart_wait_tx_done();
        }
        printf("--- end log (%u entries) ---\r\n", log_entries);
        uart_wait_tx_done();

    // *******************************************************************
    // Cores 1-3: Run same work loop with their own interrupt handling
    // *******************************************************************
    } else if (
        mhartid == 1  
       || mhartid == 2 
       || mhartid == 3
        ) {
        // Debug: print that this core is alive
        printf("    [Core %u] Started, waiting for signal...\r\n", mhartid);
        uart_wait_tx_done();

        // Two-phase wait: first ensure Core 0 has cleared DSPM (stale value from
        // previous run), then wait for the fresh ready signal from this run.
        while (read_32b(DSPM_CORES_READY_ADDR) == 0xCAFE0001) {
            asm volatile ("nop");  // wait for Core 0 to clear stale value
        }
        while (read_32b(DSPM_CORES_READY_ADDR) != 0xCAFE0001) {
            asm volatile ("nop");  // wait for Core 0's fresh ready signal
        }

        printf("    [Core %u] Got signal, setting up interrupts...\r\n", mhartid);
        uart_wait_tx_done();

        // Set up this core's interrupt handling and PMU counter
        setup_core_interrupt(mhartid);

        // Signal ready
        write_32b(DSPM_BASE_ADDR + 0x24 + mhartid * 0x4, 1);
        asm volatile ("fence iorw, iorw" ::: "memory");

        printf("    [Core %u] Setup complete, entering work loop\r\n", mhartid);
        uart_wait_tx_done();

        // Wait for core 0 to signal interference start
        while (!g_interference_go) {
            asm volatile ("nop");
        }

        printf("    [Core %u] Starting interference work\r\n", mhartid);
        uart_wait_tx_done();

        // Run interference until core 0 finishes
        while (!g_core_done[0]) {
            //core_work[mhartid]();

            //read_interference_cache(mhartid);
            //read_interference_mem(mhartid);

            //write_interference_mem(mhartid);
            //write_interference_cache(mhartid);
        }

        // Clean up: disable all interrupts so core is inert for debugger
        asm volatile ("csrw mie, zero");     // Disable all interrupt sources
        asm volatile ("csrc mstatus, %0" :: "r"(1 << 3));  // Clear mstatus.MIE

        // Clear counter overflow to prevent stale interrupts on next run
        write_32b(COUNTER_BASE_ADDR + mhartid * COUNTER_BUNDLE_SIZE, 0x00000000);
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Drain all pending PLIC interrupts for this core
        uint32_t plic_ctx = PLIC_CONTEXT_CORE(mhartid);
        uint32_t stale;
        do {
            stale = plic_claim_msg(plic_ctx);
            if (stale != 0)
                plic_complete_msg(plic_ctx, stale);
        } while (stale != 0);

        g_core_done[mhartid] = 1;
        asm volatile ("fence iorw, iorw" ::: "memory");

        // Idle cleanly
        while(1) asm volatile ("wfi");

    } else {
        end_test(mhartid);
        uart_wait_tx_done();
        //wfi 
        while(1) asm volatile ("wfi");
    }

    return 0;
}
