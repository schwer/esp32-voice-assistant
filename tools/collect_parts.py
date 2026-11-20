#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Copy flashable parts from an ESP-IDF build dir and describe them in parts.json.

Usage: collect_parts.py <build_dir> <out_dir>
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

NVS_OFFSET = 0x9000
NVS_END = 0xF000

build, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
args = json.loads((build / "flasher_args.json").read_text())

parts = []
for offset, rel in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    src = build / rel
    off = int(offset, 16)
    end = off + src.stat().st_size
    if off < NVS_END and end > NVS_OFFSET:
        sys.exit(f"{rel} at {offset} overlaps the NVS partition; refusing to publish it")
    shutil.copy(src, out / src.name)
    sha = hashlib.sha256(src.read_bytes()).hexdigest()
    parts.append({"path": f"firmware/{src.name}", "offset": off, "size": src.stat().st_size, "sha256": sha})
    print(f"{offset:>10}  {src.name}  ({src.stat().st_size} bytes)")

# Each app must leave room to grow in its slot (two slots are needed for updates).
csv = Path(__file__).resolve().parent.parent / "partitions.csv"
slots = [int(c.split(",")[4], 0) for c in csv.read_text().splitlines()
         if c.split("#")[0].strip() and c.split(",")[1].strip() == "app"]
app = max((p for p in parts if p["offset"] in (0x10000, 0x20000)), key=lambda p: p["size"], default=None)
if app and slots and app["size"] > 0.9 * min(slots):
    sys.exit(f"app is {app['size']} bytes: over 90% of its {min(slots)}-byte slot. "
             "Make it smaller or rethink partitions.csv before it stops fitting.")

names = [Path(p["path"]).name for p in parts]
if "srmodels.bin" not in names:
    sys.exit("srmodels.bin (wake word model) missing from flash_files")
(out / "parts.json").write_text(json.dumps(parts, indent=2))
