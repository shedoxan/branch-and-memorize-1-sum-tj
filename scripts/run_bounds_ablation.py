#!/usr/bin/env python3
"""Run bounds ablation for adaptive_v3 without early elimination."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

from experiment_utils import (
    as_float,
    build_release,
    find_executable,
    fmt_float,
    median_or_nan,
    p90_or_nan,
    parse_csv_list,
    parse_pair_list,
    read_rows,
    repo_root_from_script,
    reset_output_dir,
    run_command,
    run_ctest,
    status_counts,
    timeout_for_n,
)


DEFAULT_PAIRS = "0.2:0.6;0.2:0.4;0.2:0.8;0.4:0.6"
DEFAULT_N_VALUES = "50,100,300,500,700,900,1100,1200,1400"
DEFAULT_SEEDS = "0,1,2,3,4,5,6,7,8,9"


def objective_conflicts(rows: list[dict[str, str]]) -> int:
    grouped: dict[tuple[str, str, str, str], set[str]] = defaultdict(set)
    for row in rows:
        if row.get("status") == "SOLVED":
            key = (row.get("n", ""), row.get("R", ""), row.get("T", ""), row.get("seed", ""))
            grouped[key].add(row.get("objective", ""))
    return sum(1 for values in grouped.values() if len(values) > 1)


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    configs = sorted({row.get("config", "") for row in rows})
    with path.open("w", encoding="utf-8") as f:
        f.write("# Bounds Ablation Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write(f"- objective_conflicts: {objective_conflicts(rows)}\n\n")
        f.write("| Config | Rows | Status counts | Median ms | P90 ms | Median nodes | Median bound ms | Median memory bytes |\n")
        f.write("|---|---:|---|---:|---:|---:|---:|---:|\n")
        for config in configs:
            rr = [row for row in rows if row.get("config") == config]
            solved = [row for row in rr if row.get("status") == "SOLVED"]
            f.write(
                f"| {config} | {len(rr)} | {status_counts(rr)} | "
                f"{median_or_nan(as_float(row.get('time_ms', '')) for row in solved):.3f} | "
                f"{p90_or_nan(as_float(row.get('time_ms', '')) for row in solved):.3f} | "
                f"{median_or_nan(as_float(row.get('nodes', '')) for row in solved):.0f} | "
                f"{median_or_nan(as_float(row.get('bound_time_ms', '')) for row in solved):.3f} | "
                f"{median_or_nan(as_float(row.get('memo_memory_used_bytes', '')) for row in solved):.0f} |\n"
            )


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Bounds ablation runner.")
    parser.add_argument("--pairs", default=DEFAULT_PAIRS)
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "bounds_ablation"))
    parser.add_argument("--memory-limit-gb", type=float, default=12.0)
    parser.add_argument("--timeout-until-1000-sec", type=int, default=1200)
    parser.add_argument("--timeout-after-1000-sec", type=int, default=12600)
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

    solver_bench = find_executable(root, "solver_bench.exe") if not args.dry_run else root / "x64" / "Release" / "solver_bench.exe"

    for n in n_values:
        timeout_sec = timeout_for_n(n, args.timeout_until_1000_sec, args.timeout_after_1000_sec)
        for r, t in pairs:
            cmd = [
                str(solver_bench),
                "--series", "bounds-ablation",
                "--model", "adaptive_v3",
                "--n-values", str(n),
                "--R-values", fmt_float(r),
                "--T-values", fmt_float(t),
                "--seeds", ",".join(map(str, seeds)),
                "--time-limit-sec", str(timeout_sec),
                "--memory-limit-gb", str(args.memory_limit_gb),
                "--memo-backend", "custom",
                "--memo-full-key-verification", "true",
                "--enable-memo", "true",
                "--enable-exact-memo", "true",
                "--terminal-rules", "true",
                "--enable-position-filtering", "true",
                "--enable-lawler-basic-rules", "true",
                "--enable-rule4", "true",
                "--out", str(raw_csv),
                "--resume", "true",
                "--append", "true",
                "--progress", "true",
            ]
            run_command(cmd, root, args.dry_run)

    if not args.dry_run:
        rows = read_rows(raw_csv)
        write_summary(out_dir / "summary.md", rows)
        print(f"[bounds] raw csv: {raw_csv}")
        print(f"[bounds] summary: {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

