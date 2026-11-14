#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the web flasher manifest (ESP Web Tools format + our build id).

Usage: write_manifest.py <parts.json> <version> <git_sha>
"""
import json
import sys

parts = json.loads(open(sys.argv[1]).read())
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
}
print(json.dumps(manifest, indent=2))
