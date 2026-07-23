#!/usr/bin/env python3
"""
Extract profiled function entry and first-ret addresses from a .dump file.
Usage: python3 extract_addrs.py [path/to/pmu_bench.dump]
"""

import re
import sys

DUMP_FILE = sys.argv[1] if len(sys.argv) > 1 else "pmu_bench.dump"

# All disparity functions to locate
DISP_FUNCTIONS = [
    "sdvb_disparity",
    "getDisparity",
    "correlateSAD_2D",
    "computeSAD",
    "integralImage2D2D",
    "finalSAD",
    "findDisparity",
    "padarray4",
    "padarray2",
]

# The function being profiled (entry + first ret)
PROFILED_FUNCTION = "padarray4"

FUNC_RE = re.compile(r'^([0-9a-f]+)\s+<(\w+)>:')
RET_RE  = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+ret\b')

entries = {}
exits   = {}

with open(DUMP_FILE, "r") as f:
    current_func = None
    for line in f:
        m = FUNC_RE.match(line.strip())
        if m:
            addr, name = m.group(1), m.group(2)
            current_func = name if name in DISP_FUNCTIONS else None
            if current_func:
                entries[current_func] = "0x" + addr.lstrip("0").upper() or "0x0"
            continue

        if current_func == PROFILED_FUNCTION and current_func not in exits:
            r = RET_RE.match(line)
            if r:
                exits[current_func] = "0x" + r.group(1).lstrip("0").upper() or "0x0"

print(f"// Auto-extracted from {DUMP_FILE}")
print(f"// {len(entries)}/{len(DISP_FUNCTIONS)} disparity functions found\n")

print("// --- All disparity function entries ---")
for func in DISP_FUNCTIONS:
    if func in entries:
        print(f"#define ADDR_{func.upper():30s} {entries[func]}")
    else:
        print(f"// WARNING: {func} not found in dump!")

print()
entry = entries.get(PROFILED_FUNCTION, "NOT FOUND")
exit_ = exits.get(PROFILED_FUNCTION,  "NOT FOUND")
print(f"// --- Profiled function: {PROFILED_FUNCTION} ---")
print(f"//  entry     = {entry}")
print(f"//  first_ret = {exit_}")
print()
print(f"#define FUNC_ENTRY  {entry}")
print(f"#define FUNC_EXIT   {exit_}")
