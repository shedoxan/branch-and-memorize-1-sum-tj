#!/usr/bin/env python3
"""Measure wall-clock overhead of reconstructing an optimal order."""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import time
from pathlib import Path

from experiment_utils import (
    build_release,
    find_executable,
    fmt_float,
    median_or_nan,
    parse_csv_list,
    parse_pair_list,
    repo_root_from_script,
    reset_output_dir,
    run_ctest,
    timeout_for_n,
)


DEFAULT_PAIRS = "0.2:0.6;0.2:0.4;0.4:0.6"
DEFAULT_N_VALUES = "100,300,500,700,900,1100"
DEFAULT_SEEDS = "0,1,2,3,4"


RUN_TOKEN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")


def parse_run_line(stdout: str) -> dict[str, str]:
    for line in stdout.splitlines():
        if line.startswith("[run] "):
            return {key: value for key, value in RUN_TOKEN_RE.findall(line)}
    return {}


def order_length(stdout: str) -> int:
    for line in stdout.splitlines():
        if line.startswith("[order0] "):
            body = line[len("[order0] "):].strip()
            return 0 if not body else len(body.split())
    return 0


def run_solver(kursovaya: Path, root: Path, n: int, r: float, t: float, seed: int,
               reconstruct: bool, timeout_sec: int) -> dict[str, str]:
    cmd = [
        str(kursovaya),
        "--n", str(n),
        "--instances", "1",
        "--seed", str(seed),
        "--due-range", fmt_float(r),
        "--due-tardiness", fmt_float(t),
        "--model", "adaptive_v3",
        "--memo-backend", "custom",
        "--memo-full-key-verification",
        "--enable-memo",
        "--enable-exact-memo",
        "--terminal-rules",
        "--enable-position-filtering",
        "--enable-lawler-basic-rules",
        "--enable-rule4",
        "--no-lb",
        "--no-ub",
        "--mem-budget-mb", "12288",
    ]
    cmd.append("--reconstruct" if reconstruct else "--no-reconstruct")
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            cmd,
            cwd=str(root),
            text=True,
            capture_output=True,
            timeout=timeout_sec,
        )
        wall_ms = (time.perf_counter() - start) * 1000.0
        parsed = parse_run_line(completed.stdout)
        parsed["status"] = "SOLVED" if completed.returncode == 0 and parsed else "ERROR"
        parsed["error"] = "" if completed.returncode == 0 else completed.stderr.strip()
        parsed["wall_time_ms"] = f"{wall_ms:.3f}"
        parsed["order_length"] = str(order_length(completed.stdout))
        return parsed
    except subprocess.TimeoutExpired:
        return {
            "status": "OOT",
            "error": "subprocess_timeout",
            "wall_time_ms": f"{timeout_sec * 1000.0:.3f}",
            "order_length": "0",
        }


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    solved = [row for row in rows if row["status"] == "SOLVED"]
    overheads = [float(row["reconstruction_overhead_percent"]) for row in solved if row["reconstruction_overhead_percent"]]
    with path.open("w", encoding="utf-8") as f:
        f.write("# Reconstruction Overhead Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write(f"- solved_pairs: {len(solved)}\n")
        f.write(f"- median_overhead_percent: {median_or_nan(overheads):.3f}\n")
        mismatches = sum(1 for row in rows if row.get("objective_match") != "true")
        f.write(f"- objective_mismatches: {mismatches}\n")


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Reconstruction overhead runner.")
    parser.add_argument("--pairs", default=DEFAULT_PAIRS)
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "reconstruction_overhead"))
    parser.add_argument("--timeout-until-900-sec", type=int, default=1200)
    parser.add_argument("--timeout-after-900-sec", type=int, default=3600)
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    pairs = parse_pair_list(args.pairs)
    n_values = parse_csv_list(args.n_values, int)
    seeds = parse_csv_list(args.seeds, int)
    out_dir = Path(args.out_dir)
    reset_output_dir(out_dir, args.fresh, args.dry_run)
    raw_csv = out_dir / "raw_results.csv"

    if not args.skip_build:
        build_release(root, args.dry_run)
    if not args.skip_tests:
        run_ctest(root, args.dry_run)

    if args.dry_run:
        return 0

    kursovaya = find_executable(root, "kursovaya.exe")
    fieldnames = [
        "n", "R", "T", "seed", "status", "objective",
        "solve_only_wall_time_ms", "solve_with_reconstruction_wall_time_ms",
        "solve_only_reported_time_ms", "solve_with_reconstruction_reported_time_ms",
        "reconstruction_overhead_ms", "reconstruction_overhead_percent",
        "order_length", "reconstruction_success", "reconstructed_order_cost",
        "objective_match", "error",
    ]
    rows: list[dict[str, str]] = []
    with raw_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for n in n_values:
            timeout_sec = args.timeout_until_900_sec if n <= 900 else args.timeout_after_900_sec
            for r, t in pairs:
                for seed in seeds:
                    print(f"[reconstruct] n={n} R={fmt_float(r)} T={fmt_float(t)} seed={seed}", flush=True)
                    solve_only = run_solver(kursovaya, root, n, r, t, seed, False, timeout_sec)
                    solve_with = run_solver(kursovaya, root, n, r, t, seed, True, timeout_sec)
                    status = "SOLVED" if solve_only.get("status") == "SOLVED" and solve_with.get("status") == "SOLVED" else "ERROR"
                    if solve_only.get("status") == "OOT" or solve_with.get("status") == "OOT":
                        status = "OOT"
                    objective = solve_only.get("cost", "")
                    objective_match = objective != "" and objective == solve_with.get("cost", "")
                    overhead_ms = ""
                    overhead_percent = ""
                    if status == "SOLVED":
                        overhead = float(solve_with["wall_time_ms"]) - float(solve_only["wall_time_ms"])
                        overhead_ms = f"{overhead:.3f}"
                        base = float(solve_only["wall_time_ms"])
                        overhead_percent = f"{(100.0 * overhead / base) if base > 0 else 0.0:.3f}"
                    row = {
                        "n": str(n),
                        "R": fmt_float(r),
                        "T": fmt_float(t),
                        "seed": str(seed),
                        "status": status,
                        "objective": objective,
                        "solve_only_wall_time_ms": solve_only.get("wall_time_ms", ""),
                        "solve_with_reconstruction_wall_time_ms": solve_with.get("wall_time_ms", ""),
                        "solve_only_reported_time_ms": solve_only.get("time_ms", ""),
                        "solve_with_reconstruction_reported_time_ms": solve_with.get("time_ms", ""),
                        "reconstruction_overhead_ms": overhead_ms,
                        "reconstruction_overhead_percent": overhead_percent,
                        "order_length": solve_with.get("order_length", "0"),
                        "reconstruction_success": solve_with.get("reconstruction_success", "0"),
                        "reconstructed_order_cost": solve_with.get("reconstructed_order_cost", ""),
                        "objective_match": "true" if objective_match else "false",
                        "error": solve_only.get("error", "") or solve_with.get("error", ""),
                    }
                    rows.append(row)
                    writer.writerow(row)
                    f.flush()

    write_summary(out_dir / "summary.md", rows)
    print(f"[reconstruct] raw csv: {raw_csv}")
    print(f"[reconstruct] summary: {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
