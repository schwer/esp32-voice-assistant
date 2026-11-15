#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the Gemini Live server-message samples used by tests/host/test_gemini.c.

These are hand-written (no API key needed), in the shapes Google's Live API example
client reads (google-gemini/gemini-live-api-examples, geminilive.js): setupComplete,
serverContent.modelTurn.parts[].inlineData.data (base64 24 kHz s16le), input/output
transcription pieces, interrupted, turnComplete, toolCall.functionCalls, goAway, plus
fields the firmware ignores (usageMetadata, generationComplete, sessionResumptionUpdate).
Each .jsonl file is one conversation: one server message per line.
"""
import base64
import json
import struct
from pathlib import Path

HERE = Path(__file__).parent

# Known audio so the test can check every decoded sample.
AUDIO_A = [0, 1000, -1000, 32767, -32768, 12345, -12345, 1]
AUDIO_B = [5, -5, 300, -300]


def pcm_b64(samples):
    return base64.b64encode(struct.pack("<%dh" % len(samples), *samples)).decode()


def audio_msg(samples, text=None):
    sc = {"modelTurn": {"parts": [{"inlineData": {"mimeType": "audio/pcm;rate=24000",
                                                  "data": pcm_b64(samples)}}]}}
    if text:
        sc["outputTranscription"] = {"text": text}
    return {"serverContent": sc}


conversation = [
    {"setupComplete": {}},
    {"serverContent": {"inputTranscription": {"text": "What's the"}}},
    {"serverContent": {"inputTranscription": {"text": " weather tomorrow?"}}},
    {"toolCall": {"functionCalls": [{"id": "fc-1", "name": "get_weather", "args": {"days": 2}}]}},
    audio_msg(AUDIO_A, "Tomorrow will be"),
    audio_msg(AUDIO_B, " sunny and mild."),
    {"serverContent": {"generationComplete": True}},
    {"serverContent": {"turnComplete": True},
     "usageMetadata": {"promptTokenCount": 812, "responseTokenCount": 95, "totalTokenCount": 907}},
    {"sessionResumptionUpdate": {"newHandle": "abc", "resumable": True}},
    {"goAway": {"timeLeft": "10s"}},
]

interruption = [
    {"setupComplete": {}},
    audio_msg(AUDIO_A, "Here is a long"),
    {"serverContent": {"inputTranscription": {"text": "Stop."}}},
    {"serverContent": {"interrupted": True}},
    {"serverContent": {"turnComplete": True}},
]

two_tools = [
    {"setupComplete": {}},
    {"toolCall": {"functionCalls": [
        {"id": "fc-7", "name": "get_battery_status", "args": {}},
        {"id": "fc-8", "name": "get_weather", "args": {"location": "Paris, France"}},
    ]}},
    # One message can carry audio and a transcript together.
    {"serverContent": {"modelTurn": {"parts": [
        {"inlineData": {"mimeType": "audio/pcm;rate=24000", "data": pcm_b64(AUDIO_B)}},
        {"inlineData": {"mimeType": "audio/pcm;rate=24000", "data": pcm_b64(AUDIO_A)}},
    ]}, "outputTranscription": {"text": "Battery is fine; Paris is rainy."}, "turnComplete": True}},
]

for name, msgs in [("conversation", conversation), ("interruption", interruption), ("two_tools", two_tools)]:
    (HERE / f"{name}.jsonl").write_text("".join(json.dumps(m, separators=(",", ":")) + "\n" for m in msgs))
    print(f"wrote {name}.jsonl ({len(msgs)} messages)")
