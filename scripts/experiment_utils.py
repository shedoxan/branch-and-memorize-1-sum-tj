#!/usr/bin/env python3
"""Shared helpers for local experiment runners."""

from __future__ import annotations

import csv
import math
import shutil
import subprocess
from pathlib import Path
from statistics import median
from typing import Iterable, Sequence


def repo_root_from_script(script_file: str) -> Path:
    return Path(script_file).resolve().parents[1]


def parse_csv_list(text: str, cast) -> list:
    if not text:
        return []
    return [cast(part.strip()) for part in text.split(",") if part.strip()]


def parse_pair_list(text: str) -> list[tuple[float, float]]:
    pairs: list[tuple[float, float]] = []
    for token in text.split(";"):
        token = token.strip()
        if not token:
            continue
        left, right = token.split(":", 1)
        pairs.append((float(left), float(right)))
    return pairs


def fmt_float(value: float) -> str:
    if value == int(value):
        return str(int(value))
    return f"{value:.10g}"


def timeout_for_n(n: int, until_1000_sec: int, after_1000_sec: int) -> int:
    return until_1000_sec if n <= 1000 else after_1000_sec


def run_command(args: Sequence[str], cwd: Path, dry_run: bool = False) -> None:
    print("[run] " + " ".join(map(str, args)), flush=True)
    if dry_run:
        return
    completed = subprocess.run(list(map(str, args)), cwd=str(cwd), text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {completed.returncode}: {' '.join(map(str, args))}")


def build_release(root: Path, dry_run: bool = False) -> None:
    if not (root / "build" / "CMakeCache.txt").exists():
        run_command(["cmake", "-S", ".", "-B", "build"], root, dry_run)
    run_command(["cmake", "--build", "build", "--config", "Release"], root, dry_run)


def run_ctest(root: Path, dry_run: bool = False) -> None:
    run_command(["ctest", "--test-dir", "build", "-C", "Release", "--output-on-failure"], root, dry_run)


def find_executable(root: Path, name: str) -> Path:
    candidates = [
        root / "build" / "Release" / name,
        root / "build" / name,
        root / "x64" / "Release" / name,
    ]
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError(f"{name} not found; build Release first")


def read_rows(csv_path: Path) -> list[dict[str, str]]:
    if not csv_path.exists():
        return []
    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def as_float(value: str, default: float = math.nan) -> float:
    try:
        return float(value)
    except Exception:
        return default


def median_or_nan(values: Iterable[float]) -> float:
    clean = [v for v in values if not math.isnan(v)]
    return median(clean) if clean else math.nan


def p90_or_nan(values: Iterable[float]) -> float:
    clean = sorted(v for v in values if not math.isnan(v))
    if not clean:
        return math.nan
    index = max(0, min(len(clean) - 1, math.ceil(0.9 * len(clean)) - 1))
    return clean[index]


def reset_output_dir(path: Path, fresh: bool, dry_run: bool = False) -> None:
    if fresh and path.exists() and not dry_run:
        shutil.rmtree(path)
    if not dry_run:
        path.mkdir(parents=True, exist_ok=True)


def status_counts(rows: Iterable[dict[str, str]]) -> dict[str, int]:
    out: dict[str, int] = {}
    for row in rows:
        status = row.get("status", "")
        out[status] = out.get(status, 0) + 1
    return out
