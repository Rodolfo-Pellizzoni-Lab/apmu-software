# APMU Software 

| Folder | Experiment |
|---|---|
| `mempol/` | Memory-bandwidth policing: the APMU core interrupts and halts a CVA6 core when it exceeds its bandwidth budget, and resumes it when the budget refills |
| `sad_profile/` | EVU PC-milestone profiling of function entry & exit: per-call cycle/event profiles of `padarray4` inside the SD-VBS disparity (SAD) benchmark |

Each experiment's main code is in the following files:

APMU:  `pmu_firmware/main.c`, RV32IM C compiled for the Ibex PMU core.
CVA6 side: `pmu_bench.c`, RV64 bare-metal program for the CVA6 cores.

They compile together as one binary: the firmware is compiled first, its
`.text`/`.data` are turned into raw bins, and `pmu_text_wrapper.S` /
`pmu_data_wrapper.S` embed them (`.incbin`) into the CVA6 ELF. At boot the
CVA6 program copies the firmware into the APMU's ISPM (`0x10427000`) and DSPM
(`0x10428000`).


## Prerequisites

- **RV64 bare-metal toolchain** (`riscv64-unknown-elf-gcc`) for the CVA6 side.
- **RV32 bare-metal toolchain** (`riscv32-unknown-elf-gcc`, zicsr support) for
  the Ibex firmware.
- `python3` (sad_profile's two-pass address extraction).

Both toolchains are picked up from `PATH`: override with
`RISCV_PREFIX=` (RV64 side) and `RV32_PREFIX=` (firmware) on the `make`
command line if yours live elsewhere.

## Building

### mempol

```bash
cd mempol
make clean
make pmu_bench fpga=1
```

Produces `pmu_bench.riscv` 

### sad_profile

```bash
cd sad_profile
make clean
make two-pass fpga=1
```

## Running on the FPGA

Load `pmu_bench.riscv` over JTAG and run hart 0 — e.g. with OpenOCD and GDB:

```
(gdb) target extended-remote :<openocd-port>
(gdb) monitor reset halt
(gdb) load pmu_bench.riscv
(gdb) continue
```

Then watch the serial console:

```bash
picocom -b 115200 --imap lfcrlf /dev/ttyUSB1
```

Both programs set the UART to 115200 baud (50 MHz UART clock). Multi-hart bring-up is done inside `pmu_bench.c`.

---

## Experiment 1: `mempol/`: Mempol-style bandwidth policing with halt interrupts

The APMU firmware runs a token-bucket regulator over the CVA6 cores' memory
traffic. When a core spends its budget, the APMU pauses it by injecting an interrupt; when the bucket refills, it injects another interupt to trigger resume. 

## Experiment 2: `sad_profile/`: EVU function entry/exit profiling

Per-call profiling of `padarray4` (one of the disparity/SAD kernel functions)
without instrumenting the benchmark: the EVU watches the CVA6 commit PC, and
the APMU firmware records cycles and event counts between each entry and exit.


### Profiling a different function

1. Edit `extract_addrs.py` / the `DYNAMIC_PROFILE_FUNC_*` defines in
   `pmu_bench.c` to point at the new function (entry symbol + offset of its
   first `ret` from the disassembly).
2. Rebuild with `make two-pass fpga=1` — it re-extracts addresses and rebuilds
   the firmware to match.
