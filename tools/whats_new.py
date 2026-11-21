#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The latest changes from whats_new.txt, for the manifest and the spoken summary.

Usage: whats_new.py            prints the sentence Jarvis says ("Here's what's new...")
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COUNT = 3


def items(path: Path = ROOT / "whats_new.txt", count: int = COUNT) -> list:
    lines = [l.strip().lstrip("-* ").strip() for l in path.read_text().splitlines()]
    lines = [l for l in lines if l and not l.startswith("#")]
    return [l if l[-1] in ".!?" else l + "." for l in lines[:count]]


def spoken(notes: list) -> str:
    return ("Here's what's new in the update. " + " ".join(notes)) if notes else ""


if __name__ == "__main__":
    text = spoken(items(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "whats_new.txt"))
    print(text.replace("\t", " ").replace("\n", " "))
