#!/usr/bin/env python3
"""Compare the final solver with an external article-code executable."""

from __future__ import annotations

import argparse
import csv
import math
import os
import random
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
ARTICLE_TOKEN_RE = re.compile(r"#?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^\s]+)")
ARTICLE_MEM_RE = re.compile(r"#?NbBytesMem\s*=\s*(\d+)", re.IGNORECASE)
ARTICLE_RAM_RE = re.compile(r"\bRAM\s*=\s*(\d+)", re.IGNORECASE)

OUR_RUN_KEYS = [
    "n", "seed", "input", "decomp", "memo_backend", "use_ub", "use_lb",
    "terminal_rules", "position_filtering_enabled", "enable_lawler_basic_rules",
    "enable_simple_lb", "enable_lb_memo", "enable_edd_ub", "ub_depth_limit",
    "lb_depth_limit", "terminal_all_tardy_spt", "terminal_edd_at_most_one_tardy",
    "enable_memo", "enable_exact_memo", "memo_full_key_verification",
    "memo_memory_limit_mb", "cost", "reconstruction_success",
    "reconstructed_order_cost", "time_ms", "nodes", "max_depth", "memo_hits",
    "memo_misses", "memo_exact_hits", "memo_lb_hits", "rule4", "valid_pos",
    "valid_pos_pruned_3a", "valid_pos_pruned_3b", "memo_rejected",
    "memo_forced_evict", "lufo_passes", "terminal_spt", "terminal_edd",
    "memo_peak", "memo_final", "memo_evictions", "memo_clean_time_ms",
    "memo_used_mb", "memo_bytes_per_entry",
]

ARTICLE_METRIC_KEYS = [
    "TT", "time", "RAM", "NbBytesMem", "NbBytesMem2", "NbEntryMem",
    "NbEntryMem2", "NbJobsMem", "Hits", "Hits2", "Miss", "Miss2", "All",
    "MerL", "szMerL", "MerR", "szMerR", "MovL", "MovR", "MinRatio",
]


def process_working_set_bytes(pid: int) -> int:
    """Best-effort peak sampler for a running process; currently implemented on Windows."""
    if os.name != "nt":
        return 0
    try:
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_INFORMATION = 0x0400
        PROCESS_VM_READ = 0x0010

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        handle = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not handle:
            return 0
        try:
            counters = PROCESS_MEMORY_COUNTERS()
            counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
            ok = psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
            if not ok:
                return 0
            return int(counters.WorkingSetSize)
        finally:
            kernel32.CloseHandle(handle)
    except Exception:
        return 0


def run_process(command, cwd: str | None, timeout_sec: int, shell: bool) -> tuple[int | None, str, str, float, int, bool]:
    start = time.perf_counter()
    peak_bytes = 0
    proc = subprocess.Popen(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        shell=shell,
    )
    timed_out = False
    while proc.poll() is None:
        peak_bytes = max(peak_bytes, process_working_set_bytes(proc.pid))
        if time.perf_counter() - start > timeout_sec:
            timed_out = True
            proc.kill()
            break
        time.sleep(0.05)
    peak_bytes = max(peak_bytes, process_working_set_bytes(proc.pid))
    stdout, stderr = proc.communicate()
    wall_ms = (time.perf_counter() - start) * 1000.0
    return proc.returncode, stdout, stderr, wall_ms, peak_bytes, timed_out


def find_run_line(stdout: str) -> str:
    for line in stdout.splitlines():
        if line.startswith("[run] "):
            return line
    return ""


def parse_run_line(stdout: str) -> dict[str, str]:
    line = find_run_line(stdout)
    if line:
        return {key: value for key, value in RUN_TOKEN_RE.findall(line)}
    return {}


def parse_article_metrics(text: str) -> dict[str, str]:
    metrics: dict[str, str] = {}
    for key, value in ARTICLE_TOKEN_RE.findall(text):
        metrics[key] = value
    return metrics


def order_length(stdout: str) -> int:
    """Return the number of jobs printed in the reconstructed order."""
    for line in stdout.splitlines():
        if line.startswith("order:"):
            body = line[len("order:"):].strip()
            return 0 if not body else len(body.split())
        if line.startswith("[order"):
            close = line.find("]")
            if close >= 0:
                body = line[close + 1:].strip()
                return 0 if not body else len(body.split())
    return 0


def as_float(text: str) -> float | None:
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    if math.isfinite(value):
        return value
    return None


def ratio(numerator: str, denominator: str) -> str:
    a = as_float(numerator)
    b = as_float(denominator)
    if a is None or b is None or b <= 0.0:
        return ""
    return f"{a / b:.6f}"


def memo_used_bytes_from_run(parsed: dict[str, str]) -> str:
    mb = as_float(parsed.get("memo_used_mb", ""))
    if mb is None:
        return ""
    return str(int(round(mb * 1024.0 * 1024.0)))


def instance_path(data_root: Path, n: int, r: float, t: float, seed: int, offset: int) -> Path:
    file_seed = seed + offset
    return data_root / str(n) / f"SDT_{n}_{fmt_float(r)}_{fmt_float(t)}_{file_seed}.txt"


def generate_shared_instance_file(data_root: Path, n: int, r: float, t: float, seed: int,
                                  offset: int, p_min: int = 1, p_max: int = 100) -> Path:
    """Generate one shared Potts-style instance file read by both solvers."""
    rng = random.Random(seed)
    processing_times = [rng.randint(p_min, p_max) for _ in range(n)]
    total_processing = sum(processing_times)
    lower_factor = 1.0 - t - r * 0.5
    upper_factor = 1.0 - t + r * 0.5
    due_low = int(float(total_processing) * lower_factor)
    due_high = int(float(total_processing) * upper_factor)
    if due_low > due_high:
        due_low, due_high = due_high, due_low
    due_dates = [max(0, rng.randint(due_low, due_high)) for _ in range(n)]

    path = instance_path(data_root, n, r, t, seed, offset)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for p, d in zip(processing_times, due_dates):
            f.write(f"{p} {d}\n")
    return path


def article_instance_id(r: float, t: float, seed: int) -> int:
    """Map (R,T,seed=0..9) to the 1-based id used by the article code."""
    r_index = round(r / 0.2) - 1
    t_index = round(t / 0.2) - 1
    if r_index < 0 or t_index < 0 or seed < 0:
        raise ValueError(f"Cannot map R={r}, T={t}, seed={seed} to article instance id.")
    return int(r_index * 40 + t_index * 10 + seed + 1)


def prepare_article_config(article_exe: str, explicit_config: str, out_dir: Path, memory_limit_mb: int) -> str:
    """Create a config for one-instance article runs without ONLY_HARDEST filtering."""
    if not article_exe:
        return explicit_config
    source = Path(explicit_config) if explicit_config else Path(article_exe).resolve().parent / "config.ini"
    if not source.exists():
        return str(source)

    target = out_dir / "article_config_generated.ini"
    lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
    replaced_hardest = False
    with target.open("w", encoding="utf-8", newline="\n") as f:
        for line in lines:
            if line.startswith("ONLY_HARDEST="):
                f.write("ONLY_HARDEST=0\n")
                replaced_hardest = True
            elif line.startswith("RAM_LIM_MO="):
                f.write(f"RAM_LIM_MO={memory_limit_mb}\n")
            else:
                f.write(line + "\n")
        if not replaced_hardest:
            f.write("ONLY_HARDEST=0\n")
        if not any(line.startswith("RAM_LIM_MO=") for line in lines):
            f.write(f"RAM_LIM_MO={memory_limit_mb}\n")
    return str(target)


def run_ours(kursovaya: Path, root: Path, n: int, r: float, t: float, seed: int,
             input_path: Path, timeout_sec: int, reconstruct: bool, memory_limit_mb: int) -> dict[str, str]:
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
        "--mem-budget-mb", str(memory_limit_mb),
    ]
    cmd.append("--reconstruct" if reconstruct else "--no-reconstruct")
    cmd += ["--seed", str(seed)]
    cmd += ["--input", str(input_path)]
    returncode, stdout, stderr, wall_ms, peak_bytes, timed_out = run_process(
        cmd, str(root), timeout_sec, shell=False)
    if timed_out:
        return {"status": "OOT", "wall_time_ms": f"{timeout_sec * 1000.0:.3f}", "error": "subprocess_timeout"}
    parsed = parse_run_line(stdout)
    parsed["run_line"] = find_run_line(stdout)
    parsed["status"] = "SOLVED" if returncode == 0 and parsed else "ERROR"
    parsed["wall_time_ms"] = f"{wall_ms:.3f}"
    parsed["process_peak_working_set_bytes"] = str(peak_bytes)
    parsed["memo_used_bytes"] = memo_used_bytes_from_run(parsed)
    parsed["error"] = "" if returncode == 0 else stderr.strip()
    if reconstruct:
        parsed["wall_time_ms"] = f"{wall_ms:.3f}"
        parsed["order_length"] = str(order_length(stdout))
    return parsed


def run_article(command_template: str, article_exe: str, article_config: str, instance: Path,
                article_work_dir: Path,
                n: int, r: float, t: float, seed: int, timeout_sec: int,
                objective_regex: re.Pattern[str]) -> dict[str, str]:
    if not command_template and not article_exe:
        return {"status": "NOT_CONFIGURED", "objective": "", "wall_time_ms": "", "error": ""}
    art_id = article_instance_id(r, t, seed)
    cwd = str(article_work_dir)
    if command_template:
        command = command_template.format(
            article_exe=article_exe,
            article_config=article_config,
            article_id=art_id,
            instance=str(instance) if instance is not None else "",
            n=n,
            R=fmt_float(r),
            T=fmt_float(t),
            seed=seed,
            timeout_sec=timeout_sec,
        )
        shell = True
    else:
        command = [article_exe, article_config, str(n), str(art_id)]
        shell = False
    returncode, stdout, stderr, wall_ms, peak_bytes, timed_out = run_process(
        command, cwd, timeout_sec, shell=shell)
    if timed_out:
        return {"status": "OOT", "objective": "", "wall_time_ms": f"{timeout_sec * 1000.0:.3f}", "error": "subprocess_timeout"}
    text = stdout + "\n" + stderr
    article_metrics = parse_article_metrics(text)
    objective_match = objective_regex.search(text)
    memo_match = ARTICLE_MEM_RE.search(text)
    ram_match = ARTICLE_RAM_RE.search(text)
    result = {
        "status": "SOLVED" if returncode == 0 and objective_match else "ERROR",
        "objective": objective_match.group(1) if objective_match else "",
        "wall_time_ms": f"{wall_ms:.3f}",
        "process_peak_working_set_bytes": str(peak_bytes),
        "memo_bytes": memo_match.group(1) if memo_match else "",
        "reported_ram_bytes": ram_match.group(1) if ram_match else "",
        "error": "" if returncode == 0 else text.strip()[:500],
        "raw_metrics": ";".join(f"{k}={v}" for k, v in sorted(article_metrics.items())),
    }
    for key in ARTICLE_METRIC_KEYS:
        if key in article_metrics:
            result[key] = article_metrics[key]
    return result


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    ours_solved = sum(1 for row in rows if row["ours_reconstruct_status"] == "SOLVED")
    article_solved = sum(1 for row in rows if row["article_status"] == "SOLVED")
    comparable = [row for row in rows if row["article_objective"]]
    mismatches = sum(1 for row in comparable if row["objective_match"] != "true")
    ratios_reconstruct = [
        as_float(row["ratio_ours_reconstruct_wall_to_article"])
        for row in rows
        if row.get("ratio_ours_reconstruct_wall_to_article")
    ]
    ratios_reconstruct = [x for x in ratios_reconstruct if x is not None]
    with path.open("w", encoding="utf-8") as f:
        f.write("# Article Code Comparison Summary\n\n")
        f.write(f"- rows: {len(rows)}\n")
        f.write("- data_mode: generated_shared_instances\n")
        f.write("- ours_config: adaptive_v3 + custom memo + reconstruction + no process-memory gate\n")
        f.write(f"- ours_solved: {ours_solved}\n")
        f.write(f"- article_solved: {article_solved}\n")
        f.write(f"- comparable_rows: {len(comparable)}\n")
        f.write(f"- objective_mismatches: {mismatches}\n")
        if ratios_reconstruct:
            ratios_reconstruct.sort()
            f.write(f"- median_ratio_ours_reconstruct_wall_to_article: {ratios_reconstruct[len(ratios_reconstruct) // 2]:.6f}\n")


def main() -> int:
    root = repo_root_from_script(__file__)
    parser = argparse.ArgumentParser(description="Final solver vs article-code comparison runner.")
    parser.add_argument("--R-values", default=DEFAULT_R_VALUES)
    parser.add_argument("--T-values", default=DEFAULT_T_VALUES)
    parser.add_argument("--n-values", default=DEFAULT_N_VALUES)
    parser.add_argument("--seeds", default=DEFAULT_SEEDS)
    parser.add_argument("--out-dir", default=str(root / "results" / "article_code_comparison"))
    parser.add_argument("--shared-work-dir", default="", help="Directory where the runner creates data/<n>/SDT_*.txt for both solvers.")
    parser.add_argument("--file-seed-offset", type=int, default=1)
    parser.add_argument("--article-exe", default="")
    parser.add_argument("--article-config", default="", help="Optional article config.ini path. Default: next to article exe.")
    parser.add_argument("--article-command-template", default="", help="May use {article_exe}, {article_config}, {article_id}, {instance}, {n}, {R}, {T}, {seed}, {timeout_sec}.")
    parser.add_argument("--article-objective-regex", default=r"(?:TT|objective|optimum|cost|value)\D+(\d+)")
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
    shared_work_dir = Path(args.shared_work_dir) if args.shared_work_dir else out_dir
    data_root = shared_work_dir / "data"
    memory_limit_mb = int(round(args.memory_limit_gb * 1024.0))
    article_config = prepare_article_config(args.article_exe, args.article_config, out_dir, memory_limit_mb)
    objective_regex = re.compile(args.article_objective_regex, re.IGNORECASE)
    fieldnames = [
        "n", "R", "T", "seed", "instance_path", "shared_work_dir", "data_mode",
        "ours_reconstruct_status", "ours_reconstruct_objective",
        "ours_reconstruct_reported_time_ms", "ours_reconstruct_wall_time_ms",
        "ours_reconstruction_success", "ours_reconstructed_order_cost", "ours_order_length",
        "ours_reconstruct_memo_used_bytes", "ours_reconstruct_peak_working_set_bytes",
        "article_status", "article_objective", "article_wall_time_ms",
        "article_memo_bytes", "article_reported_ram_bytes", "article_peak_working_set_bytes",
        "ratio_ours_reconstruct_wall_to_article",
        "ratio_ours_reconstruct_memo_to_article_memo",
        "ratio_ours_reconstruct_peak_to_article_peak",
        "objective_match", "notes",
    ]
    fieldnames.append("ours_reconstruct_run_line")
    for key in OUR_RUN_KEYS:
        fieldnames.append(f"ours_reconstruct_run_{key}")
    fieldnames.append("article_raw_metrics")
    for key in ARTICLE_METRIC_KEYS:
        fieldnames.append(f"article_metric_{key}")

    rows: list[dict[str, str]] = []
    with raw_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for n in n_values:
            timeout_sec = timeout_for_n(n, args.timeout_until_1000_sec, args.timeout_after_1000_sec)
            for r in r_values:
                for t in t_values:
                    for seed in seeds:
                        inst_for_run = generate_shared_instance_file(
                            data_root, n, r, t, seed, args.file_seed_offset)
                        notes = ""
                        print(f"[article] n={n} R={fmt_float(r)} T={fmt_float(t)} seed={seed}", flush=True)
                        ours_reconstruct = run_ours(
                            kursovaya, root, n, r, t, seed, inst_for_run, timeout_sec, True, memory_limit_mb)
                        article = run_article(
                            args.article_command_template,
                            args.article_exe,
                            article_config,
                            inst_for_run,
                            shared_work_dir,
                            n,
                            r,
                            t,
                            seed,
                            timeout_sec,
                            objective_regex,
                        )
                        objective_match = (
                            article.get("objective", "") != ""
                            and ours_reconstruct.get("cost", "") == article.get("objective", "")
                        )
                        ratio_reconstruct = ratio(ours_reconstruct.get("wall_time_ms", ""), article.get("wall_time_ms", ""))
                        ratio_memo = ratio(ours_reconstruct.get("memo_used_bytes", ""), article.get("memo_bytes", ""))
                        ratio_peak = ratio(
                            ours_reconstruct.get("process_peak_working_set_bytes", ""),
                            article.get("process_peak_working_set_bytes", ""),
                        )
                        row = {
                            "n": str(n),
                            "R": fmt_float(r),
                            "T": fmt_float(t),
                            "seed": str(seed),
                            "instance_path": str(inst_for_run) if inst_for_run else "",
                            "shared_work_dir": str(shared_work_dir),
                            "data_mode": "generated_shared",
                            "ours_reconstruct_status": ours_reconstruct.get("status", ""),
                            "ours_reconstruct_objective": ours_reconstruct.get("cost", ""),
                            "ours_reconstruct_reported_time_ms": ours_reconstruct.get("time_ms", ""),
                            "ours_reconstruct_wall_time_ms": ours_reconstruct.get("wall_time_ms", ""),
                            "ours_reconstruction_success": ours_reconstruct.get("reconstruction_success", ""),
                            "ours_reconstructed_order_cost": ours_reconstruct.get("reconstructed_order_cost", ""),
                            "ours_order_length": ours_reconstruct.get("order_length", ""),
                            "ours_reconstruct_memo_used_bytes": ours_reconstruct.get("memo_used_bytes", ""),
                            "ours_reconstruct_peak_working_set_bytes": ours_reconstruct.get("process_peak_working_set_bytes", ""),
                            "article_status": article.get("status", ""),
                            "article_objective": article.get("objective", ""),
                            "article_wall_time_ms": article.get("wall_time_ms", ""),
                            "article_memo_bytes": article.get("memo_bytes", ""),
                            "article_reported_ram_bytes": article.get("reported_ram_bytes", ""),
                            "article_peak_working_set_bytes": article.get("process_peak_working_set_bytes", ""),
                            "ratio_ours_reconstruct_wall_to_article": ratio_reconstruct,
                            "ratio_ours_reconstruct_memo_to_article_memo": ratio_memo,
                            "ratio_ours_reconstruct_peak_to_article_peak": ratio_peak,
                            "objective_match": "true" if objective_match else "false",
                            "notes": notes or ours_reconstruct.get("error", "") or article.get("error", ""),
                        }
                        row["ours_reconstruct_run_line"] = ours_reconstruct.get("run_line", "")
                        for key in OUR_RUN_KEYS:
                            row[f"ours_reconstruct_run_{key}"] = ours_reconstruct.get(key, "")
                        row["article_raw_metrics"] = article.get("raw_metrics", "")
                        for key in ARTICLE_METRIC_KEYS:
                            row[f"article_metric_{key}"] = article.get(key, "")
                        rows.append(row)
                        writer.writerow(row)
                        f.flush()
                        print(
                            "[article] ratios "
                            f"reconstruct/article={ratio_reconstruct or 'NA'} "
                            f"peak_mem={ratio_peak or 'NA'}",
                            flush=True,
                        )

    write_summary(out_dir / "summary.md", rows)
    print(f"[article] raw csv: {raw_csv}")
    print(f"[article] summary: {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
