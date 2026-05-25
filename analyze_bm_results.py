#!/usr/bin/env python3
"""Root wrapper for scripts/analyze_bm_results.py."""

from __future__ import annotations

import runpy
from pathlib import Path


if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().parent / "scripts" / "analyze_bm_results.py"), run_name="__main__")
