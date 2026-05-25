#!/usr/bin/env python3
"""Measure overhead of the current best trace-based reconstruction mode."""

from __future__ import annotations

import argparse
import csv
import math
import re
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True

from experiment_utils import (
    build_release,
    find_executable,
    fmt_float,
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
            parsed = {key: value for key, value in RUN_TOKEN_RE.findall(line)}
            parsed["run_line"] = line
            return parsed
    return {}


def parse_order(stdout: str) -> list[int]:
    for line in stdout.splitlines():
        if line.startswith("order:"):
            body = line[len("order:"):].strip()
            return [] if not body else [int(x) for x in body.split()]
        if line.startswith("[order"):
            close = line.find("]")
            if close >= 0:
                body = line[close + 1:].strip()
                return [] if not body else [int(x) for x in body.split()]
    return []


def load_instance(path: Path) -> list[tuple[int, int]]:
    jobs: list[tuple[int, int]] = []
    with path.open("r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            jobs.append((int(parts[0]), int(parts[1])))
    return jobs


def evaluate_order(path: Path, order: list[int]) -> tuple[bool, str]:
    jobs = load_instance(path)
    n = len(jobs)
    if len(order) != n:
        return False, ""
    if sorted(order) != list(range(n)):
        return False, ""
    current = 0
    total = 0
    for job in order:
        p, d = jobs[job]
        current += p
        total += max(current - d, 0)
    return True, str(total)


def run_process(command: list[str], cwd: Path, timeout_sec: int) -> dict[str, str]:
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            text=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
        )
    except subprocess.TimeoutExpired as ex:
        wall_ms = (time.perf_counter() - start) * 1000.0
        return {
            "status": "OOT",
            "wall_time_ms": f"{wall_ms:.3f}",
            "stdout": ex.stdout or "",
            "stderr": ex.stderr or "",
            "error": "subprocess_timeout",
        }
    wall_ms = (time.perf_counter() - start) * 1000.0
    parsed = parse_run_line(completed.stdout)
    parsed["status"] = "SOLVED" if completed.returncode == 0 and parsed else "ERROR"
    parsed["exit_code"] = str(completed.returncode)
    parsed["wall_time_ms"] = f"{wall_ms:.3f}"
    parsed["stdout"] = completed.stdout
    parsed["stderr"] = completed.stderr
    parsed["error"] = "" if completed.returncode == 0 else completed.stderr.strip()
    return parsed


def instance_path(data_root: Path, n: int, r: float, t: float, seed: int, offset: int) -> Path:
    return data_root / str(n) / f"SDT_{n}_{fmt_float(r)}_{fmt_float(t)}_{seed + offset}.txt"


def generate_instance(bm_solver: Path, root: Path, data_root: Path, n: int,
                      r: float, t: float, seed: int, offset: int) -> Path:
    path = instance_path(data_root, n, r, t, seed, offset)
    path.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(bm_solver),
        "--n", str(n),
        "--p-min", "1",
        "--p-max", "100",
        "--due-range", fmt_float(r),
        "--due-tardiness", fmt_float(t),
        "--seed", str(seed),
        "--dump-instance", str(path),
    ]
    completed = subprocess.run(
        command, cwd=str(root), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        raise RuntimeError("dump-instance failed: " + completed.stderr.strip())
    return path


def best_args(memory_limit_mb: int, seed: int, input_path: Path) -> list[str]:
    return [
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
        "--mem-budget-mb", str(memory_limit_mb),
        "--seed", str(seed),
        "--input", str(input_path),
    ]


def run_solver(bm_solver: Path, root: Path, input_path: Path, seed: int,
               timeout_sec: int, memory_limit_mb: int, reconstruct_trace: bool) -> dict[str, str]:
    command = [str(bm_solver)] + best_args(memory_limit_mb, seed, input_path)
    if reconstruct_trace:
        command += ["--reconstruction-trace", "--reconstruct"]
    else:
        command += ["--no-reconstruct"]
    return run_process(command, root, timeout_sec)


def as_float(value: str) -> float | None:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return None
    return out if math.isfinite(out) else None


def ratio(numerator: str, denominator: str) -> str:
    a = as_float(numerator)
    b = as_float(denominator)
    if a is None or b is None or b <= 0.0:
        return ""
    return f"{a / b:.6f}"


def diff_ms(a: str, b: str) -> str:
    left = as_float(a)
    right = as_float(b)
    if left is None or right is None:
        return ""
    return f"{left - right:.6f}"


def pct_over(base: str, current: str) -> str:
    b = as_float(base)
    c = as_float(current)
    if b is None or c is None or b <= 0.0:
        return ""
    return f"{((c / b) - 1.0) * 100.0:.6f}"


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    comparable = [row for row in rows if row["comparison_status"] in ("MATCH", "MISMATCH")]
    mismatches = [row for row in comparable if row["comparison_status"] == "MISMATCH"]
    overheads = [
        as_float(row["reported_overhead_percent"])
        for row in rows
        if row.get("reported_overhead_percent")
    ]
    overheads = sorted(x for x in overheads if x is not None)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Best Reconstruction Overhead Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write("- config: adaptive_v3 + custom memo + no LB/UB + trace reconstruction\n")
        f.write("- process_memory_gate: false\n")
        f.write(f"- comparable_rows: {len(comparable)}\n")
        f.write(f"- objective_mismatches: {len(mismatches)}\n")
        if overheads:
            f.write(f"- median_reported_overhead_percent: {overheads[len(overheads) // 2]:.6f}\n")


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Measure overhead of current best reconstruction mode.")
    parser.add_argument("--pairs", default=DEFAULT_PAIRS, help="Semicolon-separated R:T pairs.")
    parser.add_argument("--R-values", default="", help="Optional Cartesian-grid override; requires --T-values.")
    parser.add_argument("--T-values", default="", help="Optional Cartesian-grid override; requires --R-values.")
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "best_reconstruction_overhead"))
    parser.add_argument("--file-seed-offset", type=int, default=1)
    parser.add_argument("--memory-limit-gb", type=float, default=12.0)
    parser.add_argument("--timeout-until-1000-sec", type=int, default=1200)
    parser.add_argument("--timeout-after-1000-sec", type=int, default=12600)
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.R_values or args.T_values:
        if not args.R_values or not args.T_values:
            parser.error("--R-values and --T-values must be provided together.")
        r_values = parse_csv_list(args.R_values, float)
        t_values = parse_csv_list(args.T_values, float)
        pairs = [(r, t) for r in r_values for t in t_values]
    else:
        pairs = parse_pair_list(args.pairs)
    n_values = parse_csv_list(args.n_values, int)
    seeds = parse_csv_list(args.seeds, int)
    out_dir = Path(args.out_dir)
    reset_output_dir(out_dir, args.fresh, args.dry_run)

    if not args.skip_build:
        build_release(root, args.dry_run)
    if not args.skip_tests:
        run_ctest(root, args.dry_run)
    if args.dry_run:
        return 0

    bm_solver = find_executable(root, "bm_solver.exe")
    data_root = out_dir / "data"
    memory_limit_mb = int(round(args.memory_limit_gb * 1024.0))
    raw_csv = out_dir / "raw_results.csv"
    fieldnames = [
        "n", "R", "T", "seed", "input_path", "memory_limit_mb", "timeout_sec",
        "solve_only_status", "trace_reconstruct_status", "comparison_status",
        "objective_match", "order_valid", "order_length", "reconstructed_order_cost",
        "solve_only_objective", "trace_reconstruct_objective",
        "solve_only_reported_time_ms", "trace_reconstruct_reported_time_ms",
        "reported_overhead_ms", "reported_overhead_percent",
        "solve_only_wall_time_ms", "trace_reconstruct_wall_time_ms",
        "wall_overhead_ms", "wall_overhead_percent",
        "solve_only_nodes", "trace_reconstruct_nodes", "nodes_match",
        "solve_only_memo_hits", "trace_reconstruct_memo_hits",
        "solve_only_memo_misses", "trace_reconstruct_memo_misses",
        "solve_only_memo_used_mb", "trace_reconstruct_memo_used_mb",
        "trace_reconstruction_time_ms", "trace_reconstruction_trace_hits",
        "trace_reconstruction_trace_fallbacks", "notes",
        "solve_only_run_line", "trace_reconstruct_run_line",
    ]
    rows: list[dict[str, str]] = []
    with raw_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for n in n_values:
            timeout_sec = timeout_for_n(n, args.timeout_until_1000_sec, args.timeout_after_1000_sec)
            for r, t in pairs:
                for seed in seeds:
                    input_path = generate_instance(
                        bm_solver, root, data_root, n, r, t, seed, args.file_seed_offset)
                    print(f"[reconstruct] n={n} R={fmt_float(r)} T={fmt_float(t)} seed={seed}", flush=True)
                    solve_only = run_solver(
                        bm_solver, root, input_path, seed, timeout_sec, memory_limit_mb, False)
                    trace = run_solver(
                        bm_solver, root, input_path, seed, timeout_sec, memory_limit_mb, True)

                    order = parse_order(trace.get("stdout", ""))
                    order_valid, order_cost = evaluate_order(input_path, order)
                    objective_match = (
                        solve_only.get("cost", "") != ""
                        and solve_only.get("cost", "") == trace.get("cost", "")
                        and (not order_valid or order_cost == trace.get("cost", ""))
                    )
                    if solve_only["status"] == "SOLVED" and trace["status"] == "SOLVED":
                        comparison_status = "MATCH" if objective_match and order_valid else "MISMATCH"
                    elif solve_only["status"] == "OOT" or trace["status"] == "OOT":
                        comparison_status = "OOT"
                    else:
                        comparison_status = "ERROR"

                    row = {
                        "n": str(n),
                        "R": fmt_float(r),
                        "T": fmt_float(t),
                        "seed": str(seed),
                        "input_path": str(input_path),
                        "memory_limit_mb": str(memory_limit_mb),
                        "timeout_sec": str(timeout_sec),
                        "solve_only_status": solve_only.get("status", ""),
                        "trace_reconstruct_status": trace.get("status", ""),
                        "comparison_status": comparison_status,
                        "objective_match": "true" if objective_match else "false",
                        "order_valid": "true" if order_valid else "false",
                        "order_length": str(len(order)),
                        "reconstructed_order_cost": order_cost,
                        "solve_only_objective": solve_only.get("cost", ""),
                        "trace_reconstruct_objective": trace.get("cost", ""),
                        "solve_only_reported_time_ms": solve_only.get("time_ms", ""),
                        "trace_reconstruct_reported_time_ms": trace.get("time_ms", ""),
                        "reported_overhead_ms": diff_ms(trace.get("time_ms", ""), solve_only.get("time_ms", "")),
                        "reported_overhead_percent": pct_over(solve_only.get("time_ms", ""), trace.get("time_ms", "")),
                        "solve_only_wall_time_ms": solve_only.get("wall_time_ms", ""),
                        "trace_reconstruct_wall_time_ms": trace.get("wall_time_ms", ""),
                        "wall_overhead_ms": diff_ms(trace.get("wall_time_ms", ""), solve_only.get("wall_time_ms", "")),
                        "wall_overhead_percent": pct_over(solve_only.get("wall_time_ms", ""), trace.get("wall_time_ms", "")),
                        "solve_only_nodes": solve_only.get("nodes", ""),
                        "trace_reconstruct_nodes": trace.get("nodes", ""),
                        "nodes_match": "true" if solve_only.get("nodes", "") == trace.get("nodes", "") else "false",
                        "solve_only_memo_hits": solve_only.get("memo_hits", ""),
                        "trace_reconstruct_memo_hits": trace.get("memo_hits", ""),
                        "solve_only_memo_misses": solve_only.get("memo_misses", ""),
                        "trace_reconstruct_memo_misses": trace.get("memo_misses", ""),
                        "solve_only_memo_used_mb": solve_only.get("memo_used_mb", ""),
                        "trace_reconstruct_memo_used_mb": trace.get("memo_used_mb", ""),
                        "trace_reconstruction_time_ms": trace.get("reconstruction_time_ms", ""),
                        "trace_reconstruction_trace_hits": trace.get("reconstruction_trace_hits", ""),
                        "trace_reconstruction_trace_fallbacks": trace.get("reconstruction_trace_fallbacks", ""),
                        "notes": solve_only.get("error", "") or trace.get("error", ""),
                        "solve_only_run_line": solve_only.get("run_line", ""),
                        "trace_reconstruct_run_line": trace.get("run_line", ""),
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
