#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the web flasher manifest (ESP Web Tools format + our build id), plus an
"ota" section the board reads to update itself ("Jarvis, update yourself").

Usage: write_manifest.py <parts.json> <version> <git_sha> [notes.ulaw]
  notes.ulaw: optional spoken "what's new" clip, published next to the parts
"""
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from whats_new import items  # noqa: E402

# Bump when partitions.csv changes in a way an update over Wi-Fi can't handle: the
# board then asks for one install over USB instead (components/ota, OTA_LAYOUT).
LAYOUT = 2

parts = json.loads(open(sys.argv[1]).read())


def part(name):
    p = next((p for p in parts if p["path"].endswith("/" + name)), None)
    return p and {"path": p["path"], "size": p.get("size", 0), "sha256": p.get("sha256", "")}


manifest = {
    "name": "ESP32 Voice Assistant",
    "version": sys.argv[2],
    "build": sys.argv[3][:7],
    "new_install_prompt_erase": True,   # never erase without asking
    "new_install_improv_wait_time": 15,
    "builds": [{
        "chipFamily": "ESP32-S3",
        "parts": [{"path": p["path"], "offset": p["offset"]} for p in parts],
    }],
    "ota": {"layout": LAYOUT, "app": part("esp32-voice-assistant.bin"), "model": part("srmodels.bin"),
            "notes": items()},
}
if len(sys.argv) > 4 and Path(sys.argv[4]).is_file():
    clip = Path(sys.argv[4])
    data = clip.read_bytes()
    manifest["ota"]["notes_audio"] = {"path": f"firmware/{clip.name}", "size": len(data),
                                      "sha256": hashlib.sha256(data).hexdigest()}
print(json.dumps(manifest, indent=2))
