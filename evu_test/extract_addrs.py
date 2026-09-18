#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 University of Waterloo
"""FUNC_ENTRY / FUNC_EXIT of one function from an `objdump -d` listing: the symbol's address and its first `ret`.
Usage: extract_addrs.py <dump> <function>"""
import re
import sys

dump, func = sys.argv[1], sys.argv[2]
FUNC_RE = re.compile(r"^([0-9a-f]+)\s+<(\w+)>:")
RET_RE = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+ret\b")
entry = exit_ = None
inside = False
for line in open(dump):
    m = FUNC_RE.match(line.strip())
    if m:
        inside = m.group(2) == func
        if inside:
            entry = int(m.group(1), 16)
        continue
    if inside and exit_ is None:
        r = RET_RE.match(line)
        if r:
            exit_ = int(r.group(1), 16)
if entry is None or exit_ is None:
    sys.exit(f"{func}: entry or ret not found in {dump}")
print(f"// {func}: entry 0x{entry:X}, first ret 0x{exit_:X}")
print(f"#define FUNC_ENTRY  0x{entry:X}")
print(f"#define FUNC_EXIT   0x{exit_:X}")
