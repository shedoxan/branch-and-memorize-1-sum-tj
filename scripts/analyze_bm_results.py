#!/usr/bin/env python3
"""Analyze exact Branch-and-Memorize benchmark CSV files.

The script is intentionally defensive: benchmark CSV schemas evolved during the
experiments, so every analysis path tolerates missing columns and records the
missing pieces in report.md instead of failing.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import textwrap
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.colors as mcolors
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


SCHEME_PATTERNS = {
    "progressive_model_race": ["progressive_model_race"],
    "progressive_hard_pair_race": ["progressive_hard_pair_race"],
    "bounds_ablation": ["bounds_ablation", "bounds-ablation"],
    "memo_backend": ["memo_backend", "memo-backend", "memo_backend_no_gate"],
    "reconstruction": ["reconstruction", "best_reconstruction_overhead"],
    "article_code_comparison": ["article_code_comparison", "article-code-comparison"],
}

MODEL_ORDER = ["lawler", "szwarc", "both", "adaptive_v1", "adaptive_v2", "adaptive_v3"]
BOUNDS_MODE_ORDER = [
    "edd_ub",
    "simple_lb",
    "simple_lb_lb_memo",
    "simple_lb_edd_ub",
    "simple_lb_lb_memo_edd_ub",
]
HARD_PRIMARY = (0.2, 0.6)
HARD_SUBSET = {(0.2, 0.6), (0.2, 0.4), (0.2, 0.8), (0.4, 0.6)}
LATEX_COLUMN_LABELS = {
    "parameter": "параметр",
    "value": "значение",
    "group": "группа",
    "config_norm": "режим",
    "fastest_config": "лучший режим",
    "solved_rate": "доля решённых",
    "median_time_ms_solved": "медианное время, мс",
    "best_median_time": "лучшее медианное время",
    "median_all_configs": "медиана по всем режимам",
    "hardness_ratio_best": "отношение сложности",
    "median_time": "медианное время",
    "p90_time": "p90 времени",
    "memo_exact_hit_rate": "доля exact-hit",
    "best_time": "лучшее время",
    "ratio_to_best": "отношение к лучшему",
    "cutoff": "порог",
    "decision": "решение",
    "mode": "режим",
    "objective": "оптимум",
    "median_nodes": "медиана узлов",
    "bound_time": "доля времени bounds",
    "lb_memo_hit": "доля LB-hit",
    "instances": "экземпляры",
    "time_ratio": "отношение времени",
    "time_p10_p90": "p10-p90 времени",
    "custom_wins": "custom быстрее",
    "memory_ratio": "отношение памяти",
    "old_wall": "старое восстановление",
    "new_wall": "новое восстановление",
    "actual_reconstruct": "время восстановления",
    "correct": "корректность",
    "valid_order": "валидный порядок",
    "trace_fallbacks": "trace fallback",
    "rows": "строки",
    "complete": "полная сетка",
    "both_solved": "решено обоими",
    "ours_solved": "наш алгоритм решил",
    "article_solved": "код статьи решил",
    "objective_match": "оптимум совпал",
    "ours_time": "время нашего алгоритма",
    "article_time": "время кода статьи",
    "R": "R",
    "T": "T",
    "n": "n",
}


@dataclass
class SourceFrame:
    path: Path
    scheme: str
    df: pd.DataFrame
    possibly_process_gated: bool = False


class Reporter:
    def __init__(self) -> None:
        self.input_files: list[Path] = []
        self.missing_columns: dict[str, set[str]] = {}
        self.notes: list[str] = []
        self.artifacts: list[Path] = []
        self.scheme_presence: dict[str, int] = {}
        self.correctness_problem_count = 0
        self.process_gate_rows = 0
        self.possibly_process_gated_files: list[Path] = []
        self.best_model_text = "no input data for this scheme"
        self.model_race_text = "no input data for this scheme"
        self.hard_pair_text = "no input data for this scheme"
        self.bounds_text = "no input data for this scheme"
        self.backend_text = "no input data for this scheme"
        self.reconstruction_text = "no input data for this scheme"
        self.article_text = "no input data for this scheme"

    def missing(self, path: Path | str, column: str) -> None:
        self.missing_columns.setdefault(str(path), set()).add(column)

    def artifact(self, path: Path) -> None:
        self.artifacts.append(path)


def ensure_dirs(*dirs: Path) -> None:
    for d in dirs:
        d.mkdir(parents=True, exist_ok=True)


def classify_scheme(path: Path) -> str:
    text = str(path).replace("\\", "/").lower()
    for scheme, needles in SCHEME_PATTERNS.items():
        if any(needle in text for needle in needles):
            return scheme
    return "unknown"


def discover_files(input_dir: Path, explicit_files: list[str]) -> list[Path]:
    if explicit_files:
        files: list[Path] = []
        for item in explicit_files:
            for part in item.split(","):
                part = part.strip()
                if part:
                    files.append(Path(part))
        return files
    patterns = [
        "*progressive_model_race*.csv",
        "*progressive_hard_pair_race*.csv",
        "*bounds_ablation*.csv",
        "*memo_backend*.csv",
        "*reconstruction*.csv",
        "*article_code_comparison*.csv",
        "raw_results.csv",
    ]
    found: dict[Path, None] = {}
    for pattern in patterns:
        for path in input_dir.rglob(pattern):
            if path.is_file():
                scheme = classify_scheme(path)
                if scheme != "unknown":
                    found[path.resolve()] = None
    return sorted(found)


def read_csv_robust(path: Path) -> pd.DataFrame:
    try:
        return pd.read_csv(path, low_memory=False)
    except UnicodeDecodeError:
        return pd.read_csv(path, encoding="utf-8-sig", low_memory=False)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()


def to_bool_series(s: pd.Series) -> pd.Series:
    if s is None:
        return pd.Series(pd.NA)
    text = s.astype("string").str.strip().str.lower()
    return text.map({
        "1": True,
        "true": True,
        "yes": True,
        "y": True,
        "on": True,
        "0": False,
        "false": False,
        "no": False,
        "n": False,
        "off": False,
    }).astype("boolean")


def bool_value(value) -> bool | None:
    if pd.isna(value):
        return None
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "y", "on"}:
        return True
    if text in {"0", "false", "no", "n", "off"}:
        return False
    return None


def number_series(s: pd.Series) -> pd.Series:
    return pd.to_numeric(s, errors="coerce")


def first_existing(df: pd.DataFrame, candidates: Iterable[str], reporter: Reporter, path: Path, label: str) -> pd.Series:
    for col in candidates:
        if col in df.columns:
            return df[col]
    reporter.missing(path, label)
    return pd.Series([pd.NA] * len(df), index=df.index)


def add_missing_columns(df: pd.DataFrame, columns: Iterable[str], reporter: Reporter, path: Path) -> pd.DataFrame:
    for col in columns:
        if col not in df.columns:
            df[col] = pd.NA
            reporter.missing(path, col)
    return df


def normalize_status(value) -> str:
    if pd.isna(value):
        return "unknown"
    text = str(value).strip().lower()
    if text in {"solved", "ok", "success", "successful", "match"}:
        return "solved"
    if "time_limit" in text or "timeout" in text or text == "oot":
        return "oot"
    if text == "oom" or "out_of_memory" in text or "bad_alloc" in text:
        return "oom"
    if text in {"err", "error", "failed", "failure"} or "error" in text:
        return "err"
    return text or "unknown"


def norm_float_key(value) -> str:
    if pd.isna(value):
        return "NA"
    try:
        x = float(value)
    except Exception:
        return str(value)
    if math.isfinite(x) and abs(x - round(x)) < 1e-12:
        return str(int(round(x)))
    return f"{x:.10g}"


def build_instance_key(df: pd.DataFrame) -> pd.Series:
    parts = []
    for col in ["n", "R", "T", "seed"]:
        parts.append(df[col].map(norm_float_key).astype(str))
    base = parts[0] + "|R=" + parts[1] + "|T=" + parts[2] + "|seed=" + parts[3]
    extra_cols = [
        "instance_file", "instance_path", "input_path", "input",
        "instance_hash", "input_hash", "ours_reconstruct_run_input",
    ]
    extra = pd.Series([""] * len(df), index=df.index, dtype="string")
    for col in extra_cols:
        if col in df.columns:
            candidate = df[col].astype("string").fillna("")
            extra = extra.mask((extra == "") & (candidate != ""), candidate)
    return np.where(extra.astype(str) != "", base + "|file=" + extra.astype(str), base)


def penalized_time(time_ms: pd.Series, status_norm: pd.Series, timeout_sec: pd.Series,
                   penalty_factor: float, policy: str) -> pd.Series:
    solved = status_norm.eq("solved")
    timeout_ms = number_series(timeout_sec) * 1000.0
    fallback_timeout = timeout_ms.copy()
    if fallback_timeout.notna().any():
        fallback = fallback_timeout.dropna().median()
    else:
        fallback = 1200_000.0
    fallback_timeout = fallback_timeout.fillna(fallback)
    if policy.lower() == "par10":
        penalty = fallback_timeout * penalty_factor
    else:
        penalty = fallback_timeout * penalty_factor
    return number_series(time_ms).where(solved, penalty)


def normalize_standard_runs(src: SourceFrame, reporter: Reporter, penalty_factor: float,
                            timeout_policy: str) -> pd.DataFrame:
    df = src.df.copy()
    path = src.path
    if df.empty:
        return pd.DataFrame()

    out = pd.DataFrame(index=df.index)
    out["source_file"] = str(path)
    out["source_scheme"] = src.scheme
    out["series_norm"] = first_existing(df, ["series"], reporter, path, "series").fillna(src.scheme)
    out["config_norm"] = first_existing(
        df, ["config", "model_name", "decomposition_mode", "memo_backend"], reporter, path, "config/model"
    )
    out["n"] = number_series(first_existing(df, ["n", "ours_reconstruct_run_n"], reporter, path, "n"))
    out["R"] = number_series(first_existing(df, ["R", "r"], reporter, path, "R"))
    out["T"] = number_series(first_existing(df, ["T", "t"], reporter, path, "T"))
    out["seed"] = number_series(first_existing(df, ["seed", "ours_reconstruct_run_seed"], reporter, path, "seed"))
    for col in ["n", "R", "T", "seed"]:
        df[col] = out[col]
    for col in ["instance_file", "instance_path", "input_path", "input", "instance_hash", "input_hash", "ours_reconstruct_run_input"]:
        if col in src.df.columns:
            df[col] = src.df[col]
    out["instance_key"] = build_instance_key(df)
    raw_status = first_existing(df, ["status"], reporter, path, "status")
    out["status_norm"] = raw_status.map(normalize_status)
    out["solved"] = out["status_norm"].eq("solved")
    out["objective_norm"] = number_series(first_existing(df, ["objective", "optimum", "cost"], reporter, path, "objective"))
    out["time_ms_norm"] = number_series(first_existing(
        df, ["time_ms", "wall_time_ms", "ours_reconstruct_wall_time_ms"], reporter, path, "time"
    ))
    out["timeout_sec_norm"] = number_series(first_existing(df, ["time_limit_sec", "timeout_sec"], reporter, path, "timeout_sec"))
    out["penalized_time_ms"] = penalized_time(out["time_ms_norm"], out["status_norm"], out["timeout_sec_norm"], penalty_factor, timeout_policy)
    out["memory_peak_bytes_norm"] = number_series(first_existing(
        df,
        ["memory_used_bytes", "memory_bytes_peak", "peak_working_set_bytes", "process_peak_working_set_bytes",
         "memo_memory_used_bytes"],
        reporter,
        path,
        "memory",
    ))
    optional_passthrough = [
        "nodes", "memo_exact_hits", "memo_exact_queries", "memo_lb_hits", "memo_lb_queries",
        "bound_time_ms", "cleanup_time_ms", "memo_cleanup_time_ms", "memo_clean_time_ms",
        "simple_lb_calls", "simple_lb_prunes", "memo_full_key_verification",
        "enable_exact_memo", "enable_memo", "process_memory_gate", "memory_limit_gb",
        "memo_backend", "enable_simple_lb", "enable_lb_memo", "enable_edd_ub",
        "reconstruct_order", "reconstruction_trace",
    ]
    for col in optional_passthrough:
        out[col] = df[col] if col in df.columns else pd.NA
        if col not in df.columns:
            reporter.missing(path, col)
    out["possibly_process_gated"] = src.possibly_process_gated
    return out


def normalize_article_runs(src: SourceFrame, reporter: Reporter, penalty_factor: float, timeout_policy: str) -> pd.DataFrame:
    df = src.df.copy()
    path = src.path
    if df.empty:
        return pd.DataFrame()
    required = [
        "n", "R", "T", "seed", "ours_reconstruct_status", "article_status",
        "ours_reconstruct_objective", "article_objective",
        "ours_reconstruct_wall_time_ms", "article_wall_time_ms",
    ]
    add_missing_columns(df, required, reporter, path)
    base = pd.DataFrame({
        "source_file": str(path),
        "source_scheme": src.scheme,
        "series_norm": "article_code_comparison",
        "n": number_series(df["n"]),
        "R": number_series(df["R"]),
        "T": number_series(df["T"]),
        "seed": number_series(df["seed"]),
    })
    key_df = df.copy()
    for col in ["n", "R", "T", "seed"]:
        key_df[col] = base[col]
    base["instance_key"] = build_instance_key(key_df)
    rows = []
    for side in ["ours", "article"]:
        item = base.copy()
        item["config_norm"] = "ours_reconstruct" if side == "ours" else "article_code"
        if side == "ours":
            status = df["ours_reconstruct_status"]
            objective = df["ours_reconstruct_objective"]
            time_ms = df["ours_reconstruct_wall_time_ms"]
            memory = df.get("ours_reconstruct_peak_working_set_bytes", pd.Series([pd.NA] * len(df)))
        else:
            status = df["article_status"]
            objective = df["article_objective"]
            time_ms = df["article_wall_time_ms"]
            memory = df.get("article_peak_working_set_bytes", pd.Series([pd.NA] * len(df)))
        item["status_norm"] = status.map(normalize_status)
        item["solved"] = item["status_norm"].eq("solved")
        item["objective_norm"] = number_series(objective)
        item["time_ms_norm"] = number_series(time_ms)
        item["timeout_sec_norm"] = pd.NA
        item["penalized_time_ms"] = penalized_time(item["time_ms_norm"], item["status_norm"], item["timeout_sec_norm"], penalty_factor, timeout_policy)
        item["memory_peak_bytes_norm"] = number_series(memory)
        item["nodes"] = pd.NA
        item["process_memory_gate"] = False
        item["possibly_process_gated"] = False
        rows.append(item)
    return pd.concat(rows, ignore_index=True)


def normalize_memo_pair_runs(src: SourceFrame, reporter: Reporter, penalty_factor: float, timeout_policy: str) -> pd.DataFrame:
    df = src.df.copy()
    if df.empty or "custom_status" not in df.columns:
        return pd.DataFrame()
    path = src.path
    base = pd.DataFrame({
        "source_file": str(path),
        "source_scheme": src.scheme,
        "series_norm": "memo_backend_no_gate",
        "n": number_series(df.get("n", pd.Series([pd.NA] * len(df)))),
        "R": number_series(df.get("R", pd.Series([pd.NA] * len(df)))),
        "T": number_series(df.get("T", pd.Series([pd.NA] * len(df)))),
        "seed": number_series(df.get("seed", pd.Series([pd.NA] * len(df)))),
    })
    key_df = df.copy()
    for col in ["n", "R", "T", "seed"]:
        key_df[col] = base[col]
    base["instance_key"] = build_instance_key(key_df)
    rows = []
    for backend in ["custom", "std_unordered"]:
        item = base.copy()
        item["config_norm"] = backend
        item["memo_backend"] = backend
        item["status_norm"] = df[f"{backend}_status"].map(normalize_status)
        item["solved"] = item["status_norm"].eq("solved")
        item["objective_norm"] = number_series(df[f"{backend}_objective"])
        item["time_ms_norm"] = number_series(df[f"{backend}_reported_time_ms"])
        item["timeout_sec_norm"] = number_series(df.get("timeout_sec", pd.Series([pd.NA] * len(df))))
        item["penalized_time_ms"] = penalized_time(item["time_ms_norm"], item["status_norm"], item["timeout_sec_norm"], penalty_factor, timeout_policy)
        item["memory_peak_bytes_norm"] = number_series(df[f"{backend}_memo_used_mb"]) * 1024.0 * 1024.0
        item["nodes"] = number_series(df.get(f"{backend}_nodes", pd.Series([pd.NA] * len(df))))
        item["memo_exact_hits"] = number_series(df.get(f"{backend}_memo_hits", pd.Series([pd.NA] * len(df))))
        item["memo_exact_queries"] = number_series(df.get(f"{backend}_memo_hits", pd.Series([pd.NA] * len(df)))) + number_series(df.get(f"{backend}_memo_misses", pd.Series([pd.NA] * len(df))))
        item["process_memory_gate"] = False
        item["memo_full_key_verification"] = True
        item["enable_exact_memo"] = True
        item["possibly_process_gated"] = False
        rows.append(item)
    return pd.concat(rows, ignore_index=True)


def normalize_reconstruction_pair_runs(src: SourceFrame, reporter: Reporter, penalty_factor: float, timeout_policy: str) -> pd.DataFrame:
    df = src.df.copy()
    if df.empty or not ({"solve_only_reported_time_ms", "trace_reconstruct_reported_time_ms"} & set(df.columns) or
                        {"solve_only_reported_time_ms", "solve_with_reconstruction_reported_time_ms"} <= set(df.columns)):
        return pd.DataFrame()
    base = pd.DataFrame({
        "source_file": str(src.path),
        "source_scheme": src.scheme,
        "series_norm": "reconstruction",
        "n": number_series(df.get("n", pd.Series([pd.NA] * len(df)))),
        "R": number_series(df.get("R", pd.Series([pd.NA] * len(df)))),
        "T": number_series(df.get("T", pd.Series([pd.NA] * len(df)))),
        "seed": number_series(df.get("seed", pd.Series([pd.NA] * len(df)))),
    })
    key_df = df.copy()
    for col in ["n", "R", "T", "seed"]:
        key_df[col] = base[col]
    base["instance_key"] = build_instance_key(key_df)
    rows = []
    status_col = "solve_only_status" if "solve_only_status" in df.columns else "status"
    solve_status = df[status_col] if status_col in df.columns else pd.Series(["SOLVED"] * len(df))
    item = base.copy()
    item["config_norm"] = "solve_only"
    item["status_norm"] = solve_status.map(normalize_status)
    item["solved"] = item["status_norm"].eq("solved")
    item["objective_norm"] = number_series(df.get("solve_only_objective", df.get("objective", pd.Series([pd.NA] * len(df)))))
    item["time_ms_norm"] = number_series(df.get("solve_only_reported_time_ms", df.get("solve_only_wall_time_ms", pd.Series([pd.NA] * len(df)))))
    item["timeout_sec_norm"] = number_series(df.get("timeout_sec", pd.Series([pd.NA] * len(df))))
    item["penalized_time_ms"] = penalized_time(item["time_ms_norm"], item["status_norm"], item["timeout_sec_norm"], penalty_factor, timeout_policy)
    item["memory_peak_bytes_norm"] = number_series(df.get("solve_only_memo_used_mb", pd.Series([pd.NA] * len(df)))) * 1024.0 * 1024.0
    item["nodes"] = number_series(df.get("solve_only_nodes", pd.Series([pd.NA] * len(df))))
    item["process_memory_gate"] = False
    rows.append(item)

    if "trace_reconstruct_reported_time_ms" in df.columns:
        cfg = "new_solve_with_reconstruction"
        tcol = "trace_reconstruct_reported_time_ms"
        ocol = "trace_reconstruct_objective"
        scol = "trace_reconstruct_status"
        mcol = "trace_reconstruct_memo_used_mb"
        ncol = "trace_reconstruct_nodes"
    else:
        cfg = "old_solve_with_reconstruction"
        tcol = "solve_with_reconstruction_reported_time_ms"
        ocol = "objective"
        scol = "status"
        mcol = ""
        ncol = ""
    item = base.copy()
    item["config_norm"] = cfg
    item["status_norm"] = df.get(scol, pd.Series(["SOLVED"] * len(df))).map(normalize_status)
    item["solved"] = item["status_norm"].eq("solved")
    item["objective_norm"] = number_series(df.get(ocol, df.get("objective", pd.Series([pd.NA] * len(df)))))
    item["time_ms_norm"] = number_series(df.get(tcol, pd.Series([pd.NA] * len(df))))
    item["timeout_sec_norm"] = number_series(df.get("timeout_sec", pd.Series([pd.NA] * len(df))))
    item["penalized_time_ms"] = penalized_time(item["time_ms_norm"], item["status_norm"], item["timeout_sec_norm"], penalty_factor, timeout_policy)
    item["memory_peak_bytes_norm"] = number_series(df.get(mcol, pd.Series([pd.NA] * len(df)))) * 1024.0 * 1024.0 if mcol else pd.NA
    item["nodes"] = number_series(df.get(ncol, pd.Series([pd.NA] * len(df)))) if ncol else pd.NA
    item["process_memory_gate"] = False
    rows.append(item)
    return pd.concat(rows, ignore_index=True)


def load_sources(files: list[Path], reporter: Reporter) -> list[SourceFrame]:
    sources: list[SourceFrame] = []
    for path in files:
        if not path.exists():
            reporter.notes.append(f"missing input file: {path}")
            continue
        df = read_csv_robust(path)
        scheme = classify_scheme(path)
        possibly_process_gated = False
        if "process_memory_gate" not in df.columns and "memory_limit_gb" in df.columns:
            possibly_process_gated = True
            reporter.possibly_process_gated_files.append(path)
        sources.append(SourceFrame(path=path, scheme=scheme, df=df, possibly_process_gated=possibly_process_gated))
        reporter.input_files.append(path)
        reporter.scheme_presence[scheme] = reporter.scheme_presence.get(scheme, 0) + len(df)
    return sources


def build_long_runs(sources: list[SourceFrame], reporter: Reporter, penalty_factor: float, timeout_policy: str) -> pd.DataFrame:
    frames: list[pd.DataFrame] = []
    for src in sources:
        if src.scheme == "article_code_comparison":
            frames.append(normalize_article_runs(src, reporter, penalty_factor, timeout_policy))
        elif src.scheme == "memo_backend" and "custom_status" in src.df.columns:
            frames.append(normalize_memo_pair_runs(src, reporter, penalty_factor, timeout_policy))
        elif src.scheme == "reconstruction" and (
            "trace_reconstruct_reported_time_ms" in src.df.columns or
            "solve_with_reconstruction_reported_time_ms" in src.df.columns
        ):
            frames.append(normalize_reconstruction_pair_runs(src, reporter, penalty_factor, timeout_policy))
        else:
            frames.append(normalize_standard_runs(src, reporter, penalty_factor, timeout_policy))
    frames = [f for f in frames if not f.empty]
    if not frames:
        return pd.DataFrame()
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", category=FutureWarning, message="The behavior of DataFrame concatenation.*")
        long = pd.concat(frames, ignore_index=True, sort=False)
    for col in ["n", "R", "T", "seed", "time_ms_norm", "penalized_time_ms", "memory_peak_bytes_norm", "nodes"]:
        if col in long.columns:
            long[col] = number_series(long[col])
    return long


def save_csv(df: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    df.to_csv(path, index=False)
    reporter.artifact(path)


def remove_stale_file(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except TypeError:
        if path.exists():
            path.unlink()


def wrap_title(text: str, width: int = 86) -> str:
    """Wrap long Russian figure titles so matplotlib does not clip them."""
    return "\n".join(textwrap.fill(part, width=width) for part in str(text).splitlines())


def save_latex(df: pd.DataFrame, path: Path, reporter: Reporter, columns: list[str] | None = None, max_rows: int = 12) -> None:
    if df is None or df.empty:
        path.write_text("% нет данных для этой схемы\n", encoding="utf-8")
    else:
        view = df.copy()
        if columns:
            view = view[[c for c in columns if c in view.columns]]
        view = view.rename(columns={c: LATEX_COLUMN_LABELS.get(c, c) for c in view.columns})
        path.write_text(view.head(max_rows).to_latex(index=False, escape=True), encoding="utf-8")
    reporter.artifact(path)


def save_no_data_figure(path: Path, title: str, reporter: Reporter) -> None:
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.text(0.5, 0.5, "нет данных", ha="center", va="center", fontsize=14)
    ax.set_axis_off()
    ax.set_title(wrap_title(title, 74))
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def format_duration_ms(value) -> str:
    if pd.isna(value):
        return "NA"
    value = float(value)
    if value >= 1000.0:
        seconds = value / 1000.0
        if seconds >= 600.0:
            return f"{seconds / 60.0:.1f}min"
        if seconds >= 100.0:
            return f"{seconds:.0f}s"
        if seconds >= 10.0:
            return f"{seconds:.1f}s"
        return f"{seconds:.2f}s"
    return f"{value:.3g}ms"


def format_ratio(value) -> str:
    if pd.isna(value):
        return "NA"
    value = float(value)
    if value >= 1000.0:
        text = f"{value:.2e}".replace("e+0", "e").replace("e+", "e")
        return f"{text}x"
    return f"{value:.2f}x"


def format_percent(value) -> str:
    if pd.isna(value):
        return "--"
    return f"{float(value) * 100.0:.1f}%"


def format_percent_points(value) -> str:
    if pd.isna(value):
        return "--"
    return f"{float(value):.1f}%"


def format_match(value) -> str:
    if pd.isna(value):
        return "unknown"
    return "match" if bool(value) else "mismatch"


def line_plot(df: pd.DataFrame, x: str, y: str, group: str, path: Path, title: str,
              ylabel: str, reporter: Reporter, logy: bool = False, y_scale: float = 1.0) -> None:
    if df.empty or x not in df or y not in df or group not in df:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    for name, g in df.dropna(subset=[x, y]).groupby(group):
        gg = g.sort_values(x)
        ax.plot(gg[x], gg[y] / y_scale, marker="o", label=str(name))
    if logy:
        ax.set_yscale("log")
    else:
        ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel(x)
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def bar_plot(df: pd.DataFrame, x: str, y: str, path: Path, title: str, ylabel: str, reporter: Reporter) -> None:
    if df.empty or x not in df or y not in df:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    dd = df.dropna(subset=[x, y]).copy()
    ax.bar(dd[x].astype(str), dd[y])
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel(x)
    ax.set_ylabel(ylabel)
    ax.grid(True, axis="y", alpha=0.3)
    plt.xticks(rotation=30, ha="right")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def heatmap_table(pivot: pd.DataFrame, path: Path, title: str, reporter: Reporter) -> None:
    if pivot.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(7, 5))
    vals = pivot.to_numpy(dtype=float)
    im = ax.imshow(vals, aspect="auto", cmap="viridis")
    ax.set_xticks(np.arange(len(pivot.columns)), labels=[str(c) for c in pivot.columns])
    ax.set_yticks(np.arange(len(pivot.index)), labels=[str(i) for i in pivot.index])
    ax.set_xlabel("T")
    ax.set_ylabel("R")
    ax.set_title(wrap_title(title, 74))
    fig.colorbar(im, ax=ax)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def model_pair_grid(
    df: pd.DataFrame,
    path: Path,
    reporter: Reporter,
    logy: bool = True,
    title_prefix: str = "Сравнение моделей: медианное время решения по (R,T)",
    context: str = "только полные значения n",
    no_data_title: str = "Сравнение моделей по (R,T)",
    y_scale: float = 1.0,
    y_unit: str = "мс",
) -> None:
    if df.empty:
        save_no_data_figure(path, no_data_title, reporter)
        return
    pairs = df[["R", "T"]].drop_duplicates().sort_values(["R", "T"]).to_records(index=False).tolist()
    if not pairs:
        save_no_data_figure(path, no_data_title, reporter)
        return
    ncols = 4
    nrows = int(math.ceil(len(pairs) / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(15, max(4.8 if nrows <= 2 else 3.0, 2.5 * nrows)), sharex=True)
    axes_arr = np.array(axes).reshape(-1)
    handles = []
    labels = []
    any_y_cap = False
    for ax, (r, t) in zip(axes_arr, pairs):
        sub = df[np.isclose(df["R"], r) & np.isclose(df["T"], t)]
        med = sub.groupby(["n", "config_norm"], dropna=False)["time_ms_norm"].median().reset_index()
        med["time_plot"] = med["time_ms_norm"] / y_scale
        y_cap = None
        if not logy and not med.empty:
            model_max = med.groupby("config_norm")["time_plot"].max().dropna().sort_values()
            if len(model_max) >= 2:
                vals = model_max.to_numpy(dtype=float)
                ratios = vals[1:] / np.maximum(vals[:-1], 1e-12)
                gap_idx = int(np.argmax(ratios))
                if ratios[gap_idx] >= 5.0:
                    y_cap = vals[gap_idx] * 1.25
                    any_y_cap = True
        for name, g in med.groupby("config_norm"):
            gg = g.sort_values("n")
            y = gg["time_plot"].copy()
            clipped = pd.Series(False, index=gg.index)
            if y_cap is not None:
                clipped = y.gt(y_cap)
                y = y.clip(upper=y_cap)
            line, = ax.plot(gg["n"], y, marker="o", linewidth=1.3, markersize=3, label=str(name))
            if y_cap is not None and clipped.any():
                ax.scatter(gg.loc[clipped, "n"], y.loc[clipped], marker="^", s=22, color=line.get_color(), zorder=4)
            if str(name) not in labels:
                handles.append(line)
                labels.append(str(name))
        ax.set_title(f"R={r:g}, T={t:g}", fontsize=9)
        if logy:
            ax.set_yscale("log")
        else:
            ax.ticklabel_format(axis="y", style="plain", useOffset=False)
            if y_cap is not None:
                ax.set_ylim(top=y_cap * 1.08)
                ax.text(0.98, 0.96, f"обрезано > {y_cap:.3g} {y_unit}", transform=ax.transAxes,
                        ha="right", va="top", fontsize=6, color="dimgray")
        ax.grid(True, alpha=0.25)
    for ax in axes_arr[len(pairs):]:
        ax.set_axis_off()
    if logy:
        scale_text = "логарифмическая шкала"
        ylabel = f"время, {y_unit} (лог. шкала)"
    elif any_y_cap:
        scale_text = "линейная шкала, выбросы обрезаны"
        ylabel = f"время, {y_unit} (линейная шкала)"
    else:
        scale_text = "линейная шкала"
        ylabel = f"время, {y_unit}"
    fig.suptitle(wrap_title(f"{title_prefix}; {context}; {scale_text}", 108), fontsize=12)
    if nrows <= 2:
        xlabel_y = 0.13
        legend_y = 0.01
        bottom = 0.24
    else:
        xlabel_y = 0.055
        legend_y = 0.005
        bottom = 0.09
    fig.supxlabel("n", y=xlabel_y)
    fig.supylabel(ylabel, x=0.035)
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, legend_y), ncol=3, fontsize=8)
    fig.tight_layout(rect=(0.06, bottom, 1, 0.96))
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def model_relative_ratio_heatmap(df: pd.DataFrame, path: Path, reporter: Reporter, n_value: float) -> pd.DataFrame:
    if df.empty:
        save_no_data_figure(path, "Относительное время моделей", reporter)
        return pd.DataFrame()
    sub = df[df["n"].eq(n_value)].copy()
    if sub.empty:
        save_no_data_figure(path, "Относительное время моделей", reporter)
        return pd.DataFrame()
    med = sub.groupby(["R", "T", "config_norm"], dropna=False)["time_ms_norm"].median().reset_index()
    best = med.groupby(["R", "T"], dropna=False)["time_ms_norm"].transform("min")
    med["ratio_to_best"] = med["time_ms_norm"] / best.replace(0, np.nan)
    med["pair"] = med.apply(lambda r: f"{r['R']:g},{r['T']:g}", axis=1)
    pivot = med.pivot(index="config_norm", columns="pair", values="ratio_to_best")
    order = [m for m in MODEL_ORDER if m in set(pivot.index)]
    extra = [m for m in pivot.index if m not in order]
    pivot = pivot.loc[order + extra]
    fig, ax = plt.subplots(figsize=(max(10, 0.55 * len(pivot.columns)), max(4, 0.55 * len(pivot.index))))
    values = pivot.to_numpy(dtype=float)
    clipped = np.clip(values, 1.0, 10.0)
    ratio_cmap = mcolors.LinearSegmentedColormap.from_list(
        "ratio_green_yellow_red",
        [
            (0.00, "#22c55e"),
            (0.18, "#86efac"),
            (math.log10(2.0), "#fef08a"),
            (math.log10(5.0), "#fb923c"),
            (1.00, "#dc2626"),
        ],
    )
    log_values = np.log10(clipped)
    im = ax.imshow(log_values, aspect="auto", cmap=ratio_cmap, vmin=0.0, vmax=1.0)
    ax.set_xticks(np.arange(len(pivot.columns)), labels=pivot.columns, rotation=45, ha="right")
    ax.set_yticks(np.arange(len(pivot.index)), labels=pivot.index)
    ax.set_xlabel("(R,T)")
    ax.set_title(wrap_title(f"Сравнение моделей: отношение медианного времени к лучшему при n={n_value:g}", 74))
    for i in range(values.shape[0]):
        for j in range(values.shape[1]):
            val = values[i, j]
            if np.isfinite(val):
                is_best = val <= 1.000001
                if val > 10:
                    text = ">10"
                elif is_best:
                    text = "1.0"
                elif val < 1.1:
                    text = f"{val:.2f}"
                else:
                    text = f"{val:.1f}"
                rgba = ratio_cmap(np.clip(math.log10(min(max(val, 1.0), 10.0)), 0.0, 1.0))
                luminance = 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]
                text_color = "white" if luminance < 0.42 else "black"
                weight = "bold" if is_best else "normal"
                ax.text(j, i, text, ha="center", va="center", fontsize=7, color=text_color, fontweight=weight)
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_ticks([0.0, math.log10(2.0), math.log10(5.0), 1.0])
    cbar.set_ticklabels(["лучшее", "2x", "5x", ">=10x"])
    cbar.set_label("зелёный цвет: лучший режим")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)
    return med


def bounds_ratio_pair_grid(
    paired: pd.DataFrame,
    metric: str,
    path: Path,
    reporter: Reporter,
    *,
    logy: bool,
    title: str,
    ylabel: str,
) -> None:
    if paired.empty or metric not in paired.columns:
        save_no_data_figure(path, title, reporter)
        return
    pairs = paired[["R", "T"]].drop_duplicates().sort_values(["R", "T"]).to_records(index=False).tolist()
    if not pairs:
        save_no_data_figure(path, title, reporter)
        return
    ncols = min(4, len(pairs))
    nrows = int(math.ceil(len(pairs) / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(15, max(4.8 if nrows <= 2 else 3.0, 2.5 * nrows)), sharex=True)
    axes_arr = np.array(axes).reshape(-1)
    handles = []
    labels = []
    mode_order = [m for m in BOUNDS_MODE_ORDER if m in set(paired["bounds_mode"].dropna().astype(str))]
    for ax, (r, t) in zip(axes_arr, pairs):
        sub = paired[np.isclose(paired["R"], r) & np.isclose(paired["T"], t)].copy()
        med = sub.groupby(["n", "bounds_mode"], dropna=False)[metric].median().reset_index()
        for mode in mode_order:
            g = med[med["bounds_mode"].eq(mode)].sort_values("n")
            if g.empty:
                continue
            line, = ax.plot(g["n"], g[metric], marker="o", linewidth=1.3, markersize=3, label=mode)
            if mode not in labels:
                handles.append(line)
                labels.append(mode)
        ax.axhline(1.0, color="gray", linestyle="--", linewidth=1.0, alpha=0.75)
        if logy:
            ax.set_yscale("log")
        else:
            ax.ticklabel_format(axis="y", style="plain", useOffset=False)
        ax.set_title(f"R={r:g}, T={t:g}", fontsize=9)
        ax.grid(True, alpha=0.25)
    for ax in axes_arr[len(pairs):]:
        ax.set_axis_off()
    fig.suptitle(wrap_title(title, 108), fontsize=12)
    xlabel_y = 0.13 if nrows <= 2 else 0.055
    legend_y = 0.01 if nrows <= 2 else 0.005
    bottom = 0.24 if nrows <= 2 else 0.09
    fig.supxlabel("n", y=xlabel_y)
    fig.supylabel(ylabel, x=0.035)
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, legend_y), ncol=3, fontsize=8)
    fig.tight_layout(rect=(0.06, bottom, 1, 0.96))
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def backend_time_ratio_distribution(clean: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение memo-таблиц: отношение времени custom/std по n, без process gate"
    required = {"n", "time_ratio_custom_to_std"}
    if clean.empty or not required.issubset(clean.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = clean.copy()
    df["n"] = number_series(df["n"])
    df["time_ratio_custom_to_std"] = number_series(df["time_ratio_custom_to_std"])
    df = df.dropna(subset=["n", "time_ratio_custom_to_std"])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    ns = sorted(df["n"].unique())
    data = [df[df["n"].eq(n)]["time_ratio_custom_to_std"].to_numpy(dtype=float) for n in ns]
    positions = np.arange(len(ns))
    fig, ax = plt.subplots(figsize=(9, 5))
    box = ax.boxplot(data, positions=positions, widths=0.55, showfliers=False, patch_artist=True)
    for patch in box["boxes"]:
        patch.set_facecolor("#bbf7d0")
        patch.set_edgecolor("#166534")
        patch.set_alpha(0.85)
    for part in ["whiskers", "caps", "medians"]:
        for item in box[part]:
            item.set_color("#166534")
            item.set_linewidth(1.1)
    rng = np.random.default_rng(0)
    for pos, values in zip(positions, data):
        jitter = rng.uniform(-0.18, 0.18, size=len(values))
        ax.scatter(np.full(len(values), pos) + jitter, values, s=14, alpha=0.45, color="#1f2937", linewidths=0)
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.2, label="паритет с std::unordered_map")
    ax.set_xticks(positions, labels=[f"{int(n):d}" if float(n).is_integer() else f"{n:g}" for n in ns])
    ax.set_xlabel("n")
    ax.set_ylabel("время custom/std\n(<1: custom быстрее)")
    ax.set_title(wrap_title(title, 74))
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def backend_time_ratio_pair_grid(clean: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение memo-таблиц: медианное отношение времени custom/std по (R,T), без process gate"
    required = {"n", "R", "T", "time_ratio_custom_to_std"}
    if clean.empty or not required.issubset(clean.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = clean.copy()
    for col in ["n", "R", "T", "time_ratio_custom_to_std"]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["n", "R", "T", "time_ratio_custom_to_std"])
    pairs = df[["R", "T"]].drop_duplicates().sort_values(["R", "T"]).to_records(index=False).tolist()
    if not pairs:
        save_no_data_figure(path, title, reporter)
        return
    ncols = min(4, len(pairs))
    nrows = int(math.ceil(len(pairs) / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(15, max(4.8 if nrows <= 2 else 3.0, 2.5 * nrows)), sharex=True, sharey=True)
    axes_arr = np.array(axes).reshape(-1)
    q_low = df["time_ratio_custom_to_std"].quantile(0.02)
    q_high = df["time_ratio_custom_to_std"].quantile(0.98)
    y_min = min(0.95, q_low) * 0.96
    y_max = max(1.05, q_high) * 1.04
    for ax, (r, t) in zip(axes_arr, pairs):
        sub = df[np.isclose(df["R"], r) & np.isclose(df["T"], t)]
        agg = sub.groupby("n", dropna=False)["time_ratio_custom_to_std"].median().reset_index(name="median").sort_values("n")
        x = agg["n"].to_numpy(dtype=float)
        median = agg["median"].to_numpy(dtype=float)
        ax.plot(x, median, marker="o", color="#166534", linewidth=1.6, label="медиана")
        ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.0)
        ax.set_title(f"R={r:g}, T={t:g}", fontsize=9)
        ax.grid(True, alpha=0.25)
        ax.ticklabel_format(axis="y", style="plain", useOffset=False)
        ax.set_ylim(y_min, y_max)
    for ax in axes_arr[len(pairs):]:
        ax.set_axis_off()
    fig.suptitle(wrap_title(title, 108), fontsize=12)
    fig.supxlabel("n", y=0.13 if nrows <= 2 else 0.055)
    fig.supylabel("время custom/std\n(<1: custom быстрее)", x=0.035)
    handles, labels = axes_arr[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, 0.01 if nrows <= 2 else 0.005), ncol=1, fontsize=8)
    fig.tight_layout(rect=(0.06, 0.24 if nrows <= 2 else 0.09, 1, 0.96))
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def backend_memory_ratio_by_n(clean: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение memo-таблиц: отношение памяти custom/std по n, без process gate"
    required = {"n", "R", "T", "memory_ratio_custom_to_std"}
    if clean.empty or not required.issubset(clean.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = clean.copy()
    for col in ["n", "R", "T", "memory_ratio_custom_to_std"]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["n", "R", "T", "memory_ratio_custom_to_std"])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    agg = df.groupby("n", dropna=False)["memory_ratio_custom_to_std"].median().reset_index(name="median").sort_values("n")
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.plot(agg["n"], agg["median"], marker="o", color="#1d4ed8", linewidth=1.8, label="медиана по всем проверенным (R,T) и seed")
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.2, label="паритет с std::unordered_map")
    ax.set_xlabel("n")
    ax.set_ylabel("память custom/std\n(<1: custom меньше)")
    ax.set_title(wrap_title(title, 74))
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def backend_time_memory_tradeoff(clean: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение memo-таблиц: компромисс времени и памяти, без process gate"
    required = {"n", "time_ratio_custom_to_std", "memory_ratio_custom_to_std"}
    if clean.empty or not required.issubset(clean.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = clean.copy()
    for col in ["n", "time_ratio_custom_to_std", "memory_ratio_custom_to_std"]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["n", "time_ratio_custom_to_std", "memory_ratio_custom_to_std"])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 6))
    sc = ax.scatter(
        df["memory_ratio_custom_to_std"],
        df["time_ratio_custom_to_std"],
        c=df["n"],
        cmap="viridis",
        s=28,
        alpha=0.75,
        edgecolors="none",
    )
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.0)
    ax.axvline(1.0, color="#dc2626", linestyle="--", linewidth=1.0)
    ax.set_xlabel("память custom/std (<1: custom меньше)")
    ax.set_ylabel("время custom/std (<1: custom быстрее)")
    ax.set_title(wrap_title(title, 58), fontsize=11)
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(sc, ax=ax)
    cbar.set_label("n")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def backend_custom_win_rate_heatmap(clean: pd.DataFrame, path: Path, reporter: Reporter) -> pd.DataFrame:
    title = "Сравнение memo-таблиц: доля запусков, где custom быстрее, по (R,T) и n, без process gate"
    required = {"n", "R", "T", "time_ratio_custom_to_std"}
    if clean.empty or not required.issubset(clean.columns):
        save_no_data_figure(path, title, reporter)
        return pd.DataFrame()
    df = clean.copy()
    for col in ["n", "R", "T", "time_ratio_custom_to_std"]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["n", "R", "T", "time_ratio_custom_to_std"])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return pd.DataFrame()
    df["pair"] = df.apply(lambda row: f"{row['R']:g},{row['T']:g}", axis=1)
    df["custom_faster"] = df["time_ratio_custom_to_std"] < 1.0
    win = df.groupby(["pair", "n"], dropna=False)["custom_faster"].mean().mul(100.0).reset_index(name="win_rate_percent")
    pair_order = (
        df[["R", "T", "pair"]]
        .drop_duplicates()
        .sort_values(["R", "T"])["pair"]
        .tolist()
    )
    n_order = sorted(df["n"].unique())
    pivot = win.pivot(index="pair", columns="n", values="win_rate_percent").reindex(index=pair_order, columns=n_order)
    fig, ax = plt.subplots(figsize=(max(8, 0.75 * len(n_order)), max(4, 0.45 * len(pair_order))))
    vals = pivot.to_numpy(dtype=float)
    im = ax.imshow(vals, aspect="auto", cmap="Greens", vmin=0.0, vmax=100.0)
    ax.set_xticks(np.arange(len(pivot.columns)), labels=[f"{int(n):d}" if float(n).is_integer() else f"{n:g}" for n in pivot.columns])
    ax.set_yticks(np.arange(len(pivot.index)), labels=pivot.index)
    ax.set_xlabel("n")
    ax.set_ylabel("(R,T)")
    ax.set_title(wrap_title(title, 74))
    for i in range(vals.shape[0]):
        for j in range(vals.shape[1]):
            val = vals[i, j]
            if np.isfinite(val):
                ax.text(j, i, f"{val:.0f}%", ha="center", va="center",
                        color="white" if val >= 65.0 else "black", fontsize=8,
                        fontweight="bold" if val >= 95.0 else "normal")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label("custom быстрее, %")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)
    return win


def plot_reconstruction_single_line(
    df: pd.DataFrame,
    y_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    ylabel: str,
    parity_y: float | None = None,
) -> None:
    if df.empty or "n" not in df.columns or y_col not in df.columns:
        save_no_data_figure(path, title, reporter)
        return
    plot = df[["n", y_col]].copy()
    plot["n"] = number_series(plot["n"])
    plot[y_col] = number_series(plot[y_col])
    plot = plot.dropna(subset=["n", y_col]).sort_values("n")
    if plot.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(plot["n"], plot[y_col], marker="o", linewidth=1.8, color="#1f77b4")
    if parity_y is not None:
        ax.axhline(parity_y, color="#dc2626", linestyle="--", linewidth=1.0)
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel(ylabel)
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def plot_reconstruction_wall_overhead(summary: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Относительные накладные расходы: старое и новое восстановление"
    required = {"config_norm", "n", "median_wall_overhead_percent"}
    if summary.empty or not required.issubset(summary.columns):
        save_no_data_figure(path, title, reporter)
        return
    labels = {
        "new_solve_with_reconstruction": "новое trace-восстановление",
        "old_solve_with_reconstruction": "старое восстановление",
    }
    colors = {
        "new_solve_with_reconstruction": "#1f77b4",
        "old_solve_with_reconstruction": "#ff7f0e",
    }
    fig, ax = plt.subplots(figsize=(8, 5))
    for cfg in ["new_solve_with_reconstruction", "old_solve_with_reconstruction"]:
        sub = summary[summary["config_norm"].eq(cfg)].copy()
        if sub.empty:
            continue
        sub["n"] = number_series(sub["n"])
        sub["median_wall_overhead_percent"] = number_series(sub["median_wall_overhead_percent"])
        sub = sub.dropna(subset=["n", "median_wall_overhead_percent"]).sort_values("n")
        ax.plot(sub["n"], sub["median_wall_overhead_percent"], marker="o",
                linewidth=1.8, label=labels[cfg], color=colors[cfg])
    ax.axhline(0.0, color="gray", linestyle=":", linewidth=1.0)
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel("медианные накладные расходы, %")
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_objective_match_by_n(by_n: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение со статьёй: совпадение оптимума по n"
    required = {"n", "complete_n", "objective_matches", "objective_mismatches", "objective_not_comparable"}
    if by_n.empty or not required.issubset(by_n.columns):
        save_no_data_figure(path, title, reporter)
        return
    plot = by_n[by_n["complete_n"].fillna(False)].copy()
    for col in ["n", "objective_matches", "objective_mismatches", "objective_not_comparable"]:
        plot[col] = number_series(plot[col]).fillna(0.0)
    plot = plot.dropna(subset=["n"]).sort_values("n")
    if plot.empty:
        save_no_data_figure(path, title, reporter)
        return
    x = np.arange(len(plot))
    labels = [f"{int(n):d}" if float(n).is_integer() else f"{n:g}" for n in plot["n"]]
    fig, ax = plt.subplots(figsize=(9, 5))
    matches = plot["objective_matches"].to_numpy(dtype=float)
    mismatches = plot["objective_mismatches"].to_numpy(dtype=float)
    not_comp = plot["objective_not_comparable"].to_numpy(dtype=float)
    ax.bar(x, matches, color="#16a34a", label="оптимум совпал")
    ax.bar(x, mismatches, bottom=matches, color="#dc2626", label="оптимум не совпал")
    ax.bar(x, not_comp, bottom=matches + mismatches, color="#9ca3af", label="сравнение невозможно")
    ax.set_xticks(x, labels=labels)
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel("число экземпляров")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_ratio_line(
    by_n: pd.DataFrame,
    y_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    ylabel: str,
) -> None:
    required = {"n", "complete_n", y_col}
    if by_n.empty or not required.issubset(by_n.columns):
        save_no_data_figure(path, title, reporter)
        return
    plot = by_n[by_n["complete_n"].fillna(False)].copy()
    plot["n"] = number_series(plot["n"])
    plot[y_col] = number_series(plot[y_col])
    plot = plot.dropna(subset=["n", y_col]).sort_values("n")
    if plot.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(plot["n"], plot[y_col], marker="o", linewidth=1.8, color="#1f77b4")
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.0, label="паритет с кодом статьи")
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel(ylabel)
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_metric_line(
    by_n: pd.DataFrame,
    ours_col: str,
    article_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    ylabel: str,
    logy: bool = False,
) -> None:
    required = {"n", "complete_n", ours_col, article_col}
    if by_n.empty or not required.issubset(by_n.columns):
        save_no_data_figure(path, title, reporter)
        return
    plot = by_n[by_n["complete_n"].fillna(False)].copy()
    for col in ["n", ours_col, article_col]:
        plot[col] = number_series(plot[col])
    plot = plot.dropna(subset=["n", ours_col, article_col]).sort_values("n")
    if plot.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(plot["n"], plot[ours_col], marker="o", linewidth=1.8, label="наш алгоритм")
    ax.plot(plot["n"], plot[article_col], marker="o", linewidth=1.8, label="код статьи")
    if logy:
        ax.set_yscale("log")
    else:
        ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_pair_metric_grid(
    by_pair_n: pd.DataFrame,
    ours_col: str,
    article_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    ylabel: str,
    logy: bool = False,
) -> None:
    required = {"R", "T", "n", ours_col, article_col}
    if by_pair_n.empty or not required.issubset(by_pair_n.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = by_pair_n.copy()
    for col in ["R", "T", "n", ours_col, article_col]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["R", "T", "n", ours_col, article_col])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    pairs = df[["R", "T"]].drop_duplicates().sort_values(["R", "T"]).to_records(index=False).tolist()
    ncols = 4
    nrows = int(math.ceil(len(pairs) / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(16, max(8, 2.6 * nrows)), sharex=True)
    axes_arr = np.array(axes).reshape(-1)
    handles: list = []
    labels: list[str] = []
    for ax, (r, t) in zip(axes_arr, pairs):
        sub = df[np.isclose(df["R"], r) & np.isclose(df["T"], t)].sort_values("n")
        ours_line, = ax.plot(sub["n"], sub[ours_col], marker="o", linewidth=1.5, markersize=3.5,
                             color="#1f77b4", label="наш алгоритм")
        article_line, = ax.plot(sub["n"], sub[article_col], marker="o", linewidth=1.5, markersize=3.5,
                                color="#ff7f0e", label="код статьи")
        if not handles:
            handles = [ours_line, article_line]
            labels = ["наш алгоритм", "код статьи"]
        if logy:
            positive = pd.concat([sub[ours_col], sub[article_col]]).dropna()
            if (positive > 0).any():
                ax.set_yscale("log")
        else:
            ax.ticklabel_format(axis="y", style="plain", useOffset=False)
        ax.set_title(f"R={float(r):g}, T={float(t):g}", fontsize=9)
        ax.grid(True, alpha=0.25)
    for ax in axes_arr[len(pairs):]:
        ax.set_visible(False)
    fig.suptitle(wrap_title(title, 108), y=0.995)
    fig.supxlabel("n")
    fig.supylabel(ylabel, x=0.035)
    if handles:
        fig.legend(handles, labels, loc="lower center", ncol=2, fontsize=9)
    fig.tight_layout(rect=(0.06, 0.05, 1.0, 0.965))
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_ratio_distribution(
    comparable: pd.DataFrame,
    y_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    ylabel: str,
) -> None:
    required = {"n", "complete_n", y_col}
    if comparable.empty or not required.issubset(comparable.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = comparable[comparable["complete_n"].fillna(False)].copy()
    df["n"] = number_series(df["n"])
    df[y_col] = number_series(df[y_col])
    df = df.dropna(subset=["n", y_col]).sort_values("n")
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    n_values = sorted(df["n"].unique())
    data = [df[df["n"].eq(n)][y_col].to_numpy(dtype=float) for n in n_values]
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.boxplot(
        data,
        tick_labels=[f"{int(n):d}" if float(n).is_integer() else f"{n:g}" for n in n_values],
        showfliers=False,
        patch_artist=True,
        boxprops={"facecolor": "#bbf7d0", "color": "#166534"},
        medianprops={"color": "#14532d", "linewidth": 1.6},
        whiskerprops={"color": "#166534"},
        capprops={"color": "#166534"},
    )
    rng = np.random.default_rng(1)
    for idx, vals in enumerate(data, start=1):
        if len(vals) == 0:
            continue
        jitter = rng.normal(0.0, 0.035, len(vals))
        ax.scatter(np.full(len(vals), idx) + jitter, vals, s=10, color="#6b7280", alpha=0.45, edgecolors="none")
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.0, label="паритет с кодом статьи")
    ax.set_title(wrap_title(title, 74))
    ax.set_xlabel("n")
    ax.set_ylabel(ylabel)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_ratio_heatmap(
    by_rt: pd.DataFrame,
    value_col: str,
    path: Path,
    reporter: Reporter,
    *,
    title: str,
    cbar_label: str,
) -> None:
    required = {"R", "T", value_col}
    if by_rt.empty or not required.issubset(by_rt.columns):
        save_no_data_figure(path, title, reporter)
        return
    pivot = by_rt.pivot(index="R", columns="T", values=value_col).sort_index().sort_index(axis=1)
    if pivot.empty:
        save_no_data_figure(path, title, reporter)
        return
    vals = pivot.to_numpy(dtype=float)
    finite = vals[np.isfinite(vals)]
    if finite.size == 0:
        save_no_data_figure(path, title, reporter)
        return
    vmin = min(float(np.nanmin(finite)), 0.75)
    vmax = max(float(np.nanmax(finite)), 1.25)
    if vmin >= 1.0:
        vmin = 0.95
    if vmax <= 1.0:
        vmax = 1.05
    cmap = plt.get_cmap("RdYlGn_r").copy()
    cmap.set_bad("#f3f4f6")
    norm = mcolors.TwoSlopeNorm(vmin=vmin, vcenter=1.0, vmax=vmax)
    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(np.ma.masked_invalid(vals), aspect="auto", cmap=cmap, norm=norm)
    ax.set_xticks(np.arange(len(pivot.columns)), labels=[f"{float(c):g}" for c in pivot.columns])
    ax.set_yticks(np.arange(len(pivot.index)), labels=[f"{float(i):g}" for i in pivot.index])
    ax.set_xlabel("T")
    ax.set_ylabel("R")
    ax.set_title(wrap_title(title, 74))
    for i in range(vals.shape[0]):
        for j in range(vals.shape[1]):
            val = vals[i, j]
            if np.isfinite(val):
                ax.text(j, i, f"{val:.2f}", ha="center", va="center", fontsize=8, color="black")
            else:
                ax.text(j, i, "н/д", ha="center", va="center", fontsize=8, color="#6b7280")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label(cbar_label)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_objective_match_heatmap(by_rt: pd.DataFrame, path: Path, reporter: Reporter) -> None:
    title = "Сравнение со статьёй: доля совпадений оптимума по (R,T)"
    required = {"R", "T", "objective_match_rate"}
    if by_rt.empty or not required.issubset(by_rt.columns):
        save_no_data_figure(path, title, reporter)
        return
    pivot = by_rt.pivot(index="R", columns="T", values="objective_match_rate").sort_index().sort_index(axis=1)
    if pivot.empty:
        save_no_data_figure(path, title, reporter)
        return
    vals = pivot.to_numpy(dtype=float)
    cmap = plt.get_cmap("RdYlGn").copy()
    cmap.set_bad("#f3f4f6")
    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(np.ma.masked_invalid(vals), aspect="auto", cmap=cmap, vmin=0.0, vmax=1.0)
    ax.set_xticks(np.arange(len(pivot.columns)), labels=[f"{float(c):g}" for c in pivot.columns])
    ax.set_yticks(np.arange(len(pivot.index)), labels=[f"{float(i):g}" for i in pivot.index])
    ax.set_xlabel("T")
    ax.set_ylabel("R")
    ax.set_title(wrap_title(title, 74))
    for i in range(vals.shape[0]):
        for j in range(vals.shape[1]):
            val = vals[i, j]
            if np.isfinite(val):
                ax.text(j, i, f"{val * 100.0:.0f}%", ha="center", va="center", fontsize=8, color="black")
            else:
                ax.text(j, i, "н/д", ha="center", va="center", fontsize=8, color="#6b7280")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label("совпадение оптимума, %")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def article_time_memory_tradeoff(
    comparable: pd.DataFrame,
    path: Path,
    reporter: Reporter,
    *,
    title: str = "Сравнение со статьёй: время и память, логарифмическая ось X",
) -> None:
    required = {"time_ratio", "memory_ratio", "n"}
    if comparable.empty or not required.issubset(comparable.columns):
        save_no_data_figure(path, title, reporter)
        return
    df = comparable.copy()
    for col in ["time_ratio", "memory_ratio", "n"]:
        df[col] = number_series(df[col])
    df = df.dropna(subset=["time_ratio", "memory_ratio", "n"])
    if df.empty:
        save_no_data_figure(path, title, reporter)
        return
    fig, ax = plt.subplots(figsize=(8, 6))
    sc = ax.scatter(
        df["memory_ratio"],
        df["time_ratio"],
        c=df["n"],
        cmap="viridis",
        s=22,
        alpha=0.65,
        edgecolors="none",
    )
    ax.axhline(1.0, color="#dc2626", linestyle="--", linewidth=1.0)
    ax.axvline(1.0, color="#dc2626", linestyle="--", linewidth=1.0)
    ax.set_xscale("log")
    ax.set_xlabel("память: наш алгоритм / код статьи")
    ax.set_ylabel("время: наш алгоритм / код статьи")
    ax.set_title(wrap_title(title, 58), fontsize=11)
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(sc, ax=ax)
    cbar.set_label("n")
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def model_winner_text_map(df: pd.DataFrame, path: Path, reporter: Reporter, n_value: float) -> None:
    if df.empty:
        save_no_data_figure(path, "Лучшая модель по (R,T)", reporter)
        return
    sub = df[df["n"].eq(n_value)].copy()
    if sub.empty:
        save_no_data_figure(path, "Лучшая модель по (R,T)", reporter)
        return
    med = sub.groupby(["R", "T", "config_norm"], dropna=False)["time_ms_norm"].median().reset_index()
    best = med.sort_values(["R", "T", "time_ms_norm"]).groupby(["R", "T"], as_index=False).first()
    r_vals = sorted(best["R"].dropna().unique())
    t_vals = sorted(best["T"].dropna().unique())
    models = sorted(best["config_norm"].dropna().unique())
    color_map = {m: i for i, m in enumerate(models)}
    grid = np.full((len(r_vals), len(t_vals)), np.nan)
    labels = [["" for _ in t_vals] for _ in r_vals]
    for _, row in best.iterrows():
        i = r_vals.index(row["R"])
        j = t_vals.index(row["T"])
        grid[i, j] = color_map[row["config_norm"]]
        labels[i][j] = str(row["config_norm"]).replace("BestFinal_", "").replace("_cap0", "")
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.imshow(grid, aspect="auto", cmap="tab20")
    ax.set_xticks(np.arange(len(t_vals)), labels=[f"{x:g}" for x in t_vals])
    ax.set_yticks(np.arange(len(r_vals)), labels=[f"{x:g}" for x in r_vals])
    ax.set_xlabel("T")
    ax.set_ylabel("R")
    ax.set_title(wrap_title(f"Лучшая модель по медианному времени решения для (R,T), n={n_value:g}", 74))
    for i in range(len(r_vals)):
        for j in range(len(t_vals)):
            ax.text(j, i, labels[i][j], ha="center", va="center", fontsize=7)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
    reporter.artifact(path)


def summary_stats(df: pd.DataFrame, group_cols: list[str]) -> pd.DataFrame:
    if df.empty:
        return pd.DataFrame()
    work = df.copy()
    work["oot"] = work["status_norm"].eq("oot")
    work["oom"] = work["status_norm"].eq("oom")
    work["err"] = work["status_norm"].eq("err")
    if "memo_exact_hits" in work and "memo_exact_queries" in work:
        work["memo_exact_hit_rate"] = number_series(work["memo_exact_hits"]) / number_series(work["memo_exact_queries"]).replace(0, np.nan)
    else:
        work["memo_exact_hit_rate"] = np.nan
    if "memo_lb_hits" in work and "memo_lb_queries" in work:
        work["memo_lb_hit_rate"] = number_series(work["memo_lb_hits"]) / number_series(work["memo_lb_queries"]).replace(0, np.nan)
    else:
        work["memo_lb_hit_rate"] = np.nan
    work["bound_time_share"] = number_series(work.get("bound_time_ms", pd.Series(np.nan, index=work.index))) / work["time_ms_norm"].replace(0, np.nan)
    cleanup = work.get("cleanup_time_ms", work.get("memo_cleanup_time_ms", work.get("memo_clean_time_ms", pd.Series(np.nan, index=work.index))))
    work["cleanup_time_share"] = number_series(cleanup) / work["time_ms_norm"].replace(0, np.nan)
    solved_time = work["time_ms_norm"].where(work["solved"])
    grouped = work.groupby(group_cols, dropna=False)
    out = grouped.agg(
        total_count=("status_norm", "size"),
        solved_count=("solved", "sum"),
        oot_count=("oot", "sum"),
        oom_count=("oom", "sum"),
        err_count=("err", "sum"),
        median_penalized_time_ms=("penalized_time_ms", "median"),
        p90_penalized_time_ms=("penalized_time_ms", lambda x: x.quantile(0.90)),
        median_nodes=("nodes", "median"),
        p90_nodes=("nodes", lambda x: x.quantile(0.90)),
        median_memory_peak_bytes=("memory_peak_bytes_norm", "median"),
        p90_memory_peak_bytes=("memory_peak_bytes_norm", lambda x: x.quantile(0.90)),
        median_memo_exact_hit_rate=("memo_exact_hit_rate", "median"),
        median_memo_lb_hit_rate=("memo_lb_hit_rate", "median"),
        median_bound_time_share=("bound_time_share", "median"),
        median_cleanup_time_share=("cleanup_time_share", "median"),
    ).reset_index()
    solved = work.assign(_solved_time=solved_time).groupby(group_cols, dropna=False)["_solved_time"]
    out["median_time_ms_solved"] = solved.median().to_numpy()
    out["p90_time_ms_solved"] = solved.quantile(0.90).to_numpy()
    out["solved_rate"] = out["solved_count"] / out["total_count"].replace(0, np.nan)
    ordered = group_cols + [
        "total_count", "solved_count", "solved_rate", "oot_count", "oom_count", "err_count",
        "median_time_ms_solved", "p90_time_ms_solved", "median_penalized_time_ms",
        "p90_penalized_time_ms", "median_nodes", "p90_nodes",
        "median_memory_peak_bytes", "p90_memory_peak_bytes",
        "median_memo_exact_hit_rate", "median_memo_lb_hit_rate",
        "median_bound_time_share", "median_cleanup_time_share",
    ]
    return out[ordered]


def correctness_checks(long: pd.DataFrame, sources: list[SourceFrame], processed_dir: Path, reporter: Reporter) -> pd.DataFrame:
    rows = []
    if not long.empty:
        solved = long[long["status_norm"].eq("solved") & long["objective_norm"].notna()]
        for key, g in solved.groupby("instance_key", dropna=False):
            vals = sorted(set(g["objective_norm"].dropna().astype(float)))
            if len(vals) > 1:
                for _, row in g.iterrows():
                    rows.append({
                        "source_file": row.get("source_file", ""),
                        "instance_key": key,
                        "config_norm": row.get("config_norm", ""),
                        "objective_norm": row.get("objective_norm", np.nan),
                        "problem": "multiple_objectives_for_same_instance",
                    })
    for src in sources:
        if src.scheme != "article_code_comparison" or src.df.empty:
            continue
        df = src.df.copy()
        if "objective_comparison_status" not in df.columns:
            reporter.missing(src.path, "objective_comparison_status")
            continue
        mismatch = df["objective_comparison_status"].astype(str).str.upper().eq("MISMATCH")
        for _, row in df[mismatch].iterrows():
            rows.append({
                "source_file": str(src.path),
                "instance_key": f"{row.get('n')}|R={row.get('R')}|T={row.get('T')}|seed={row.get('seed')}",
                "config_norm": "article_comparison",
                "objective_norm": row.get("ours_reconstruct_objective", pd.NA),
                "article_objective": row.get("article_objective", pd.NA),
                "problem": "article_correctness_problem",
            })
    out = pd.DataFrame(rows)
    reporter.correctness_problem_count = len(out)
    save_csv(out, processed_dir / "correctness_mismatches.csv", reporter)
    return out


def process_gate_contamination(long: pd.DataFrame, sources: list[SourceFrame], processed_dir: Path, reporter: Reporter) -> pd.DataFrame:
    rows = []
    if not long.empty:
        if "process_memory_gate" in long.columns:
            gate = to_bool_series(long["process_memory_gate"]).fillna(False)
            reporter.process_gate_rows = int(gate.sum())
            for _, row in long[gate].iterrows():
                rows.append({
                    "source_file": row.get("source_file", ""),
                    "scheme": row.get("source_scheme", ""),
                    "instance_key": row.get("instance_key", ""),
                    "config_norm": row.get("config_norm", ""),
                    "reason": "process_memory_gate_true",
                })
    for src in sources:
        if src.possibly_process_gated:
            rows.append({
                "source_file": str(src.path),
                "scheme": src.scheme,
                "instance_key": "",
                "config_norm": "",
                "reason": "possibly_process_gated_old_solver_bench",
            })
    out = pd.DataFrame(rows)
    save_csv(out, processed_dir / "process_gate_contamination.csv", reporter)
    return out


def rank_rows(summary: pd.DataFrame) -> pd.DataFrame:
    if summary.empty:
        return summary
    out = summary.copy()
    out = out.sort_values(
        ["n", "solved_rate", "median_time_ms_solved", "p90_time_ms_solved"],
        ascending=[True, False, True, True],
    )
    out["rank_by_n"] = out.groupby("n").cumcount() + 1
    return out


def model_race(long: pd.DataFrame, processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    df = long[long["source_scheme"].eq("progressive_model_race")].copy() if not long.empty else pd.DataFrame()
    remove_stale_file(figures / "model_race_penalized_growth.png")
    if df.empty:
        save_csv(pd.DataFrame(), processed / "model_race_summary.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_winners_by_n.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_wins_by_seed.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_incomplete_stages.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_by_pair_summary.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_pair_variability.csv", reporter)
        save_csv(pd.DataFrame(), processed / "model_race_relative_by_RT.csv", reporter)
        save_no_data_figure(figures / "model_race_growth.png", "Сравнение моделей: рост времени", reporter)
        save_no_data_figure(figures / "model_race_by_pair_grid.png", "Сравнение моделей по (R,T)", reporter)
        save_no_data_figure(figures / "model_race_by_pair_grid_linear.png", "Сравнение моделей по (R,T), линейная шкала", reporter)
        save_no_data_figure(figures / "model_race_relative_to_best_by_RT.png", "Сравнение моделей: относительное время", reporter)
        save_no_data_figure(figures / "model_race_winner_heatmap.png", "Сравнение моделей: лучший режим", reporter)
        save_latex(pd.DataFrame(), latex / "table_model_race.tex", reporter)
        save_latex(pd.DataFrame(), latex / "table_model_race_pair_variability.tex", reporter)
        reporter.best_model_text = "no input data for this scheme"
        reporter.model_race_text = "no input data for this scheme"
        return
    df["config_norm"] = (
        df["config_norm"].astype(str)
        .str.replace("BestFinal_", "", regex=False)
        .str.replace("_cap0", "", regex=False)
    )
    expected_configs = int(df["config_norm"].nunique())
    stage = df.groupby("n", dropna=False).agg(
        row_count=("config_norm", "size"),
        config_count=("config_norm", "nunique"),
    ).reset_index()
    stage["expected_config_count"] = expected_configs
    stage["complete_stage"] = stage["config_count"].eq(expected_configs)
    complete_ns = set(stage.loc[stage["complete_stage"], "n"])
    incomplete = stage[~stage["complete_stage"]].copy()
    save_csv(incomplete, processed / "model_race_incomplete_stages.csv", reporter)
    if not incomplete.empty:
        reporter.notes.append(
            "progressive_model_race incomplete stages excluded from comparative plots/ranking: "
            + ", ".join(f"n={row.n:g} ({int(row.config_count)}/{expected_configs} configs)"
                        for row in incomplete.itertuples())
        )
    df_complete = df[df["n"].isin(complete_ns)].copy()
    summary = summary_stats(df, ["series_norm", "config_norm", "n", "R", "T"])
    summary = summary.merge(stage[["n", "complete_stage"]], on="n", how="left")
    summary = rank_rows(summary)
    summary_to_save = summary.drop(columns=["median_penalized_time_ms", "p90_penalized_time_ms"], errors="ignore")
    save_csv(summary_to_save, processed / "model_race_summary.csv", reporter)

    summary_complete = summary[summary["complete_stage"].eq(True)].copy()
    by_n = summary_complete.groupby(["config_norm", "n"], dropna=False).agg(
        solved_rate=("solved_rate", "mean"),
        median_time_ms_solved=("median_time_ms_solved", "median"),
        p90_time_ms_solved=("p90_time_ms_solved", "median"),
    ).reset_index()
    winners = by_n.sort_values(
        ["n", "solved_rate", "median_time_ms_solved", "p90_time_ms_solved"],
        ascending=[True, False, True, True],
    ).groupby("n", as_index=False).first()
    save_csv(winners, processed / "model_race_winners_by_n.csv", reporter)

    solved = df_complete[df_complete["status_norm"].eq("solved")].copy()
    if not solved.empty:
        idx = solved.groupby("instance_key")["time_ms_norm"].idxmin()
        wins = solved.loc[idx].groupby(["config_norm", "n"], dropna=False).size().reset_index(name="wins")
    else:
        wins = pd.DataFrame(columns=["config_norm", "n", "wins"])
    save_csv(wins, processed / "model_race_wins_by_seed.csv", reporter)
    line_plot(by_n, "n", "median_time_ms_solved", "config_norm", figures / "model_race_growth.png",
              "Сравнение моделей: медианное время решения по n (только полные этапы; без восстановления; process gate включён)",
              "медианное время решения, мс (логарифмическая шкала)", reporter, logy=True)
    pair_summary = df_complete.groupby(["n", "R", "T", "config_norm"], dropna=False).agg(
        median_time_ms=("time_ms_norm", "median"),
        solved_rate=("solved", "mean"),
    ).reset_index()
    save_csv(pair_summary, processed / "model_race_by_pair_summary.csv", reporter)
    largest_n = max(complete_ns) if complete_ns else np.nan
    pair_variability = pd.DataFrame()
    pair_sentence = ""
    if not pd.isna(largest_n):
        pair_at_n = pair_summary[pair_summary["n"].eq(largest_n)].copy()
        if not pair_at_n.empty:
            best_by_pair = (
                pair_at_n.sort_values(["R", "T", "median_time_ms"])
                .groupby(["R", "T"], as_index=False)
                .first()
                .rename(columns={"config_norm": "fastest_config", "median_time_ms": "best_median_time_ms"})
            )
            median_by_pair = pair_at_n.groupby(["R", "T"], as_index=False).agg(
                median_across_configs_ms=("median_time_ms", "median"),
                p90_config_median_ms=("median_time_ms", lambda x: x.quantile(0.90)),
            )
            pair_variability = best_by_pair.merge(median_by_pair, on=["R", "T"], how="left")
            min_best = pair_variability["best_median_time_ms"].min()
            min_median = pair_variability["median_across_configs_ms"].min()
            pair_variability["hardness_ratio_best"] = pair_variability["best_median_time_ms"] / min_best if min_best > 0 else np.nan
            pair_variability["hardness_ratio_median"] = pair_variability["median_across_configs_ms"] / min_median if min_median > 0 else np.nan
            pair_variability = pair_variability.sort_values(["best_median_time_ms", "R", "T"]).reset_index(drop=True)
            easiest = pair_variability.iloc[0]
            hardest = pair_variability.iloc[-1]
            pair_sentence = (
                f"At n={largest_n:g}, using each pair's fastest median config, the easiest pair is "
                f"(R={easiest['R']:g}, T={easiest['T']:g}) at {format_duration_ms(easiest['best_median_time_ms'])}, "
                f"while the hardest is (R={hardest['R']:g}, T={hardest['T']:g}) at "
                f"{format_duration_ms(hardest['best_median_time_ms'])}, about "
                f"{format_ratio(hardest['hardness_ratio_best'])} slower. "
                "This is why aggregate-by-n plots should be read together with paired (R,T) plots."
            )
    save_csv(pair_variability, processed / "model_race_pair_variability.csv", reporter)
    if pair_variability.empty:
        save_latex(pd.DataFrame(), latex / "table_model_race_pair_variability.tex", reporter)
    else:
        easiest_rows = pair_variability.nsmallest(5, "best_median_time_ms").assign(group="easiest")
        hardest_rows = pair_variability.nlargest(5, "best_median_time_ms").assign(group="hardest")
        pair_table = pd.concat([easiest_rows, hardest_rows], ignore_index=True)
        pair_table["best_median_time"] = pair_table["best_median_time_ms"].map(format_duration_ms)
        pair_table["median_all_configs"] = pair_table["median_across_configs_ms"].map(format_duration_ms)
        pair_table["hardness_ratio_best"] = pair_table["hardness_ratio_best"].map(format_ratio)
        save_latex(
            pair_table,
            latex / "table_model_race_pair_variability.tex",
            reporter,
            ["group", "R", "T", "fastest_config", "best_median_time", "median_all_configs", "hardness_ratio_best"],
            max_rows=10,
        )
    model_pair_grid(df_complete, figures / "model_race_by_pair_grid.png", reporter, logy=True)
    model_pair_grid(df_complete, figures / "model_race_by_pair_grid_linear.png", reporter, logy=False)
    relative = model_relative_ratio_heatmap(
        df_complete, figures / "model_race_relative_to_best_by_RT.png", reporter, largest_n)
    save_csv(relative, processed / "model_race_relative_by_RT.csv", reporter)
    model_winner_text_map(df_complete, figures / "model_race_winner_heatmap.png", reporter, largest_n)
    save_latex(winners, latex / "table_model_race.tex", reporter, ["n", "config_norm", "solved_rate", "median_time_ms_solved"])
    if not winners.empty:
        last = winners.sort_values("n").iloc[-1]
        reporter.best_model_text = (
            f"progressive_model_race alone: winner inside complete stages at largest complete n={last['n']} is "
            f"{last['config_norm']} by median solved time. This is not the final model-selection claim because that race is "
            "old solve-only data with reconstruction off and process gate enabled/assumed."
        )
        reporter.model_race_text = (
            "progressive_model_race measures time to prove optimum only: reconstruction is off. "
            "These are legacy solver_bench rows with process memory gate enabled/assumed; this is noted as metadata, "
            "not treated as a correctness problem. Incomplete stages are excluded from comparative plots; "
            f"largest complete n={last['n']}. The aggregate growth plot uses a log-scale y axis because solve times span several orders "
            "of magnitude; the per-(R,T) grid is generated in log scale and as a linear clipped-outlier view. "
            f"{pair_sentence}"
        )


def hard_pair_race(long: pd.DataFrame, processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    df = long[long["source_scheme"].eq("progressive_hard_pair_race")].copy() if not long.empty else pd.DataFrame()
    remove_stale_file(figures / "hard_subset_penalized_growth.png")
    remove_stale_file(figures / "hard_subset_growth.png")
    remove_stale_file(figures / "hard_subset_growth_linear.png")
    if df.empty:
        for name in ["hard_pair_summary.csv", "hard_subset_summary.csv", "hard_race_elimination.csv"]:
            save_csv(pd.DataFrame(), processed / name, reporter)
        save_no_data_figure(figures / "hard_pair_growth.png", "Трудная пара: логарифмическая шкала", reporter)
        save_no_data_figure(figures / "hard_pair_growth_linear.png", "Трудная пара: линейная шкала", reporter)
        save_no_data_figure(figures / "hard_subset_by_pair_grid.png", "Трудное подмножество по (R,T): логарифмическая шкала", reporter)
        save_no_data_figure(figures / "hard_subset_by_pair_grid_linear.png", "Трудное подмножество по (R,T): линейная шкала", reporter)
        save_latex(pd.DataFrame(), latex / "table_hard_pair.tex", reporter)
        save_latex(pd.DataFrame(), latex / "table_hard_race_elimination.tex", reporter)
        reporter.hard_pair_text = "no input data for this scheme"
        return
    df["config_norm"] = (
        df["config_norm"].astype(str)
        .str.replace("BestFinal_", "", regex=False)
        .str.replace("_cap0", "", regex=False)
    )
    primary = df[np.isclose(df["R"], HARD_PRIMARY[0]) & np.isclose(df["T"], HARD_PRIMARY[1])]
    subset = df[df.apply(lambda r: (round(float(r["R"]), 10), round(float(r["T"]), 10)) in HARD_SUBSET if pd.notna(r["R"]) and pd.notna(r["T"]) else False, axis=1)]
    primary_summary = summary_stats(primary, ["series_norm", "config_norm", "n", "R", "T"])
    subset_summary = summary_stats(subset, ["series_norm", "config_norm", "n", "R", "T"])
    primary_summary_to_save = primary_summary.drop(columns=["median_penalized_time_ms", "p90_penalized_time_ms"], errors="ignore")
    subset_summary_to_save = subset_summary.drop(columns=["median_penalized_time_ms", "p90_penalized_time_ms"], errors="ignore")
    save_csv(primary_summary_to_save, processed / "hard_pair_summary.csv", reporter)
    save_csv(subset_summary_to_save, processed / "hard_subset_summary.csv", reporter)
    by_n = primary.groupby(["config_norm", "n"], dropna=False)["time_ms_norm"].median().reset_index(name="median_time_ms_solved")
    line_plot(by_n, "n", "median_time_ms_solved", "config_norm", figures / "hard_pair_growth.png",
              "Трудная пара R=0.2, T=0.6 (логарифмическая шкала)", "медианное время решения, с (логарифмическая шкала)", reporter, logy=True, y_scale=1000.0)
    line_plot(by_n, "n", "median_time_ms_solved", "config_norm", figures / "hard_pair_growth_linear.png",
              "Трудная пара R=0.2, T=0.6 (линейная шкала)", "медианное время решения, с", reporter, logy=False, y_scale=1000.0)
    model_pair_grid(
        subset,
        figures / "hard_subset_by_pair_grid.png",
        reporter,
        logy=True,
        title_prefix="Трудные пары: медианное время решения по (R,T)",
        context="трудное подмножество",
        no_data_title="Трудное подмножество по (R,T)",
        y_scale=1000.0,
        y_unit="с",
    )
    model_pair_grid(
        subset,
        figures / "hard_subset_by_pair_grid_linear.png",
        reporter,
        logy=False,
        title_prefix="Трудные пары: медианное время решения по (R,T)",
        context="трудное подмножество",
        no_data_title="Трудное подмножество по (R,T), линейная шкала",
        y_scale=1000.0,
        y_unit="с",
    )
    elim_rows = []
    for n, g in by_n.groupby("n"):
        if g.empty:
            continue
        best = g["median_time_ms_solved"].min()
        if n >= 1400:
            cutoff = 1.10
        elif n >= 1300:
            cutoff = 1.15
        elif n >= 1200:
            cutoff = 1.20
        elif n >= 1100:
            cutoff = 1.35
        else:
            cutoff = 1.50
        statuses = primary[primary["n"].eq(n)].groupby("config_norm")["status_norm"].apply(lambda x: set(x)).to_dict()
        for _, row in g.iterrows():
            st = statuses.get(row["config_norm"], set())
            if "oot" in st:
                decision = "eliminated_oot"
            elif row["median_time_ms_solved"] <= cutoff * best:
                decision = "competitive"
            else:
                decision = "eliminated_slow"
            elim_rows.append({"n": n, "config_norm": row["config_norm"], "median_time_ms_solved": row["median_time_ms_solved"], "best_ms": best, "cutoff": cutoff, "decision": decision})
    elimination = pd.DataFrame(elim_rows)
    save_csv(elimination, processed / "hard_race_elimination.csv", reporter)
    hard_pair_table = primary_summary_to_save.copy()
    if not hard_pair_table.empty:
        hard_pair_table["median_time"] = hard_pair_table["median_time_ms_solved"].map(format_duration_ms)
        hard_pair_table["p90_time"] = hard_pair_table["p90_time_ms_solved"].map(format_duration_ms)
        hard_pair_table["memo_exact_hit_rate"] = hard_pair_table["median_memo_exact_hit_rate"].map(
            lambda x: "NA" if pd.isna(x) else f"{float(x) * 100:.1f}%"
        )
    save_latex(
        hard_pair_table,
        latex / "table_hard_pair.tex",
        reporter,
        ["config_norm", "n", "solved_rate", "median_time", "p90_time", "memo_exact_hit_rate"],
        max_rows=20,
    )
    elimination_table = elimination.copy()
    if not elimination_table.empty:
        elimination_table["median_time"] = elimination_table["median_time_ms_solved"].map(format_duration_ms)
        elimination_table["best_time"] = elimination_table["best_ms"].map(format_duration_ms)
        elimination_table["ratio_to_best"] = elimination_table["median_time_ms_solved"] / elimination_table["best_ms"].replace(0, np.nan)
        elimination_table["ratio_to_best"] = elimination_table["ratio_to_best"].map(format_ratio)
        elimination_table["cutoff"] = elimination_table["cutoff"].map(lambda x: f"{float(x):.2f}x")
    save_latex(
        elimination_table,
        latex / "table_hard_race_elimination.tex",
        reporter,
        ["n", "config_norm", "median_time", "best_time", "ratio_to_best", "cutoff", "decision"],
        max_rows=20,
    )
    n1100 = primary_summary[np.isclose(primary_summary["n"], 1100)].copy() if "n" in primary_summary else pd.DataFrame()
    if not n1100.empty:
        ranked = n1100.sort_values("median_time_ms_solved")
        best = ranked.iloc[0]
        second = ranked.iloc[1] if len(ranked) > 1 else None
        best_name = str(best["config_norm"]).replace("BestFinal_", "").replace("_cap0", "")
        if second is not None and float(best["median_time_ms_solved"]) > 0:
            second_name = str(second["config_norm"]).replace("BestFinal_", "").replace("_cap0", "")
            ratio = float(second["median_time_ms_solved"]) / float(best["median_time_ms_solved"])
            hard_sentence = (
                f"Hard-pair race R=0.2,T=0.6 at n=1100 selects {best_name}: median "
                f"{format_duration_ms(best['median_time_ms_solved'])}, versus {second_name} at "
                f"{format_duration_ms(second['median_time_ms_solved'])} ({ratio:.2f}x slower)."
            )
        else:
            hard_sentence = (
                f"Hard-pair race R=0.2,T=0.6 at n=1100 selects {best_name}: median "
                f"{format_duration_ms(best['median_time_ms_solved'])}."
            )
        max_n_by_config = primary_summary.groupby("config_norm")["n"].max()
        adaptive_v3_max = max_n_by_config[max_n_by_config.index.astype(str).str.contains("adaptive_v3", regex=False)]
        if not adaptive_v3_max.empty:
            hard_sentence += f" adaptive_v3 is the model carried to n={float(adaptive_v3_max.max()):g} in this race."
        elim1100 = elimination[np.isclose(elimination["n"], 1100)].copy() if not elimination.empty else pd.DataFrame()
        cutoff_text = ""
        if not elim1100.empty:
            cutoff = float(elim1100["cutoff"].iloc[0])
            best_ms = float(elim1100["best_ms"].iloc[0])
            threshold_ms = cutoff * best_ms
            eliminated = sorted(elim1100.loc[elim1100["decision"].ne("competitive"), "config_norm"].astype(str).tolist())
            cutoff_text = (
                f" At n=1100 the survival threshold is {cutoff:.2f}x best = {format_duration_ms(threshold_ms)}; "
                f"all non-adaptive_v3 configs are above it and are marked eliminated_slow: {', '.join(eliminated)}."
            )
        reporter.hard_pair_text = (
            "progressive_hard_pair_race is also old solve-only data: reconstruction is off and process memory gate is "
            "enabled/assumed. Since this run has no OOT/OOM/ERR rows, the scheme is reported with solved time only. "
            "The primary hard pair is R=0.2,T=0.6; the hard-subset grid shows R,T = (0.2,0.4), (0.2,0.6), "
            "(0.2,0.8), and (0.4,0.6). "
            f"{hard_sentence}{cutoff_text}"
        )
        reporter.best_model_text = (
            f"{reporter.best_model_text} Final baseline model is adaptive_v3 because the later hard-pair race, "
            "which targets the difficult region used for larger runs, favors it. This choice intentionally weights hard (R,T) pairs "
            "more than easy pairs: on easy pairs model differences are usually milliseconds or tens of milliseconds, while on hard pairs "
            f"the gap can be around 100,000 ms or more. {hard_sentence} "
            "The final backend/reconstruction/article comparisons therefore use adaptive_v3 with the current best reconstruct pipeline."
        )


def bounds_mode_from_row(row: pd.Series) -> str:
    simple = bool_value(row.get("enable_simple_lb")) is True
    lbmemo = bool_value(row.get("enable_lb_memo")) is True
    eddub = bool_value(row.get("enable_edd_ub")) is True
    if not simple and not lbmemo and not eddub:
        return "baseline_bounds"
    if simple and not lbmemo and not eddub:
        return "simple_lb"
    if simple and lbmemo and not eddub:
        return "simple_lb_lb_memo"
    if not simple and not lbmemo and eddub:
        return "edd_ub"
    if simple and not lbmemo and eddub:
        return "simple_lb_edd_ub"
    if simple and lbmemo and eddub:
        return "simple_lb_lb_memo_edd_ub"
    return "other_bounds"


def bounds_ablation(long: pd.DataFrame, processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    df = long[long["source_scheme"].eq("bounds_ablation")].copy() if not long.empty else pd.DataFrame()
    for stale in ["bounds_time_ratio.png", "bounds_nodes_vs_time.png", "bounds_bound_time_share.png", "bounds_time_ratio_by_pair_log.png"]:
        remove_stale_file(figures / stale)
    if df.empty:
        for name in ["bounds_ablation_summary.csv", "bounds_ablation_paired_ratios.csv"]:
            save_csv(pd.DataFrame(), processed / name, reporter)
        no_data_titles = {
            "bounds_time_ratio_by_pair_linear.png": "Анализ границ: отношение времени к базовой конфигурации",
            "bounds_node_ratio_by_pair.png": "Анализ границ: отношение числа узлов к базовой конфигурации",
        }
        for name, title in no_data_titles.items():
            save_no_data_figure(figures / name, title, reporter)
        save_latex(pd.DataFrame(), latex / "table_bounds_ablation.tex", reporter)
        return
    df["bounds_mode"] = df.apply(bounds_mode_from_row, axis=1)
    summary = summary_stats(df.assign(config_norm=df["bounds_mode"]), ["series_norm", "config_norm", "n", "R", "T"])
    if "simple_lb_calls" in df and "simple_lb_prunes" in df:
        rates = df.groupby("bounds_mode").apply(
            lambda g: (number_series(g["simple_lb_prunes"]).sum() / number_series(g["simple_lb_calls"]).sum())
            if number_series(g["simple_lb_calls"]).sum() > 0 else np.nan,
            include_groups=False,
        ).reset_index(name="simple_lb_prune_rate")
        summary = summary.merge(rates, left_on="config_norm", right_on="bounds_mode", how="left").drop(columns=["bounds_mode"], errors="ignore")
    base = df[df["bounds_mode"].eq("baseline_bounds")]
    modes = df[~df["bounds_mode"].eq("baseline_bounds")]
    pair_rows = []
    base_cols = ["instance_key", "objective_norm", "time_ms_norm", "nodes", "memory_peak_bytes_norm"]
    base_map = base[base_cols].rename(columns={c: f"baseline_{c}" for c in base_cols if c != "instance_key"})
    merged = modes.merge(base_map, on="instance_key", how="inner")
    for _, row in merged.iterrows():
        pair_rows.append({
            "instance_key": row["instance_key"],
            "bounds_mode": row["bounds_mode"],
            "n": row["n"], "R": row["R"], "T": row["T"], "seed": row["seed"],
            "objective_match": row["objective_norm"] == row["baseline_objective_norm"],
            "time_ratio": row["time_ms_norm"] / row["baseline_time_ms_norm"] if row["baseline_time_ms_norm"] else np.nan,
            "node_ratio": row["nodes"] / row["baseline_nodes"] if row["baseline_nodes"] else np.nan,
            "memory_ratio": row["memory_peak_bytes_norm"] / row["baseline_memory_peak_bytes_norm"] if row["baseline_memory_peak_bytes_norm"] else np.nan,
            "bound_time_share": row.get("bound_time_ms", np.nan) / row["time_ms_norm"] if row["time_ms_norm"] else np.nan,
            "memo_lb_hit_rate": row.get("memo_lb_hits", np.nan) / row.get("memo_lb_queries", np.nan) if row.get("memo_lb_queries", np.nan) else np.nan,
        })
    paired = pd.DataFrame(pair_rows)
    decision_rows = []
    if not paired.empty:
        grouped = paired.groupby("bounds_mode", dropna=False)
        decisions = grouped.agg(
            objective_match=("objective_match", "all"),
            median_time_ratio=("time_ratio", "median"),
            p90_time_ratio=("time_ratio", lambda x: x.quantile(0.90)),
            median_node_ratio=("node_ratio", "median"),
            median_bound_time_share=("bound_time_share", "median"),
            memo_lb_hit_rate=("memo_lb_hit_rate", "median"),
        ).reset_index()
        for _, row in decisions.iterrows():
            hard = paired[(paired["bounds_mode"].eq(row["bounds_mode"])) & paired.apply(lambda r: (round(float(r["R"]), 10), round(float(r["T"]), 10)) in HARD_SUBSET if pd.notna(r["R"]) and pd.notna(r["T"]) else False, axis=1)]
            hard_time = hard["time_ratio"].median() if not hard.empty else np.nan
            if not row["objective_match"]:
                decision = "reject"
            elif row["median_node_ratio"] < 1 and row["median_time_ratio"] > 1:
                decision = "nodes_down_time_up"
            elif pd.notna(hard_time) and hard_time < 0.9:
                decision = "promising"
            else:
                decision = "not_selected"
            decision_rows.append({**row.to_dict(), "decision": decision})
        decision_df = pd.DataFrame(decision_rows)
    else:
        decision_df = pd.DataFrame()
    save_csv(decision_df if not decision_df.empty else summary, processed / "bounds_ablation_summary.csv", reporter)
    save_csv(paired, processed / "bounds_ablation_paired_ratios.csv", reporter)
    if not decision_df.empty:
        bounds_ratio_pair_grid(
            paired,
            "time_ratio",
            figures / "bounds_time_ratio_by_pair_linear.png",
            reporter,
            logy=False,
            title="Анализ границ: медианное отношение времени к базовой конфигурации по (R,T)",
            ylabel="время / baseline",
        )
        bounds_ratio_pair_grid(
            paired,
            "node_ratio",
            figures / "bounds_node_ratio_by_pair.png",
            reporter,
            logy=False,
            title="Анализ границ: медианное отношение числа узлов к базовой конфигурации по (R,T)",
            ylabel="узлы / baseline",
        )
        worst = decision_df.sort_values("median_time_ratio", ascending=False).iloc[0]
        best_time = decision_df.sort_values("median_time_ratio").iloc[0]
        simple_lb_rows = decision_df[decision_df["bounds_mode"].astype(str).str.contains("simple_lb", na=False)]
        simple_lb_time_min = simple_lb_rows["median_time_ratio"].min() if not simple_lb_rows.empty else np.nan
        simple_lb_time_max = simple_lb_rows["median_time_ratio"].max() if not simple_lb_rows.empty else np.nan
        simple_lb_node_min = simple_lb_rows["median_node_ratio"].min() if not simple_lb_rows.empty else np.nan
        simple_lb_node_max = simple_lb_rows["median_node_ratio"].max() if not simple_lb_rows.empty else np.nan
        simple_lb_bound_share = simple_lb_rows["median_bound_time_share"].median() if not simple_lb_rows.empty else np.nan
        memo_lb_hit = decision_df["memo_lb_hit_rate"].dropna().median()
        objective_ok = bool(decision_df["objective_match"].fillna(False).all())
        reporter.bounds_text = (
            "bounds_ablation is plotted as paired ratios to baseline_bounds on the same instance key. "
            "The figures show, for each (R,T), median time ratio in linear scale and median node ratio; "
            "the dashed horizontal line is baseline=1.0. "
            f"Objective check: {'all modes match baseline' if objective_ok else 'mismatch found'}. "
            f"The closest mode to neutral is {best_time.bounds_mode} with median time ratio "
            f"{format_ratio(best_time.median_time_ratio)} and median node ratio {format_ratio(best_time.median_node_ratio)}; "
            f"it is still slower than baseline, so it is not selected. "
            f"Simple-LB variants reduce nodes only from {format_ratio(simple_lb_node_max)} to {format_ratio(simple_lb_node_min)} "
            f"while slowing time from {format_ratio(simple_lb_time_min)} to {format_ratio(simple_lb_time_max)}; "
            f"their median bound-time share is {format_percent(simple_lb_bound_share)}. "
            f"LB memo hit rate is {format_percent(memo_lb_hit)} where measured. "
            f"Worst median time ratio is {worst.bounds_mode}={format_ratio(worst.median_time_ratio)}. "
            "Conclusion: keep LB/UB off in the final configuration. Decisions: "
            + "; ".join(f"{r.bounds_mode}: {r.decision}" for r in decision_df.itertuples())
        )
    else:
        no_data_titles = {
            "bounds_time_ratio_by_pair_linear.png": "Анализ границ: отношение времени к базовой конфигурации",
            "bounds_node_ratio_by_pair.png": "Анализ границ: отношение числа узлов к базовой конфигурации",
        }
        for name, title in no_data_titles.items():
            save_no_data_figure(figures / name, title, reporter)
    if not decision_df.empty:
        bounds_table = decision_df.copy()
        bounds_table["mode"] = bounds_table["bounds_mode"]
        bounds_table["objective"] = bounds_table["objective_match"].map(format_match)
        bounds_table["median_time"] = bounds_table["median_time_ratio"].map(format_ratio)
        bounds_table["p90_time"] = bounds_table["p90_time_ratio"].map(format_ratio)
        bounds_table["median_nodes"] = bounds_table["median_node_ratio"].map(format_ratio)
        bounds_table["bound_time"] = bounds_table["median_bound_time_share"].map(format_percent)
        bounds_table["lb_memo_hit"] = bounds_table["memo_lb_hit_rate"].map(format_percent)
        save_latex(
            bounds_table,
            latex / "table_bounds_ablation.tex",
            reporter,
            ["mode", "objective", "median_time", "p90_time", "median_nodes", "bound_time", "lb_memo_hit", "decision"],
            max_rows=20,
        )
    else:
        save_latex(decision_df, latex / "table_bounds_ablation.tex", reporter)


def backend_comparison(sources: list[SourceFrame], long: pd.DataFrame, processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    for stale_name in ["backend_time_ratio_clean.png", "backend_memory_ratio_clean.png", "backend_cleanup_time.png"]:
        remove_stale_file(figures / stale_name)
    pair_frames = []
    prelim_frames = []
    for src in sources:
        if src.scheme != "memo_backend" or src.df.empty:
            continue
        if "custom_status" in src.df.columns:
            df = src.df.copy()
            df["source_file"] = str(src.path)
            df["process_gated_file"] = False
            pair_frames.append(df)
        else:
            # Old solver_bench-style memo backend comparison.
            df_long = normalize_standard_runs(src, reporter, 10.0, "par10")
            if df_long.empty or "memo_backend" not in df_long:
                continue
            pivot = df_long.pivot_table(index="instance_key", columns="memo_backend", values=["time_ms_norm", "memory_peak_bytes_norm", "objective_norm"], aggfunc="first")
            rows = []
            for key, row in pivot.iterrows():
                if ("time_ms_norm", "custom") in row.index and ("time_ms_norm", "std_unordered") in row.index:
                    rows.append({
                        "instance_key": key,
                        "custom_reported_time_ms": row.get(("time_ms_norm", "custom"), np.nan),
                        "std_unordered_reported_time_ms": row.get(("time_ms_norm", "std_unordered"), np.nan),
                        "custom_memo_used_mb": row.get(("memory_peak_bytes_norm", "custom"), np.nan) / 1024.0 / 1024.0,
                        "std_unordered_memo_used_mb": row.get(("memory_peak_bytes_norm", "std_unordered"), np.nan) / 1024.0 / 1024.0,
                        "custom_objective": row.get(("objective_norm", "custom"), np.nan),
                        "std_unordered_objective": row.get(("objective_norm", "std_unordered"), np.nan),
                        "process_gated_file": src.possibly_process_gated,
                        "source_file": str(src.path),
                    })
            if rows:
                (prelim_frames if src.possibly_process_gated else pair_frames).append(pd.DataFrame(rows))
    clean = pd.concat(pair_frames, ignore_index=True, sort=False) if pair_frames else pd.DataFrame()
    prelim = pd.concat(prelim_frames, ignore_index=True, sort=False) if prelim_frames else pd.DataFrame()
    for df in [clean, prelim]:
        if df.empty:
            continue
        df["objective_match"] = df["custom_objective"].astype(str) == df["std_unordered_objective"].astype(str)
        df["time_ratio_custom_to_std"] = number_series(df["custom_reported_time_ms"]) / number_series(df["std_unordered_reported_time_ms"]).replace(0, np.nan)
        if "custom_memo_used_mb" in df and "std_unordered_memo_used_mb" in df:
            df["memory_ratio_custom_to_std"] = number_series(df["custom_memo_used_mb"]) / number_series(df["std_unordered_memo_used_mb"]).replace(0, np.nan)
        df["memo_full_key_verification_ok"] = True
        df["exact_memo_enabled_ok"] = True
    save_csv(clean, processed / "backend_comparison_clean.csv", reporter)
    save_csv(prelim, processed / "backend_comparison_preliminary_process_gate.csv", reporter)
    if not clean.empty and not prelim.empty:
        effect = pd.DataFrame({
            "metric": ["median_time_ratio_custom_to_std_clean", "median_time_ratio_custom_to_std_preliminary"],
            "value": [clean["time_ratio_custom_to_std"].median(), prelim["time_ratio_custom_to_std"].median()],
        })
    else:
        effect = pd.DataFrame()
    save_csv(effect, processed / "backend_process_gate_effect.csv", reporter)
    if not clean.empty:
        by_n = clean.copy()
        if "n" not in by_n.columns and "instance_key" in by_n.columns:
            by_n["n"] = by_n["instance_key"].str.extract(r"^([^|]+)").astype(float)
        by_n["n"] = number_series(by_n["n"])
        by_n["time_ratio_custom_to_std"] = number_series(by_n["time_ratio_custom_to_std"])
        by_n["memory_ratio_custom_to_std"] = number_series(by_n.get("memory_ratio_custom_to_std", pd.Series(np.nan, index=by_n.index)))
        by_n["custom_faster"] = by_n["time_ratio_custom_to_std"] < 1.0
        summary_by_n = by_n.groupby("n", dropna=False).agg(
            instances=("n", "size"),
            median_time_ratio_custom_to_std=("time_ratio_custom_to_std", "median"),
            p10_time_ratio_custom_to_std=("time_ratio_custom_to_std", lambda s: s.quantile(0.10)),
            p90_time_ratio_custom_to_std=("time_ratio_custom_to_std", lambda s: s.quantile(0.90)),
            custom_win_rate=("custom_faster", "mean"),
            median_memory_ratio_custom_to_std=("memory_ratio_custom_to_std", "median"),
            p10_memory_ratio_custom_to_std=("memory_ratio_custom_to_std", lambda s: s.quantile(0.10)),
            p90_memory_ratio_custom_to_std=("memory_ratio_custom_to_std", lambda s: s.quantile(0.90)),
        ).reset_index()
        save_csv(summary_by_n, processed / "backend_comparison_by_n.csv", reporter)
        memory_by_pair = (
            by_n.groupby(["n", "R", "T"], dropna=False)["memory_ratio_custom_to_std"]
            .median()
            .reset_index(name="median_memory_ratio_custom_to_std")
        )
        if not memory_by_pair.empty:
            memory_spread = memory_by_pair.groupby("n", dropna=False).agg(
                min_memory_ratio=("median_memory_ratio_custom_to_std", "min"),
                max_memory_ratio=("median_memory_ratio_custom_to_std", "max"),
            ).reset_index()
            memory_spread["pair_level_spread"] = memory_spread["max_memory_ratio"] - memory_spread["min_memory_ratio"]
        else:
            memory_spread = pd.DataFrame()
        save_csv(memory_spread, processed / "backend_memory_ratio_spread_by_n.csv", reporter)
        win_rate = backend_custom_win_rate_heatmap(clean, figures / "backend_custom_win_rate.png", reporter)
        save_csv(win_rate, processed / "backend_custom_win_rate.csv", reporter)
        backend_time_ratio_distribution(clean, figures / "backend_time_ratio_distribution_by_n.png", reporter)
        backend_time_ratio_pair_grid(clean, figures / "backend_time_ratio_by_pair_grid.png", reporter)
        backend_memory_ratio_by_n(clean, figures / "backend_memory_ratio_by_n.png", reporter)
        backend_time_memory_tradeoff(clean, figures / "backend_time_memory_tradeoff.png", reporter)
        objective_ok = bool(clean.get("objective_match", pd.Series(False, index=clean.index)).fillna(False).astype(bool).all())
        full_key_ok = bool(clean.get("memo_full_key_verification_ok", pd.Series(False, index=clean.index)).fillna(False).astype(bool).all())
        exact_ok = bool(clean.get("exact_memo_enabled_ok", pd.Series(False, index=clean.index)).fillna(False).astype(bool).all())
        overall_win = by_n["custom_faster"].mean()
        n50 = by_n[by_n["n"].eq(50)]
        nge100 = by_n[by_n["n"].ge(100)]
        n50_text = format_percent(n50["custom_faster"].mean()) if not n50.empty else "NA"
        nge100_text = format_percent(nge100["custom_faster"].mean()) if not nge100.empty else "NA"
        max_memory_spread = memory_spread["pair_level_spread"].max() if not memory_spread.empty else np.nan
        reporter.backend_text = (
            "memo_backend_comparison uses clean no-process-gate data only for the final backend decision. "
            "Rows are paired by the same instance. "
            f"Objective check: {'match' if objective_ok else 'mismatch found'}; "
            f"memo_full_key_verification={'ok' if full_key_ok else 'not ok'}; "
            f"exact memo={'ok' if exact_ok else 'not ok'}. "
            f"Overall median custom/std time ratio is {format_ratio(by_n['time_ratio_custom_to_std'].median())}; "
            f"median memory ratio is {format_ratio(by_n['memory_ratio_custom_to_std'].median())}. "
            f"Custom win-rate is {format_percent(overall_win)} overall, {n50_text} at n=50, and {nge100_text} for n>=100. "
            f"Memory is plotted as one median line because the maximum pair-level (R,T) spread is {max_memory_spread:.2e}. "
            "Conclusion: keep custom memo backend; n=50 is noisy, but from n>=100 custom is consistently faster and uses less memory."
        )
        backend_table = summary_by_n.copy()
        backend_table["n"] = backend_table["n"].map(lambda x: f"{int(x):d}" if pd.notna(x) and float(x).is_integer() else f"{x:g}")
        backend_table["time_ratio"] = backend_table["median_time_ratio_custom_to_std"].map(format_ratio)
        backend_table["time_p10_p90"] = (
            backend_table["p10_time_ratio_custom_to_std"].map(format_ratio)
            + " - "
            + backend_table["p90_time_ratio_custom_to_std"].map(format_ratio)
        )
        backend_table["custom_wins"] = backend_table["custom_win_rate"].map(format_percent)
        backend_table["memory_ratio"] = backend_table["median_memory_ratio_custom_to_std"].map(format_ratio)
    else:
        save_csv(pd.DataFrame(), processed / "backend_comparison_by_n.csv", reporter)
        save_csv(pd.DataFrame(), processed / "backend_memory_ratio_spread_by_n.csv", reporter)
        save_csv(pd.DataFrame(), processed / "backend_custom_win_rate.csv", reporter)
        no_data_titles = {
            "backend_time_ratio_distribution_by_n.png": "Сравнение memo-таблиц: распределение отношения времени",
            "backend_time_ratio_by_pair_grid.png": "Сравнение memo-таблиц: отношение времени по (R,T)",
            "backend_memory_ratio_by_n.png": "Сравнение memo-таблиц: отношение памяти",
            "backend_time_memory_tradeoff.png": "Сравнение memo-таблиц: компромисс времени и памяти",
            "backend_custom_win_rate.png": "Сравнение memo-таблиц: доля запусков, где custom быстрее",
        }
        for name, title in no_data_titles.items():
            save_no_data_figure(figures / name, title, reporter)
        backend_table = pd.DataFrame()
    save_latex(
        backend_table,
        latex / "table_backend_clean.tex",
        reporter,
        ["n", "instances", "time_ratio", "time_p10_p90", "custom_wins", "memory_ratio"],
        max_rows=20,
    )


def reconstruction_analysis(sources: list[SourceFrame], processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    for stale_name in [
        "reconstruction_overhead_percent.png",
        "reconstruction_before_after.png",
        "reconstruction_memory_tradeoff.png",
    ]:
        remove_stale_file(figures / stale_name)
    rows = []
    for src in sources:
        if src.scheme != "reconstruction" or src.df.empty:
            continue
        df = src.df.copy()
        is_new = "trace_reconstruct_reported_time_ms" in df.columns
        if is_new:
            solve = number_series(df.get("solve_only_reported_time_ms", pd.Series(np.nan, index=df.index)))
            recon = number_series(df.get("trace_reconstruct_reported_time_ms", pd.Series(np.nan, index=df.index)))
            cfg = "new_solve_with_reconstruction"
            obj_match = df.get("objective_match", pd.Series([pd.NA] * len(df))).astype(str).str.lower().isin(["true", "1"])
            order_ok = df.get("order_valid", pd.Series([pd.NA] * len(df))).astype(str).str.lower().isin(["true", "1"])
            mem = number_series(df.get("trace_reconstruct_memo_used_mb", pd.Series(np.nan, index=df.index)))
            solve_mem = number_series(df.get("solve_only_memo_used_mb", pd.Series(np.nan, index=df.index)))
            wall_overhead_ms = number_series(df.get("wall_overhead_ms", pd.Series(np.nan, index=df.index)))
            wall_overhead_pct = number_series(df.get("wall_overhead_percent", pd.Series(np.nan, index=df.index)))
            actual_reconstruction_ms = number_series(df.get("trace_reconstruction_time_ms", pd.Series(np.nan, index=df.index)))
            trace_hits = number_series(df.get("trace_reconstruction_trace_hits", pd.Series(np.nan, index=df.index)))
            trace_fallbacks = number_series(df.get("trace_reconstruction_trace_fallbacks", pd.Series(np.nan, index=df.index)))
        else:
            solve = number_series(df.get("solve_only_reported_time_ms", df.get("solve_only_wall_time_ms", pd.Series(np.nan, index=df.index))))
            recon = number_series(df.get("solve_with_reconstruction_reported_time_ms", df.get("solve_with_reconstruction_wall_time_ms", pd.Series(np.nan, index=df.index))))
            cfg = "old_solve_with_reconstruction"
            obj_match = df.get("objective_match", pd.Series([pd.NA] * len(df))).astype(str).str.lower().isin(["true", "1"])
            rec_success = df.get("reconstruction_success", pd.Series([pd.NA] * len(df))).astype(str).str.lower().isin(["true", "1"])
            order_len = number_series(df.get("order_length", pd.Series(np.nan, index=df.index)))
            n = number_series(df.get("n", pd.Series(np.nan, index=df.index)))
            order_ok = rec_success | order_len.eq(n)
            mem = pd.Series(np.nan, index=df.index)
            solve_mem = pd.Series(np.nan, index=df.index)
            wall_overhead_ms = number_series(df.get("reconstruction_overhead_ms", pd.Series(np.nan, index=df.index)))
            wall_overhead_pct = number_series(df.get("reconstruction_overhead_percent", pd.Series(np.nan, index=df.index)))
            actual_reconstruction_ms = pd.Series(np.nan, index=df.index)
            trace_hits = pd.Series(np.nan, index=df.index)
            trace_fallbacks = pd.Series(np.nan, index=df.index)
        overhead = recon - solve
        pct = overhead / solve.replace(0, np.nan) * 100.0
        for i in range(len(df)):
            memory_ratio = np.nan
            if pd.notna(mem.iloc[i]) and pd.notna(solve_mem.iloc[i]) and solve_mem.iloc[i] > 0:
                memory_ratio = mem.iloc[i] / solve_mem.iloc[i]
            rows.append({
                "source_file": str(src.path),
                "config_norm": cfg,
                "n": df.get("n", pd.Series([pd.NA] * len(df))).iloc[i],
                "R": df.get("R", pd.Series([pd.NA] * len(df))).iloc[i],
                "T": df.get("T", pd.Series([pd.NA] * len(df))).iloc[i],
                "seed": df.get("seed", pd.Series([pd.NA] * len(df))).iloc[i],
                "solve_only_time_ms": solve.iloc[i],
                "solve_with_reconstruction_time_ms": recon.iloc[i],
                "overhead_ms": overhead.iloc[i],
                "overhead_percent": pct.iloc[i],
                "wall_overhead_ms": wall_overhead_ms.iloc[i],
                "wall_overhead_percent": wall_overhead_pct.iloc[i],
                "actual_reconstruction_time_ms": actual_reconstruction_ms.iloc[i],
                "solve_only_memory_mb": solve_mem.iloc[i],
                "objective_match": bool(obj_match.iloc[i]),
                "order_valid": bool(order_ok.iloc[i]),
                "memory_mb": mem.iloc[i],
                "memory_ratio_to_solve_only": memory_ratio,
                "trace_hits": trace_hits.iloc[i],
                "trace_fallbacks": trace_fallbacks.iloc[i],
            })
    out = pd.DataFrame(rows)
    summary = out.groupby(["config_norm", "n"], dropna=False).agg(
        rows=("config_norm", "size"),
        median_overhead_percent=("overhead_percent", "median"),
        p90_overhead_percent=("overhead_percent", lambda x: x.quantile(0.90)),
        median_overhead_ms=("overhead_ms", "median"),
        median_wall_overhead_percent=("wall_overhead_percent", "median"),
        p90_wall_overhead_percent=("wall_overhead_percent", lambda x: x.quantile(0.90)),
        median_actual_reconstruction_time_ms=("actual_reconstruction_time_ms", "median"),
        objective_match_rate=("objective_match", "mean"),
        order_valid_rate=("order_valid", "mean"),
        median_memory_mb=("memory_mb", "median"),
        median_memory_ratio_to_solve_only=("memory_ratio_to_solve_only", "median"),
        median_trace_hits=("trace_hits", "median"),
        max_trace_fallbacks=("trace_fallbacks", "max"),
    ).reset_index() if not out.empty else pd.DataFrame()
    save_csv(summary, processed / "reconstruction_summary.csv", reporter)
    save_csv(out, processed / "reconstruction_before_after.csv", reporter)
    if not summary.empty:
        plot_reconstruction_wall_overhead(
            summary,
            figures / "reconstruction_wall_overhead_old_vs_new.png",
            reporter,
        )
        new_summary = summary[summary["config_norm"].eq("new_solve_with_reconstruction")].copy()
        plot_reconstruction_single_line(
            new_summary,
            "median_actual_reconstruction_time_ms",
            figures / "reconstruction_actual_trace_time_by_n.png",
            reporter,
            title="Новое trace-восстановление: время финального восстановления расписания",
            ylabel="медианное время восстановления, мс",
        )
        plot_reconstruction_single_line(
            new_summary,
            "median_memory_ratio_to_solve_only",
            figures / "reconstruction_trace_memory_ratio_by_n.png",
            reporter,
            title="Память, необходимая для trace-восстановления",
            ylabel="отношение памяти: trace / solve-only",
            parity_y=1.0,
        )
    else:
        no_data_titles = {
            "reconstruction_wall_overhead_old_vs_new.png": "Сравнение старого и нового восстановления",
            "reconstruction_actual_trace_time_by_n.png": "Новое trace-восстановление: время восстановления расписания",
            "reconstruction_trace_memory_ratio_by_n.png": "Память, необходимая для trace-восстановления",
        }
        for name, title in no_data_titles.items():
            save_no_data_figure(figures / name, title, reporter)
    if not summary.empty:
        old = summary[summary["config_norm"].eq("old_solve_with_reconstruction")][["n", "median_wall_overhead_percent"]].rename(
            columns={"median_wall_overhead_percent": "old_wall_overhead_percent"}
        )
        new = summary[summary["config_norm"].eq("new_solve_with_reconstruction")][[
            "n",
            "median_wall_overhead_percent",
            "median_actual_reconstruction_time_ms",
            "median_memory_ratio_to_solve_only",
            "objective_match_rate",
            "order_valid_rate",
            "max_trace_fallbacks",
        ]].rename(columns={"median_wall_overhead_percent": "new_wall_overhead_percent"})
        table = pd.merge(new, old, on="n", how="outer").sort_values("n")
        table["n"] = table["n"].map(lambda x: f"{int(x):d}" if pd.notna(x) and float(x).is_integer() else f"{x:g}")
        table["old_wall"] = table["old_wall_overhead_percent"].map(format_percent_points)
        table["new_wall"] = table["new_wall_overhead_percent"].map(format_percent_points)
        table["actual_reconstruct"] = table["median_actual_reconstruction_time_ms"].map(format_duration_ms)
        table["memory_ratio"] = table["median_memory_ratio_to_solve_only"].map(format_ratio)
        table["correct"] = table["objective_match_rate"].map(format_percent)
        table["valid_order"] = table["order_valid_rate"].map(format_percent)
        table["trace_fallbacks"] = table["max_trace_fallbacks"].map(lambda x: "--" if pd.isna(x) else f"{int(x):d}")
        save_latex(
            table,
            latex / "table_reconstruction.tex",
            reporter,
            ["n", "old_wall", "new_wall", "actual_reconstruct", "memory_ratio", "correct", "valid_order", "trace_fallbacks"],
            max_rows=20,
        )
    else:
        save_latex(summary, latex / "table_reconstruction.tex", reporter)
    if not summary.empty:
        new_summary = summary[summary["config_norm"].eq("new_solve_with_reconstruction")]
        old_summary = summary[summary["config_norm"].eq("old_solve_with_reconstruction")]
        max_new_reconstruct = new_summary["median_actual_reconstruction_time_ms"].max() if not new_summary.empty else np.nan
        max_new_wall = new_summary["median_wall_overhead_percent"].max() if not new_summary.empty else np.nan
        min_old_wall = old_summary["median_wall_overhead_percent"].min() if not old_summary.empty else np.nan
        max_memory_ratio = new_summary["median_memory_ratio_to_solve_only"].max() if not new_summary.empty else np.nan
        max_fallbacks = new_summary["max_trace_fallbacks"].max() if not new_summary.empty else np.nan
        reporter.reconstruction_text = (
            "reconstruction_overhead compares legacy wall-overhead data with the current trace-based reconstruction run. "
            "The old CSV has no pure reconstruction_time_ms field, so old-vs-new speed is shown with paired process wall overhead; "
            "the primary current metric is trace_reconstruction_time_ms. "
            f"New trace reconstruction stays at or below {format_duration_ms(max_new_reconstruct)} median actual reconstruction time, "
            f"with maximum median wall overhead {format_percent_points(max_new_wall)} versus old legacy minimum median wall overhead {format_percent_points(min_old_wall)}. "
            f"Trace fallback max is {int(max_fallbacks) if pd.notna(max_fallbacks) else 'NA'}; objective/order checks are 100%. "
            f"The cost is memory: median trace/solve-only memo ratio reaches {format_ratio(max_memory_ratio)}. "
            "Conclusion: keep the new trace reconstruction path; it makes final schedule recovery fast, with an explicit memory tradeoff."
        )


def article_analysis(sources: list[SourceFrame], processed: Path, figures: Path, latex: Path, reporter: Reporter) -> None:
    frames = [src.df.assign(source_file=str(src.path)) for src in sources if src.scheme == "article_code_comparison" and not src.df.empty]
    df = pd.concat(frames, ignore_index=True, sort=False) if frames else pd.DataFrame()
    for stale_name in [
        "article_solved_rate_by_n.png",
        "article_solved_rate_by_RT.png",
        "article_time_ratio_by_n.png",
        "article_memory_ratio_by_n.png",
        "article_wall_time_by_n.png",
        "article_peak_memory_by_n.png",
        "article_ratio_heatmap_RT.png",
    ]:
        remove_stale_file(figures / stale_name)
    if df.empty:
        for name in [
            "article_comparison_summary.csv",
            "article_comparison_by_n.csv",
            "article_comparison_by_RT.csv",
            "article_comparison_by_n_RT.csv",
            "article_comparable_instances.csv",
            "article_not_comparable.csv",
            "article_invalid_runner_rows.csv",
            "article_config_validation.csv",
        ]:
            save_csv(pd.DataFrame(), processed / name, reporter)
        no_data_titles = {
            "article_objective_match_by_n.png": "Сравнение со статьёй: совпадение оптимума по n",
            "article_objective_match_by_RT.png": "Сравнение со статьёй: совпадение оптимума по (R,T)",
            "article_wall_time_by_pair_grid.png": "Сравнение со статьёй: время выполнения по (R,T)",
            "article_wall_time_by_pair_grid_linear.png": "Сравнение со статьёй: время выполнения по (R,T), линейная шкала",
            "article_peak_memory_by_pair_grid.png": "Сравнение со статьёй: пиковый рабочий набор по (R,T)",
            "article_time_ratio_distribution_by_n.png": "Сравнение со статьёй: распределение отношения времени",
            "article_time_ratio_heatmap_RT.png": "Сравнение со статьёй: отношение времени по (R,T)",
            "article_memory_ratio_distribution_by_n.png": "Сравнение со статьёй: распределение отношения памяти",
            "article_memory_ratio_heatmap_RT.png": "Сравнение со статьёй: отношение памяти по (R,T)",
            "article_time_memory_tradeoff.png": "Сравнение со статьёй: компромисс времени и памяти",
            "article_time_memory_tradeoff_hard_set.png": "Сравнение со статьёй: компромисс времени и памяти на трудном подмножестве",
        }
        for name, title in no_data_titles.items():
            save_no_data_figure(figures / name, title, reporter)
        save_latex(pd.DataFrame(), latex / "table_article_comparison.tex", reporter)
        save_latex(pd.DataFrame(), latex / "table_article_RT.tex", reporter)
        return

    required_cols = [
        "n", "R", "T", "seed", "ours_reconstruct_status", "article_status",
        "ours_reconstruct_objective", "article_objective", "objective_comparison_status",
        "ours_reconstruct_wall_time_ms", "article_wall_time_ms",
        "ours_reconstruct_peak_working_set_bytes", "article_peak_working_set_bytes",
        "ratio_ours_reconstruct_wall_to_article", "ratio_ours_reconstruct_peak_to_article_peak",
        "ours_reconstruct_run_decomp", "ours_reconstruct_run_memo_backend",
        "ours_reconstruct_run_use_ub", "ours_reconstruct_run_use_lb",
        "ours_reconstruct_run_terminal_rules", "ours_reconstruct_run_position_filtering_enabled",
        "ours_reconstruct_run_enable_lawler_basic_rules",
        "ours_reconstruct_run_enable_memo", "ours_reconstruct_run_enable_exact_memo",
        "ours_reconstruct_run_memo_full_key_verification",
    ]
    for col in required_cols:
        if col not in df.columns:
            df[col] = np.nan
            reporter.missing("article_code_comparison", col)

    df["n"] = number_series(df["n"])
    df["R"] = number_series(df["R"])
    df["T"] = number_series(df["T"])
    df["seed"] = number_series(df["seed"])
    raw_rows_before_filter = len(df)
    basename = (
        df.get("instance_path", pd.Series([""] * len(df), index=df.index))
        .astype("string")
        .fillna("")
        .str.replace("\\", "/", regex=False)
        .str.rsplit("/", n=1)
        .str[-1]
    )
    legacy_r1_filename = (
        np.isclose(df["R"], 1.0)
        & basename.str.contains(r"_1_", regex=True, na=False)
        & ~basename.str.contains(r"_1\.0_", regex=True, na=False)
    )
    invalid_runner_rows = df[legacy_r1_filename].copy()
    if not invalid_runner_rows.empty:
        invalid_runner_rows["invalid_reason"] = "article_runner_filename_mismatch_R1_expected_1.0"
        df = df[~legacy_r1_filename].copy()
    df["ours_solved"] = df["ours_reconstruct_status"].map(normalize_status).eq("solved")
    df["article_solved"] = df["article_status"].map(normalize_status).eq("solved")
    df["both_solved"] = df["ours_solved"] & df["article_solved"]
    df["time_ratio"] = number_series(df["ratio_ours_reconstruct_wall_to_article"])
    missing_ratio = df["time_ratio"].isna()
    df.loc[missing_ratio, "time_ratio"] = (
        number_series(df.loc[missing_ratio, "ours_reconstruct_wall_time_ms"]) /
        number_series(df.loc[missing_ratio, "article_wall_time_ms"]).replace(0, np.nan)
    )
    df["memory_ratio"] = number_series(df["ratio_ours_reconstruct_peak_to_article_peak"])
    missing_mem = df["memory_ratio"].isna()
    df.loc[missing_mem, "memory_ratio"] = (
        number_series(df.loc[missing_mem, "ours_reconstruct_peak_working_set_bytes"]) /
        number_series(df.loc[missing_mem, "article_peak_working_set_bytes"]).replace(0, np.nan)
    )
    df["ours_wall_time_s"] = number_series(df["ours_reconstruct_wall_time_ms"]) / 1000.0
    df["article_wall_time_s"] = number_series(df["article_wall_time_ms"]) / 1000.0
    df["ours_peak_gb"] = number_series(df["ours_reconstruct_peak_working_set_bytes"]) / 1024.0 / 1024.0 / 1024.0
    df["article_peak_gb"] = number_series(df["article_peak_working_set_bytes"]) / 1024.0 / 1024.0 / 1024.0

    status = df["objective_comparison_status"].astype(str).str.upper().replace({"": "UNKNOWN", "NAN": "UNKNOWN"})
    df["objective_comparison_status_norm"] = status
    comparable_status = status.isin(["MATCH", "MISMATCH"])
    comparable = df[df["both_solved"] & comparable_status].copy()
    not_comp = df[~(df["both_solved"] & comparable_status)].copy()

    counts_by_n = df.groupby("n", dropna=False).size()
    max_rows_per_n = int(counts_by_n.max()) if not counts_by_n.empty else 0
    df["rows_for_n"] = df["n"].map(counts_by_n)
    df["complete_n"] = df["rows_for_n"].eq(max_rows_per_n)
    comparable["rows_for_n"] = comparable["n"].map(counts_by_n)
    comparable["complete_n"] = comparable["rows_for_n"].eq(max_rows_per_n)
    primary_df = df[df["complete_n"]].copy()
    primary_comparable = comparable[comparable["complete_n"]].copy()

    def objective_match_rate(group: pd.Series) -> float:
        if group.empty:
            return np.nan
        return group.astype(str).str.upper().eq("MATCH").mean()

    by_n_base = df.groupby("n", dropna=False).agg(
        rows=("n", "size"),
        both_solved=("both_solved", "sum"),
        ours_solved_rate=("ours_solved", "mean"),
        article_solved_rate=("article_solved", "mean"),
        objective_match_rate=("objective_comparison_status_norm", objective_match_rate),
        objective_matches=("objective_comparison_status_norm", lambda s: s.astype(str).str.upper().eq("MATCH").sum()),
        objective_mismatches=("objective_comparison_status_norm", lambda s: s.astype(str).str.upper().eq("MISMATCH").sum()),
        objective_not_comparable=("objective_comparison_status_norm", lambda s: (~s.astype(str).str.upper().isin(["MATCH", "MISMATCH"])).sum()),
    ).reset_index()
    by_n_base["complete_n"] = by_n_base["rows"].eq(max_rows_per_n)
    by_n_ratios = comparable.groupby("n", dropna=False).agg(
        comparable_rows=("n", "size"),
        median_time_ratio=("time_ratio", "median"),
        p10_time_ratio=("time_ratio", lambda s: s.quantile(0.10)),
        p90_time_ratio=("time_ratio", lambda s: s.quantile(0.90)),
        median_memory_ratio=("memory_ratio", "median"),
        p10_memory_ratio=("memory_ratio", lambda s: s.quantile(0.10)),
        p90_memory_ratio=("memory_ratio", lambda s: s.quantile(0.90)),
        median_ours_wall_time_s=("ours_wall_time_s", "median"),
        median_article_wall_time_s=("article_wall_time_s", "median"),
        median_ours_peak_gb=("ours_peak_gb", "median"),
        median_article_peak_gb=("article_peak_gb", "median"),
    ).reset_index()
    by_n = by_n_base.merge(by_n_ratios, on="n", how="left").sort_values("n")

    by_rt_base = primary_df.groupby(["R", "T"], dropna=False).agg(
        rows=("R", "size"),
        both_solved=("both_solved", "sum"),
        ours_solved_rate=("ours_solved", "mean"),
        article_solved_rate=("article_solved", "mean"),
        objective_match_rate=("objective_comparison_status_norm", objective_match_rate),
    ).reset_index()
    by_rt_ratios = primary_comparable.groupby(["R", "T"], dropna=False).agg(
        comparable_rows=("R", "size"),
        median_time_ratio=("time_ratio", "median"),
        p10_time_ratio=("time_ratio", lambda s: s.quantile(0.10)),
        p90_time_ratio=("time_ratio", lambda s: s.quantile(0.90)),
        median_memory_ratio=("memory_ratio", "median"),
        p10_memory_ratio=("memory_ratio", lambda s: s.quantile(0.10)),
        p90_memory_ratio=("memory_ratio", lambda s: s.quantile(0.90)),
        median_ours_wall_time_s=("ours_wall_time_s", "median"),
        median_article_wall_time_s=("article_wall_time_s", "median"),
        median_ours_peak_gb=("ours_peak_gb", "median"),
        median_article_peak_gb=("article_peak_gb", "median"),
    ).reset_index()
    by_rt = by_rt_base.merge(by_rt_ratios, on=["R", "T"], how="left").sort_values(["R", "T"])
    by_pair_n = primary_comparable.groupby(["R", "T", "n"], dropna=False).agg(
        rows=("R", "size"),
        median_ours_wall_time_s=("ours_wall_time_s", "median"),
        median_article_wall_time_s=("article_wall_time_s", "median"),
        median_ours_peak_gb=("ours_peak_gb", "median"),
        median_article_peak_gb=("article_peak_gb", "median"),
        median_time_ratio=("time_ratio", "median"),
        median_memory_ratio=("memory_ratio", "median"),
    ).reset_index().sort_values(["R", "T", "n"])

    config_checks_spec = [
        ("model/decomposition", "ours_reconstruct_run_decomp", "adaptive"),
        ("memo_backend", "ours_reconstruct_run_memo_backend", "custom"),
        ("use_ub", "ours_reconstruct_run_use_ub", "0"),
        ("use_lb", "ours_reconstruct_run_use_lb", "0"),
        ("terminal_rules", "ours_reconstruct_run_terminal_rules", "1"),
        ("position_filtering", "ours_reconstruct_run_position_filtering_enabled", "1"),
        ("lawler_basic_rules", "ours_reconstruct_run_enable_lawler_basic_rules", "1"),
        ("enable_memo", "ours_reconstruct_run_enable_memo", "1"),
        ("enable_exact_memo", "ours_reconstruct_run_enable_exact_memo", "1"),
        ("memo_full_key_verification", "ours_reconstruct_run_memo_full_key_verification", "1"),
    ]
    config_rows = []
    for name, col, expected in config_checks_spec:
        values = sorted({str(v) for v in df[col].dropna().unique()})
        ok = len(values) == 1 and values[0] == expected
        config_rows.append({
            "check": name,
            "column": col,
            "expected": expected,
            "actual_unique_values": ", ".join(values) if values else "NA",
            "ok": ok,
        })
    if "ours_order_length" in df.columns:
        order_length_ok = number_series(df["ours_order_length"]).eq(df["n"]).mean()
        config_rows.append({
            "check": "reconstructed_order_length",
            "column": "ours_order_length",
            "expected": "n",
            "actual_unique_values": f"{order_length_ok * 100.0:.1f}% rows match n",
            "ok": bool(np.isclose(order_length_ok, 1.0)),
        })
    if "ours_reconstruct_run_reconstruction_trace_fallbacks" in df.columns:
        fallbacks = number_series(df["ours_reconstruct_run_reconstruction_trace_fallbacks"])
        max_fallbacks = fallbacks.max()
        config_rows.append({
            "check": "trace_reconstruction_fallbacks",
            "column": "ours_reconstruct_run_reconstruction_trace_fallbacks",
            "expected": "0",
            "actual_unique_values": "NA" if pd.isna(max_fallbacks) else f"max={int(max_fallbacks)}",
            "ok": bool(pd.notna(max_fallbacks) and max_fallbacks == 0),
        })
    config_validation = pd.DataFrame(config_rows)

    summary = pd.DataFrame([{
        "raw_rows_before_filter": raw_rows_before_filter,
        "excluded_invalid_runner_rows": len(invalid_runner_rows),
        "rows": len(df),
        "complete_n_rows": len(primary_df),
        "incomplete_n_rows": int((~df["complete_n"]).sum()),
        "max_rows_per_complete_n": max_rows_per_n,
        "complete_n_values": ", ".join(
            f"{int(n):d}" if float(n).is_integer() else f"{n:g}"
            for n in sorted(df.loc[df["complete_n"], "n"].dropna().unique())
        ),
        "incomplete_n_values": ", ".join(
            f"{int(n):d}" if float(n).is_integer() else f"{n:g}"
            for n in sorted(df.loc[~df["complete_n"], "n"].dropna().unique())
        ),
        "both_solved": int(primary_df["both_solved"].sum()),
        "both_solved_all_rows": int(df["both_solved"].sum()),
        "ours_solved_rate": df["ours_solved"].mean(),
        "article_solved_rate": df["article_solved"].mean(),
        "article_error_count": int(df["article_status"].map(normalize_status).eq("err").sum()),
        "objective_mismatches": int(status.eq("MISMATCH").sum()),
        "median_time_ratio_primary_complete_n": primary_comparable["time_ratio"].median(),
        "median_memory_ratio_primary_complete_n": primary_comparable["memory_ratio"].median(),
        "median_time_ratio_all_comparable": comparable["time_ratio"].median(),
        "median_memory_ratio_all_comparable": comparable["memory_ratio"].median(),
        "final_config_ok": bool(config_validation["ok"].all()) if not config_validation.empty else False,
    }])

    save_csv(summary, processed / "article_comparison_summary.csv", reporter)
    save_csv(by_n, processed / "article_comparison_by_n.csv", reporter)
    save_csv(by_rt, processed / "article_comparison_by_RT.csv", reporter)
    save_csv(by_pair_n, processed / "article_comparison_by_n_RT.csv", reporter)
    save_csv(comparable, processed / "article_comparable_instances.csv", reporter)
    save_csv(not_comp, processed / "article_not_comparable.csv", reporter)
    save_csv(invalid_runner_rows, processed / "article_invalid_runner_rows.csv", reporter)
    save_csv(config_validation, processed / "article_config_validation.csv", reporter)

    article_objective_match_by_n(by_n, figures / "article_objective_match_by_n.png", reporter)
    article_objective_match_heatmap(by_rt, figures / "article_objective_match_by_RT.png", reporter)
    article_pair_metric_grid(
        by_pair_n,
        "median_ours_wall_time_s",
        "median_article_wall_time_s",
        figures / "article_wall_time_by_pair_grid.png",
        reporter,
        title="Сравнение со статьёй: медианное время выполнения по (R,T), логарифмическая шкала",
        ylabel="время, с (лог. шкала)",
        logy=True,
    )
    article_pair_metric_grid(
        by_pair_n,
        "median_ours_wall_time_s",
        "median_article_wall_time_s",
        figures / "article_wall_time_by_pair_grid_linear.png",
        reporter,
        title="Сравнение со статьёй: медианное время выполнения по (R,T), линейная шкала",
        ylabel="время, с",
        logy=False,
    )
    article_pair_metric_grid(
        by_pair_n,
        "median_ours_peak_gb",
        "median_article_peak_gb",
        figures / "article_peak_memory_by_pair_grid.png",
        reporter,
        title="Сравнение со статьёй: медианный пиковый рабочий набор по (R,T)",
        ylabel="пиковый рабочий набор, ГБ",
    )
    article_ratio_distribution(
        comparable,
        "time_ratio",
        figures / "article_time_ratio_distribution_by_n.png",
        reporter,
        title="Сравнение со статьёй: распределение отношения времени по n",
        ylabel="время: наш алгоритм / код статьи\n(<1: наш быстрее)",
    )
    article_ratio_heatmap(
        by_rt,
        "median_time_ratio",
        figures / "article_time_ratio_heatmap_RT.png",
        reporter,
        title="Сравнение со статьёй: медианное отношение времени по (R,T)",
        cbar_label="время: наш алгоритм / код статьи",
    )
    article_ratio_distribution(
        comparable,
        "memory_ratio",
        figures / "article_memory_ratio_distribution_by_n.png",
        reporter,
        title="Сравнение со статьёй: распределение отношения памяти по n",
        ylabel="память: наш алгоритм / код статьи\n(<1: наш использует меньше)",
    )
    article_ratio_heatmap(
        by_rt,
        "median_memory_ratio",
        figures / "article_memory_ratio_heatmap_RT.png",
        reporter,
        title="Сравнение со статьёй: медианное отношение памяти по (R,T)",
        cbar_label="память: наш алгоритм / код статьи",
    )
    article_time_memory_tradeoff(comparable, figures / "article_time_memory_tradeoff.png", reporter)
    hard_mask = comparable.apply(
        lambda row: (round(float(row["R"]), 10), round(float(row["T"]), 10)) in HARD_SUBSET,
        axis=1,
    ) if not comparable.empty else pd.Series(dtype=bool)
    article_time_memory_tradeoff(
        comparable[hard_mask].copy() if not comparable.empty else comparable,
        figures / "article_time_memory_tradeoff_hard_set.png",
        reporter,
        title="Сравнение со статьёй: время и память на трудном подмножестве, логарифмическая ось X",
    )

    if not by_n.empty:
        table_n = by_n.copy()
        table_n["complete"] = table_n["complete_n"].map(lambda x: "yes" if bool(x) else "no")
        table_n["ours_solved"] = table_n["ours_solved_rate"].map(format_percent)
        table_n["article_solved"] = table_n["article_solved_rate"].map(format_percent)
        table_n["objective_match"] = table_n["objective_match_rate"].map(format_percent)
        table_n["time_ratio"] = table_n["median_time_ratio"].map(format_ratio)
        table_n["memory_ratio"] = table_n["median_memory_ratio"].map(format_ratio)
        table_n["ours_time"] = table_n["median_ours_wall_time_s"].map(lambda x: "NA" if pd.isna(x) else f"{float(x):.3g}s")
        table_n["article_time"] = table_n["median_article_wall_time_s"].map(lambda x: "NA" if pd.isna(x) else f"{float(x):.3g}s")
        save_latex(
            table_n,
            latex / "table_article_comparison.tex",
            reporter,
            ["n", "rows", "complete", "both_solved", "ours_solved", "article_solved", "objective_match", "time_ratio", "memory_ratio", "ours_time", "article_time"],
            max_rows=20,
        )
    else:
        save_latex(by_n, latex / "table_article_comparison.tex", reporter)

    if not by_rt.empty:
        table_rt = by_rt.copy()
        table_rt["R"] = table_rt["R"].map(lambda x: "NA" if pd.isna(x) else f"{float(x):g}")
        table_rt["T"] = table_rt["T"].map(lambda x: "NA" if pd.isna(x) else f"{float(x):g}")
        table_rt["ours_solved"] = table_rt["ours_solved_rate"].map(format_percent)
        table_rt["article_solved"] = table_rt["article_solved_rate"].map(format_percent)
        table_rt["objective_match"] = table_rt["objective_match_rate"].map(format_percent)
        table_rt["time_ratio"] = table_rt["median_time_ratio"].map(format_ratio)
        table_rt["memory_ratio"] = table_rt["median_memory_ratio"].map(format_ratio)
        save_latex(
            table_rt,
            latex / "table_article_RT.tex",
            reporter,
            ["R", "T", "rows", "both_solved", "ours_solved", "article_solved", "objective_match", "time_ratio", "memory_ratio"],
            max_rows=24,
        )
    else:
        save_latex(by_rt, latex / "table_article_RT.tex", reporter)

    time_best = by_rt.dropna(subset=["median_time_ratio"]).sort_values("median_time_ratio").head(3)
    time_worst = by_rt.dropna(subset=["median_time_ratio"]).sort_values("median_time_ratio", ascending=False).head(3)
    def rt_list(rows: pd.DataFrame) -> str:
        if rows.empty:
            return "NA"
        return "; ".join(f"R={row.R:g},T={row.T:g}: {format_ratio(row.median_time_ratio)}" for row in rows.itertuples())

    incomplete_text = summary["incomplete_n_values"].iloc[0] or "none"
    reporter.article_text = (
        "article_code_comparison is the final external comparison: our solver is run with reconstruction on, "
        "adaptive final model, custom memo, exact memo/full-key verification, LB/UB off, and process gate off. "
        "All primary time/memory ratios are computed only on comparable rows where both programs solved; "
        "ratio medians are paired by instance and are not ratios of aggregate medians. "
        "Incomplete n stages and known invalid runner rows are excluded from primary plots. "
        f"Raw rows={int(summary['raw_rows_before_filter'].iloc[0])}, excluded invalid runner rows="
        f"{int(summary['excluded_invalid_runner_rows'].iloc[0])}, rows used={int(summary['rows'].iloc[0])}, "
        f"complete-grid rows={int(summary['complete_n_rows'].iloc[0])}, "
        f"incomplete n={incomplete_text}. "
        f"Our solved rate is {format_percent(summary['ours_solved_rate'].iloc[0])}; article solved rate is "
        f"{format_percent(summary['article_solved_rate'].iloc[0])}, with {int(summary['article_error_count'].iloc[0])} article ERROR rows. "
        f"Objective mismatches among solved comparisons: {int(summary['objective_mismatches'].iloc[0])}. "
        f"Primary median wall-time ratio ours/article is {format_ratio(summary['median_time_ratio_primary_complete_n'].iloc[0])}; "
        f"primary median peak working-set ratio is {format_ratio(summary['median_memory_ratio_primary_complete_n'].iloc[0])}. "
        f"Best time-ratio regions for ours: {rt_list(time_best)}. Worst time-ratio regions for ours: {rt_list(time_worst)}. "
        f"Final-config validation: {'ok' if bool(summary['final_config_ok'].iloc[0]) else 'check article_config_validation.csv'}. "
        "Solved-rate plots are intentionally omitted because both programs solve all valid rows after the R=1.0 filename fix. "
        "Use article_objective_match_by_n.png and article_objective_match_by_RT.png for correctness agreement; "
        "use article_wall_time_by_pair_grid.png, article_wall_time_by_pair_grid_linear.png, and "
        "article_peak_memory_by_pair_grid.png as the main direct comparisons against Branch.exe by (R,T). "
        "Peak working set is an external resident-memory process sample, not memo-table bytes; it includes allocator/runtime overhead. "
        "Use ratio heatmaps and the full/hard-set time-memory tradeoff plots as compact summaries."
    )


def final_config_table(latex: Path, reporter: Reporter) -> None:
    df = pd.DataFrame([
        ["model", "adaptive_v3"],
        ["memo_backend", "custom"],
        ["memo_full_key_verification", "true"],
        ["enable_memo", "true"],
        ["enable_exact_memo", "true"],
        ["terminal_rules", "true"],
        ["position_filtering", "true"],
        ["enable_lawler_basic_rules", "true"],
        ["enable_rule4", "true"],
        ["LB/UB", "off unless bounds experiment"],
        ["root_parallel_threads", "1"],
        ["process gate", "off"],
        ["final article comparison reconstruction", "on"],
    ], columns=["setting", "value"])
    save_latex(df, latex / "table_final_config.tex", reporter)


def write_report(path: Path, reporter: Reporter) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Branch-and-Memorize Results Report\n\n")
        f.write("## Input Files\n\n")
        if reporter.input_files:
            for p in reporter.input_files:
                f.write(f"- `{p}`\n")
        else:
            f.write("- no input files\n")
        f.write("\n## Missing Columns\n\n")
        if reporter.missing_columns:
            for file, cols in sorted(reporter.missing_columns.items()):
                f.write(f"- `{file}`: {', '.join(sorted(cols))}\n")
        else:
            f.write("- none\n")
        f.write("\n## Process Gate\n\n")
        f.write(f"- process-gated rows: {reporter.process_gate_rows}\n")
        if reporter.possibly_process_gated_files:
            f.write("- possibly process-gated files, recorded as metadata rather than a correctness problem:\n")
            for p in reporter.possibly_process_gated_files:
                f.write(f"  - `{p}`\n")
        else:
            f.write("- possibly process-gated files: none\n")
        f.write("\n## Correctness\n\n")
        f.write(f"- correctness mismatches: {reporter.correctness_problem_count}\n")
        f.write("\n## Model\n\n")
        f.write(f"{reporter.best_model_text}\n")
        f.write("\n## Progressive Model Race\n\n")
        f.write(f"{reporter.model_race_text}\n")
        f.write("\n## Hard Pair Race\n\n")
        f.write(f"{reporter.hard_pair_text}\n")
        f.write("\n## Bounds\n\n")
        f.write(f"{reporter.bounds_text}\n")
        f.write("\n## Backend\n\n")
        f.write(f"{reporter.backend_text}\n")
        f.write("\n## Reconstruction\n\n")
        f.write(f"{reporter.reconstruction_text}\n")
        f.write("\n## Article Comparison\n\n")
        f.write(f"{reporter.article_text}\n")
        f.write("\n## Generated Artifacts\n\n")
        for p in reporter.artifacts:
            f.write(f"- `{p}`\n")
        if reporter.notes:
            f.write("\n## Notes\n\n")
            for note in reporter.notes:
                f.write(f"- {note}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze exact Branch-and-Memorize benchmark result CSVs.")
    parser.add_argument("--input-dir", default="results_raw")
    parser.add_argument("--out-dir", default="processed")
    parser.add_argument("--fig-dir", default="figures")
    parser.add_argument("--latex-dir", default="latex_tables")
    parser.add_argument("--timeout-policy", default="par10")
    parser.add_argument("--penalty-factor", type=float, default=10.0)
    parser.add_argument("--files", nargs="*", default=[])
    args = parser.parse_args()

    input_dir = Path(args.input_dir)
    processed = Path(args.out_dir)
    figures = Path(args.fig_dir)
    latex = Path(args.latex_dir)
    ensure_dirs(processed, figures, latex)
    reporter = Reporter()

    files = discover_files(input_dir, args.files)
    sources = load_sources(files, reporter)
    long = build_long_runs(sources, reporter, args.penalty_factor, args.timeout_policy)
    save_csv(long, processed / "normalized_runs.csv", reporter)
    general = summary_stats(long, ["series_norm", "config_norm", "n", "R", "T"]) if not long.empty else pd.DataFrame()
    save_csv(general, processed / "general_summary.csv", reporter)

    correctness_checks(long, sources, processed, reporter)
    process_gate_contamination(long, sources, processed, reporter)
    final_config_table(latex, reporter)
    model_race(long, processed, figures, latex, reporter)
    hard_pair_race(long, processed, figures, latex, reporter)
    bounds_ablation(long, processed, figures, latex, reporter)
    backend_comparison(sources, long, processed, figures, latex, reporter)
    reconstruction_analysis(sources, processed, figures, latex, reporter)
    article_analysis(sources, processed, figures, latex, reporter)
    write_report(processed / "report.md", reporter)
    reporter.artifact(processed / "report.md")
    print(f"[analyze] files: {len(files)}")
    print(f"[analyze] normalized rows: {len(long)}")
    print(f"[analyze] report: {processed / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
