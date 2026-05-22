#!/usr/bin/env python3
"""Compare the final solver with an external article-code executable."""

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
    parse_csv_list,
    read_rows,
    repo_root_from_script,
    reset_output_dir,
    run_ctest,
    timeout_for_n,
)


DEFAULT_R_VALUES = "0.2,0.4,0.6,0.8,1.0"
DEFAULT_T_VALUES = "0.2,0.4,0.6,0.8"
DEFAULT_N_VALUES = "50,100,200,300,400,500,600,700,800,900,1000,1100,1200,1300"
DEFAULT_SEEDS = "0,1,2,3,4,5,6,7,8,9"
RUN_TOKEN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")


def parse_run_line(stdout: str) -> dict[str, str]:
    for line in stdout.splitlines():
        if line.startswith("[run] "):
            return {key: value for key, value in RUN_TOKEN_RE.findall(line)}
    return {}


def instance_path(data_root: Path, n: int, r: float, t: float, seed: int, offset: int) -> Path:
    file_seed = seed + offset
    return data_root / str(n) / f"SDT_{n}_{fmt_float(r)}_{fmt_float(t)}_{file_seed}.txt"


def run_ours(kursovaya: Path, root: Path, n: int, r: float, t: float, seed: int,
             input_path: Path | None, timeout_sec: int) -> dict[str, str]:
    cmd = [
        str(kursovaya),
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
        "--no-reconstruct",
        "--mem-budget-mb", "12288",
    ]
    if input_path is not None:
        cmd += ["--input", str(input_path)]
    else:
        cmd += [
            "--n", str(n),
            "--seed", str(seed),
            "--due-range", fmt_float(r),
            "--due-tardiness", fmt_float(t),
        ]
    start = time.perf_counter()
    try:
        completed = subprocess.run(cmd, cwd=str(root), text=True, capture_output=True, timeout=timeout_sec)
        wall_ms = (time.perf_counter() - start) * 1000.0
        parsed = parse_run_line(completed.stdout)
        parsed["status"] = "SOLVED" if completed.returncode == 0 and parsed else "ERROR"
        parsed["wall_time_ms"] = f"{wall_ms:.3f}"
        parsed["error"] = "" if completed.returncode == 0 else completed.stderr.strip()
        return parsed
    except subprocess.TimeoutExpired:
        return {"status": "OOT", "wall_time_ms": f"{timeout_sec * 1000.0:.3f}", "error": "subprocess_timeout"}


def run_article(command_template: str, article_exe: str, instance: Path | None,
                n: int, r: float, t: float, seed: int, timeout_sec: int,
                objective_regex: re.Pattern[str]) -> dict[str, str]:
    if not command_template:
        return {"status": "NOT_CONFIGURED", "objective": "", "wall_time_ms": "", "error": ""}
    command = command_template.format(
        article_exe=article_exe,
        instance=str(instance) if instance is not None else "",
        n=n,
        R=fmt_float(r),
        T=fmt_float(t),
        seed=seed,
        timeout_sec=timeout_sec,
    )
    start = time.perf_counter()
    try:
        completed = subprocess.run(command, text=True, capture_output=True, timeout=timeout_sec, shell=True)
        wall_ms = (time.perf_counter() - start) * 1000.0
        text = completed.stdout + "\n" + completed.stderr
        match = objective_regex.search(text)
        return {
            "status": "SOLVED" if completed.returncode == 0 else "ERROR",
            "objective": match.group(1) if match else "",
            "wall_time_ms": f"{wall_ms:.3f}",
            "error": "" if completed.returncode == 0 else text.strip()[:500],
        }
    except subprocess.TimeoutExpired:
        return {"status": "OOT", "objective": "", "wall_time_ms": f"{timeout_sec * 1000.0:.3f}", "error": "subprocess_timeout"}


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    ours_solved = sum(1 for row in rows if row["ours_status"] == "SOLVED")
    article_solved = sum(1 for row in rows if row["article_status"] == "SOLVED")
    comparable = [row for row in rows if row["article_objective"]]
    mismatches = sum(1 for row in comparable if row["objective_match"] != "true")
    with path.open("w", encoding="utf-8") as f:
        f.write("# Article Code Comparison Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write(f"- ours_solved: {ours_solved}\n")
        f.write(f"- article_solved: {article_solved}\n")
        f.write(f"- comparable_rows: {len(comparable)}\n")
        f.write(f"- objective_mismatches: {mismatches}\n")


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Final solver vs article-code comparison runner.")
    parser.add_argument("--R-values", default=DEFAULT_R_VALUES)
    parser.add_argument("--T-values", default=DEFAULT_T_VALUES)
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "article_code_comparison"))
    parser.add_argument("--data-root", default="", help="Optional root with n/SDT_n_R_T_seed.txt files.")
    parser.add_argument("--file-seed-offset", type=int, default=0)
    parser.add_argument("--article-exe", default="")
    parser.add_argument("--article-command-template", default="", help="May use {article_exe}, {instance}, {n}, {R}, {T}, {seed}, {timeout_sec}.")
    parser.add_argument("--article-objective-regex", default=r"(?:objective|optimum|cost|value)\D+(\d+)")
    parser.add_argument("--memory-limit-gb", type=float, default=12.0)
    parser.add_argument("--timeout-until-1000-sec", type=int, default=1200)
    parser.add_argument("--timeout-after-1000-sec", type=int, default=12600)
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    r_values = parse_csv_list(args.R_values, float)
    t_values = parse_csv_list(args.T_values, float)
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
    data_root = Path(args.data_root) if args.data_root else None
    objective_regex = re.compile(args.article_objective_regex, re.IGNORECASE)
    fieldnames = [
        "n", "R", "T", "seed", "instance_path",
        "ours_status", "ours_objective", "ours_reported_time_ms", "ours_wall_time_ms",
        "article_status", "article_objective", "article_wall_time_ms",
        "objective_match", "notes",
    ]
    rows: list[dict[str, str]] = []
    with raw_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for n in n_values:
            timeout_sec = timeout_for_n(n, args.timeout_until_1000_sec, args.timeout_after_1000_sec)
            for r in r_values:
                for t in t_values:
                    for seed in seeds:
                        inst = instance_path(data_root, n, r, t, seed, args.file_seed_offset) if data_root else None
                        if inst is not None and not inst.exists():
                            notes = "missing_instance_file"
                            inst_for_run = None
                        else:
                            notes = ""
                            inst_for_run = inst
                        print(f"[article] n={n} R={fmt_float(r)} T={fmt_float(t)} seed={seed}", flush=True)
                        ours = run_ours(kursovaya, root, n, r, t, seed, inst_for_run, timeout_sec)
                        article = run_article(
                            args.article_command_template,
                            args.article_exe,
                            inst_for_run,
                            n,
                            r,
                            t,
                            seed,
                            timeout_sec,
                            objective_regex,
                        )
                        objective_match = (
                            article.get("objective", "") != ""
                            and ours.get("cost", "") == article.get("objective", "")
                        )
                        row = {
                            "n": str(n),
                            "R": fmt_float(r),
                            "T": fmt_float(t),
                            "seed": str(seed),
                            "instance_path": str(inst_for_run) if inst_for_run else "",
                            "ours_status": ours.get("status", ""),
                            "ours_objective": ours.get("cost", ""),
                            "ours_reported_time_ms": ours.get("time_ms", ""),
                            "ours_wall_time_ms": ours.get("wall_time_ms", ""),
                            "article_status": article.get("status", ""),
                            "article_objective": article.get("objective", ""),
                            "article_wall_time_ms": article.get("wall_time_ms", ""),
                            "objective_match": "true" if objective_match else "false",
                            "notes": notes or ours.get("error", "") or article.get("error", ""),
                        }
                        rows.append(row)
                        writer.writerow(row)
                        f.flush()

    write_summary(out_dir / "summary.md", rows)
    print(f"[article] raw csv: {raw_csv}")
    print(f"[article] summary: {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
