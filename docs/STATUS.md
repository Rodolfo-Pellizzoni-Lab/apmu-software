# Artifact Evaluation — Status

This repository holds the bare-metal software used for the case studies in our **EMSOFT
2026** paper. It is part of the artifact for which we apply for the **Artifacts Available**
badge; the SoC these programs run on is the companion
[`he-soc`](https://github.com/Rodolfo-Pellizzoni-Lab/he-soc) repository.

## Why the artifact deserves the "Available" badge

- **Open-source.** The software we wrote — the APMU firmware, the CVA6-side benchmark
  harnesses, the profiling glue, and the build scripts — is released under the **MIT
  License** (see [`../LICENSE`](../LICENSE)). Vendored third-party components keep their own
  open-source licenses (SD-VBS © UC San Diego; the PULP/AlSaqr drivers under Apache-2.0; the
  RISC-V `riscv-tests` boilerplate under its BSD license), as documented in the
  [README](../README.md#license).
- **Publicly hosted.** The complete software is in a public repository that anyone can
  access, clone, and inspect.

## Why not "Functional" / "Reusable"

Executing these programs requires a physical Xilinx **VCU118** FPGA board programmed with
the `he-soc` bitstream, plus a **JTAG** debugger (OpenOCD/GDB) to load the binary and a
serial console to read the results. A reviewer without the board and the programmed SoC
cannot run them, so a reviewer-run functional/reproduction evaluation is not feasible within
the artifact-evaluation process. We therefore pursue the Available badge only.
