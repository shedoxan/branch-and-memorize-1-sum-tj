#!/usr/bin/env python3
"""Compare custom memo with std_unordered memo without process-memory gate."""

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


DEFAULT_PAIRS = "0.2:0.6;0.2:0.4;0.2:0.8;0.4:0.6"
DEFAULT_N_VALUES = "50,100,200,300,500,700,900,1100,1300"
DEFAULT_SEEDS = "0,1,2,3,4,5,6,7,8,9"
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


def solver_args(backend: str, memory_limit_mb: int, seed: int, input_path: Path) -> list[str]:
    return [
        "--model", "adaptive_v3",
        "--memo-backend", backend,
        "--memo-full-key-verification",
        "--enable-memo",
        "--enable-exact-memo",
        "--terminal-rules",
        "--enable-position-filtering",
        "--enable-lawler-basic-rules",
        "--enable-rule4",
        "--no-lb",
        "--no-ub",
        "--reconstruction-trace",
        "--mem-budget-mb", str(memory_limit_mb),
        "--reconstruct",
        "--seed", str(seed),
        "--input", str(input_path),
    ]


def run_solver(bm_solver: Path, root: Path, input_path: Path, seed: int,
               timeout_sec: int, memory_limit_mb: int, backend: str) -> dict[str, str]:
    command = [str(bm_solver)] + solver_args(backend, memory_limit_mb, seed, input_path)
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


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    comparable = [row for row in rows if row["comparison_status"] in ("MATCH", "MISMATCH")]
    mismatches = [row for row in comparable if row["comparison_status"] == "MISMATCH"]
    ratios = [
        as_float(row["ratio_std_unordered_to_custom_reported_time"])
        for row in rows
        if row.get("ratio_std_unordered_to_custom_reported_time")
    ]
    ratios = sorted(x for x in ratios if x is not None)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Memo Backend No-Gate Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write("- config: adaptive_v3 + trace reconstruction + no LB/UB\n")
        f.write("- process_memory_gate: false\n")
        f.write("- backends: custom vs std_unordered\n")
        f.write(f"- comparable_rows: {len(comparable)}\n")
        f.write(f"- objective_mismatches: {len(mismatches)}\n")
        if ratios:
            f.write(f"- median_std_unordered_to_custom_reported_time: {ratios[len(ratios) // 2]:.6f}\n")


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Compare custom memo and std_unordered memo without process gate.")
    parser.add_argument("--pairs", default=DEFAULT_PAIRS, help="Semicolon-separated R:T pairs.")
    parser.add_argument("--R-values", default="", help="Optional Cartesian-grid override; requires --T-values.")
    parser.add_argument("--T-values", default="", help="Optional Cartesian-grid override; requires --R-values.")
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "memo_backend_no_gate"))
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
        "custom_status", "std_unordered_status", "comparison_status",
        "objective_match", "custom_order_valid", "std_unordered_order_valid",
        "custom_order_length", "std_unordered_order_length",
        "custom_reconstructed_order_cost", "std_unordered_reconstructed_order_cost",
        "custom_objective", "std_unordered_objective",
        "custom_reported_time_ms", "std_unordered_reported_time_ms",
        "ratio_std_unordered_to_custom_reported_time",
        "custom_wall_time_ms", "std_unordered_wall_time_ms",
        "ratio_std_unordered_to_custom_wall_time",
        "custom_nodes", "std_unordered_nodes", "nodes_match",
        "custom_memo_hits", "std_unordered_memo_hits",
        "custom_memo_misses", "std_unordered_memo_misses",
        "custom_memo_used_mb", "std_unordered_memo_used_mb",
        "custom_memo_evictions", "std_unordered_memo_evictions",
        "custom_reconstruction_time_ms", "std_unordered_reconstruction_time_ms",
        "custom_trace_fallbacks", "std_unordered_trace_fallbacks",
        "process_memory_gate", "notes",
        "custom_run_line", "std_unordered_run_line",
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
                    print(f"[memo] n={n} R={fmt_float(r)} T={fmt_float(t)} seed={seed}", flush=True)
                    custom = run_solver(
                        bm_solver, root, input_path, seed, timeout_sec, memory_limit_mb, "custom")
                    std_unordered = run_solver(
                        bm_solver, root, input_path, seed, timeout_sec, memory_limit_mb, "std_unordered")

                    custom_order = parse_order(custom.get("stdout", ""))
                    std_order = parse_order(std_unordered.get("stdout", ""))
                    custom_valid, custom_cost = evaluate_order(input_path, custom_order)
                    std_valid, std_cost = evaluate_order(input_path, std_order)
                    objective_match = (
                        custom.get("cost", "") != ""
                        and custom.get("cost", "") == std_unordered.get("cost", "")
                        and (not custom_valid or custom_cost == custom.get("cost", ""))
                        and (not std_valid or std_cost == std_unordered.get("cost", ""))
                    )
                    if custom["status"] == "SOLVED" and std_unordered["status"] == "SOLVED":
                        comparison_status = "MATCH" if objective_match and custom_valid and std_valid else "MISMATCH"
                    elif custom["status"] == "OOT" or std_unordered["status"] == "OOT":
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
                        "custom_status": custom.get("status", ""),
                        "std_unordered_status": std_unordered.get("status", ""),
                        "comparison_status": comparison_status,
                        "objective_match": "true" if objective_match else "false",
                        "custom_order_valid": "true" if custom_valid else "false",
                        "std_unordered_order_valid": "true" if std_valid else "false",
                        "custom_order_length": str(len(custom_order)),
                        "std_unordered_order_length": str(len(std_order)),
                        "custom_reconstructed_order_cost": custom_cost,
                        "std_unordered_reconstructed_order_cost": std_cost,
                        "custom_objective": custom.get("cost", ""),
                        "std_unordered_objective": std_unordered.get("cost", ""),
                        "custom_reported_time_ms": custom.get("time_ms", ""),
                        "std_unordered_reported_time_ms": std_unordered.get("time_ms", ""),
                        "ratio_std_unordered_to_custom_reported_time": ratio(
                            std_unordered.get("time_ms", ""), custom.get("time_ms", "")),
                        "custom_wall_time_ms": custom.get("wall_time_ms", ""),
                        "std_unordered_wall_time_ms": std_unordered.get("wall_time_ms", ""),
                        "ratio_std_unordered_to_custom_wall_time": ratio(
                            std_unordered.get("wall_time_ms", ""), custom.get("wall_time_ms", "")),
                        "custom_nodes": custom.get("nodes", ""),
                        "std_unordered_nodes": std_unordered.get("nodes", ""),
                        "nodes_match": "true" if custom.get("nodes", "") == std_unordered.get("nodes", "") else "false",
                        "custom_memo_hits": custom.get("memo_hits", ""),
                        "std_unordered_memo_hits": std_unordered.get("memo_hits", ""),
                        "custom_memo_misses": custom.get("memo_misses", ""),
                        "std_unordered_memo_misses": std_unordered.get("memo_misses", ""),
                        "custom_memo_used_mb": custom.get("memo_used_mb", ""),
                        "std_unordered_memo_used_mb": std_unordered.get("memo_used_mb", ""),
                        "custom_memo_evictions": custom.get("memo_evictions", ""),
                        "std_unordered_memo_evictions": std_unordered.get("memo_evictions", ""),
                        "custom_reconstruction_time_ms": custom.get("reconstruction_time_ms", ""),
                        "std_unordered_reconstruction_time_ms": std_unordered.get("reconstruction_time_ms", ""),
                        "custom_trace_fallbacks": custom.get("reconstruction_trace_fallbacks", ""),
                        "std_unordered_trace_fallbacks": std_unordered.get("reconstruction_trace_fallbacks", ""),
                        "process_memory_gate": "false",
                        "notes": custom.get("error", "") or std_unordered.get("error", ""),
                        "custom_run_line": custom.get("run_line", ""),
                        "std_unordered_run_line": std_unordered.get("run_line", ""),
                    }
                    rows.append(row)
                    writer.writerow(row)
                    f.flush()
    write_summary(out_dir / "summary.md", rows)
    print(f"[memo] raw csv: {raw_csv}")
    print(f"[memo] summary: {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
