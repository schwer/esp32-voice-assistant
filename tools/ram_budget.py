#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Internal RAM reserved for task stacks, counted from the source.

Every task stack comes out of the ESP32-S3's ~300 KB of internal RAM unless it's
created WithCaps(MALLOC_CAP_SPIRAM). A Gemini voice session needs a 12 KB block of
it, so new tasks must fit the budget in firmware_budget.json
("internal_task_stacks_max_bytes"). Lines marked "fallback" (only used when PSRAM
can't be) don't count.

Usage: ram_budget.py      prints each stack and the total
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CALL = re.compile(r'\bxTaskCreate(?:PinnedToCore)?\s*\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*([^,]+),')
FIELD = re.compile(r'\.(task_stack|stack_size)\s*=\s*([^,;]+)[,;]|\b(cfg\.stack_size)\s*=\s*([^;]+);')
KCONF = re.compile(r'^(CONFIG_\w*STACK(?:_SIZE|_DEPTH))=(\d+)$', re.M)


def number(expr: str) -> int:
    expr = expr.strip()
    if not re.fullmatch(r"[\d\s*+()]+", expr):
        raise ValueError(f"can't read a stack size from {expr!r}: use a plain number")
    return int(eval(expr))     # digits and * + ( ) only


def stacks(root: Path = ROOT) -> list:
    found = []
    for path in sorted(list((root / "components").rglob("*.c")) + list((root / "main").rglob("*.c"))):
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if "fallback" in line or line.lstrip().startswith("//"):
                continue
            m = CALL.search(line)
            if m and "WithCaps" not in line:
                found.append((f"{path.relative_to(root)}:{n}", m.group(1), number(m.group(2))))
                continue
            f = FIELD.search(line)
            if f:
                name, expr = (f.group(1), f.group(2)) if f.group(1) else (f.group(3), f.group(4))
                found.append((f"{path.relative_to(root)}:{n}", name, number(expr)))
    for m in KCONF.finditer((root / "sdkconfig.defaults").read_text()):
        found.append(("sdkconfig.defaults", m.group(1), int(m.group(2))))
    return found


if __name__ == "__main__":
    total = 0
    for where, name, size in stacks():
        total += size
        print(f"{size:6d}  {name:40s} {where}")
    print(f"{total:6d}  total")
    sys.exit(0)
