#!/usr/bin/env python3
# Regenerates the opcode table in src/spv_check.h (which SPIR-V instructions have a result id / a result type) from
# the vendored SPIR-V headers. Prints the C++ array body; paste it between the markers in spv_check.h.
#   python3 tools/gen_spv_ops.py > /tmp/ops.txt
import os
import re

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "..", "third_party", "spirv-headers", "include", "spirv", "unified1", "spirv.h"), encoding="utf-8").read()
ops = dict(re.findall(r"^\s*SpvOp(\w+)\s*=\s*(\d+),", src, re.M))
body = src[src.index("inline void SpvHasResultAndType"):]
body = body[:body.index("\n}\n")]
table = {}
for name, res, typ in re.findall(r"case SpvOp(\w+): \*hasResult = (true|false); \*hasResultType = (true|false);", body):
    table[int(ops[name])] = (res == "true") * 1 + (typ == "true") * 2
entries = [op << 2 | flags for op, flags in sorted(table.items())]
line = "   "
for e in entries:
    item = " %d," % e
    if len(line) + len(item) > 118:
        print(line)
        line = "   "
    line += item
print(line)
