# SPDX-License-Identifier: MIT
"""Checks for the build tools and for things that must stay in sync across files.

Run: python -m pytest tests/tools
"""
import hashlib
import json
import os
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


# ---------------------------------------------------------------- check_size.py

def run_size(tmp: Path, size: int, budget: int):
    app = tmp / "app.bin"
    app.write_bytes(b"\0" * size)
    b = tmp / "budget.json"
    b.write_text(json.dumps({"app_max_bytes": budget}))
    env = dict(os.environ, GITHUB_STEP_SUMMARY=str(tmp / "summary.md"))
    return subprocess.run([sys.executable, str(TOOLS / "check_size.py"), str(app), str(b)],
                          capture_output=True, text=True, env=env)


def test_size_check_passes_within_budget_and_writes_a_summary(tmp_path):
    r = run_size(tmp_path, 1000, 2000)
    assert r.returncode == 0, r.stderr
    assert "Within budget" in (tmp_path / "summary.md").read_text()


def test_size_check_fails_over_budget(tmp_path):
    r = run_size(tmp_path, 2001, 2000)
    assert r.returncode != 0
    assert "over its budget" in r.stderr


def test_budget_leaves_room_in_the_update_slot():
    budget = json.loads((ROOT / "firmware_budget.json").read_text())["app_max_bytes"]
    assert budget <= 0.8 * 0x470000, "keep the budget well under the 4.4 MB update slot"


def test_size_check_reports_and_enforces_static_internal_ram(tmp_path):
    app = tmp_path / "app.bin"
    app.write_bytes(b"\0" * 100)
    mem = tmp_path / "size.json"
    mem.write_text("Executing action: size\n" + json.dumps(
        {"memory_types": {"DIRAM": {"used": 150000, "size": 340000}, "IRAM": {"used": 16384, "size": 16384},
                          "RTC FAST": {"used": 100, "size": 8192}}}))
    for limit, want in ((None, 0), (200000, 0), (160000, 1)):
        b = tmp_path / "budget.json"
        b.write_text(json.dumps({"app_max_bytes": 1000, "internal_ram_static_max_bytes": limit}))
        r = subprocess.run([sys.executable, str(TOOLS / "check_size.py"), str(app), str(b), "", str(mem)],
                           capture_output=True, text=True)
        assert r.returncode == want, (limit, r.stdout, r.stderr)
        assert "166,384" in r.stdout                                  # DIRAM + IRAM, not RTC


def test_task_stacks_in_internal_ram_fit_the_budget():
    # A Gemini voice session needs a free 12 KB block of internal RAM. Every task stack
    # comes out of it unless created WithCaps(MALLOC_CAP_SPIRAM); see tools/ram_budget.py.
    sys.path.insert(0, str(TOOLS))
    from ram_budget import stacks
    found = stacks()
    total = sum(s for _, _, s in found)
    budget = json.loads((ROOT / "firmware_budget.json").read_text())["internal_task_stacks_max_bytes"]
    listing = "\n".join(f"{s:6d} {n} ({w})" for w, n, s in sorted(found, key=lambda x: -x[2]))
    assert total <= budget, f"task stacks in internal RAM: {total} > {budget} bytes\n{listing}"
    names = {n for _, n, _ in found}
    assert {"afe_feed", "task_stack", "CONFIG_ESP_MAIN_TASK_STACK_SIZE"} <= names   # the counter still sees them
    assert "leds" not in names and "audio_play" not in names                       # PSRAM stacks don't count


def test_stack_counter_reads_calls_fields_and_skips_psram_and_fallbacks(tmp_path):
    sys.path.insert(0, str(TOOLS))
    from ram_budget import stacks
    (tmp_path / "components/x").mkdir(parents=True)
    (tmp_path / "main").mkdir()
    (tmp_path / "components/x/x.c").write_text(
        'xTaskCreate(a, "a", 4 * 1024, NULL, 1, NULL);\n'
        'xTaskCreatePinnedToCore(b, "b", 2048, NULL, 1, NULL, 0);\n'
        'xTaskCreateWithCaps(c, "c", 9000, NULL, 1, NULL, MALLOC_CAP_SPIRAM);\n'
        'xTaskCreate(c, "c", 9000, NULL, 1, NULL);   // fallback (no PSRAM)\n'
        '    .task_stack = 12 * 1024,\n')
    (tmp_path / "sdkconfig.defaults").write_text("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192\n")
    got = {n: s for _, n, s in stacks(tmp_path)}
    assert got == {"a": 4096, "b": 2048, "task_stack": 12288, "CONFIG_ESP_MAIN_TASK_STACK_SIZE": 8192}


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
    assert 1 <= len(ota["notes"]) <= 3 and all(n.endswith((".", "!", "?")) for n in ota["notes"])
    assert "notes_audio" not in ota                                  # no clip given
    assert ota["app"] == {"path": "firmware/esp32-voice-assistant.bin", "size": 5, "sha256": "aa"}
    assert ota["model"] == {"path": "firmware/srmodels.bin", "size": 7, "sha256": "bb"}


def test_manifest_lists_the_spoken_whats_new_clip(tmp_path):
    pj = tmp_path / "parts.json"
    pj.write_text(json.dumps([{"path": "firmware/esp32-voice-assistant.bin", "offset": 0x20000, "size": 5}]))
    clip = tmp_path / "notes.ulaw"
    clip.write_bytes(b"\xff" * 1600)
    r = subprocess.run([sys.executable, str(TOOLS / "write_manifest.py"), str(pj), "v", "abc1234", str(clip)],
                       capture_output=True, text=True, check=True)
    audio = json.loads(r.stdout)["ota"]["notes_audio"]
    assert audio == {"path": "firmware/notes.ulaw", "size": 1600,
                     "sha256": hashlib.sha256(b"\xff" * 1600).hexdigest()}


def test_whats_new_reads_newest_first_and_skips_comments(tmp_path):
    sys.path.insert(0, str(TOOLS))
    from whats_new import items, spoken
    f = tmp_path / "w.txt"
    f.write_text("# comment\n\n- First change\nSecond change!\nThird.\nFourth.\n")
    assert items(f) == ["First change.", "Second change!", "Third."]
    assert spoken(items(f)).startswith("Here's what's new in the update. First change.")
    assert spoken([]) == ""
    real = items()
    assert real and all(len(n) < 160 for n in real), "keep whats_new.txt lines short: they're spoken"


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
        assert (ROOT / f"components/prompts/clips/{pid}.adpcm").exists(), f"missing clip {pid}.adpcm"
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


def test_adpcm_fixture_is_what_the_python_encoder_writes(tmp_path):
    # tests/host/test_dsp.c checks the C encoder against this fixture byte for byte.
    subprocess.run([sys.executable, str(TOOLS / "adpcm.py"), "--fixture", str(tmp_path)], check=True)
    for name in ("sweep.pcm", "sweep.adpcm"):
        assert (tmp_path / name).read_bytes() == (ROOT / "tests/host/fixtures/adpcm" / name).read_bytes()


def test_clips_are_16khz_mono_adpcm_of_reasonable_length():
    for pid in tsv_ids():
        n = (ROOT / f"components/prompts/clips/{pid}.adpcm").stat().st_size
        assert not (ROOT / f"components/prompts/clips/{pid}.pcm").exists()
        assert not (ROOT / f"components/prompts/clips/{pid}.ulaw").exists()
        seconds = n * 2 / 16000
        assert 0.3 < seconds < 12, f"{pid}: {seconds:.1f} s"


# ---------------------------------------------------------------- settings / Kconfig

def test_kconfig_defaults_used_by_ci_exist():
    k = (ROOT / "main/Kconfig.projbuild").read_text()
    for name in ("VA_GEMINI_MODEL", "VA_VOICE_NAME", "VA_TIMEZONE", "VA_WAKENET_KEYWORD"):
        assert f"config {name}" in k
    assert 'default "jarvis"' in k


def test_static_internal_ram_budget_is_set_and_leaves_room():
    b = json.loads((ROOT / "firmware_budget.json").read_text())
    limit = b["internal_ram_static_max_bytes"]
    assert isinstance(limit, int), "set internal_ram_static_max_bytes so the build enforces it"
    assert limit <= 200 * 1024, "keep static internal RAM well below the ~340 KB the chip has"
