# Requirements

These programs are the *on-board* software for the paper's case studies; they run on the
[`he-soc`](https://github.com/Rodolfo-Pellizzoni-Lab/he-soc) SoC on a Xilinx VCU118 board.

## Hardware

- A Xilinx **VCU118** FPGA board, programmed with the `he-soc` bitstream (see the `he-soc`
  repository for the bitstream-generation flow).
- A **JTAG** connection to the board (to load the program — e.g. via OpenOCD).
- A **USB-UART / serial** connection to the board (to read the program's output).

## Software

| Tool | Purpose | Version |
|------|---------|---------|
| **RISC-V GCC** | build both the CVA6 side (RV64IM) and the APMU-PE firmware (RV32IM) | **GCC 15.1.0** |
| **python3** | `sad_profile` two-pass address extraction (`extract_addrs.py`) | 3.x |
| **OpenOCD** + **GDB** | load the `.riscv` onto the board over JTAG | — |
| **picocom** (or any serial terminal) | watch the UART output (115200 baud, 8N1) | — |
| Host **OS** | — | Linux |

> **Toolchain scope.** GCC **15.1.0** is the toolchain used to build the board software for
> the paper's experiments, per the paper's evaluation setup ("*We compile code for the CVA6
> application cores (RV64IM) and the APMU-PE (RV32IM) using the same GCC 15.1.0 toolchain,
> with appropriate target configurations for each ISA*"). Both the RV64
> (`riscv64-unknown-elf-`) and RV32 (`riscv32-unknown-elf-`, with `zicsr`) targets come from
> that single toolchain; override the prefixes with `RISCV_PREFIX=` and `RV32_PREFIX=` on the
> `make` line if yours are named or located differently.
