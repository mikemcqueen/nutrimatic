#!/usr/bin/env python3

from pathlib import Path


def line_count(path: Path) -> int:
    with path.open("r", encoding="utf-8") as f:
        n_lines = sum(1 for _ in f)
    return n_lines
