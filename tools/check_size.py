#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail the build if the firmware is over its size budget, and report what takes space.

Usage: check_size.py <app.bin> <firmware_budget.json> [size_components.txt]
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
    size = app.stat().st_size
    budget = json.loads(budget_file.read_text())["app_max_bytes"]
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
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
