# Install & quick test

This builds the two experiments and loads them onto the `he-soc` SoC running on a VCU118
board. You need the RISC-V GCC 15.1.0 toolchain (RV64 + RV32), `python3`, and OpenOCD/GDB
(see [`REQUIREMENTS.md`](./REQUIREMENTS.md)).

## 1. Clone

```
git clone git@github.com:Rodolfo-Pellizzoni-Lab/apmu-software.git
cd apmu-software
```

## 2. Build

Each experiment compiles the APMU firmware (RV32) first, turns its `.text`/`.data` into raw
bins, and embeds them into the CVA6 (RV64) ELF, producing a single `pmu_bench.riscv`.

**mempol** — bandwidth policing:
```
cd mempol
make clean
make pmu_bench fpga=1        # -> pmu_bench.riscv
```

**sad_profile** — EVU function entry/exit profiling:
```
cd sad_profile
make clean
make two-pass fpga=1         # re-extracts PC addresses, rebuilds firmware, -> pmu_bench.riscv
```

If your toolchains are not on `PATH`, pass their prefixes explicitly, e.g.:
```
make pmu_bench fpga=1 RISCV_PREFIX=riscv64-unknown-elf- RV32_PREFIX=riscv32-unknown-elf-
```

## 3. Load on the board and run

```
(gdb) target extended-remote :<openocd-port>
(gdb) monitor reset halt
(gdb) load pmu_bench.riscv
(gdb) continue
```

Then watch the serial console (115200 baud, 8N1):
```
picocom -b 115200 --imap lfcrlf /dev/ttyUSB1
```

## Expected result

A successful build is the first check that the toolchain is set up correctly: it produces
`pmu_bench.riscv` with no errors (known-good reference binaries are tracked under each
experiment's `prebuilt/` directory for comparison).

On the board, the CVA6 program brings up the cores, copies the APMU firmware into the APMU's
ISPM (`0x10427000`) / DSPM (`0x10428000`), starts the APMU core, and streams the run's
results over the UART:

- **mempol** — the token-bucket policing activity (each core's budget spend / refill, and the
  pause/resume interrupts the APMU injects).
- **sad_profile** — the per-call cycle and event counts recorded between the profiled
  function's entry and exit.

Seeing the UART output advance and terminate cleanly confirms the software built, loaded, and
ran on the SoC.
