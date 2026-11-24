#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail the build if the firmware is over its size budget, and report what takes space.

Usage: check_size.py <app.bin> <firmware_budget.json> [size_components.txt] [size.json]
  size.json: idf.py size --format json2 (static memory use per memory type)
Writes a Markdown summary to $GITHUB_STEP_SUMMARY when set.
"""
import json
import os
import sys
from pathlib import Path

SLOT = 0x470000   # each update slot in partitions.csv


def main() -> int:
    app, budget_file = Path(sys.argv[1]), Path(sys.argv[2])
    breakdown = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    mem_file = Path(sys.argv[4]) if len(sys.argv) > 4 else None
    size = app.stat().st_size
    budgets = json.loads(budget_file.read_text())
    budget = budgets["app_max_bytes"]
    ok = size <= budget
    lines = [
        "## Firmware size",
        "",
        f"| | bytes | MiB |",
        f"|---|---:|---:|",
        f"| Firmware | {size:,} | {size / 2**20:.2f} |",
        f"| Budget (`{budget_file.name}`) | {budget:,} | {budget / 2**20:.2f} |",
        f"| Room left in budget | {budget - size:,} | {(budget - size) / 2**20:.2f} |",
        f"| Update slot | {SLOT:,} | {SLOT / 2**20:.2f} |",
        "",
        f"{'Within budget' if ok else '**Over budget**'}: {size / budget:.0%} of the budget, "
        f"{size / SLOT:.0%} of a slot.",
    ]
    ram_ok = True
    if mem_file:
        used = internal_static(mem_file)
        limit = budgets.get("internal_ram_static_max_bytes")
        if used is None:
            ram_ok = False
            lines += ["", "**Couldn't read static internal RAM from the size report.**"]
        else:
            ram_ok = limit is None or used <= limit
            lines += ["", f"Static internal RAM (code, data, bss): {used:,} bytes"
                      + (f" of a {limit:,}-byte budget" if limit else " (no budget set: report only)")
                      + ("" if ram_ok else " **over budget**")]
    if breakdown and breakdown.is_file():
        lines += ["", "<details><summary>Size by component</summary>", "", "```",
                  breakdown.read_text().strip()[:60000], "```", "</details>"]
    text = "\n".join(lines) + "\n"
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(text)
    if not ok:
        print(f"Firmware is {size - budget:,} bytes over its budget. Make it smaller, or raise "
              f"app_max_bytes in {budget_file.name} on purpose in its own change.", file=sys.stderr)
    if not ram_ok:
        print("Static internal RAM is over internal_ram_static_max_bytes (or couldn't be read). Voice "
              "sessions need free internal RAM: move buffers to PSRAM (EXT_RAM_BSS_ATTR) or raise the "
              "budget on purpose.", file=sys.stderr)
    return 0 if ok and ram_ok else 1


def internal_static(path: Path):
    """Bytes of internal RAM used statically, from idf.py size --format json2 output."""
    text = path.read_text(errors="replace")
    try:
        data = json.loads(text[text.index("{"):text.rindex("}") + 1])
    except ValueError:
        return None
    types = data.get("memory_types") or data.get("layout") or {}
    if isinstance(types, list):
        types = {t.get("name", ""): t for t in types}
    used = [v.get("used") for k, v in types.items()
            if isinstance(v, dict) and isinstance(v.get("used"), int) and
            any(n in k.upper() for n in ("DIRAM", "DRAM", "IRAM")) and "RTC" not in k.upper()]
    return sum(used) if used else None


if __name__ == "__main__":
    sys.exit(main())
