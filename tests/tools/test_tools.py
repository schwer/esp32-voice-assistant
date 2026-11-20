# SPDX-License-Identifier: MIT
"""Checks for the build tools and for things that must stay in sync across files.

Run: python -m pytest tests/tools
"""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"


# ---------------------------------------------------------------- collect_parts.py

def make_build(tmp: Path, files: dict) -> Path:
    """Fake ESP-IDF build dir: {offset: (relative path, size)}."""
    build = tmp / "build"
    flash = {}
    for off, (rel, size) in files.items():
        f = build / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"\xAA" * size)
        flash[off] = rel
    (build / "flasher_args.json").write_text(json.dumps({"flash_files": flash}))
    return build


def run_collect(build: Path, out: Path):
    return subprocess.run([sys.executable, str(TOOLS / "collect_parts.py"), str(build), str(out)],
                          capture_output=True, text=True)


GOOD = {
    "0x0": ("bootloader/bootloader.bin", 21000),
    "0x8000": ("partition_table/partition-table.bin", 3072),
    "0x10000": ("ota_data_initial.bin", 8192),
    "0x20000": ("esp32-voice-assistant.bin", 400000),
    "0xf10000": ("srmodels/srmodels.bin", 290000),
}


def test_collect_parts_lists_every_part_in_offset_order(tmp_path):
    out = tmp_path / "out"
    r = run_collect(make_build(tmp_path, GOOD), out)
    assert r.returncode == 0, r.stderr
    parts = json.loads((out / "parts.json").read_text())
    assert [p["offset"] for p in parts] == [0x0, 0x8000, 0x10000, 0x20000, 0xf10000]
    assert all((out / Path(p["path"]).name).exists() for p in parts)
    app = next(p for p in parts if p["path"].endswith("esp32-voice-assistant.bin"))
    data = (out / "esp32-voice-assistant.bin").read_bytes()
    assert app["sha256"] == hashlib.sha256(data).hexdigest() and app["size"] == len(data)


def test_collect_parts_refuses_anything_overlapping_nvs(tmp_path):
    # A merged image starting at 0 would overwrite Wi-Fi and the API key on update.
    bad = dict(GOOD)
    bad["0x0"] = ("merged.bin", 0x20000)
    r = run_collect(make_build(tmp_path, bad), tmp_path / "out")
    assert r.returncode != 0
    assert "NVS" in (r.stderr + r.stdout)


def test_collect_parts_refuses_an_app_that_nearly_fills_its_slot(tmp_path):
    big = dict(GOOD)
    big["0x20000"] = ("esp32-voice-assistant.bin", int(0x470000 * 0.95))
    r = run_collect(make_build(tmp_path, big), tmp_path / "out")
    assert r.returncode != 0
    assert "slot" in (r.stderr + r.stdout)


def test_collect_parts_requires_the_wake_word_model(tmp_path):
    no_model = {k: v for k, v in GOOD.items() if "srmodels" not in v[0]}
    r = run_collect(make_build(tmp_path, no_model), tmp_path / "out")
    assert r.returncode != 0
    assert "srmodels" in (r.stderr + r.stdout)


# ---------------------------------------------------------------- write_manifest.py

def test_manifest_format(tmp_path):
    parts = [{"path": "firmware/a.bin", "offset": 0, "size": 1}, {"path": "firmware/b.bin", "offset": 65536, "size": 1}]
    pj = tmp_path / "parts.json"
    pj.write_text(json.dumps(parts))
    r = subprocess.run([sys.executable, str(TOOLS / "write_manifest.py"), str(pj), "2026.11.15-abc1234",
                        "abc1234deadbeef"], capture_output=True, text=True, check=True)
    m = json.loads(r.stdout)
    assert m["name"] == "ESP32 Voice Assistant"     # must match the firmware's Improv device info
    assert m["build"] == "abc1234"
    assert m["new_install_prompt_erase"] is True     # never erase without asking
    assert m["builds"][0]["chipFamily"] == "ESP32-S3"
    assert m["builds"][0]["parts"] == [{"path": "firmware/a.bin", "offset": 0},
                                       {"path": "firmware/b.bin", "offset": 65536}]


def test_manifest_describes_the_update_for_the_board(tmp_path):
    parts = [{"path": "firmware/bootloader.bin", "offset": 0, "size": 1, "sha256": "00"},
             {"path": "firmware/esp32-voice-assistant.bin", "offset": 0x20000, "size": 5, "sha256": "aa"},
             {"path": "firmware/srmodels.bin", "offset": 0xf10000, "size": 7, "sha256": "bb"}]
    pj = tmp_path / "parts.json"
    pj.write_text(json.dumps(parts))
    r = subprocess.run([sys.executable, str(TOOLS / "write_manifest.py"), str(pj), "2026.11.20-abc1234",
                        "abc1234deadbeef"], capture_output=True, text=True, check=True)
    ota = json.loads(r.stdout)["ota"]
    assert ota["layout"] == 2
    assert ota["app"] == {"path": "firmware/esp32-voice-assistant.bin", "size": 5, "sha256": "aa"}
    assert ota["model"] == {"path": "firmware/srmodels.bin", "size": 7, "sha256": "bb"}


def test_firmware_reports_the_manifest_name():
    improv = (ROOT / "components/improv/improv.c").read_text()
    assert '"ESP32 Voice Assistant"' in improv


# ---------------------------------------------------------------- partition table

def partitions():
    rows = []
    for line in (ROOT / "partitions.csv").read_text().splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, ptype, sub, off, size = [c.strip() for c in line.split(",")][:5]
        rows.append({"name": name, "type": ptype, "offset": off, "size": int(size, 0)})
    # Fill in implicit offsets like ESP-IDF does (data/app aligned; app at 64 KB).
    cur = 0
    for r in rows:
        if r["offset"]:
            cur = int(r["offset"], 0)
        else:
            align = 0x10000 if r["type"] == "app" else 0x1000
            cur = (cur + align - 1) // align * align
        r["offset"] = cur
        cur += r["size"]
    return rows


def test_nvs_never_moves():
    # Wi-Fi, the API key and settings live here; moving it would wipe them on update.
    nvs = next(r for r in partitions() if r["name"] == "nvs")
    assert (nvs["offset"], nvs["size"]) == (0x9000, 0x6000)


def test_storage_never_moves():
    # Recordings, conversations and alarms live here; it kept its place when the
    # two firmware slots were added, so nothing was lost.
    st = next(r for r in partitions() if r["name"] == "storage")
    assert (st["offset"], st["size"]) == (0x910000, 0x600000)


def test_two_firmware_slots_fit_the_app_with_room_to_grow():
    slots = [r for r in partitions() if r["type"] == "app"]
    assert [r["name"] for r in slots] == ["ota_0", "ota_1"]
    assert all(r["size"] >= 0x3D67D0 * 1.1 for r in slots)        # app was 0x3D67D0 bytes
    assert any(r["name"] == "otadata" for r in partitions())


def test_partitions_fit_16mb_flash_without_overlap():
    rows = sorted(partitions(), key=lambda r: r["offset"])
    for a, b in zip(rows, rows[1:]):
        assert a["offset"] + a["size"] <= b["offset"], f"{a['name']} overlaps {b['name']}"
    assert rows[-1]["offset"] + rows[-1]["size"] <= 16 * 1024 * 1024


# ---------------------------------------------------------------- spoken lines

def tsv_ids():
    return [l.split("\t")[0] for l in (ROOT / "tools/prompts.tsv").read_text().splitlines()
            if l.strip() and not l.startswith("#")]


def test_every_phrase_has_text():
    for line in (ROOT / "tools/prompts.tsv").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            pid, _, text = line.partition("\t")
            assert text.strip(), f"{pid} has no text"


def test_phrase_list_enum_ids_and_clips_agree():
    ids = tsv_ids()
    header = (ROOT / "components/prompts/include/prompts.h").read_text()
    enum = [e.lower() for e in re.findall(r"^\s+PROMPT_([A-Z0-9_]+),", header, re.M)]
    src = (ROOT / "components/prompts/prompts.c").read_text()
    table = src[src.index("IDS[PROMPT_COUNT]"):]
    table = re.findall(r'"([a-z0-9_]+)"', table[:table.index("};")])
    cmake = (ROOT / "components/prompts/CMakeLists.txt").read_text()
    clips = re.search(r"set\(clips(.*?)\)", cmake, re.S).group(1).split()

    assert enum == table, "prompts.h enum and prompts.c IDS[] are out of order"
    assert sorted(ids) == sorted(table), "tools/prompts.tsv and IDS[] differ"
    assert clips == table, "CMakeLists clip list must follow the enum order"
    for pid in table:
        assert (ROOT / f"components/prompts/clips/{pid}.ulaw").exists(), f"missing clip {pid}.ulaw"
    assert (ROOT / "components/prompts/clips/voice.txt").exists()


def test_ulaw_encoder_matches_the_firmware():
    # Port of dsp_ulaw_encode() (components/dsp/dsp.c), checked for every 16-bit value.
    sys.path.insert(0, str(TOOLS))
    import numpy as np
    from ulaw import ulaw_encode

    def c_encode(pcm):
        sign = (pcm >> 8) & 0x80
        v = -pcm if sign else pcm
        v = min(v, 32635) + 0x84
        exp, mask = 7, 0x4000
        while (v & mask) == 0 and exp > 0:
            mask >>= 1
            exp -= 1
        return ~(sign | (exp << 4) | ((v >> (exp + 3)) & 0x0F)) & 0xFF

    values = np.arange(-32768, 32768, dtype=np.int32)
    expected = np.array([c_encode(int(v)) for v in values], dtype=np.uint8)
    assert (ulaw_encode(values.astype(np.int16)) == expected).all()


def test_clips_are_16khz_mono_ulaw_of_reasonable_length():
    for pid in tsv_ids():
        n = (ROOT / f"components/prompts/clips/{pid}.ulaw").stat().st_size
        assert not (ROOT / f"components/prompts/clips/{pid}.pcm").exists()
        seconds = n / 16000
        assert 0.3 < seconds < 12, f"{pid}: {seconds:.1f} s"


# ---------------------------------------------------------------- settings / Kconfig

def test_kconfig_defaults_used_by_ci_exist():
    k = (ROOT / "main/Kconfig.projbuild").read_text()
    for name in ("VA_GEMINI_MODEL", "VA_VOICE_NAME", "VA_TIMEZONE", "VA_WAKENET_KEYWORD"):
        assert f"config {name}" in k
    assert 'default "jarvis"' in k
