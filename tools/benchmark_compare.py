import argparse
import csv
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import ctypes
import ctypes.wintypes as wintypes
import datetime as dt
import json
import re
import signal
import shutil
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
import statistics
import threading
import math
import random
from typing import Dict, Iterable, List, Optional, Tuple

STOP_REQUESTED = threading.Event()
STOP_SIGNAL: Optional[int] = None
STOP_ANNOUNCED = False


def on_interrupt_signal(sig, _frame):
    global STOP_SIGNAL
    STOP_SIGNAL = int(sig)
    STOP_REQUESTED.set()


def announce_stop_request():
    global STOP_ANNOUNCED
    if STOP_REQUESTED.is_set() and not STOP_ANNOUNCED:
        STOP_ANNOUNCED = True
        reason = f"signal {STOP_SIGNAL}" if STOP_SIGNAL is not None else "external request"
        print(f"stop requested: {reason}")


CASE_RE = re.compile(
    r"^SDT_(?P<n>\d+)_(?P<r>\d\.\d)_(?P<t>\d\.\d)_(?P<k>\d+)\.txt$",
    re.IGNORECASE,
)
MINE_COST_RE = re.compile(r"\bcost=(\d+)\b")
MINE_TIME_RE = re.compile(r"\btime_ms=([0-9.+-eE]+)\b")
MINE_MEMO_USED_MB_RE = re.compile(r"\bmemo_used_mb=([0-9.+-eE]+)\b")
MINE_MEMO_PEAK_ENTRIES_RE = re.compile(r"\bmemo_peak=(\d+)\b")
MINE_NODES_RE = re.compile(r"\bnodes=(\d+)\b")
BRANCH_TT_RE = re.compile(r"TT=\s*(\d+)")
BRANCH_TIME_RE = re.compile(r"Solved in\s+(\d+)\s*/\s*(\d+)s", re.IGNORECASE)
BRANCH_CPU_MS_RE = re.compile(r"\bCPUms=([0-9.+-eE]+)\b")
BRANCH_WALL_MS_RE = re.compile(r"\bWALLms=([0-9.+-eE]+)\b")
BRANCH_RAM_RE = re.compile(r"\bRAM=(\d+)\b")
BRANCH_NB_BYTES_MEM_RE = re.compile(r"#NbBytesMem=(\d+)\b")
BRANCH_NB_ENTRY_MEM_RE = re.compile(r"NbEntryMem=(\d+)\b")

R_INDEX = {"0.2": 1, "0.4": 2, "0.6": 3, "0.8": 4, "1.0": 5}
T_INDEX = {"0.2": 1, "0.4": 2, "0.6": 3, "0.8": 4}
DEFAULT_R_GRID = ["0.2", "0.4", "0.6", "0.8", "1.0"]
DEFAULT_T_GRID = ["0.2", "0.4", "0.6", "0.8"]
DEFAULT_K_GRID = list(range(1, 11))
DEFAULT_ARTICLE_TIMEOUT_SEC = 7.5 * 3600.0
DEFAULT_ARTICLE_MEMORY_MB = 8 * 1024
DEFAULT_SCALING_N_FROM = 100
DEFAULT_SCALING_N_TO = 1500
DEFAULT_SCALING_N_STEP = 100
EXPERIMENT_TS_FMT = "%Y%m%d-%H%M%S"


@dataclass(frozen=True)
class Case:
    path: Path
    n: int
    r: str
    t: str
    k: int

    @property
    def name(self) -> str:
        return self.path.name

    @property
    def branch_id(self) -> int:
        return (R_INDEX[self.r] - 1) * 40 + (T_INDEX[self.t] - 1) * 10 + self.k


@dataclass
class ExperimentPaths:
    root: Path
    detail_csv: Path
    case_summary_csv: Path
    grid_summary_csv: Path
    frontier_csv: Path
    manifest_json: Path
    selected_cases_csv: Path
    generated_cases_csv: Path


@dataclass
class RuntimeLimits:
    timeout_sec: float
    memory_limit_mb: Optional[int]


def parse_case(path: Path) -> Optional[Case]:
    m = CASE_RE.match(path.name)
    if not m:
        return None
    return Case(
        path=path,
        n=int(m.group("n")),
        r=m.group("r"),
        t=m.group("t"),
        k=int(m.group("k")),
    )


def parse_csv_set(raw: Optional[str], cast):
    if raw is None or raw == "":
        return None
    values = set()
    for part in raw.replace(";", ",").split(","):
        part = part.strip()
        if not part:
            continue
        values.add(cast(part))
    return values


def detect_article_repo_root(repo_root: Path) -> Path:
    candidates = [
        repo_root / "1-dj-Sum-Tj",
        repo_root / "1-dj-Sum-Tj-master",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def discover_cases(
    data_root: Path,
    n_filter=None,
    r_filter=None,
    t_filter=None,
    k_filter=None,
) -> List[Case]:
    cases: List[Case] = []
    for p in data_root.rglob("SDT_*.txt"):
        c = parse_case(p)
        if c is None:
            continue
        if n_filter is not None and c.n not in n_filter:
            continue
        if r_filter is not None and c.r not in r_filter:
            continue
        if t_filter is not None and c.t not in t_filter:
            continue
        if k_filter is not None and c.k not in k_filter:
            continue
        cases.append(c)
    cases.sort(key=lambda x: (x.n, float(x.r), float(x.t), x.k))
    return cases


def set_default_arg(args, name: str, value) -> None:
    if getattr(args, name) in (None, "", False):
        setattr(args, name, value)


def normalize_filters_from_args(args):
    n_filter = parse_csv_set(args.n_list, int)
    n_range_filter = parse_n_range(args.n_from, args.n_to, args.n_step)
    if n_filter is None:
        n_filter = n_range_filter
    r_filter = parse_csv_set(args.r_list, str)
    t_filter = parse_csv_set(args.t_list, str)
    k_filter = parse_csv_set(args.k_list, int)
    return n_filter, r_filter, t_filter, k_filter


def apply_preset_defaults(args, repo_root: Path, article_root: Path) -> None:
    preset = args.preset
    if preset is None and len(sys.argv) == 1:
        preset = "full-article-compare"
        args.preset = preset
    if preset is None:
        return

    if preset == "official-full-article":
        args.check_branch = True
        if args.timeout_sec == 120.0:
            args.timeout_sec = DEFAULT_ARTICLE_TIMEOUT_SEC
        if args.branch_timeout_sec == 120.0:
            args.branch_timeout_sec = DEFAULT_ARTICLE_TIMEOUT_SEC
        if args.memory_limit_mb is None:
            args.memory_limit_mb = DEFAULT_ARTICLE_MEMORY_MB
        args.jobs = max(1, args.jobs)
        if not args.mine_arg:
            args.mine_arg = ["--no-profiling"]
        return

    if preset == "generated-scaling-article":
        args.check_branch = True
        args.no_sol_compare = True
        args.generate_missing = True
        if args.timeout_sec == 120.0:
            args.timeout_sec = DEFAULT_ARTICLE_TIMEOUT_SEC
        if args.branch_timeout_sec == 120.0:
            args.branch_timeout_sec = DEFAULT_ARTICLE_TIMEOUT_SEC
        if args.memory_limit_mb is None:
            args.memory_limit_mb = DEFAULT_ARTICLE_MEMORY_MB
        if args.n_from is None:
            args.n_from = DEFAULT_SCALING_N_FROM
        if args.n_to is None:
            args.n_to = DEFAULT_SCALING_N_TO
        if args.n_step <= 0:
            args.n_step = DEFAULT_SCALING_N_STEP
        if args.generated_data_root is None:
            args.generated_data_root = repo_root / "compare_out" / "generated_scaling_data"
        args.data_root = args.generated_data_root
        if not args.mine_arg:
            args.mine_arg = ["--no-profiling"]
        return

    if preset == "full-article-compare":
        # handled as a suite in main()
        return

    raise ValueError(f"unknown preset: {preset}")


def resolve_repeat_counts(args) -> Tuple[int, int]:
    base = max(1, int(args.repeats))
    repeat_mine = max(1, int(args.repeat_mine)) if args.repeat_mine is not None else base
    repeat_branch = max(1, int(args.repeat_branch)) if args.repeat_branch is not None else base
    return repeat_mine, repeat_branch


def resolve_limit_pair(args) -> Tuple[RuntimeLimits, RuntimeLimits]:
    common_memory = args.memory_limit_mb
    mine_memory = args.mine_memory_limit_mb if args.mine_memory_limit_mb is not None else common_memory
    branch_memory = args.branch_memory_limit_mb if args.branch_memory_limit_mb is not None else common_memory
    return (
        RuntimeLimits(timeout_sec=float(args.timeout_sec), memory_limit_mb=mine_memory),
        RuntimeLimits(timeout_sec=float(args.branch_timeout_sec), memory_limit_mb=branch_memory),
    )


def resolve_output_paths(args, default_name: str) -> ExperimentPaths:
    if args.output_dir is not None:
        root = args.output_dir
    else:
        output_root = Path(__file__).resolve().parents[1] / "compare_out" / "experiments"
        root = output_root / make_experiment_name(default_name)
    paths = ensure_experiment_paths(root)
    if args.csv_out is not None:
        paths.detail_csv = args.csv_out
    if args.case_summary_csv is not None:
        paths.case_summary_csv = args.case_summary_csv
    if args.summary_csv is not None:
        paths.grid_summary_csv = args.summary_csv
    if args.frontier_csv is not None:
        paths.frontier_csv = args.frontier_csv
    return paths


def load_sol_map(sol_zip: Path, n_values: Iterable[int]) -> Dict[str, int]:
    needed = {int(n) for n in n_values}
    out: Dict[str, int] = {}
    with zipfile.ZipFile(sol_zip) as zf:
        name_map = {name: name for name in zf.namelist()}
        for n in sorted(needed):
            suffix = f"/sol/sol_{n}.txt"
            member = None
            for candidate in name_map:
                if candidate.endswith(suffix):
                    member = candidate
                    break
            if member is None:
                continue
            data = zf.read(member).decode("utf-8", errors="replace")
            for line in data.splitlines():
                line = line.strip()
                if not line:
                    continue
                parts = re.split(r"\s+", line)
                if len(parts) < 2:
                    continue
                name = parts[0]
                try:
                    tt = int(parts[1])
                except ValueError:
                    continue
                if not name.lower().endswith(".txt"):
                    name += ".txt"
                out[name] = tt
    return out


if sys.platform.startswith("win"):
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    JobObjectExtendedLimitInformation = 9

    class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
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
            ("PrivateUsage", ctypes.c_size_t),
        ]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_uint64),
            ("WriteOperationCount", ctypes.c_uint64),
            ("OtherOperationCount", ctypes.c_uint64),
            ("ReadTransferCount", ctypes.c_uint64),
            ("WriteTransferCount", ctypes.c_uint64),
            ("OtherTransferCount", ctypes.c_uint64),
        ]

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_int64),
            ("PerJobUserTimeLimit", ctypes.c_int64),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ("IoInfo", IO_COUNTERS),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    _OpenProcess = ctypes.windll.kernel32.OpenProcess
    _CloseHandle = ctypes.windll.kernel32.CloseHandle
    _GetProcessMemoryInfo = ctypes.windll.psapi.GetProcessMemoryInfo
    _CreateJobObjectW = ctypes.windll.kernel32.CreateJobObjectW
    _SetInformationJobObject = ctypes.windll.kernel32.SetInformationJobObject
    _AssignProcessToJobObject = ctypes.windll.kernel32.AssignProcessToJobObject
    _OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _OpenProcess.restype = wintypes.HANDLE
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL
    _GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    _GetProcessMemoryInfo.restype = wintypes.BOOL
    _CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _CreateJobObjectW.restype = wintypes.HANDLE
    _SetInformationJobObject.argtypes = [wintypes.HANDLE, wintypes.INT, ctypes.c_void_p, wintypes.DWORD]
    _SetInformationJobObject.restype = wintypes.BOOL
    _AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _AssignProcessToJobObject.restype = wintypes.BOOL


def query_process_memory_counters(pid: int) -> Optional[Dict[str, int]]:
    if not sys.platform.startswith("win"):
        return None
    h = _OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, int(pid))
    if not h:
        return None
    try:
        pmc = PROCESS_MEMORY_COUNTERS_EX()
        pmc.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX)
        ok = _GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
        if not ok:
            return None
        return {
            "peak_working_set_bytes": int(pmc.PeakWorkingSetSize),
            "working_set_bytes": int(pmc.WorkingSetSize),
            "private_bytes": int(pmc.PrivateUsage),
            "pagefile_bytes": int(pmc.PagefileUsage),
            "peak_pagefile_bytes": int(pmc.PeakPagefileUsage),
        }
    finally:
        _CloseHandle(h)


def query_process_peak_rss_bytes(pid: int) -> Optional[int]:
    counters = query_process_memory_counters(pid)
    if counters is None:
        return None
    return counters.get("peak_working_set_bytes")


def mb_to_bytes(value_mb: Optional[int]) -> Optional[int]:
    if value_mb is None:
        return None
    return int(value_mb) * 1024 * 1024


def current_memory_usage_bytes(mem: Optional[Dict[str, int]]) -> Optional[int]:
    if mem is None:
        return None
    candidates = [
        mem.get("working_set_bytes"),
        mem.get("private_bytes"),
        mem.get("pagefile_bytes"),
    ]
    values = [int(v) for v in candidates if v is not None and int(v) > 0]
    if not values:
        return None
    return max(values)


def peak_memory_usage_bytes(mem: Optional[Dict[str, int]]) -> Optional[int]:
    if mem is None:
        return None
    candidates = [
        mem.get("peak_working_set_bytes"),
        mem.get("private_bytes"),
        mem.get("peak_pagefile_bytes"),
    ]
    values = [int(v) for v in candidates if v is not None and int(v) > 0]
    if not values:
        return None
    return max(values)


def assign_process_memory_job_limit(proc: subprocess.Popen, memory_limit_bytes: Optional[int]):
    if not sys.platform.startswith("win") or not memory_limit_bytes or memory_limit_bytes <= 0:
        return None
    try:
        process_handle = wintypes.HANDLE(int(proc._handle))  # type: ignore[attr-defined]
    except Exception:
        return None

    job = _CreateJobObjectW(None, None)
    if not job:
        return None

    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = (
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    )
    info.ProcessMemoryLimit = ctypes.c_size_t(int(memory_limit_bytes))
    ok = _SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if not ok:
        _CloseHandle(job)
        return None

    ok = _AssignProcessToJobObject(job, process_handle)
    if not ok:
        _CloseHandle(job)
        return None
    return job


def run_cmd_monitored(
    cmd: List[str],
    cwd: Optional[Path],
    timeout_sec: float,
    memory_limit_mb: Optional[int] = None,
):
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    peak_rss = 0
    peak_private = 0
    peak_commit = 0
    peak_pagefile = 0
    stopped = False
    timed_out = False
    oom = False
    oom_kind = ""
    stop_evt = threading.Event()
    memory_limit_bytes = mb_to_bytes(memory_limit_mb)
    job_handle = assign_process_memory_job_limit(proc, memory_limit_bytes)

    def monitor():
        nonlocal peak_rss, peak_private, peak_commit, peak_pagefile, oom, oom_kind
        while not stop_evt.is_set():
            if proc.pid:
                mem = query_process_memory_counters(proc.pid)
                if mem is not None:
                    rss = mem.get("peak_working_set_bytes")
                    if rss is not None and rss > peak_rss:
                        peak_rss = rss
                    private_b = mem.get("private_bytes")
                    if private_b is not None and private_b > peak_private:
                        peak_private = private_b
                    peak_commit_b = mem.get("peak_pagefile_bytes")
                    if peak_commit_b is not None and peak_commit_b > peak_commit:
                        peak_commit = peak_commit_b
                    pagefile_b = mem.get("pagefile_bytes")
                    if pagefile_b is not None and pagefile_b > peak_pagefile:
                        peak_pagefile = pagefile_b
                    current_bytes = current_memory_usage_bytes(mem)
                    if (
                        memory_limit_bytes is not None
                        and current_bytes is not None
                        and current_bytes > memory_limit_bytes
                    ):
                        oom = True
                        oom_kind = "process_memory_limit"
                        try:
                            proc.kill()
                        except OSError:
                            pass
                        break
            if proc.poll() is not None:
                break
            time.sleep(0.02)
        if proc.pid:
            mem = query_process_memory_counters(proc.pid)
            if mem is not None:
                rss = mem.get("peak_working_set_bytes")
                if rss is not None and rss > peak_rss:
                    peak_rss = rss
                private_b = mem.get("private_bytes")
                if private_b is not None and private_b > peak_private:
                    peak_private = private_b
                peak_commit_b = mem.get("peak_pagefile_bytes")
                if peak_commit_b is not None and peak_commit_b > peak_commit:
                    peak_commit = peak_commit_b
                pagefile_b = mem.get("pagefile_bytes")
                if pagefile_b is not None and pagefile_b > peak_pagefile:
                    peak_pagefile = pagefile_b

    th = threading.Thread(target=monitor, daemon=True)
    th.start()
    t0 = time.perf_counter()
    out = ""
    err = ""
    try:
        while True:
            if STOP_REQUESTED.is_set():
                announce_stop_request()
                stopped = True
                try:
                    proc.kill()
                finally:
                    out, err = proc.communicate()
                break

            elapsed_sec = time.perf_counter() - t0
            remaining = timeout_sec - elapsed_sec
            if remaining <= 0:
                raise subprocess.TimeoutExpired(cmd, timeout_sec)

            try:
                out, err = proc.communicate(timeout=min(0.1, remaining))
                break
            except subprocess.TimeoutExpired:
                continue
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            proc.kill()
        finally:
            out, err = proc.communicate()
            stop_evt.set()
            th.join(timeout=1.0)
            if job_handle:
                _CloseHandle(job_handle)
        raise
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    stop_evt.set()
    th.join(timeout=1.0)
    if job_handle:
        _CloseHandle(job_handle)
    return {
        "rc": proc.returncode,
        "stdout": out,
        "stderr": err,
        "elapsed_ms": elapsed_ms,
        "mem": {
            "peak_rss_bytes": peak_rss if peak_rss > 0 else None,
            "peak_private_bytes": peak_private if peak_private > 0 else None,
            "peak_commit_bytes": peak_commit if peak_commit > 0 else None,
            "peak_pagefile_bytes": peak_pagefile if peak_pagefile > 0 else None,
        },
        "stopped": stopped,
        "timed_out": timed_out,
        "oom": oom,
        "oom_kind": oom_kind,
    }


def run_mine(
    mine_exe: Path,
    case: Case,
    extra_args: List[str],
    timeout_sec: float,
    memory_limit_mb: Optional[int] = None,
):
    cmd = [str(mine_exe), "--input", str(case.path), "--no-reconstruct"] + extra_args
    monitored = run_cmd_monitored(
        cmd,
        cwd=None,
        timeout_sec=timeout_sec,
        memory_limit_mb=memory_limit_mb,
    )
    rc = monitored["rc"]
    out = monitored["stdout"]
    err = monitored["stderr"]
    elapsed = monitored["elapsed_ms"]
    mem = monitored["mem"]
    stopped = monitored["stopped"]

    merged = (out or "") + ("\n" + err if err else "")
    m_cost = MINE_COST_RE.search(merged)
    m_time = MINE_TIME_RE.search(merged)
    m_memo_used_mb = MINE_MEMO_USED_MB_RE.search(merged)
    m_memo_peak_entries = MINE_MEMO_PEAK_ENTRIES_RE.search(merged)
    m_nodes = MINE_NODES_RE.search(merged)
    parsed_cost = int(m_cost.group(1)) if m_cost else None
    parsed_time_ms = float(m_time.group(1)) if m_time else None
    parsed_memo_used_mb = float(m_memo_used_mb.group(1)) if m_memo_used_mb else None
    parsed_memo_peak_entries = int(m_memo_peak_entries.group(1)) if m_memo_peak_entries else None
    parsed_nodes = int(m_nodes.group(1)) if m_nodes else None

    return {
        "rc": rc,
        "stdout": out,
        "stderr": err,
        "cost": parsed_cost,
        "time_ms": parsed_time_ms if parsed_time_ms is not None else elapsed,
        "elapsed_ms": elapsed,
        "peak_rss_bytes": mem.get("peak_rss_bytes"),
        "peak_private_bytes": mem.get("peak_private_bytes"),
        "peak_commit_bytes": mem.get("peak_commit_bytes"),
        "peak_pagefile_bytes": mem.get("peak_pagefile_bytes"),
        "memo_used_mb": parsed_memo_used_mb,
        "memo_peak_entries": parsed_memo_peak_entries,
        "nodes": parsed_nodes,
        "stopped": stopped,
        "timed_out": bool(monitored.get("timed_out")),
        "oom": bool(monitored.get("oom")),
        "oom_kind": monitored.get("oom_kind") or "",
    }


def run_branch(
    branch_exe: Path,
    workdir: Path,
    config_arg: str,
    case: Case,
    timeout_sec: float,
    memory_limit_mb: Optional[int] = None,
):
    out_dir = workdir / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    res_file = out_dir / f"res{case.n}.txt"
    if res_file.exists():
        try:
            res_file.unlink()
        except OSError:
            pass

    cmd = [str(branch_exe), config_arg, str(case.n), str(case.branch_id)]
    monitored = run_cmd_monitored(
        cmd,
        cwd=workdir,
        timeout_sec=timeout_sec,
        memory_limit_mb=memory_limit_mb,
    )
    rc = monitored["rc"]
    out = monitored["stdout"]
    err = monitored["stderr"]
    elapsed_ms = monitored["elapsed_ms"]
    mem = monitored["mem"]
    stopped = monitored["stopped"]
    merged = (out or "") + ("\n" + err if err else "")
    m_tt = BRANCH_TT_RE.search(merged)
    m_time = BRANCH_TIME_RE.search(merged)
    m_cpu_ms = BRANCH_CPU_MS_RE.search(merged)
    m_wall_ms = BRANCH_WALL_MS_RE.search(merged)
    m_ram = BRANCH_RAM_RE.search(merged)
    m_nb_bytes_mem = BRANCH_NB_BYTES_MEM_RE.search(merged)
    m_nb_entry_mem = BRANCH_NB_ENTRY_MEM_RE.search(merged)
    tt = int(m_tt.group(1)) if m_tt else None
    cpu_s = int(m_time.group(1)) if m_time else None
    wall_s = int(m_time.group(2)) if m_time else None
    cpu_ms_precise = float(m_cpu_ms.group(1)) if m_cpu_ms else None
    wall_ms_precise = float(m_wall_ms.group(1)) if m_wall_ms else None
    ram_bytes = int(m_ram.group(1)) if m_ram else None
    nb_bytes_mem = int(m_nb_bytes_mem.group(1)) if m_nb_bytes_mem else None
    nb_entry_mem = int(m_nb_entry_mem.group(1)) if m_nb_entry_mem else None
    return {
        "rc": rc,
        "stdout": out,
        "stderr": err,
        "tt": tt,
        "cpu_s": cpu_s,
        "wall_s": wall_s,
        "cpu_ms_precise": cpu_ms_precise,
        "wall_ms_precise": wall_ms_precise,
        "elapsed_ms": elapsed_ms,
        "peak_rss_bytes": mem.get("peak_rss_bytes"),
        "peak_private_bytes": mem.get("peak_private_bytes"),
        "peak_commit_bytes": mem.get("peak_commit_bytes"),
        "peak_pagefile_bytes": mem.get("peak_pagefile_bytes"),
        "ram_bytes": ram_bytes,
        "nb_bytes_mem": nb_bytes_mem,
        "nb_entry_mem": nb_entry_mem,
        "stopped": stopped,
        "timed_out": bool(monitored.get("timed_out")),
        "oom": bool(monitored.get("oom")),
        "oom_kind": monitored.get("oom_kind") or "",
    }


def ensure_branch_case_input(case: Case, branch_data_root: Path):
    target = branch_data_root / str(case.n) / case.name
    try:
        if case.path.resolve() == target.resolve():
            return
    except OSError:
        pass

    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        src_stat = case.path.stat()
        dst_stat = target.stat()
        if src_stat.st_size == dst_stat.st_size and src_stat.st_mtime_ns == dst_stat.st_mtime_ns:
            return
    except OSError:
        pass
    shutil.copy2(case.path, target)


def build_parser() -> argparse.ArgumentParser:
    repo_root = Path(__file__).resolve().parents[1]
    article_root = detect_article_repo_root(repo_root)
    default_branch_config = repo_root / "tools" / "branch_compare_all.ini"
    if not default_branch_config.exists():
        default_branch_config = Path("config.ini")
    p = argparse.ArgumentParser(
        description="Compare local solver against SDT instances and reference TT values (and optionally Branch.exe)."
    )
    p.add_argument(
        "--preset",
        default=None,
        help="Experiment preset: official-full-article | generated-scaling-article | full-article-compare",
    )
    p.add_argument("--output-dir", type=Path, help="Experiment output directory (default: compare_out/experiments/<ts>_<preset>)")
    p.add_argument("--experiment-name", help="Friendly experiment name used in manifests/output folders")
    p.add_argument("--data-root", type=Path, default=article_root / "Branch" / "data")
    p.add_argument("--sol-zip", type=Path, default=article_root / "DataSets" / "1SumTi_data_sol.zip")
    p.add_argument("--mine-exe", type=Path, default=repo_root / "x64" / "Release" / "kursovaya.exe")
    p.add_argument("--n", dest="n_list", help="Comma-separated n values, e.g. 100,200")
    p.add_argument("--n-from", type=int, help="Start of n range for generated/scaling experiments")
    p.add_argument("--n-to", type=int, help="End of n range for generated/scaling experiments")
    p.add_argument("--n-step", type=int, default=DEFAULT_SCALING_N_STEP, help="Step for n range (default: 100)")
    p.add_argument("--r", dest="r_list", help="Comma-separated R values, e.g. 0.2,0.6")
    p.add_argument("--t", dest="t_list", help="Comma-separated T values, e.g. 0.6")
    p.add_argument("--k", dest="k_list", help="Comma-separated instance ids 1..10")
    p.add_argument("--limit", type=int, default=0, help="Max number of cases to run (0 = all selected)")
    p.add_argument("--jobs", type=int, default=1, help="Parallel workers for case execution (default: 1)")
    p.add_argument("--timeout-sec", type=float, default=120.0)
    p.add_argument("--memory-limit-mb", type=int, help="Hard process memory limit for both solvers")
    p.add_argument("--mine-memory-limit-mb", type=int, help="Hard process memory limit for local solver only")
    p.add_argument("--branch-memory-limit-mb", type=int, help="Hard process memory limit for Branch.exe only")
    p.add_argument("--repeats", type=int, default=1, help="Number of repeated runs per case for both solvers")
    p.add_argument("--repeat-mine", type=int, help="Override repeats for local solver")
    p.add_argument("--repeat-branch", type=int, help="Override repeats for Branch.exe")
    p.add_argument("--mine-arg", action="append", default=[], help="Extra arg forwarded to local solver (repeatable)")
    p.add_argument("--csv-out", type=Path, help="Optional CSV output path")
    p.add_argument("--summary-csv", type=Path, help="Optional summary CSV grouped by n,r,t (also output path in rebuild mode)")
    p.add_argument("--case-summary-csv", type=Path, help="Optional case-level summary CSV")
    p.add_argument("--frontier-csv", type=Path, help="Optional frontier CSV")
    p.add_argument(
        "--rebuild-summary-from-csv",
        type=Path,
        help="Rebuild summary CSV from an existing detail CSV (no solver execution). Requires --summary-csv.",
    )
    p.add_argument("--generate-missing", action="store_true", help="Generate missing SDT files in --data-root before running")
    p.add_argument("--generate-overwrite", action="store_true", help="Regenerate and overwrite SDT files in --data-root")
    p.add_argument("--gen-seed-base", type=int, default=1, help="Base seed for generated SDT cases (sequential by case order)")
    p.add_argument("--gen-p-min", type=int, default=1, help="Generator p_min (default: 1)")
    p.add_argument("--gen-p-max", type=int, default=100, help="Generator p_max (default: 100)")
    p.add_argument("--generated-data-root", type=Path, help="Explicit directory where generated SDT instances are saved")
    p.add_argument("--no-sol-compare", action="store_true", help="Disable comparison against sol_*.txt reference values")
    p.add_argument("--check-branch", action="store_true", help="Also run authors' Branch.exe")
    p.add_argument("--branch-exe", type=Path, default=article_root / "Executable Branch-Memorize" / "Branch.exe")
    p.add_argument("--branch-workdir", type=Path, default=article_root / "Branch")
    p.add_argument(
        "--branch-data-root",
        type=Path,
        help="Directory mirrored into the data tree read by Branch.exe (default: <branch-workdir>/data)",
    )
    p.add_argument("--branch-config", default=str(default_branch_config), help="Config path passed to Branch.exe (use relative path in branch workdir)")
    p.add_argument("--branch-timeout-sec", type=float, default=120.0)
    p.add_argument("--stop-on-mismatch", action="store_true")
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "--console-ms-only",
        action="store_true",
        help="Compact console progress: status + ms timings only (CSV unaffected)",
    )
    return p


def safe_mean(values: List[float]) -> Optional[float]:
    return statistics.fmean(values) if values else None


def safe_median(values: List[float]) -> Optional[float]:
    return statistics.median(values) if values else None


def safe_stdev(values: List[float]) -> Optional[float]:
    return statistics.stdev(values) if len(values) >= 2 else None


def fmt_float(v: Optional[float], ndigits: int = 3) -> str:
    return "" if v is None else f"{v:.{ndigits}f}"


def safe_float(v) -> Optional[float]:
    if v is None:
        return None
    s = str(v).strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def safe_int(v) -> Optional[int]:
    if v is None:
        return None
    s = str(v).strip()
    if not s:
        return None
    try:
        return int(s)
    except ValueError:
        return None


DETAIL_FIELDNAMES = [
    "experiment", "dataset_kind", "repeat_idx", "repeat_count", "case", "data_path",
    "n", "r", "t", "k", "branch_id", "generated_seed",
    "mine_timeout_sec", "mine_memory_limit_mb", "branch_timeout_sec", "branch_memory_limit_mb",
    "expected_tt",
    "mine_tt", "mine_time_ms", "mine_elapsed_ms", "mine_rc", "mine_ok", "mine_status",
    "mine_peak_rss_bytes", "mine_peak_rss_mb",
    "mine_peak_private_bytes", "mine_peak_private_mb",
    "mine_peak_commit_bytes", "mine_peak_commit_mb",
    "mine_peak_pagefile_bytes", "mine_peak_pagefile_mb",
    "mine_memo_used_mb", "mine_memo_peak_entries", "mine_nodes", "mine_oom_kind",
    "branch_tt", "branch_cpu_s", "branch_wall_s", "branch_cpu_ms_internal", "branch_wall_ms_internal",
    "branch_wall_ms", "branch_elapsed_ms", "branch_rc", "branch_ok", "branch_status",
    "branch_peak_rss_bytes", "branch_peak_rss_mb",
    "branch_peak_private_bytes", "branch_peak_private_mb",
    "branch_peak_commit_bytes", "branch_peak_commit_mb",
    "branch_peak_pagefile_bytes", "branch_peak_pagefile_mb",
    "branch_ram_bytes", "branch_ram_mb", "branch_nbbytesmem", "branch_nbentrymem", "branch_oom_kind",
    "mine_over_branch_wall", "branch_over_mine",
    "mine_over_branch_peak_rss", "branch_over_mine_peak_rss",
    "status",
]

CASE_SUMMARY_FIELDNAMES = [
    "experiment", "dataset_kind", "case", "data_path", "n", "r", "t", "k", "branch_id", "generated_seed",
    "runs", "mine_runs", "branch_runs", "expected_tt",
    "mine_ok_all", "branch_ok_all", "both_ok_all", "mismatch_any",
    "mine_timeout_count", "mine_oom_count", "mine_fail_count",
    "branch_timeout_count", "branch_oom_count", "branch_fail_count",
    "mine_elapsed_ms_mean", "mine_elapsed_ms_median", "mine_elapsed_ms_min", "mine_elapsed_ms_max", "mine_elapsed_ms_stdev",
    "branch_elapsed_ms_mean", "branch_elapsed_ms_median", "branch_elapsed_ms_min", "branch_elapsed_ms_max", "branch_elapsed_ms_stdev",
    "mine_peak_rss_mb_mean", "mine_peak_rss_mb_max",
    "branch_peak_rss_mb_mean", "branch_peak_rss_mb_max",
    "mine_over_branch_wall_mean", "branch_over_mine_mean",
    "status",
]

GRID_SUMMARY_FIELDNAMES = [
    "experiment", "dataset_kind", "group", "n", "r", "t", "k",
    "count", "mine_ok_count", "branch_ok_count", "both_ok_count", "mismatch_count",
    "mine_timeout_count", "mine_oom_count", "mine_fail_count",
    "branch_timeout_count", "branch_oom_count", "branch_fail_count",
    "mine_elapsed_ms_mean", "mine_elapsed_ms_median", "mine_elapsed_ms_min", "mine_elapsed_ms_max", "mine_elapsed_ms_stdev",
    "branch_elapsed_ms_mean", "branch_elapsed_ms_median", "branch_elapsed_ms_min", "branch_elapsed_ms_max", "branch_elapsed_ms_stdev",
    "mine_peak_rss_mb_mean", "mine_peak_rss_mb_max",
    "branch_peak_rss_mb_mean", "branch_peak_rss_mb_max",
    "mine_over_branch_wall_mean", "mine_over_branch_wall_median",
    "branch_over_mine_mean", "branch_over_mine_median",
]

FRONTIER_FIELDNAMES = [
    "experiment", "dataset_kind", "group", "r", "t", "k",
    "mine_max_n", "branch_max_n", "both_max_n",
    "mine_ok_sizes", "branch_ok_sizes", "both_ok_sizes",
]


SUMMARY_FIELDNAMES = [
    "group", "n", "r", "t", "count", "ok_count", "mismatch_count", "failure_count",
    "mine_time_ms_mean", "mine_time_ms_median", "mine_time_ms_min", "mine_time_ms_max",
    "mine_elapsed_ms_mean", "mine_elapsed_ms_median",
    "mine_peak_rss_mb_mean", "mine_peak_rss_mb_median",
    "mine_peak_private_mb_mean", "mine_peak_private_mb_median",
    "mine_peak_commit_mb_mean", "mine_peak_commit_mb_median",
    "mine_memo_used_mb_mean", "mine_memo_used_mb_median",
    "branch_wall_ms_mean", "branch_wall_ms_median",
    "branch_elapsed_ms_mean", "branch_elapsed_ms_median",
    "branch_peak_rss_mb_mean", "branch_peak_rss_mb_median",
    "branch_peak_private_mb_mean", "branch_peak_private_mb_median",
    "branch_peak_commit_mb_mean", "branch_peak_commit_mb_median",
    "branch_ram_mb_mean", "branch_ram_mb_median",
    "branch_over_mine_mean", "mine_over_branch_wall_mean",
    "branch_over_mine_peak_rss_mean", "mine_over_branch_peak_rss_mean",
]


def compute_summary_rows_from_detail_csv_rows(detail_rows: Iterable[dict]) -> List[dict]:
    groups = defaultdict(list)
    rows = list(detail_rows)
    for r in rows:
        groups[(r.get("n", ""), r.get("r", ""), r.get("t", ""))].append(r)

    out_rows: List[dict] = []

    def write_group(group_key, recs: List[dict], group_name: str):
        mine_times = [x for x in (safe_float(r.get("mine_time_ms")) for r in recs) if x is not None]
        mine_elapsed_times = [x for x in (safe_float(r.get("mine_elapsed_ms")) for r in recs) if x is not None]
        mine_peak_rss = [x for x in (safe_float(r.get("mine_peak_rss_mb")) for r in recs) if x is not None]
        mine_peak_private = [x for x in (safe_float(r.get("mine_peak_private_mb")) for r in recs) if x is not None]
        mine_peak_commit = [x for x in (safe_float(r.get("mine_peak_commit_mb")) for r in recs) if x is not None]
        mine_memo_used = [x for x in (safe_float(r.get("mine_memo_used_mb")) for r in recs) if x is not None]
        branch_walls = [x for x in (safe_float(r.get("branch_wall_ms")) for r in recs) if x is not None]
        branch_elapseds = [x for x in (safe_float(r.get("branch_elapsed_ms")) for r in recs) if x is not None]
        branch_peak_rss = [x for x in (safe_float(r.get("branch_peak_rss_mb")) for r in recs) if x is not None]
        branch_peak_private = [x for x in (safe_float(r.get("branch_peak_private_mb")) for r in recs) if x is not None]
        branch_peak_commit = [x for x in (safe_float(r.get("branch_peak_commit_mb")) for r in recs) if x is not None]
        branch_ram = [x for x in (safe_float(r.get("branch_ram_mb")) for r in recs) if x is not None]

        mine_over_branch_vals: List[float] = []
        branch_over_mine_vals: List[float] = []
        mine_over_branch_rss_vals: List[float] = []
        branch_over_mine_rss_vals: List[float] = []

        for r in recs:
            me = safe_float(r.get("mine_elapsed_ms"))
            be = safe_float(r.get("branch_elapsed_ms"))
            if me is not None and be is not None and me > 0 and be > 0:
                mine_over_branch_vals.append(me / be)
                branch_over_mine_vals.append(be / me)

            mr = safe_float(r.get("mine_peak_rss_mb"))
            br = safe_float(r.get("branch_peak_rss_mb"))
            if mr is not None and br is not None and mr > 0 and br > 0:
                mine_over_branch_rss_vals.append(mr / br)
                branch_over_mine_rss_vals.append(br / mr)

        mismatch_count = sum(1 for r in recs if "mismatch" in (r.get("status") or ""))
        failure_count = sum(1 for r in recs if "fail" in (r.get("status") or ""))
        ok_count = sum(1 for r in recs if (r.get("status") or "") == "ok")

        n_val, r_val, t_val = group_key
        out_rows.append(
            {
                "group": group_name,
                "n": n_val,
                "r": r_val,
                "t": t_val,
                "count": len(recs),
                "ok_count": ok_count,
                "mismatch_count": mismatch_count,
                "failure_count": failure_count,
                "mine_time_ms_mean": fmt_float(safe_mean(mine_times)),
                "mine_time_ms_median": fmt_float(safe_median(mine_times)),
                "mine_time_ms_min": fmt_float(min(mine_times) if mine_times else None),
                "mine_time_ms_max": fmt_float(max(mine_times) if mine_times else None),
                "mine_elapsed_ms_mean": fmt_float(safe_mean(mine_elapsed_times)),
                "mine_elapsed_ms_median": fmt_float(safe_median(mine_elapsed_times)),
                "mine_peak_rss_mb_mean": fmt_float(safe_mean(mine_peak_rss)),
                "mine_peak_rss_mb_median": fmt_float(safe_median(mine_peak_rss)),
                "mine_peak_private_mb_mean": fmt_float(safe_mean(mine_peak_private)),
                "mine_peak_private_mb_median": fmt_float(safe_median(mine_peak_private)),
                "mine_peak_commit_mb_mean": fmt_float(safe_mean(mine_peak_commit)),
                "mine_peak_commit_mb_median": fmt_float(safe_median(mine_peak_commit)),
                "mine_memo_used_mb_mean": fmt_float(safe_mean(mine_memo_used)),
                "mine_memo_used_mb_median": fmt_float(safe_median(mine_memo_used)),
                "branch_wall_ms_mean": fmt_float(safe_mean(branch_walls)),
                "branch_wall_ms_median": fmt_float(safe_median(branch_walls)),
                "branch_elapsed_ms_mean": fmt_float(safe_mean(branch_elapseds)),
                "branch_elapsed_ms_median": fmt_float(safe_median(branch_elapseds)),
                "branch_peak_rss_mb_mean": fmt_float(safe_mean(branch_peak_rss)),
                "branch_peak_rss_mb_median": fmt_float(safe_median(branch_peak_rss)),
                "branch_peak_private_mb_mean": fmt_float(safe_mean(branch_peak_private)),
                "branch_peak_private_mb_median": fmt_float(safe_median(branch_peak_private)),
                "branch_peak_commit_mb_mean": fmt_float(safe_mean(branch_peak_commit)),
                "branch_peak_commit_mb_median": fmt_float(safe_median(branch_peak_commit)),
                "branch_ram_mb_mean": fmt_float(safe_mean(branch_ram)),
                "branch_ram_mb_median": fmt_float(safe_median(branch_ram)),
                "branch_over_mine_mean": fmt_float(safe_mean(branch_over_mine_vals), 6),
                "mine_over_branch_wall_mean": fmt_float(safe_mean(mine_over_branch_vals), 6),
                "branch_over_mine_peak_rss_mean": fmt_float(safe_mean(branch_over_mine_rss_vals), 6),
                "mine_over_branch_peak_rss_mean": fmt_float(safe_mean(mine_over_branch_rss_vals), 6),
            }
        )

    for key in sorted(
        groups.keys(),
        key=lambda x: (safe_int(x[0]) or -1, safe_float(x[1]) or -1.0, safe_float(x[2]) or -1.0),
    ):
        write_group(key, groups[key], "nrt")

    if rows:
        write_group(("", "", ""), rows, "overall")

    return out_rows


def write_summary_csv_rows(path: Path, rows: List[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as sf:
        sw = csv.DictWriter(sf, fieldnames=SUMMARY_FIELDNAMES)
        sw.writeheader()
        sw.writerows(rows)


def metric_stats(rows: List[dict], key: str) -> dict:
    values = [x for x in (safe_float(r.get(key)) for r in rows) if x is not None]
    return {
        "mean": safe_mean(values),
        "median": safe_median(values),
        "min": min(values) if values else None,
        "max": max(values) if values else None,
        "stdev": safe_stdev(values),
    }


def compute_case_summary_rows(detail_rows: Iterable[dict]) -> List[dict]:
    groups = defaultdict(list)
    for row in detail_rows:
        key = (
            row.get("experiment", ""),
            row.get("dataset_kind", ""),
            row.get("case", ""),
        )
        groups[key].append(row)

    out: List[dict] = []
    for key in sorted(groups.keys(), key=lambda x: (x[0], x[1], x[2])):
        rows = groups[key]
        first = rows[0]
        mine_elapsed = metric_stats(rows, "mine_elapsed_ms")
        branch_elapsed = metric_stats(rows, "branch_elapsed_ms")
        mine_rss = metric_stats(rows, "mine_peak_rss_mb")
        branch_rss = metric_stats(rows, "branch_peak_rss_mb")
        mine_over_branch = [x for x in (safe_float(r.get("mine_over_branch_wall")) for r in rows) if x is not None]
        branch_over_mine = [x for x in (safe_float(r.get("branch_over_mine")) for r in rows) if x is not None]

        mine_statuses = [str(r.get("mine_status") or "") for r in rows]
        branch_statuses = [str(r.get("branch_status") or "") for r in rows]
        mismatch_any = any("mismatch" in str(r.get("status") or "") for r in rows)
        mine_ok_all = bool(rows) and all(s == "ok" for s in mine_statuses if s and s != "not_run")
        branch_ran = any(s and s != "not_run" for s in branch_statuses)
        branch_ok_all = branch_ran and all(s == "ok" for s in branch_statuses if s and s != "not_run")
        both_ok_all = mine_ok_all and (branch_ok_all if branch_ran else True) and not mismatch_any

        parts = []
        if not mine_ok_all:
            if any(s == "timeout" for s in mine_statuses):
                parts.append("mine_timeout")
            elif any(s == "oom" for s in mine_statuses):
                parts.append("mine_oom")
            else:
                parts.append("mine_fail")
        if branch_ran and not branch_ok_all:
            if any(s == "timeout" for s in branch_statuses):
                parts.append("branch_timeout")
            elif any(s == "oom" for s in branch_statuses):
                parts.append("branch_oom")
            else:
                parts.append("branch_fail")
        if mismatch_any:
            parts.append("mismatch")

        out.append(
            {
                "experiment": first.get("experiment", ""),
                "dataset_kind": first.get("dataset_kind", ""),
                "case": first.get("case", ""),
                "data_path": first.get("data_path", ""),
                "n": first.get("n", ""),
                "r": first.get("r", ""),
                "t": first.get("t", ""),
                "k": first.get("k", ""),
                "branch_id": first.get("branch_id", ""),
                "generated_seed": first.get("generated_seed", ""),
                "runs": len(rows),
                "mine_runs": sum(1 for s in mine_statuses if s and s != "not_run"),
                "branch_runs": sum(1 for s in branch_statuses if s and s != "not_run"),
                "expected_tt": first.get("expected_tt", ""),
                "mine_ok_all": int(mine_ok_all),
                "branch_ok_all": int(branch_ok_all) if branch_ran else "",
                "both_ok_all": int(both_ok_all),
                "mismatch_any": int(mismatch_any),
                "mine_timeout_count": sum(1 for s in mine_statuses if s == "timeout"),
                "mine_oom_count": sum(1 for s in mine_statuses if s == "oom"),
                "mine_fail_count": sum(1 for s in mine_statuses if s not in ("ok", "", "not_run", "timeout", "oom")),
                "branch_timeout_count": sum(1 for s in branch_statuses if s == "timeout"),
                "branch_oom_count": sum(1 for s in branch_statuses if s == "oom"),
                "branch_fail_count": sum(1 for s in branch_statuses if s not in ("ok", "", "not_run", "timeout", "oom")),
                "mine_elapsed_ms_mean": fmt_float(mine_elapsed["mean"]),
                "mine_elapsed_ms_median": fmt_float(mine_elapsed["median"]),
                "mine_elapsed_ms_min": fmt_float(mine_elapsed["min"]),
                "mine_elapsed_ms_max": fmt_float(mine_elapsed["max"]),
                "mine_elapsed_ms_stdev": fmt_float(mine_elapsed["stdev"]),
                "branch_elapsed_ms_mean": fmt_float(branch_elapsed["mean"]),
                "branch_elapsed_ms_median": fmt_float(branch_elapsed["median"]),
                "branch_elapsed_ms_min": fmt_float(branch_elapsed["min"]),
                "branch_elapsed_ms_max": fmt_float(branch_elapsed["max"]),
                "branch_elapsed_ms_stdev": fmt_float(branch_elapsed["stdev"]),
                "mine_peak_rss_mb_mean": fmt_float(mine_rss["mean"]),
                "mine_peak_rss_mb_max": fmt_float(mine_rss["max"]),
                "branch_peak_rss_mb_mean": fmt_float(branch_rss["mean"]),
                "branch_peak_rss_mb_max": fmt_float(branch_rss["max"]),
                "mine_over_branch_wall_mean": fmt_float(safe_mean(mine_over_branch), 6),
                "branch_over_mine_mean": fmt_float(safe_mean(branch_over_mine), 6),
                "status": join_status_parts(parts),
            }
        )
    return out


def compute_grid_summary_rows(case_rows: Iterable[dict]) -> List[dict]:
    groups = defaultdict(list)
    rows = list(case_rows)
    for row in rows:
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "nrt", row.get("n", ""), row.get("r", ""), row.get("t", ""), "")].append(row)
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "n", row.get("n", ""), "", "", "")].append(row)
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "rt", "", row.get("r", ""), row.get("t", ""), "")].append(row)
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "overall", "", "", "", "")].append(row)

    out: List[dict] = []
    for key in sorted(groups.keys(), key=lambda x: (x[0], x[1], x[2], safe_int(x[3]) or -1, safe_float(x[4]) or -1.0, safe_float(x[5]) or -1.0)):
        exp, dataset_kind, group_name, n_val, r_val, t_val, k_val = key
        recs = groups[key]
        mine_elapsed = metric_stats(recs, "mine_elapsed_ms_mean")
        branch_elapsed = metric_stats(recs, "branch_elapsed_ms_mean")
        mine_rss = metric_stats(recs, "mine_peak_rss_mb_mean")
        branch_rss = metric_stats(recs, "branch_peak_rss_mb_mean")
        mine_over_branch_vals = [x for x in (safe_float(r.get("mine_over_branch_wall_mean")) for r in recs) if x is not None]
        branch_over_mine_vals = [x for x in (safe_float(r.get("branch_over_mine_mean")) for r in recs) if x is not None]
        out.append(
            {
                "experiment": exp,
                "dataset_kind": dataset_kind,
                "group": group_name,
                "n": n_val,
                "r": r_val,
                "t": t_val,
                "k": k_val,
                "count": len(recs),
                "mine_ok_count": sum(1 for r in recs if safe_int(r.get("mine_ok_all")) == 1),
                "branch_ok_count": sum(1 for r in recs if safe_int(r.get("branch_ok_all")) == 1),
                "both_ok_count": sum(1 for r in recs if safe_int(r.get("both_ok_all")) == 1),
                "mismatch_count": sum(1 for r in recs if safe_int(r.get("mismatch_any")) == 1),
                "mine_timeout_count": sum(safe_int(r.get("mine_timeout_count")) or 0 for r in recs),
                "mine_oom_count": sum(safe_int(r.get("mine_oom_count")) or 0 for r in recs),
                "mine_fail_count": sum(safe_int(r.get("mine_fail_count")) or 0 for r in recs),
                "branch_timeout_count": sum(safe_int(r.get("branch_timeout_count")) or 0 for r in recs),
                "branch_oom_count": sum(safe_int(r.get("branch_oom_count")) or 0 for r in recs),
                "branch_fail_count": sum(safe_int(r.get("branch_fail_count")) or 0 for r in recs),
                "mine_elapsed_ms_mean": fmt_float(mine_elapsed["mean"]),
                "mine_elapsed_ms_median": fmt_float(mine_elapsed["median"]),
                "mine_elapsed_ms_min": fmt_float(mine_elapsed["min"]),
                "mine_elapsed_ms_max": fmt_float(mine_elapsed["max"]),
                "mine_elapsed_ms_stdev": fmt_float(mine_elapsed["stdev"]),
                "branch_elapsed_ms_mean": fmt_float(branch_elapsed["mean"]),
                "branch_elapsed_ms_median": fmt_float(branch_elapsed["median"]),
                "branch_elapsed_ms_min": fmt_float(branch_elapsed["min"]),
                "branch_elapsed_ms_max": fmt_float(branch_elapsed["max"]),
                "branch_elapsed_ms_stdev": fmt_float(branch_elapsed["stdev"]),
                "mine_peak_rss_mb_mean": fmt_float(mine_rss["mean"]),
                "mine_peak_rss_mb_max": fmt_float(mine_rss["max"]),
                "branch_peak_rss_mb_mean": fmt_float(branch_rss["mean"]),
                "branch_peak_rss_mb_max": fmt_float(branch_rss["max"]),
                "mine_over_branch_wall_mean": fmt_float(safe_mean(mine_over_branch_vals), 6),
                "mine_over_branch_wall_median": fmt_float(safe_median(mine_over_branch_vals), 6),
                "branch_over_mine_mean": fmt_float(safe_mean(branch_over_mine_vals), 6),
                "branch_over_mine_median": fmt_float(safe_median(branch_over_mine_vals), 6),
            }
        )
    return out


def compute_frontier_rows(case_rows: Iterable[dict]) -> List[dict]:
    rows = list(case_rows)
    out: List[dict] = []

    def max_ok_n(recs: List[dict], key_name: str) -> Optional[int]:
        ok_ns = [safe_int(r.get("n")) for r in recs if safe_int(r.get(key_name)) == 1]
        ok_ns = [n for n in ok_ns if n is not None]
        return max(ok_ns) if ok_ns else None

    groups = defaultdict(list)
    for row in rows:
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "rtk", row.get("r", ""), row.get("t", ""), row.get("k", ""))].append(row)
        groups[(row.get("experiment", ""), row.get("dataset_kind", ""), "rt", row.get("r", ""), row.get("t", ""), "")].append(row)

    for key in sorted(groups.keys(), key=lambda x: (x[0], x[1], x[2], safe_float(x[3]) or -1.0, safe_float(x[4]) or -1.0, safe_int(x[5]) or -1)):
        exp, dataset_kind, group_name, r_val, t_val, k_val = key
        recs = groups[key]
        out.append(
            {
                "experiment": exp,
                "dataset_kind": dataset_kind,
                "group": group_name,
                "r": r_val,
                "t": t_val,
                "k": k_val,
                "mine_max_n": max_ok_n(recs, "mine_ok_all") or "",
                "branch_max_n": max_ok_n(recs, "branch_ok_all") or "",
                "both_max_n": max_ok_n(recs, "both_ok_all") or "",
                "mine_ok_sizes": sum(1 for r in recs if safe_int(r.get("mine_ok_all")) == 1),
                "branch_ok_sizes": sum(1 for r in recs if safe_int(r.get("branch_ok_all")) == 1),
                "both_ok_sizes": sum(1 for r in recs if safe_int(r.get("both_ok_all")) == 1),
            }
        )
    return out


def generate_potts_sdt_instance_lines(
    n: int,
    due_range: float,
    tardiness_factor: float,
    seed: int,
    p_min: int = 1,
    p_max: int = 100,
) -> List[str]:
    rng = random.Random(int(seed))
    ps = [rng.randint(p_min, p_max) for _ in range(n)]
    total_p = sum(ps)
    lower_factor = 1.0 - tardiness_factor - due_range * 0.5
    upper_factor = 1.0 - tardiness_factor + due_range * 0.5
    due_low = math.floor(total_p * lower_factor)
    due_high = math.floor(total_p * upper_factor)
    if due_low > due_high:
        due_low, due_high = due_high, due_low
    ds = [max(0, rng.randint(due_low, due_high)) for _ in range(n)]
    return [f"{p} {d}" for p, d in zip(ps, ds)]


def synthesize_target_cases(n_filter, r_filter, t_filter, k_filter) -> List[Tuple[int, str, str, int]]:
    if n_filter is None or len(n_filter) == 0:
        raise ValueError("--generate-missing requires --n (at least one n value).")
    r_values = sorted((r_filter or set(DEFAULT_R_GRID)), key=float)
    t_values = sorted((t_filter or set(DEFAULT_T_GRID)), key=float)
    k_values = sorted((k_filter or set(DEFAULT_K_GRID)))
    targets = []
    for n in sorted(n_filter):
        for r in r_values:
            for t in t_values:
                for k in k_values:
                    targets.append((int(n), str(r), str(t), int(k)))
    return targets


def generate_missing_sdt_files(
    data_root: Path,
    targets: List[Tuple[int, str, str, int]],
    seed_base: int,
    p_min: int,
    p_max: int,
    overwrite: bool,
) -> Tuple[int, int, List[dict]]:
    generated = 0
    skipped = 0
    records: List[dict] = []
    for idx, (n, r, t, k) in enumerate(targets):
        folder = data_root / str(n)
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"SDT_{n}_{r}_{t}_{k}.txt"
        seed = int(seed_base) + idx
        if path.exists() and not overwrite:
            skipped += 1
            records.append(
                {
                    "case": path.name,
                    "path": str(path),
                    "n": n,
                    "r": r,
                    "t": t,
                    "k": k,
                    "seed": seed,
                    "status": "skipped_existing",
                }
            )
            continue
        lines = generate_potts_sdt_instance_lines(
            n=n,
            due_range=float(r),
            tardiness_factor=float(t),
            seed=seed,
            p_min=p_min,
            p_max=p_max,
        )
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        generated += 1
        records.append(
            {
                "case": path.name,
                "path": str(path),
                "n": n,
                "r": r,
                "t": t,
                "k": k,
                "seed": seed,
                "status": "generated",
            }
        )
    return generated, skipped, records


def clone_args(args, **updates):
    data = dict(vars(args))
    data.update(updates)
    return argparse.Namespace(**data)


def prepare_suite_args(args, repo_root: Path, article_root: Path):
    if args.preset != "full-article-compare":
        return [(args.experiment_name or args.preset or "compare", args)]

    suite_root_name = args.experiment_name or "full-article-compare"
    if args.output_dir is None:
        output_root = repo_root / "compare_out" / "experiments" / make_experiment_name(suite_root_name)
    else:
        output_root = args.output_dir
    output_root.mkdir(parents=True, exist_ok=True)

    official_args = clone_args(
        args,
        preset="official-full-article",
        output_dir=output_root / "official_full",
        experiment_name="official_full",
    )
    apply_preset_defaults(official_args, repo_root, article_root)

    generated_args = clone_args(
        args,
        preset="generated-scaling-article",
        output_dir=output_root / "generated_scaling",
        experiment_name="generated_scaling",
        generated_data_root=args.generated_data_root or (output_root / "generated_data"),
    )
    apply_preset_defaults(generated_args, repo_root, article_root)
    return [("official_full", official_args), ("generated_scaling", generated_args)]


def write_selected_cases_csv(path: Path, cases: List[Case], expected_map: Dict[str, int], generated_seed_map: Dict[str, int]) -> None:
    rows = []
    for case in cases:
        rows.append(
            {
                "case": case.name,
                "path": str(case.path),
                "n": case.n,
                "r": case.r,
                "t": case.t,
                "k": case.k,
                "branch_id": case.branch_id,
                "expected_tt": expected_map.get(case.name, ""),
                "generated_seed": generated_seed_map.get(case.name, ""),
            }
        )
    write_rows_csv(
        path,
        ["case", "path", "n", "r", "t", "k", "branch_id", "expected_tt", "generated_seed"],
        rows,
    )


def handle_rebuild_summary(args) -> int:
    if args.rebuild_summary_from_csv is None:
        return -1
    if args.summary_csv is None:
        print("--rebuild-summary-from-csv requires --summary-csv", file=sys.stderr)
        return 2
    if not args.rebuild_summary_from_csv.exists():
        print(f"detail csv not found: {args.rebuild_summary_from_csv}", file=sys.stderr)
        return 2
    with args.rebuild_summary_from_csv.open("r", newline="", encoding="utf-8") as df:
        detail_rows = list(csv.DictReader(df))
    if not detail_rows:
        print("detail csv has no rows", file=sys.stderr)
        return 2
    case_rows = compute_case_summary_rows(detail_rows)
    grid_rows = compute_grid_summary_rows(case_rows)
    frontier_rows = compute_frontier_rows(case_rows)
    write_rows_csv(args.summary_csv, GRID_SUMMARY_FIELDNAMES, grid_rows)
    if args.case_summary_csv is not None:
        write_rows_csv(args.case_summary_csv, CASE_SUMMARY_FIELDNAMES, case_rows)
    if args.frontier_csv is not None:
        write_rows_csv(args.frontier_csv, FRONTIER_FIELDNAMES, frontier_rows)
    print(
        f"rebuild_summary: in={args.rebuild_summary_from_csv} "
        f"grid={args.summary_csv} case_rows={len(case_rows)} grid_rows={len(grid_rows)} frontier_rows={len(frontier_rows)}"
    )
    return 0


def execute_experiment(args, repo_root: Path, article_root: Path) -> int:
    n_filter, r_filter, t_filter, k_filter = normalize_filters_from_args(args)
    experiment_name = args.experiment_name or args.preset or "compare"
    paths = resolve_output_paths(args, experiment_name)

    if not args.mine_exe.exists():
        print(f"mine exe not found: {args.mine_exe}", file=sys.stderr)
        return 2

    repeat_mine, repeat_branch = resolve_repeat_counts(args)
    mine_limits, branch_limits = resolve_limit_pair(args)
    mine_args = ensure_mine_args_limits(list(args.mine_arg), mine_limits.memory_limit_mb)

    data_root: Path = args.data_root
    generated_records: List[dict] = []
    generated_seed_map: Dict[str, int] = {}
    if args.generate_missing or args.generate_overwrite:
        try:
            targets = synthesize_target_cases(n_filter, r_filter, t_filter, k_filter)
        except ValueError as ex:
            print(str(ex), file=sys.stderr)
            return 2
        data_root.mkdir(parents=True, exist_ok=True)
        gen_count, skip_count, generated_records = generate_missing_sdt_files(
            data_root=data_root,
            targets=targets,
            seed_base=args.gen_seed_base,
            p_min=args.gen_p_min,
            p_max=args.gen_p_max,
            overwrite=bool(args.generate_overwrite),
        )
        generated_seed_map = {
            str(rec["case"]): int(rec["seed"])
            for rec in generated_records
            if rec.get("seed") is not None
        }
        write_rows_csv(
            paths.generated_cases_csv,
            ["case", "path", "n", "r", "t", "k", "seed", "status"],
            generated_records,
        )
        print(f"generated_sdt: generated={gen_count} skipped={skip_count} root={data_root}")
        if not args.no_sol_compare:
            print(
                "warning: generated SDT files usually do not match official sol_*.txt values; "
                "use --no-sol-compare for branch-vs-mine experiments",
                file=sys.stderr,
            )

    if not data_root.exists():
        print(f"data root not found: {data_root}", file=sys.stderr)
        return 2

    cases = discover_cases(data_root, n_filter, r_filter, t_filter, k_filter)
    if args.limit and args.limit > 0:
        cases = cases[: args.limit]
    if not cases:
        print("no cases matched filters", file=sys.stderr)
        return 2

    sol_map: Dict[str, int] = {}
    if args.no_sol_compare:
        sol_map = {}
    elif args.sol_zip and args.sol_zip.exists():
        sol_map = load_sol_map(args.sol_zip, {c.n for c in cases})
    else:
        print(f"warning: sol zip not found, expected TT comparison disabled: {args.sol_zip}", file=sys.stderr)

    if args.check_branch:
        if not args.branch_exe.exists():
            print(f"branch exe not found: {args.branch_exe}", file=sys.stderr)
            return 2
        if not args.branch_workdir.exists():
            print(f"branch workdir not found: {args.branch_workdir}", file=sys.stderr)
            return 2

    base_branch_config = Path(args.branch_config)
    if not base_branch_config.is_absolute():
        base_branch_config = (args.branch_workdir / base_branch_config).resolve()
    runtime_branch_config = write_branch_runtime_config(
        base_branch_config,
        paths.root / "branch_runtime.ini",
        branch_limits.memory_limit_mb,
    )

    write_selected_cases_csv(paths.selected_cases_csv, cases, sol_map, generated_seed_map)
    manifest_payload = {
        "experiment_name": experiment_name,
        "preset": args.preset or "",
        "data_root": data_root,
        "mine_exe": args.mine_exe,
        "branch_exe": args.branch_exe if args.check_branch else "",
        "branch_workdir": args.branch_workdir if args.check_branch else "",
        "branch_runtime_config": runtime_branch_config if args.check_branch else "",
        "n_filter": list_from_filter(n_filter),
        "r_filter": list_from_filter(r_filter, float) if r_filter is not None else None,
        "t_filter": list_from_filter(t_filter, float) if t_filter is not None else None,
        "k_filter": list_from_filter(k_filter),
        "repeat_mine": repeat_mine,
        "repeat_branch": repeat_branch,
        "mine_limits": {"timeout_sec": mine_limits.timeout_sec, "memory_limit_mb": mine_limits.memory_limit_mb},
        "branch_limits": {"timeout_sec": branch_limits.timeout_sec, "memory_limit_mb": branch_limits.memory_limit_mb},
        "mine_args": mine_args,
        "total_cases": len(cases),
        "generated_records": len(generated_records),
    }
    write_manifest(paths.manifest_json, manifest_payload)

    detail_rows: List[dict] = []
    if args.jobs <= 0:
        args.jobs = 1
    if args.stop_on_mismatch and args.jobs > 1:
        print("warning: --stop-on-mismatch with --jobs>1 is best-effort", file=sys.stderr)
    branch_data_root = args.branch_data_root if args.branch_data_root is not None else (args.branch_workdir / "data")
    branch_lock = threading.Lock() if args.check_branch and args.jobs > 1 else None

    print(f"experiment={experiment_name} cases={len(cases)} out={paths.root}")
    print(f"mine_exe={args.mine_exe}")
    if args.check_branch:
        print(f"branch_exe={args.branch_exe} workdir={args.branch_workdir} data_root={branch_data_root}")

    csv_file = paths.detail_csv.open("w", newline="", encoding="utf-8")
    csv_writer = csv.DictWriter(csv_file, fieldnames=DETAIL_FIELDNAMES)
    csv_writer.writeheader()

    def process_case(case: Case):
        expected = sol_map.get(case.name)
        generated_seed = generated_seed_map.get(case.name)
        rows: List[dict] = []
        total_repeats = max(repeat_mine, repeat_branch)
        for repeat_idx in range(1, total_repeats + 1):
            if STOP_REQUESTED.is_set():
                announce_stop_request()
                mine = make_stopped_result()
                branch = make_stopped_result() if args.check_branch else None
            else:
                if repeat_idx <= repeat_mine:
                    try:
                        mine = run_mine(
                            args.mine_exe,
                            case,
                            mine_args,
                            mine_limits.timeout_sec,
                            mine_limits.memory_limit_mb,
                        )
                    except subprocess.TimeoutExpired:
                        mine = make_timeout_result(mine_limits.timeout_sec)
                    except Exception as ex:
                        mine = make_exception_result("mine", ex)
                else:
                    mine = None

                if args.check_branch and repeat_idx <= repeat_branch:
                    try:
                        if branch_lock is not None:
                            with branch_lock:
                                ensure_branch_case_input(case, branch_data_root)
                                branch = run_branch(
                                    args.branch_exe,
                                    args.branch_workdir,
                                    str(runtime_branch_config),
                                    case,
                                    branch_limits.timeout_sec,
                                    branch_limits.memory_limit_mb,
                                )
                        else:
                            ensure_branch_case_input(case, branch_data_root)
                            branch = run_branch(
                                args.branch_exe,
                                args.branch_workdir,
                                str(runtime_branch_config),
                                case,
                                branch_limits.timeout_sec,
                                branch_limits.memory_limit_mb,
                            )
                    except subprocess.TimeoutExpired:
                        branch = make_timeout_result(branch_limits.timeout_sec)
                    except Exception as ex:
                        branch = make_exception_result("branch", ex)
                else:
                    branch = None

            mine_ok = bool(mine and mine["rc"] == 0 and mine.get("cost") is not None and not mine.get("oom"))
            branch_ok = bool(
                branch and branch["rc"] == 0 and branch.get("tt") is not None and not branch.get("oom")
            ) if args.check_branch else None
            mine_status = run_status_from_result(mine, mine_ok)
            branch_status = run_status_from_result(branch, bool(branch_ok)) if args.check_branch else "not_run"

            status_parts: List[str] = []
            if mine_status != "ok":
                status_parts.append(f"mine_{mine_status}")
            if expected is not None and mine_ok and int(mine["cost"]) != int(expected):
                status_parts.append("mine_vs_expected_mismatch")
            if args.check_branch and branch_status != "ok":
                status_parts.append(f"branch_{branch_status}")
            if args.check_branch and mine_ok and branch_ok and int(mine["cost"]) != int(branch["tt"]):
                status_parts.append("mine_vs_branch_mismatch")
            status = join_status_parts(status_parts)

            mine_elapsed_ms = mine.get("elapsed_ms") if mine else None
            branch_elapsed_ms = branch.get("elapsed_ms") if branch else None
            mine_peak_rss_bytes = mine.get("peak_rss_bytes") if mine else None
            mine_peak_rss_mb = (float(mine_peak_rss_bytes) / (1024.0 * 1024.0)) if mine_peak_rss_bytes else None
            mine_peak_private_bytes = mine.get("peak_private_bytes") if mine else None
            mine_peak_private_mb = (float(mine_peak_private_bytes) / (1024.0 * 1024.0)) if mine_peak_private_bytes else None
            mine_peak_commit_bytes = mine.get("peak_commit_bytes") if mine else None
            mine_peak_commit_mb = (float(mine_peak_commit_bytes) / (1024.0 * 1024.0)) if mine_peak_commit_bytes else None
            mine_peak_pagefile_bytes = mine.get("peak_pagefile_bytes") if mine else None
            mine_peak_pagefile_mb = (float(mine_peak_pagefile_bytes) / (1024.0 * 1024.0)) if mine_peak_pagefile_bytes else None

            branch_peak_rss_bytes = branch.get("peak_rss_bytes") if branch else None
            branch_peak_rss_mb = (float(branch_peak_rss_bytes) / (1024.0 * 1024.0)) if branch_peak_rss_bytes else None
            branch_peak_private_bytes = branch.get("peak_private_bytes") if branch else None
            branch_peak_private_mb = (float(branch_peak_private_bytes) / (1024.0 * 1024.0)) if branch_peak_private_bytes else None
            branch_peak_commit_bytes = branch.get("peak_commit_bytes") if branch else None
            branch_peak_commit_mb = (float(branch_peak_commit_bytes) / (1024.0 * 1024.0)) if branch_peak_commit_bytes else None
            branch_peak_pagefile_bytes = branch.get("peak_pagefile_bytes") if branch else None
            branch_peak_pagefile_mb = (float(branch_peak_pagefile_bytes) / (1024.0 * 1024.0)) if branch_peak_pagefile_bytes else None
            branch_ram_bytes = branch.get("ram_bytes") if branch else None
            branch_ram_mb = (float(branch_ram_bytes) / (1024.0 * 1024.0)) if branch_ram_bytes else None

            row = {
                "experiment": experiment_name,
                "dataset_kind": "generated" if generated_seed is not None else ("official" if expected is not None else "custom"),
                "repeat_idx": repeat_idx,
                "repeat_count": total_repeats,
                "case": case.name,
                "data_path": str(case.path),
                "n": case.n,
                "r": case.r,
                "t": case.t,
                "k": case.k,
                "branch_id": case.branch_id,
                "generated_seed": generated_seed if generated_seed is not None else "",
                "mine_timeout_sec": fmt_float(mine_limits.timeout_sec),
                "mine_memory_limit_mb": mine_limits.memory_limit_mb if mine_limits.memory_limit_mb is not None else "",
                "branch_timeout_sec": fmt_float(branch_limits.timeout_sec),
                "branch_memory_limit_mb": branch_limits.memory_limit_mb if branch_limits.memory_limit_mb is not None else "",
                "expected_tt": expected if expected is not None else "",
                "mine_tt": mine.get("cost") if mine and mine.get("cost") is not None else "",
                "mine_time_ms": fmt_float(mine.get("time_ms")) if mine else "",
                "mine_elapsed_ms": fmt_float(mine_elapsed_ms),
                "mine_rc": mine["rc"] if mine is not None else "",
                "mine_ok": int(mine_ok) if mine is not None else "",
                "mine_status": mine_status,
                "mine_peak_rss_bytes": mine_peak_rss_bytes if mine_peak_rss_bytes is not None else "",
                "mine_peak_rss_mb": fmt_float(mine_peak_rss_mb),
                "mine_peak_private_bytes": mine_peak_private_bytes if mine_peak_private_bytes is not None else "",
                "mine_peak_private_mb": fmt_float(mine_peak_private_mb),
                "mine_peak_commit_bytes": mine_peak_commit_bytes if mine_peak_commit_bytes is not None else "",
                "mine_peak_commit_mb": fmt_float(mine_peak_commit_mb),
                "mine_peak_pagefile_bytes": mine_peak_pagefile_bytes if mine_peak_pagefile_bytes is not None else "",
                "mine_peak_pagefile_mb": fmt_float(mine_peak_pagefile_mb),
                "mine_memo_used_mb": fmt_float(mine.get("memo_used_mb")) if mine else "",
                "mine_memo_peak_entries": mine.get("memo_peak_entries") if mine and mine.get("memo_peak_entries") is not None else "",
                "mine_nodes": mine.get("nodes") if mine and mine.get("nodes") is not None else "",
                "mine_oom_kind": mine.get("oom_kind", "") if mine else "",
                "branch_tt": branch.get("tt") if branch and branch.get("tt") is not None else "",
                "branch_cpu_s": branch.get("cpu_s") if branch and branch.get("cpu_s") is not None else "",
                "branch_wall_s": branch.get("wall_s") if branch and branch.get("wall_s") is not None else "",
                "branch_cpu_ms_internal": fmt_float(branch.get("cpu_ms_precise")) if branch else "",
                "branch_wall_ms_internal": fmt_float(branch.get("wall_ms_precise")) if branch else "",
                "branch_wall_ms": fmt_float(branch.get("wall_ms_precise")) if branch and branch.get("wall_ms_precise") is not None else "",
                "branch_elapsed_ms": fmt_float(branch_elapsed_ms),
                "branch_rc": branch["rc"] if branch is not None else "",
                "branch_ok": int(bool(branch_ok)) if branch_ok is not None else "",
                "branch_status": branch_status,
                "branch_peak_rss_bytes": branch_peak_rss_bytes if branch_peak_rss_bytes is not None else "",
                "branch_peak_rss_mb": fmt_float(branch_peak_rss_mb),
                "branch_peak_private_bytes": branch_peak_private_bytes if branch_peak_private_bytes is not None else "",
                "branch_peak_private_mb": fmt_float(branch_peak_private_mb),
                "branch_peak_commit_bytes": branch_peak_commit_bytes if branch_peak_commit_bytes is not None else "",
                "branch_peak_commit_mb": fmt_float(branch_peak_commit_mb),
                "branch_peak_pagefile_bytes": branch_peak_pagefile_bytes if branch_peak_pagefile_bytes is not None else "",
                "branch_peak_pagefile_mb": fmt_float(branch_peak_pagefile_mb),
                "branch_ram_bytes": branch_ram_bytes if branch_ram_bytes is not None else "",
                "branch_ram_mb": fmt_float(branch_ram_mb),
                "branch_nbbytesmem": branch.get("nb_bytes_mem") if branch and branch.get("nb_bytes_mem") is not None else "",
                "branch_nbentrymem": branch.get("nb_entry_mem") if branch and branch.get("nb_entry_mem") is not None else "",
                "branch_oom_kind": branch.get("oom_kind", "") if branch else "",
                "mine_over_branch_wall": fmt_float(safe_ratio(mine_elapsed_ms, branch_elapsed_ms), 6),
                "branch_over_mine": fmt_float(safe_ratio(branch_elapsed_ms, mine_elapsed_ms), 6),
                "mine_over_branch_peak_rss": fmt_float(safe_ratio(mine_peak_rss_mb, branch_peak_rss_mb), 6),
                "branch_over_mine_peak_rss": fmt_float(safe_ratio(branch_peak_rss_mb, mine_peak_rss_mb), 6),
                "status": status,
            }
            rows.append(row)
        return rows

    def print_case_progress(case_rows: List[dict], done_idx: int, total_cases: int) -> bool:
        detail_rows.extend(case_rows)
        for row in case_rows:
            csv_writer.writerow(row)
        csv_file.flush()

        first = case_rows[0]
        statuses = [str(r["status"]) for r in case_rows]
        case_status = "ok" if all(s == "ok" for s in statuses) else join_status_parts(statuses)
        mine_times = [safe_float(r.get("mine_elapsed_ms")) for r in case_rows if safe_float(r.get("mine_elapsed_ms")) is not None]
        branch_times = [safe_float(r.get("branch_elapsed_ms")) for r in case_rows if safe_float(r.get("branch_elapsed_ms")) is not None]
        mine_ms_console = safe_median(mine_times)
        branch_ms_console = safe_median(branch_times)
        progress = f"[{done_idx}/{total_cases}] "
        if args.console_ms_only:
            line = (
                progress
                + f"{case_status:20s} {first['case']:24s} "
                + f"mine_ms={mine_ms_console if mine_ms_console is not None else 0:>10.2f}"
            )
            if branch_ms_console is not None:
                line += f" branch_ms={branch_ms_console:>10.2f}"
                if mine_ms_console is not None and branch_ms_console > 0:
                    line += f" ratio={mine_ms_console / branch_ms_console:>7.3f}x"
            print(line)
        else:
            print(
                progress
                + f"{case_status:24s} {first['case']:24s} "
                + f"exp={first['expected_tt'] if first['expected_tt'] != '' else '-':>10} "
                + f"mine={first['mine_tt'] if first['mine_tt'] != '' else '-':>10} "
                + f"mine_ms={mine_ms_console if mine_ms_console is not None else 0:>10.2f}"
                + (
                    f" branch={first['branch_tt'] if first['branch_tt'] != '' else '-':>10}"
                    f" branch_ms={branch_ms_console if branch_ms_console is not None else 0:>10.2f}"
                    if args.check_branch
                    else ""
                )
            )
        return case_status == "ok"

    total = 0
    failures = 0
    mismatches = 0
    try:
        if args.jobs == 1:
            for idx, case in enumerate(cases, start=1):
                ok = print_case_progress(process_case(case), idx, len(cases))
                total += 1
                if not ok:
                    if "mismatch" in detail_rows[-1]["status"]:
                        mismatches += 1
                    else:
                        failures += 1
                if STOP_REQUESTED.is_set():
                    break
                if args.stop_on_mismatch and not ok:
                    break
        else:
            futures = []
            with ThreadPoolExecutor(max_workers=args.jobs) as ex:
                for case in cases:
                    futures.append(ex.submit(process_case, case))
                done = 0
                for fut in as_completed(futures):
                    done += 1
                    rows = fut.result()
                    ok = print_case_progress(rows, done, len(cases))
                    total += 1
                    if not ok:
                        if any("mismatch" in str(r.get("status")) for r in rows):
                            mismatches += 1
                        else:
                            failures += 1
                    if STOP_REQUESTED.is_set():
                        for pending in futures:
                            pending.cancel()
                        break
                    if args.stop_on_mismatch and not ok:
                        pass
    finally:
        csv_file.close()

    case_rows = compute_case_summary_rows(detail_rows)
    grid_rows = compute_grid_summary_rows(case_rows)
    frontier_rows = compute_frontier_rows(case_rows)
    write_rows_csv(paths.case_summary_csv, CASE_SUMMARY_FIELDNAMES, case_rows)
    write_rows_csv(paths.grid_summary_csv, GRID_SUMMARY_FIELDNAMES, grid_rows)
    write_rows_csv(paths.frontier_csv, FRONTIER_FIELDNAMES, frontier_rows)

    print(f"summary: total_cases={total} mismatches={mismatches} failures={failures}")
    if STOP_REQUESTED.is_set():
        reason = f"signal {STOP_SIGNAL}" if STOP_SIGNAL is not None else "external request"
        print(f"stopped_early: reason={reason}")
    print(f"detail csv written: {paths.detail_csv}")
    print(f"case summary written: {paths.case_summary_csv}")
    print(f"grid summary written: {paths.grid_summary_csv}")
    print(f"frontier csv written: {paths.frontier_csv}")
    print(f"manifest written: {paths.manifest_json}")

    return 1 if (mismatches or failures) else 0


def parse_n_range(n_from: Optional[int], n_to: Optional[int], n_step: int) -> Optional[set]:
    if n_from is None or n_to is None:
        return None
    step = int(n_step) if int(n_step) > 0 else 1
    if n_from > n_to:
        n_from, n_to = n_to, n_from
    return set(range(int(n_from), int(n_to) + 1, step))


def list_from_filter(values, sort_key=None):
    if values is None:
        return None
    items = list(values)
    if sort_key is None:
        items.sort()
    else:
        items.sort(key=sort_key)
    return items


def has_cli_option(args_list: List[str], flag: str) -> bool:
    return any(arg == flag for arg in args_list)


def has_cli_option_with_value(args_list: List[str], flag: str) -> bool:
    for idx, arg in enumerate(args_list):
        if arg == flag:
            return True
        if arg.startswith(flag + "="):
            return True
        if idx > 0 and args_list[idx - 1] == flag:
            return True
    return False


def ensure_mine_args_limits(args_list: List[str], memory_limit_mb: Optional[int]) -> List[str]:
    out = list(args_list)
    if memory_limit_mb is not None and not has_cli_option_with_value(out, "--mem-budget-mb"):
        out.extend(["--mem-budget-mb", str(int(memory_limit_mb))])
    if memory_limit_mb is not None and not has_cli_option(out, "--process-memory-gate"):
        out.append("--process-memory-gate")
    if not has_cli_option(out, "--no-reconstruct") and not has_cli_option(out, "--reconstruct"):
        out.append("--no-reconstruct")
    return out


def update_ini_text(base_text: str, overrides: Dict[str, str]) -> str:
    lines = base_text.splitlines()
    remaining = {str(k): str(v) for k, v in overrides.items()}
    out_lines: List[str] = []
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in line:
            out_lines.append(line)
            continue
        key, _value = line.split("=", 1)
        key = key.strip()
        if key in remaining:
            out_lines.append(f"{key}={remaining.pop(key)}")
        else:
            out_lines.append(line)
    for key, value in remaining.items():
        out_lines.append(f"{key}={value}")
    return "\n".join(out_lines) + "\n"


def write_branch_runtime_config(
    base_config_path: Path,
    out_path: Path,
    memory_limit_mb: Optional[int],
) -> Path:
    base_text = ""
    if base_config_path.exists():
        base_text = base_config_path.read_text(encoding="utf-8", errors="ignore")
    overrides = {
        "ONLY_HARDEST": "0",
    }
    if memory_limit_mb is not None:
        overrides["ENABLE_MEM_PB"] = "1"
        overrides["RAM_LIM_MO"] = str(int(memory_limit_mb))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(update_ini_text(base_text, overrides), encoding="utf-8")
    return out_path


def make_experiment_name(prefix: str) -> str:
    safe_prefix = re.sub(r"[^A-Za-z0-9._-]+", "-", prefix).strip("-")
    if not safe_prefix:
        safe_prefix = "experiment"
    timestamp = dt.datetime.now().strftime(EXPERIMENT_TS_FMT)
    return f"{timestamp}_{safe_prefix}"


def ensure_experiment_paths(root: Path) -> ExperimentPaths:
    root.mkdir(parents=True, exist_ok=True)
    return ExperimentPaths(
        root=root,
        detail_csv=root / "detail.csv",
        case_summary_csv=root / "case_summary.csv",
        grid_summary_csv=root / "grid_summary.csv",
        frontier_csv=root / "frontier.csv",
        manifest_json=root / "manifest.json",
        selected_cases_csv=root / "selected_cases.csv",
        generated_cases_csv=root / "generated_cases.csv",
    )


def write_rows_csv(path: Path, fieldnames: List[str], rows: Iterable[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def serialize_path(value):
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, dict):
        return {k: serialize_path(v) for k, v in value.items()}
    if isinstance(value, list):
        return [serialize_path(v) for v in value]
    return value


def write_manifest(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(serialize_path(payload), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def safe_ratio(numerator: Optional[float], denominator: Optional[float]) -> Optional[float]:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return float(numerator) / float(denominator)


def join_status_parts(parts: Iterable[str]) -> str:
    uniq = []
    for part in parts:
        if part and part not in uniq:
            uniq.append(part)
    return "+".join(uniq) if uniq else "ok"


def run_status_from_result(result: Optional[dict], ok_condition: bool) -> str:
    if result is None:
        return "not_run"
    if result.get("stopped"):
        return "stopped"
    if result.get("oom"):
        return "oom"
    if result.get("timed_out"):
        return "timeout"
    if ok_condition:
        return "ok"
    return "fail"


def make_timeout_result(timeout_sec: float) -> dict:
    return {
        "rc": -9,
        "stdout": "",
        "stderr": "timeout",
        "cost": None,
        "tt": None,
        "time_ms": None,
        "elapsed_ms": float(timeout_sec) * 1000.0,
        "peak_rss_bytes": None,
        "peak_private_bytes": None,
        "peak_commit_bytes": None,
        "peak_pagefile_bytes": None,
        "memo_used_mb": None,
        "memo_peak_entries": None,
        "nodes": None,
        "cpu_s": None,
        "wall_s": None,
        "cpu_ms_precise": None,
        "wall_ms_precise": None,
        "ram_bytes": None,
        "nb_bytes_mem": None,
        "nb_entry_mem": None,
        "stopped": False,
        "timed_out": True,
        "oom": False,
        "oom_kind": "",
    }


def make_exception_result(prefix: str, ex: Exception) -> dict:
    return {
        "rc": -10,
        "stdout": "",
        "stderr": f"{prefix}_exception: {ex}",
        "cost": None,
        "tt": None,
        "time_ms": None,
        "elapsed_ms": None,
        "peak_rss_bytes": None,
        "peak_private_bytes": None,
        "peak_commit_bytes": None,
        "peak_pagefile_bytes": None,
        "memo_used_mb": None,
        "memo_peak_entries": None,
        "nodes": None,
        "cpu_s": None,
        "wall_s": None,
        "cpu_ms_precise": None,
        "wall_ms_precise": None,
        "ram_bytes": None,
        "nb_bytes_mem": None,
        "nb_entry_mem": None,
        "stopped": False,
        "timed_out": False,
        "oom": False,
        "oom_kind": "",
    }


def make_stopped_result() -> dict:
    return {
        "rc": -11,
        "stdout": "",
        "stderr": "stopped",
        "cost": None,
        "tt": None,
        "time_ms": None,
        "elapsed_ms": None,
        "peak_rss_bytes": None,
        "peak_private_bytes": None,
        "peak_commit_bytes": None,
        "peak_pagefile_bytes": None,
        "memo_used_mb": None,
        "memo_peak_entries": None,
        "nodes": None,
        "cpu_s": None,
        "wall_s": None,
        "cpu_ms_precise": None,
        "wall_ms_precise": None,
        "ram_bytes": None,
        "nb_bytes_mem": None,
        "nb_entry_mem": None,
        "stopped": True,
        "timed_out": False,
        "oom": False,
        "oom_kind": "",
    }


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    article_root = detect_article_repo_root(repo_root)

    signal.signal(signal.SIGINT, on_interrupt_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_interrupt_signal)

    rebuild_rc = handle_rebuild_summary(args)
    if rebuild_rc >= 0:
        return rebuild_rc

    try:
        apply_preset_defaults(args, repo_root, article_root)
    except ValueError as ex:
        print(str(ex), file=sys.stderr)
        return 2

    suite_runs = prepare_suite_args(args, repo_root, article_root)
    final_rc = 0
    for idx, (name, exp_args) in enumerate(suite_runs, start=1):
        if len(suite_runs) > 1:
            print(f"=== suite {idx}/{len(suite_runs)}: {name} ===")
        rc = execute_experiment(exp_args, repo_root, article_root)
        if rc != 0:
            final_rc = rc
            if exp_args.stop_on_mismatch:
                break
    return final_rc


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as ex:
        print(f"timeout: {ex.cmd}", file=sys.stderr)
        raise
