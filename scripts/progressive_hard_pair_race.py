#!/usr/bin/env python3
"""Progressive hard race for exact 1||ΣT_j solver models.

Runner запускает solver_bench последовательно: без параллельных тяжёлых solver-ов.
После каждого n он анализирует все строки этого слоя и отбрасывает явно слабые модели.
"""

from __future__ import annotations

import argparse
import csv
import math
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import median
from typing import Iterable


DEFAULT_MODELS = [
    "lawler",
    "szwarc",
    "both",
    "adaptive_v1",
    "adaptive_v2",
    "adaptive_v3",
]

DEFAULT_N_VALUES = [1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000]
DEFAULT_R_VALUES = [0.2, 0.4, 0.6, 0.8, 1.0]
DEFAULT_T_VALUES = [0.2, 0.4, 0.6, 0.8]
DEFAULT_SEEDS = list(range(10))
DEFAULT_PRIMARY_HARD_PAIR = "0.2:0.6"
DEFAULT_HARD_SUBSET = "0.2:0.6;0.2:0.4;0.2:0.8;0.4:0.6"


@dataclass
class ModelStageStats:
    model: str
    rows: int = 0
    solved: int = 0
    objective_mismatches: int = 0
    oot: int = 0
    oom: int = 0
    errors: int = 0
    missing: int = 0
    seed_wins: int = 0
    primary_wins: int = 0
    hard_wins: int = 0
    median_time_ms: float = math.inf
    p90_time_ms: float = math.inf
    median_penalized_time_ms: float = math.inf
    primary_median_ms: float = math.inf
    primary_p90_ms: float = math.inf
    primary_penalized_median_ms: float = math.inf
    hard_median_ms: float = math.inf
    hard_p90_ms: float = math.inf
    hard_penalized_median_ms: float = math.inf
    median_nodes: float = math.inf
    median_memory_bytes: float = math.inf
    decision: str = "keep"
    reason: str = ""


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_csv_list(text: str, cast) -> list:
    if not text:
        return []
    return [cast(part.strip()) for part in text.split(",") if part.strip()]


def fmt_float(value: float) -> str:
    if value == int(value):
        return str(int(value))
    return f"{value:.10g}"


def hard_pairs_text(r_values: Iterable[float], t_values: Iterable[float]) -> str:
    return ";".join(f"{fmt_float(r)}:{fmt_float(t)}" for r in r_values for t in t_values)


def parse_pair_token(text: str) -> tuple[str, str]:
    parts = text.strip().split(":")
    if len(parts) != 2:
        raise ValueError(f"Invalid pair token: {text}")
    return fmt_float(float(parts[0])), fmt_float(float(parts[1]))


def parse_pair_list(text: str) -> set[tuple[str, str]]:
    pairs: set[tuple[str, str]] = set()
    for token in text.split(";"):
        token = token.strip()
        if token:
            pairs.add(parse_pair_token(token))
    return pairs


def row_pair(row: dict[str, str]) -> tuple[str, str]:
    return row.get("R", ""), row.get("T", "")


def timeout_for_n(n: int, timeout_until_1000_sec: int, timeout_after_1000_sec: int) -> int:
    return timeout_until_1000_sec if n <= 1000 else timeout_after_1000_sec


def run_command(args: list[str], cwd: Path, dry_run: bool) -> None:
    print("[race] " + " ".join(args), flush=True)
    if dry_run:
        return
    completed = subprocess.run(args, cwd=str(cwd), text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {completed.returncode}: {' '.join(args)}")


def ensure_solver_bench(root: Path) -> Path:
    candidates = [
        root / "x64" / "Release" / "solver_bench.exe",
        root / "build" / "Release" / "solver_bench.exe",
        root / "build" / "solver_bench.exe",
    ]
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError("solver_bench.exe not found; build Release first")


def read_rows(csv_path: Path) -> list[dict[str, str]]:
    if not csv_path.exists():
        return []
    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def row_model(row: dict[str, str]) -> str:
    model = row.get("model_name", "").strip()
    if model:
        return model
    return row.get("config", "").strip()


def row_key(row: dict[str, str]) -> tuple[int, str, str, str]:
    return (
        int(row["n"]),
        row["R"],
        row["T"],
        row["seed"],
    )


def parse_float(row: dict[str, str], column: str, default: float = 0.0) -> float:
    try:
        value = row.get(column, "")
        if value == "":
            return default
        return float(value)
    except ValueError:
        return default


def percentile(values: list[float], q: float) -> float:
    if not values:
        return math.inf
    ordered = sorted(values)
    idx = math.ceil(q * len(ordered)) - 1
    idx = max(0, min(idx, len(ordered) - 1))
    return ordered[idx]


def expected_instance_count(r_values: list[float], t_values: list[float], seeds: list[int]) -> int:
    return len(r_values) * len(t_values) * len(seeds)


def build_reference_objectives(stage_rows: list[dict[str, str]]) -> tuple[dict[tuple[int, str, str, str], str], list[str]]:
    refs: dict[tuple[int, str, str, str], str] = {}
    conflicts: list[str] = []
    grouped: dict[tuple[int, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in stage_rows:
        grouped[row_key(row)].append(row)

    for key, rows in grouped.items():
        solved_objectives = [row.get("objective", "") for row in rows if row.get("status") == "SOLVED" and row.get("objective", "")]
        if not solved_objectives:
            continue
        counts = Counter(solved_objectives)
        most_common = counts.most_common()
        if len(most_common) > 1 and most_common[0][1] == most_common[1][1]:
            conflicts.append(f"{key}: objective conflict {dict(counts)}")
            continue
        refs[key] = most_common[0][0]
    return refs, conflicts


def fill_scope_metrics(
    item: ModelStageStats,
    rows: list[dict[str, str]],
    expected_count: int,
    timeout_ms: float,
    median_attr: str,
    p90_attr: str,
    penalized_attr: str,
) -> None:
    solved_times: list[float] = []
    penalized_times: list[float] = []
    for row in rows:
        if row.get("status") == "SOLVED":
            value = parse_float(row, "time_ms", timeout_ms)
            solved_times.append(value)
            penalized_times.append(value)
        else:
            penalized_times.append(timeout_ms)

    missing = max(0, expected_count - len(rows))
    for _ in range(missing):
        penalized_times.append(timeout_ms)

    if solved_times:
        setattr(item, median_attr, median(solved_times))
        setattr(item, p90_attr, percentile(solved_times, 0.90))
    if penalized_times:
        setattr(item, penalized_attr, median(penalized_times))


def analyze_stage(
    n: int,
    active_models: list[str],
    all_rows: list[dict[str, str]],
    r_values: list[float],
    t_values: list[float],
    seeds: list[int],
    timeout_sec: int,
    primary_pair: tuple[str, str],
    hard_subset: set[tuple[str, str]],
) -> tuple[dict[str, ModelStageStats], list[str]]:
    active = set(active_models)
    stage_rows = [
        row for row in all_rows
        if row.get("n") == str(n) and row_model(row) in active
    ]
    refs, conflicts = build_reference_objectives(stage_rows)

    rows_by_model: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in stage_rows:
        rows_by_model[row_model(row)].append(row)

    expected = expected_instance_count(r_values, t_values, seeds)
    stats: dict[str, ModelStageStats] = {model: ModelStageStats(model=model) for model in active_models}

    # Победа считается по каждому instance среди solved-моделей с consensus objective.
    # seed_wins — по всем 20 парам, primary/hard wins — только по важным трудным классам.
    grouped: dict[tuple[int, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in stage_rows:
        grouped[row_key(row)].append(row)
    for key, rows in grouped.items():
        ref = refs.get(key)
        if ref is None:
            continue
        candidates = [
            row for row in rows
            if row.get("status") == "SOLVED"
            and row.get("objective") == ref
            and row_model(row) in active
        ]
        if not candidates:
            continue
        best_time = min(parse_float(row, "time_ms", math.inf) for row in candidates)
        for row in candidates:
            if parse_float(row, "time_ms", math.inf) <= best_time * 1.000001:
                model_stats = stats[row_model(row)]
                model_stats.seed_wins += 1
                pair = row_pair(row)
                if pair == primary_pair:
                    model_stats.primary_wins += 1
                if pair in hard_subset:
                    model_stats.hard_wins += 1

    timeout_ms = timeout_sec * 1000.0
    for model in active_models:
        model_rows = rows_by_model.get(model, [])
        item = stats[model]
        item.rows = len(model_rows)
        item.missing = max(0, expected - item.rows)

        solved_times: list[float] = []
        penalized_times: list[float] = []
        nodes: list[float] = []
        memory: list[float] = []

        for row in model_rows:
            status = row.get("status", "")
            key = row_key(row)
            ref = refs.get(key)
            if status == "SOLVED":
                item.solved += 1
                if ref is not None and row.get("objective") != ref:
                    item.objective_mismatches += 1
                time_ms = parse_float(row, "time_ms", timeout_ms)
                solved_times.append(time_ms)
                penalized_times.append(time_ms)
                nodes.append(parse_float(row, "nodes", 0.0))
                memory.append(parse_float(row, "memo_memory_used_bytes", 0.0))
            elif status == "OOT":
                item.oot += 1
                penalized_times.append(timeout_ms)
            elif status == "OOM":
                item.oom += 1
                penalized_times.append(timeout_ms)
            else:
                item.errors += 1
                penalized_times.append(timeout_ms)

        for _ in range(item.missing):
            penalized_times.append(timeout_ms)

        if solved_times:
            item.median_time_ms = median(solved_times)
            item.p90_time_ms = percentile(solved_times, 0.90)
        if penalized_times:
            item.median_penalized_time_ms = median(penalized_times)
        if nodes:
            item.median_nodes = median(nodes)
        if memory:
            item.median_memory_bytes = median(memory)

        primary_rows = [row for row in model_rows if row_pair(row) == primary_pair]
        hard_rows = [row for row in model_rows if row_pair(row) in hard_subset]
        fill_scope_metrics(
            item,
            primary_rows,
            len(seeds),
            timeout_ms,
            "primary_median_ms",
            "primary_p90_ms",
            "primary_penalized_median_ms",
        )
        fill_scope_metrics(
            item,
            hard_rows,
            len(hard_subset) * len(seeds),
            timeout_ms,
            "hard_median_ms",
            "hard_p90_ms",
            "hard_penalized_median_ms",
        )

    return stats, conflicts


def speed_threshold(n: int) -> float | None:
    if n >= 1400:
        return 1.10
    if n >= 1300:
        return 1.15
    if n >= 1200:
        return 1.20
    if n >= 1100:
        return 1.35
    return None


def decide_elimination(n: int, stats: dict[str, ModelStageStats]) -> list[str]:
    survivors = []
    eliminated = []
    max_solved = max((s.solved for s in stats.values()), default=0)
    best_primary = min((s.primary_penalized_median_ms for s in stats.values()), default=math.inf)
    threshold = speed_threshold(n)

    # Сначала строгие причины: correctness и стабильность.
    for model, item in stats.items():
        if item.objective_mismatches > 0:
            item.decision = "drop"
            item.reason = "objective mismatch"
        elif item.oot > 0:
            item.decision = "drop"
            item.reason = "OOT"
        elif item.oom > 0:
            item.decision = "drop"
            item.reason = "OOM"
        elif item.errors > 0:
            item.decision = "drop"
            item.reason = "runtime error"
        elif item.solved == 0:
            item.decision = "drop"
            item.reason = "solved_count=0"
        elif n >= 200 and item.solved < max_solved:
            item.decision = "drop"
            item.reason = f"solved_count {item.solved} < best {max_solved}"
        elif threshold is not None and item.primary_penalized_median_ms > threshold * best_primary:
            item.decision = "drop"
            item.reason = (
                f"primary pair median > {threshold:.2f}x best "
                f"({item.primary_penalized_median_ms:.3f} ms vs {best_primary:.3f} ms)"
            )

    # Не выбрасываем всех сразу только если хотя бы одна модель прошла строгие проверки.
    # Если у единственного survivor случился OOT/OOM/ERROR/mismatch, гонка должна закончиться без победителя.
    kept = [model for model, item in stats.items() if item.decision == "keep"]
    if not kept and stats:
        rescue_candidates = [
            s for s in stats.values()
            if s.objective_mismatches == 0
            and s.oot == 0
            and s.oom == 0
            and s.errors == 0
            and s.solved > 0
        ]
        if not rescue_candidates:
            return []
        best_model = min(
            rescue_candidates,
            key=lambda s: (s.primary_penalized_median_ms, s.hard_penalized_median_ms, s.median_penalized_time_ms, -s.solved),
        ).model
        stats[best_model].decision = "keep"
        stats[best_model].reason = "best remaining model kept"

    for model, item in stats.items():
        if item.decision == "keep":
            survivors.append(model)
        else:
            eliminated.append(model)
    return survivors


def write_stage_summary(
    path: Path,
    n: int,
    timeout_sec: int,
    stats: dict[str, ModelStageStats],
    conflicts: list[str],
    survivors: list[str],
    primary_pair: tuple[str, str],
    hard_subset: set[tuple[str, str]],
) -> None:
    rows = sorted(
        stats.values(),
        key=lambda s: (
            s.decision != "keep",
            s.primary_penalized_median_ms,
            s.hard_penalized_median_ms,
            s.median_penalized_time_ms,
            -s.primary_wins,
            s.model,
        ),
    )
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f"# Stage n={n}\n\n")
        f.write(f"- timeout_sec: {timeout_sec}\n")
        f.write(f"- primary_pair: R={primary_pair[0]}, T={primary_pair[1]}\n")
        hard_text = ", ".join(f"({r},{t})" for r, t in sorted(hard_subset))
        f.write(f"- hard_subset: {hard_text}\n")
        f.write(f"- survivors: {', '.join(survivors) if survivors else 'none'}\n\n")
        if conflicts:
            f.write("## Objective Conflicts\n\n")
            for conflict in conflicts:
                f.write(f"- {conflict}\n")
        f.write("\n")
        f.write("## Ranking\n\n")
        f.write("| Model | Decision | Reason | Rows | Solved | Mismatch | OOT | OOM | ERR | Primary wins | Primary median | Primary p90 | Hard wins | Hard median | Hard p90 | All wins | All median | All p90 | Median nodes | Median memory bytes |\n")
        f.write("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for s in rows:
            f.write(
                f"| {s.model} | {s.decision} | {s.reason} | {s.rows} | {s.solved} | "
                f"{s.objective_mismatches} | {s.oot} | {s.oom} | {s.errors} | "
                f"{s.primary_wins} | {s.primary_penalized_median_ms:.3f} | {s.primary_p90_ms:.3f} | "
                f"{s.hard_wins} | {s.hard_penalized_median_ms:.3f} | {s.hard_p90_ms:.3f} | "
                f"{s.seed_wins} | {s.median_penalized_time_ms:.3f} | {s.p90_time_ms:.3f} | "
                f"{s.median_nodes:.0f} | {s.median_memory_bytes:.0f} |\n"
            )


def write_final_report(
    path: Path,
    completed_n: list[int],
    survivors: list[str],
    eliminated: dict[str, str],
    primary_pair: tuple[str, str],
    hard_subset: set[tuple[str, str]],
) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Progressive Hard-Pair Race Final Report\n\n")
        f.write(f"- completed_n: {', '.join(map(str, completed_n)) if completed_n else 'none'}\n")
        f.write(f"- primary_pair: R={primary_pair[0]}, T={primary_pair[1]}\n")
        hard_text = ", ".join(f"({r},{t})" for r, t in sorted(hard_subset))
        f.write(f"- hard_subset: {hard_text}\n")
        f.write(f"- final_survivors: {', '.join(survivors) if survivors else 'none'}\n\n")
        f.write("## Eliminated Models\n\n")
        if not eliminated:
            f.write("No models eliminated.\n")
        else:
            f.write("| Model | Reason |\n|---|---|\n")
            for model, reason in eliminated.items():
                f.write(f"| {model} | {reason} |\n")


def main(argv: list[str]) -> int:
    root = repo_root_from_script()
    parser = argparse.ArgumentParser(description="Progressive hard-pair model race runner for solver_bench.")
    parser.add_argument("--models", default=",".join(DEFAULT_MODELS))
    parser.add_argument("--n-values", default=",".join(map(str, DEFAULT_N_VALUES)))
    parser.add_argument("--R-values", default=",".join(map(str, DEFAULT_R_VALUES)))
    parser.add_argument("--T-values", default=",".join(map(str, DEFAULT_T_VALUES)))
    parser.add_argument("--seeds", default=",".join(map(str, DEFAULT_SEEDS)))
    parser.add_argument("--out-dir", default=str(root / "results" / "progressive_hard_pair_race"))
    parser.add_argument("--primary-hard-pair", default=DEFAULT_PRIMARY_HARD_PAIR)
    parser.add_argument("--hard-subset", default=DEFAULT_HARD_SUBSET)
    parser.add_argument("--memory-limit-gb", type=float, default=12.0)
    parser.add_argument("--timeout-until-1000-sec", type=int, default=1200)
    parser.add_argument("--timeout-after-1000-sec", type=int, default=12600)
    parser.add_argument("--fresh", action="store_true", help="Delete previous race output directory before running.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    models = parse_csv_list(args.models, str)
    n_values = parse_csv_list(args.n_values, int)
    r_values = parse_csv_list(args.R_values, float)
    t_values = parse_csv_list(args.T_values, float)
    seeds = parse_csv_list(args.seeds, int)
    primary_pair = parse_pair_token(args.primary_hard_pair)
    hard_subset = parse_pair_list(args.hard_subset)
    hard_subset.add(primary_pair)

    out_dir = Path(args.out_dir)
    raw_csv = out_dir / "raw_results.csv"
    if args.fresh and out_dir.exists() and not args.dry_run:
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_build:
        run_command(["cmake", "--build", "build", "--config", "Release"], root, args.dry_run)
    if not args.skip_tests:
        run_command(["ctest", "--test-dir", "build", "-C", "Release", "--output-on-failure"], root, args.dry_run)

    solver_bench = ensure_solver_bench(root) if not args.dry_run else root / "x64" / "Release" / "solver_bench.exe"
    pairs = hard_pairs_text(r_values, t_values)

    active_models = list(models)
    eliminated: dict[str, str] = {}
    completed_n: list[int] = []

    for n in n_values:
        if not active_models:
            break
        timeout_sec = timeout_for_n(n, args.timeout_until_1000_sec, args.timeout_after_1000_sec)
        print(f"[race] stage n={n}, timeout={timeout_sec}s, active={active_models}", flush=True)

        for model in list(active_models):
            cmd = [
                str(solver_bench),
                "--series", "hard",
                "--model", model,
                "--hard-pairs", pairs,
                "--n-values", str(n),
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
                "--use-lower-bounds", "false",
                "--use-upper-bounds", "false",
                "--out", str(raw_csv),
                "--resume", "true",
                "--append", "true",
                "--progress", "true",
            ]
            run_command(cmd, root, args.dry_run)

        if args.dry_run:
            continue

        all_rows = read_rows(raw_csv)
        stats, conflicts = analyze_stage(
            n,
            active_models,
            all_rows,
            r_values,
            t_values,
            seeds,
            timeout_sec,
            primary_pair,
            hard_subset,
        )
        if conflicts:
            write_stage_summary(
                out_dir / f"stage_n{n}_summary.md",
                n,
                timeout_sec,
                stats,
                conflicts,
                active_models,
                primary_pair,
                hard_subset,
            )
            raise RuntimeError(f"Objective conflict at n={n}; see stage summary.")

        survivors = decide_elimination(n, stats)
        for model, item in stats.items():
            if item.decision != "keep" and model not in eliminated:
                eliminated[model] = f"n={n}: {item.reason}"
        write_stage_summary(
            out_dir / f"stage_n{n}_summary.md",
            n,
            timeout_sec,
            stats,
            conflicts,
            survivors,
            primary_pair,
            hard_subset,
        )
        completed_n.append(n)
        active_models = survivors

    write_final_report(out_dir / "final_report.md", completed_n, active_models, eliminated, primary_pair, hard_subset)
    print(f"[race] final survivors: {active_models}", flush=True)
    print(f"[race] raw csv: {raw_csv}", flush=True)
    print(f"[race] final report: {out_dir / 'final_report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
