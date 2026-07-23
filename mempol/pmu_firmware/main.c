/*
 * PMU Firmware - Mempol implementation
 
 PMU here uses a modified mempol algorithm to police
 cores 1-3, such that they don't interfere with core 0

 This is done with a token bucket, where every polling 
 period, P, each core is allocated a budget Aj, where j 
 is core number 1-3.

 Vj represents the value of the bandwidth used by core j 
 in the current polling period. This value is the sum of 
 the weighted values of the counters:

 Vj = aLLCr * LLCr + aLLCw * LLCw + aMEMr* MEMr + aMEMw * MEMw

 The token bucket for core j, Tj, is then allocated it's budget,
 Aj. After this allocation, Tj is subtracted by Vj. When Tj is 
 less than 0, core j has used all of it's available allocation, 
 and is throttled until Tj is allocated enough budget to 
 become positive.

 *   cnt.wr: .word 0x00e79007  (a5=counter_idx, a4=value)
 */

#include <stddef.h>
#include <stdint.h>
#include "include/pmu_hw_desc.h"

// DSPM communication addresses
#define DSPM_STATUS_ADDR        (DSPM_BASE_ADDR + 0x00)
#define DSPM_CMD_ADDR           (DSPM_BASE_ADDR + 0x80)
#define DSPM_POLL_CYCLES_ADDR   (DSPM_BASE_ADDR + 0x180)
// V trace buffer: stores V for cores 1-3 every 10 iterations
#define DSPM_V_TRACE_ADDR       (DSPM_BASE_ADDR + 0x1000)
#define V_SAMPLE_INTERVAL       10
#define MAX_V_SAMPLES           400

// DSPM mempol log: 3 words per polling iteration for core 0
#define DSPM_LOG_BASE           (DSPM_BASE_ADDR + 0x2000)
#define DSPM_LOG_MAX_ENTRIES    10000

// Status codes
#define STATUS_STARTED          0xBB000001
#define STATUS_SENDING_IRQS     0xBB000010
#define STATUS_ALL_IRQS_SENT    0xBB000099

// Number of cores and interrupts per core
#define NUM_CORES               4
#define NUM_INTERRUPTS          6    // Per core (3 pause/resume cycles)
#define DELAY_BETWEEN_IRQS      300

// Command codes
#define CMD_NONE                0x00000000
#define CMD_TRIGGER_INTERRUPT   0x000000FF

// Memory access macros
#define WRITE_MEM(addr, value)  (*((volatile uint32_t*)(addr)) = (value))
#define READ_MEM(addr)          (*((volatile uint32_t*)(addr)))


// =====================================================================
// MEMPOL PARAMETERS - All configurable
// =====================================================================
#define NUM_POLICED_CORES 4  // Policing cores 0-3

// Counter index bases (20 counters total)
// cnt 0-3:   interrupt overflow triggers (cores 0-3)
// cnt 4-7:   LLC read requests (cores 0-3)
// cnt 8-11:  LLC write requests (cores 0-3)
// cnt 12-15: MEM read requests (cores 0-3)
// cnt 16-19: MEM write requests (cores 0-3)
// cnt 20:    PMU poll loop period in cycles (written by firmware)
#define CNT_IRQ_BASE      0
#define CNT_LLC_RD_BASE   4
#define CNT_LLC_WR_BASE   8
#define CNT_MEM_RD_BASE   12
#define CNT_MEM_WR_BASE   16
#define CNT_POLL_PERIOD   20
// cnt 21-24: IRQ send count for cores 0-3 (written by firmware)
#define CNT_IRQ_SENT_BASE 21
// cnt 25: cumulative mcycle total across all poll iterations (written by firmware)
#define CNT_TOTAL_CYCLES  25
// cnt 26: debug breadcrumb (written by firmware)
#define CNT_BREADCRUMB    26
// cnt 27: max PMU work cycles before padding (written by firmware)
#define CNT_MAX_WORK_CYCLES 27
// cnt 28-31: throttled state per core (written by firmware, readable by CVA6)
#define CNT_THROTTLED_BASE  28

// Polling period P, this is num of clock cycles in the main polling loop, calculated
static volatile uint32_t poll_period = 2000;

uint32_t CVA_ACK_MASK = 0x00100000; //set to 1 when CVA does requested task
uint32_t PMU_PR_MASK =  0x01000000; //1 = running, 0 = paused

volatile int32_t B = 86; // 10k * 0.0086 inst/ 1 clock cycles


// Budget allocation Aj per period for each policed core
// Index 0 = core 0, index 1 = core 1, index 2 = core 2, index 3 = core 3
volatile int32_t budget[NUM_POLICED_CORES] = {86, 86, 86, 86};  // 10k * 0.0086 inst/ clock cycle

// Bandwidth weights for V calculation:
// Vj = aLLCr * LLCr + aLLCw * LLCw + aMEMr * MEMr + aMEMw * MEMw
volatile uint32_t aLLCr = 10000;
volatile uint32_t aLLCw = 0;
volatile uint32_t aMEMr = 6120;
volatile uint32_t aMEMw = 4400;

uint32_t P = 2000;

// Token bucket cap multiplier: cap = w * P * budget 
volatile uint32_t w = 2;

// Token buckets Tj (signed to allow negative values for over-budget detection)
static int32_t token[NUM_POLICED_CORES] = {0, 0, 0, 0};

// Throttle state now stored in hardware counters CNT_THROTTLED_BASE + core
// (0 = running, 1 = throttled) — readable by CVA6 via memory-mapped counters


/*
 * Write value to counter using custom instruction
 * cnt.wr: opcode=0x07, funct3=1, rd=x0, rs1=counter_idx (a5), rs2=value (a4)
 * Encoding: 0000000 | 01110 | 01111 | 001 | 00000 | 0000111 = 0x00e79007
 */
static inline void cnt_wr(uint32_t counter_idx, uint32_t value) {
    asm volatile(
        "mv a5, %0\n\t"            // a5 = counter_idx
        "mv a4, %1\n\t"            // a4 = value
        ".word 0x00e79007\n\t"     // cnt.wr a5, a4
        :
        : "r" (counter_idx), "r" (value)
        : "a4", "a5"
    );
}

/*
 * Read value from counter using custom instruction
 * cnt.rd: opcode=0x07, funct3=0, rd=x14(a4), rs1=x15(a5), rs2=x0
 * Encoding: 0000000 | 00000 | 01111 | 000 | 01110 | 0000111 = 0x00078707
 */
static inline uint32_t cnt_rd(uint32_t counter_idx) {
    uint32_t value;
    asm volatile(
        "mv a5, %1\n\t"            // a5 = counter_idx
        ".word 0x00078707\n\t"     // cnt.rd a4, a5
        "mv %0, a4\n\t"            // value = a4
        : "=r" (value)
        : "r" (counter_idx)
        : "a4", "a5"
    );
    return value;
}

//#define DSPM_DUMP_ADDR (DSPM_BASE_ADDR + 0x1000); //should give us arounf 12KB
// DSPM Mem Format: [31-0]: [31]-HALT/RESUME DECISION HAPPENS, [30-24]-MEMw, [23]- 1/0 HALT/RESUME, [22-16] MEMr. [15-8] LLCw, [7-0] LLCr
// Basically every byte

  

int main() {
    // Startup NOPs
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");
    asm volatile ("nop");

    WRITE_MEM(0x10429000, 0xAAAABBBB);


    // Enable mcycle counter (clear mcountinhibit bit 0)
    asm volatile ("csrw 0x320, zero");

    // Clear command
    WRITE_MEM(DSPM_CMD_ADDR, CMD_NONE);

    // Clear overflow hardware for all IRQ counters at boot, BEFORE signaling
    // STATUS_STARTED. Only cnt_wr clears the hw overflow latch — write_32b
    // from CVA6 cannot. CVA6 waits for STATUS_STARTED before cores 1-3
    // run setup_core_interrupt, so overflow hw is clean when they drain PLIC.
    for (uint32_t c = 0; c < NUM_CORES; c++) {
        cnt_wr(CNT_IRQ_BASE + c, 0x00000000);
    }

    // Signal PMU started (overflow hw is now clean)
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_STARTED);

    // Wait for command from CVA6
    while (READ_MEM(DSPM_CMD_ADDR) != CMD_TRIGGER_INTERRUPT) {
        asm volatile ("nop");
    }

    // Clear command
    WRITE_MEM(DSPM_CMD_ADDR, CMD_NONE);

    // Set CVA_ACK_MASK so the ACK check passes on first mempol iteration
    for (uint32_t c = 0; c < NUM_CORES; c++) {
        cnt_wr(CNT_IRQ_BASE + c, CVA_ACK_MASK);
    }

    // Update status - starting interrupt sequence
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_SENDING_IRQS);

    // Update status - all interrupts sent, cores are all awake
    WRITE_MEM(DSPM_STATUS_ADDR, STATUS_ALL_IRQS_SENT);


    // ================================================================
    // Mempol implementation - Token bucket bandwidth policing
    //
    // Counter mapping (set up by CVA6 in pmu_test.c):
    //   cnt 0-3:   IRQ overflow (cores 0-3)
    //   cnt 4-7:   LLC read reqs (cores 0-3)
    //   cnt 8-11:  LLC write reqs (cores 0-3)
    //   cnt 12-15: MEM read reqs (cores 0-3)
    //   cnt 16-19: MEM write reqs (cores 0-3)
    // ================================================================

    int32_t V;
    uint32_t poll_count = 0;
    uint32_t irq_sent[NUM_POLICED_CORES] = {0, 0, 0, 0};

    uint32_t cyc_start;
    uint32_t cyc_now;
    uint32_t max_work_cycles = 0;

    // DSPM log variables for core 0
    uint32_t log_llc_rd_0 = 0, log_llc_wr_0 = 0, log_mem_rd_0 = 0, log_mem_wr_0 = 0;
    uint32_t log_decision_0 = 0;
    uint32_t log_count = 0;
    WRITE_MEM(DSPM_LOG_BASE, 0);

    asm volatile ("csrr %0, mcycle" : "=r"(cyc_start));

    // Mempol main loop
    uint32_t iter_start;
    asm volatile ("csrr %0, mcycle" : "=r"(iter_start));

 while (1) {

        poll_count++;
        cnt_wr(CNT_POLL_PERIOD, poll_count);
        cnt_wr(CNT_BREADCRUMB, 1);  // breadcrumb: top of loop

        log_decision_0 = 0;

        // Process each policed core (cores 0-3)
        for (uint32_t idx = 0; idx < NUM_POLICED_CORES; idx++) {

                uint32_t core = idx;  // core IDs 0, 1, 2, 3

                cnt_wr(CNT_BREADCRUMB, 10 + core);  // breadcrumb: 11/12/13 = before READ_MEM core 1/2/3

                // Read performance counters for this core via memory-mapped regs (mask off overflow bit)
                uint32_t llc_rd = READ_MEM(COUNTER_BASE_ADDR + (CNT_LLC_RD_BASE + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
                uint32_t llc_wr = READ_MEM(COUNTER_BASE_ADDR + (CNT_LLC_WR_BASE + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
                uint32_t mem_rd = READ_MEM(COUNTER_BASE_ADDR + (CNT_MEM_RD_BASE + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;
                uint32_t mem_wr = READ_MEM(COUNTER_BASE_ADDR + (CNT_MEM_WR_BASE + core) * COUNTER_BUNDLE_SIZE) & 0x7FFFFFFF;

                // Save core 0 counter values for DSPM log
                if (idx == 0) { log_llc_rd_0 = llc_rd; log_llc_wr_0 = llc_wr; log_mem_rd_0 = mem_rd; log_mem_wr_0 = mem_wr; }

                cnt_wr(CNT_BREADCRUMB, 20 + core);  // breadcrumb: 21/22/23 = after READ_MEM, before reset

                // Reset performance counters immediately after read so delay/compute
                // time doesn't bleed into next period's counters
                cnt_wr(CNT_LLC_RD_BASE + core, 0);
                cnt_wr(CNT_LLC_WR_BASE + core, 0);
                cnt_wr(CNT_MEM_RD_BASE + core, 0);
                cnt_wr(CNT_MEM_WR_BASE + core, 0);

                cnt_wr(CNT_BREADCRUMB, 30 + core);  // breadcrumb: 31/32/33 = after reset, before V compute

                // Compute bandwidth usage:
                // Vj = aLLCr * LLCr + aLLCw * LLCw + aMEMr * MEMr + aMEMw * MEMw
                V = (aLLCr * llc_rd + aLLCw * llc_wr + aMEMr * mem_rd + aMEMw * mem_wr);

                // Token bucket update: Tj = Tj + Aj - Vj
                token[idx] += (B * P) - V;

                // Cap token bucket to prevent unbounded accumulation
                int32_t cap = (int32_t)(w * poll_period * B);
                if (token[idx] > cap)
                    token[idx] = cap;


            //if core has acked and acted on last request
            if ((READ_MEM(COUNTER_BASE_ADDR + core * COUNTER_BUNDLE_SIZE) & CVA_ACK_MASK) == CVA_ACK_MASK) {


                cnt_wr(CNT_BREADCRUMB, 40 + core);  // breadcrumb: 41/42/43 = before throttle check

                // Throttle: token bucket depleted -> pause core via overflow interrupt
                if (token[idx] < 0 && !cnt_rd(CNT_THROTTLED_BASE + idx)) {
                    cnt_wr(CNT_BREADCRUMB, 50 + core);  // breadcrumb: 51/52/53 = sending pause IRQ
                    cnt_wr(CNT_IRQ_BASE + core, 0x40000000);
                    cnt_wr(CNT_THROTTLED_BASE + idx, 1);
                    irq_sent[idx]++;
                    cnt_wr(CNT_IRQ_SENT_BASE + idx, irq_sent[idx]);
                    if (idx == 0) log_decision_0 = 1;
                }

                // Resume: token bucket replenished -> resume core via overflow interrupt
                else if (token[idx] >= 0 && cnt_rd(CNT_THROTTLED_BASE + idx)) {
                    cnt_wr(CNT_BREADCRUMB, 60 + core);  // breadcrumb: 61/62/63 = sending resume IRQ
                    cnt_wr(CNT_IRQ_BASE + core, 0x41000000);
                    cnt_wr(CNT_THROTTLED_BASE + idx, 0);
                    irq_sent[idx]++;
                    cnt_wr(CNT_IRQ_SENT_BASE + idx, irq_sent[idx]);
                    if (idx == 0) log_decision_0 = 2;
                }
            } 


        // Core has not done our bidding yet/acknowledged, lets wait/skip it this loop
        else {

        }



        }

            cnt_wr(CNT_BREADCRUMB, 70);  // breadcrumb: before mcycle pad loop

            // Measure actual work time before padding begins
            uint32_t pre_pad;
            asm volatile ("csrr %0, mcycle" : "=r"(pre_pad));
            uint32_t work_cycles = pre_pad - iter_start;
            if (work_cycles > max_work_cycles) {
                max_work_cycles = work_cycles;
                cnt_wr(CNT_MAX_WORK_CYCLES, max_work_cycles);
            }

            // Write DSPM log record for core 0
            if (log_count < DSPM_LOG_MAX_ENTRIES) {
                uint32_t lr = log_llc_rd_0 > 511 ? 511 : log_llc_rd_0;
                uint32_t lw = log_llc_wr_0 > 511 ? 511 : log_llc_wr_0;
                uint32_t mr = log_mem_rd_0 > 127 ? 127 : log_mem_rd_0;
                uint32_t mw = log_mem_wr_0 > 127 ? 127 : log_mem_wr_0;
                WRITE_MEM(DSPM_LOG_BASE + 4 + log_count * 12,     (lr << 23) | (lw << 14) | (mr << 7) | mw);
                WRITE_MEM(DSPM_LOG_BASE + 4 + log_count * 12 + 4, (log_decision_0 << 30) | ((uint32_t)token[0] & 0x3FFFFFFF));
                //WRITE_MEM(DSPM_LOG_BASE + 4 + log_count * 12 + 8, pre_pad - cyc_start);
                log_count++;
                WRITE_MEM(DSPM_LOG_BASE, log_count);
            }

            // Pad iteration to exactly poll_period cycles
            uint32_t now;
            do {
                asm volatile ("csrr %0, mcycle" : "=r"(now));
            } while ((now - iter_start) < poll_period);
            iter_start = now;

            cnt_wr(CNT_BREADCRUMB, 80);  // breadcrumb: after pad, before total_cycles write

            // cnt_wr(CNT_IRQ_BASE + 0, 0x00000000);
            // cnt_wr(CNT_IRQ_BASE + 1, 0x00000000);
            // cnt_wr(CNT_IRQ_BASE + 2, 0x00000000);
            // cnt_wr(CNT_IRQ_BASE + 3, 0x00000000);


            asm volatile ("csrr %0, mcycle" : "=r"(cyc_now));
            cnt_wr(CNT_TOTAL_CYCLES, cyc_now - cyc_start);

            cnt_wr(CNT_BREADCRUMB, 90);  // breadcrumb: end of iteration
    

}

        //reset_counters after all iters;
        // cnt_wr(CNT_IRQ_BASE + 0, 0x00000000);
        // cnt_wr(CNT_IRQ_BASE + 1, 0x00000000);
        // cnt_wr(CNT_IRQ_BASE + 2, 0x00000000);
        // cnt_wr(CNT_IRQ_BASE + 3, 0x00000000);

        // Read mcycle at end of iteration

        //cnt_wr(CNT_BREADCRUMB, 99);
   // }
}
