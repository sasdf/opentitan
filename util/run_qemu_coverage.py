#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Unified Coverage Runner, Aggregate Collector, and Per-Test SQLite Indexer for QEMU.

Given one or more Bazel test target patterns (e.g. //sw/device/tests/...), or one or more
existing Bazel Build Event Protocol (`--bep-file`) JSON files, this script:
1. Optionally runs `./bazelisk.sh test --config=qemu_coverage` with `--build_event_json_file`
   so Bazel selects `@qemu_opentitan//:build/qemu-system-riscv32-cov` inside the sandbox.
2. Parses `<bep.json>` to locate `testActionOutput` (`test.outputs/gcov/*.gcda` or `outputs.zip`)
   for all executed and cached test targets.
3. Unless `--skip-aggregate-report` is set, merges `.gcda` files in parallel against `.gcno` files
   in `@qemu_opentitan` (`libsystem-ot-cov.a.p` and `libqemu-riscv32-softmmu-ot-cov.a.p`) and writes
   `uncovered_chunks.json` and `summary.md` across all QEMU sources.
4. When `--per-test-db` (`--out-db`) is specified (e.g. `/root/coverage/per_test_coverage.sqlite`),
   processes each test's `.gcda` outputs across multiprocessing workers and writes the per-test
   interval coverage SQLite database (`per_test_coverage.sqlite` + `per_test_coverage_meta.json`).
"""

import argparse
import atexit
import concurrent.futures
import datetime
import glob
import json
import multiprocessing
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.parse
import zipfile
from typing import Dict, List, Optional, Tuple

COV_SUBDIRS = (
    "libsystem-ot-cov.a.p",
    "libqemu-riscv32-softmmu-ot-cov.a.p",
)


def normalize_qemu_rel_path(f_path: str, qemu_dir: str) -> str:
    rel_path = f_path
    while rel_path.startswith("../"):
        rel_path = rel_path[3:]
    if rel_path.startswith(qemu_dir + "/"):
        return rel_path[len(qemu_dir) + 1:]
    for marker in ("/qemu/", "/+qemu+qemu_opentitan/", "/qemu_opentitan/"):
        idx = rel_path.find(marker)
        if idx != -1:
            return rel_path[idx + len(marker):]
    return rel_path


def is_qemu_source(
    rel_path: str,
    qemu_dir: str,
    existing_cache: Optional[Dict[str, bool]] = None,
) -> bool:
    """Include all source/header/inc files that exist in the QEMU repository."""
    if not rel_path or rel_path.startswith("/"):
        return False
    if existing_cache is not None:
        cached = existing_cache.get(rel_path)
        if cached is not None:
            return cached
        exists = os.path.isfile(os.path.join(qemu_dir, rel_path))
        existing_cache[rel_path] = exists
        return exists
    return os.path.isfile(os.path.join(qemu_dir, rel_path))


def load_expected_gcno_stamps(cov_build_dir: str) -> Dict[str, bytes]:
    """Return map of '<stem>.gcda' -> 8-byte (version, stamp) from .gcno header[4:12]."""
    stamps: Dict[str, bytes] = {}
    for sub in COV_SUBDIRS:
        for gcno_path in glob.glob(os.path.join(cov_build_dir, sub, "*.gcno")):
            try:
                with open(gcno_path, "rb") as f:
                    hdr = f.read(12)
                if len(hdr) == 12 and hdr[:4] == b"oncg":
                    gcda_name = os.path.basename(gcno_path)[:-5] + ".gcda"
                    stamps[gcda_name] = hdr[4:12]
            except Exception:
                pass
    return stamps


def get_jj_commit(repo_path: str, rev: str = "mainline") -> str:
    try:
        res = subprocess.run(
            [
                "jj",
                "--no-pager",
                "--ignore-working-copy",
                "-R",
                repo_path,
                "log",
                "-r",
                rev,
                "-T",
                "commit_id",
                "--no-graph",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        return res.stdout.strip()
    except Exception:
        return "unknown"


def split_bazel_args_and_targets(extra: List[str]) -> Tuple[List[str], List[str]]:
    """Partition unconsumed CLI arguments into Bazel flags and target patterns."""
    bazel_flags: List[str] = []
    targets: List[str] = []
    bool_short_flags = {"-k", "-s", "-q", "-i"}
    i = 0
    while i < len(extra):
        tok = extra[i]
        if tok == "--":
            targets.extend(extra[i + 1:])
            break
        if tok.startswith(("//", "@", ":", "-//", "-@", "-:")):
            targets.append(tok)
            i += 1
        elif tok.startswith("-"):
            bazel_flags.append(tok)
            if (
                "=" not in tok and
                tok not in bool_short_flags and
                not tok.startswith("--no") and
                i + 1 < len(extra) and
                not extra[i + 1].startswith(("-", "//", "@", ":"))
            ):
                bazel_flags.append(extra[i + 1])
                i += 2
            else:
                i += 1
        else:
            targets.append(tok)
            i += 1
    return bazel_flags, targets


def run_bazel_coverage_tests(
    ot_dir: str,
    targets: List[str],
    bep_path: str,
    bazel_args: List[str],
) -> int:
    """Run bazelisk.sh test with --config=qemu_coverage and write BEP JSON to bep_path."""
    has_tag_filter = any(
        a == "--test_tag_filters" or a.startswith("--test_tag_filters=")
        for a in bazel_args
    )
    cmd = [
        "./bazelisk.sh",
        "test",
        "--config=qemu_coverage",
        f"--build_event_json_file={bep_path}",
    ]
    if not has_tag_filter:
        cmd.append("--test_tag_filters=qemu,-broken")
    cmd.extend(bazel_args)
    if targets:
        cmd.extend(targets)
    elif not any(a.startswith("--target_pattern_file") for a in bazel_args):
        cmd.append("//sw/device/tests/...")

    print("Running Bazel coverage command:")
    print("  " + " ".join(cmd))
    res = subprocess.run(cmd, cwd=ot_dir)
    return res.returncode


def parse_bep_test_candidates(
    bep_files: List[str],
) -> Tuple[List[Tuple[str, Optional[str], Optional[str]]], Dict[str, int]]:
    """Extract unique test targets and their gcov_dir / outputs.zip paths from BEP JSON files."""
    by_label: Dict[str, Tuple[Optional[str], Optional[str]]] = {}
    stats = {
        "bep_test_results": 0,
        "cached_test_results": 0,
    }
    for bep_path in bep_files:
        if not bep_path or not os.path.exists(bep_path):
            continue
        with open(bep_path, "r", errors="replace") as bf:
            for line in bf:
                line = line.strip()
                if not line or '"testResult"' not in line:
                    continue
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                if "testResult" not in ev:
                    continue
                tr_id = ev.get("id", {}).get("testResult", {})
                label = tr_id.get("label", "")
                if not label:
                    continue
                tr = ev["testResult"]
                stats["bep_test_results"] += 1
                if tr.get("cachedAttempt") or tr.get("executionInfo", {}).get("cachedRemotely"):
                    stats["cached_test_results"] += 1
                zip_uri = None
                gcov_dir = None
                for out_entry in tr.get("testActionOutput", []):
                    name = out_entry.get("name", "")
                    uri = out_entry.get("uri", "")
                    if not uri.startswith("file://"):
                        continue
                    path = urllib.parse.unquote(uri[len("file://"):])
                    if name.endswith(".gcda") or path.endswith(".gcda"):
                        parent = os.path.dirname(path)
                        if os.path.isdir(parent):
                            gcov_dir = parent
                            break
                    elif name == "test.outputs__outputs.zip" or path.endswith("outputs.zip"):
                        unpacked = os.path.join(os.path.dirname(path), "gcov")
                        if os.path.isdir(unpacked):
                            gcov_dir = unpacked
                            break
                        elif os.path.isfile(path):
                            zip_uri = path
                if gcov_dir or zip_uri:
                    by_label[label] = (gcov_dir, zip_uri)
    return sorted((lbl, gd, zp) for lbl, (gd, zp) in by_label.items()), stats


def extract_gcov_dirs_from_bep(
    bep_files: List[str],
    staging_root: str,
    expected_stamps: Dict[str, bytes],
) -> Tuple[List[str], Dict[str, int]]:
    """Parse BEP JSON file(s) and stage valid .gcda directories matching expected .gcno stamps."""
    candidates, base_stats = parse_bep_test_candidates(bep_files)
    stats = {
        "bep_test_results": base_stats["bep_test_results"],
        "cached_test_results": base_stats["cached_test_results"],
        "valid_gcov_dirs": 0,
        "skipped_missing_outputs": 0,
        "skipped_stamp_mismatch_files": 0,
    }

    staged_dirs: List[str] = []
    os.makedirs(staging_root, exist_ok=True)

    for idx, (_label, gcov_dir, zip_path) in enumerate(candidates):
        dest = os.path.join(staging_root, f"t_{idx}")
        matched_count = 0

        if gcov_dir and os.path.isdir(gcov_dir):
            for fn in os.listdir(gcov_dir):
                if not fn.endswith(".gcda"):
                    continue
                expected_hdr = expected_stamps.get(fn)
                if not expected_hdr:
                    continue
                src_f = os.path.join(gcov_dir, fn)
                try:
                    with open(src_f, "rb") as gf:
                        hdr = gf.read(12)
                    if len(hdr) == 12 and hdr[:4] == b"adcg" and hdr[4:12] == expected_hdr:
                        os.makedirs(dest, exist_ok=True)
                        os.symlink(src_f, os.path.join(dest, fn))
                        matched_count += 1
                    else:
                        stats["skipped_stamp_mismatch_files"] += 1
                except Exception:
                    pass
        elif zip_path and os.path.isfile(zip_path):
            try:
                with zipfile.ZipFile(zip_path, "r") as zf:
                    for member in zf.namelist():
                        if not member.endswith(".gcda"):
                            continue
                        fn = os.path.basename(member)
                        expected_hdr = expected_stamps.get(fn)
                        if not expected_hdr:
                            continue
                        data = zf.read(member)
                        if len(data) >= 12 and data[:4] == b"adcg" and data[4:12] == expected_hdr:
                            os.makedirs(dest, exist_ok=True)
                            with open(os.path.join(dest, fn), "wb") as out_f:
                                out_f.write(data)
                            matched_count += 1
                        else:
                            stats["skipped_stamp_mismatch_files"] += 1
            except Exception:
                pass

        if matched_count > 0:
            staged_dirs.append(dest)
            stats["valid_gcov_dirs"] += 1
        else:
            stats["skipped_missing_outputs"] += 1

    return staged_dirs, stats


def merge_two_dirs(args: Tuple[str, str, str]) -> str:
    d1, d2, out_d = args
    os.makedirs(out_d, exist_ok=True)
    subprocess.check_call(["gcov-tool", "merge", d1, d2, "-o", out_d])
    return out_d


def parallel_tree_merge(dirs: List[str], merge_root: str) -> str:
    if not dirs:
        empty = os.path.join(merge_root, "empty")
        os.makedirs(empty, exist_ok=True)
        return empty
    if len(dirs) == 1:
        return dirs[0]

    current = list(dirs)
    level = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
        while len(current) > 1:
            next_level = []
            tasks = []
            for i in range(0, len(current), 2):
                if i + 1 < len(current):
                    out_d = os.path.join(merge_root, f"lvl_{level}_{i // 2}")
                    tasks.append((current[i], current[i + 1], out_d))
                else:
                    next_level.append(current[i])
            merged = list(pool.map(merge_two_dirs, tasks))
            next_level.extend(merged)
            current = next_level
            level += 1
    return current[0]


def run_gcov_on_obj(args: Tuple[str, str, str]) -> Dict:
    eval_build_dir, obj_rel, qemu_dir = args
    raw = subprocess.check_output(
        ["gcov", "-j", "-t", obj_rel],
        cwd=eval_build_dir,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    data = json.loads(raw)
    results = {}
    existing_cache: Dict[str, bool] = {}
    for f_entry in data.get("files", []):
        f_path = f_entry.get("file", "")
        rel_path = normalize_qemu_rel_path(f_path, qemu_dir)
        if not is_qemu_source(rel_path, qemu_dir, existing_cache):
            continue

        line_map = {}
        func_for_line = {}
        for l_info in f_entry.get("lines", []):
            ln = l_info["line_number"]
            cnt = l_info["count"]
            line_map[ln] = max(line_map.get(ln, 0), cnt)
            if l_info.get("function_name"):
                func_for_line[ln] = l_info["function_name"]

        funcs = {}
        for fn in f_entry.get("functions", []):
            name = fn["name"]
            cnt = fn["execution_count"]
            if name not in funcs or cnt > funcs[name]["execution_count"]:
                funcs[name] = {
                    "name": name,
                    "start_line": fn["start_line"],
                    "end_line": fn.get("end_line", fn["start_line"]),
                    "execution_count": cnt,
                }

        results[rel_path] = {
            "line_map": line_map,
            "func_for_line": func_for_line,
            "funcs": funcs,
        }
    return results


def build_file_summary(rel_path: str, raw_entry: Dict, qemu_dir: str) -> Dict:
    src_abs = os.path.join(qemu_dir, rel_path)
    src_lines = []
    if os.path.exists(src_abs):
        with open(src_abs, "r", errors="replace") as sf:
            src_lines = sf.readlines()

    line_map = raw_entry["line_map"]
    func_for_line = raw_entry["func_for_line"]
    funcs = list(raw_entry["funcs"].values())

    exec_lines = sorted(line_map.keys())
    covered_lines = [ln for ln in exec_lines if line_map[ln] > 0]
    uncovered_lines = [ln for ln in exec_lines if line_map[ln] == 0]

    chunks = []
    if uncovered_lines:
        start = uncovered_lines[0]
        prev = start
        chunk_unexec = [start]
        for ln in uncovered_lines[1:]:
            has_covered_between = any(
                (prev < k < ln) and line_map[k] > 0 for k in exec_lines
            )
            if not has_covered_between and (ln - prev) <= 4:
                chunk_unexec.append(ln)
                prev = ln
            else:
                chunks.append((start, prev, chunk_unexec))
                start = ln
                prev = ln
                chunk_unexec = [ln]
        chunks.append((start, prev, chunk_unexec))

    formatted_chunks = []
    for c_start, c_end, c_lines in chunks:
        c_funcs = sorted({func_for_line.get(ln, "") for ln in c_lines if func_for_line.get(ln)})
        snippet = ""
        if src_lines:
            lo = max(1, c_start - 1)
            hi = min(len(src_lines), c_end + 1)
            snippet = "".join(
                f"{idx}: {src_lines[idx - 1]}" for idx in range(lo, hi + 1)
            )
        formatted_chunks.append({
            "start_line": c_start,
            "end_line": c_end,
            "uncovered_executable_lines": len(c_lines),
            "functions": c_funcs,
            "snippet": snippet,
        })

    line_cov_pct = (
        round(100.0 * len(covered_lines) / len(exec_lines), 2) if exec_lines else 100.0
    )
    return {
        "file": rel_path,
        "total_executable_lines": len(exec_lines),
        "covered_executable_lines": len(covered_lines),
        "uncovered_executable_lines": len(uncovered_lines),
        "line_coverage_pct": line_cov_pct,
        "total_functions": len(funcs),
        "covered_functions": sum(1 for fn in funcs if fn["execution_count"] > 0),
        "uncovered_functions": [fn["name"] for fn in funcs if fn["execution_count"] == 0],
        "function_coverage_pct": round(
            100.0 * sum(1 for fn in funcs if fn["execution_count"] > 0) / len(funcs), 2
        ) if funcs else 100.0,
        "uncovered_chunks": formatted_chunks,
    }


# ---------------------------------------------------------------------------
# Per-Test SQLite Coverage Indexer (merged from build_per_test_coverage.py)
# ---------------------------------------------------------------------------

_WORKER_TMP: Optional[str] = None
_WORKER_QEMU_DIR: str = ""
_WORKER_EXPECTED_STAMPS: Dict[str, bytes] = {}
_WORKER_GCDA_PATHS: Dict[str, str] = {}
_WORKER_OBJS: List[str] = []
_WORKER_EXISTS_CACHE: Dict[str, bool] = {}


def _init_per_test_worker(
    cov_build_dir: str, qemu_dir: str, expected_stamps: Dict[str, bytes]
) -> None:
    global _WORKER_TMP, _WORKER_QEMU_DIR, _WORKER_EXPECTED_STAMPS
    global _WORKER_GCDA_PATHS, _WORKER_OBJS, _WORKER_EXISTS_CACHE
    _WORKER_QEMU_DIR = qemu_dir
    _WORKER_EXPECTED_STAMPS = expected_stamps
    _WORKER_TMP = tempfile.mkdtemp(prefix=f"ptcov_w_{os.getpid()}_")
    atexit.register(shutil.rmtree, _WORKER_TMP, True)
    _WORKER_GCDA_PATHS = {}
    _WORKER_OBJS = []
    _WORKER_EXISTS_CACHE = {}

    for sub in COV_SUBDIRS:
        sub_dir = os.path.join(_WORKER_TMP, sub)
        os.makedirs(sub_dir, exist_ok=True)
        for gcno_path in sorted(glob.glob(os.path.join(cov_build_dir, sub, "*.gcno"))):
            base = os.path.basename(gcno_path)
            dst_gcno = os.path.join(sub_dir, base)
            os.symlink(gcno_path, dst_gcno)
            gcda_name = base[:-5] + ".gcda"
            _WORKER_GCDA_PATHS[gcda_name] = os.path.join(sub_dir, gcda_name)
            _WORKER_OBJS.append(os.path.join(sub, base[:-5] + ".o"))


def _process_single_test(
    task: Tuple[int, str, Optional[str], Optional[str], bool]
) -> Tuple[
    int,
    str,
    int,
    Dict[str, List[Tuple[int, int, int]]],
    Optional[Dict[str, Dict[int, str]]],
]:
    test_id, label, gcov_dir, zip_path, need_exec_lines = task
    assert _WORKER_TMP is not None

    for dst_gcda in _WORKER_GCDA_PATHS.values():
        if os.path.lexists(dst_gcda):
            try:
                os.unlink(dst_gcda)
            except OSError:
                pass

    matched_count = 0
    if gcov_dir and os.path.isdir(gcov_dir):
        try:
            for fn in os.listdir(gcov_dir):
                dst_gcda = _WORKER_GCDA_PATHS.get(fn)
                if not dst_gcda:
                    continue
                expected_hdr = _WORKER_EXPECTED_STAMPS.get(fn)
                if not expected_hdr:
                    continue
                src_f = os.path.join(gcov_dir, fn)
                try:
                    with open(src_f, "rb") as gf:
                        hdr = gf.read(12)
                    if len(hdr) == 12 and hdr[:4] == b"adcg" and hdr[4:12] == expected_hdr:
                        os.symlink(src_f, dst_gcda)
                        matched_count += 1
                except Exception:
                    pass
        except Exception:
            pass
    elif zip_path and os.path.isfile(zip_path):
        try:
            with zipfile.ZipFile(zip_path, "r") as zf:
                for member in zf.namelist():
                    if not member.endswith(".gcda"):
                        continue
                    fn = os.path.basename(member)
                    dst_gcda = _WORKER_GCDA_PATHS.get(fn)
                    if not dst_gcda:
                        continue
                    expected_hdr = _WORKER_EXPECTED_STAMPS.get(fn)
                    if not expected_hdr:
                        continue
                    data = zf.read(member)
                    if len(data) >= 12 and data[:4] == b"adcg" and data[4:12] == expected_hdr:
                        with open(dst_gcda, "wb") as out_f:
                            out_f.write(data)
                        matched_count += 1
        except Exception:
            pass

    if matched_count == 0:
        return test_id, label, 0, {}, None

    try:
        raw = subprocess.check_output(
            ["gcov", "-j", "-t"] + _WORKER_OBJS,
            cwd=_WORKER_TMP,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except Exception:
        return test_id, label, 0, {}, None

    file_line_counts: Dict[str, Dict[int, int]] = {}
    exec_lines_meta: Optional[Dict[str, Dict[int, str]]] = {} if need_exec_lines else None

    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            data = json.loads(line)
        except Exception:
            continue
        for f_entry in data.get("files", []):
            f_path = f_entry.get("file", "")
            rel_path = normalize_qemu_rel_path(f_path, _WORKER_QEMU_DIR)
            if not is_qemu_source(rel_path, _WORKER_QEMU_DIR, _WORKER_EXISTS_CACHE):
                continue

            lmap = file_line_counts.setdefault(rel_path, {})
            el_map = (
                exec_lines_meta.setdefault(rel_path, {})
                if exec_lines_meta is not None
                else None
            )

            for l_info in f_entry.get("lines", []):
                ln = l_info["line_number"]
                cnt = l_info["count"]
                if el_map is not None:
                    fn_name = l_info.get("function_name", "")
                    if ln not in el_map or (fn_name and not el_map[ln]):
                        el_map[ln] = fn_name
                prev = lmap.get(ln, -1)
                if cnt > prev:
                    lmap[ln] = cnt

    intervals_by_file: Dict[str, List[Tuple[int, int, int]]] = {}
    for rel_path, lmap in file_line_counts.items():
        if not lmap:
            continue
        sorted_exec_lns = sorted(lmap.keys())
        intervals: List[Tuple[int, int, int]] = []
        in_cov = False
        start_ln = 0
        prev_ln = 0
        max_cnt = 0
        for ln in sorted_exec_lns:
            c = lmap[ln]
            if c > 0:
                if not in_cov:
                    in_cov = True
                    start_ln = ln
                    prev_ln = ln
                    max_cnt = c
                else:
                    prev_ln = ln
                    if c > max_cnt:
                        max_cnt = c
            else:
                if in_cov:
                    intervals.append((start_ln, prev_ln, max_cnt))
                    in_cov = False
        if in_cov:
            intervals.append((start_ln, prev_ln, max_cnt))
        if intervals:
            intervals_by_file[rel_path] = intervals

    return test_id, label, matched_count, intervals_by_file, exec_lines_meta


def classify_test_tier_hint(label: str) -> int:
    """Return 1 for consistency/coverage tests, 2 for unit/func tests, 3 for e2e/general."""
    if (
        "//sw/device/tests/qemu_consistency:" in label or
        "//sw/device/tests/qemu/model_coverage:" in label
    ):
        return 1
    if "//sw/device/silicon_creator/rom/e2e/" in label:
        return 3
    if any(k in label for k in ("_functest", "_unittest", "_test_sim_qemu")):
        return 2
    return 3


def init_sqlite_schema(conn: sqlite3.Connection) -> None:
    cur = conn.cursor()
    cur.executescript(
        """
        PRAGMA journal_mode = OFF;
        PRAGMA synchronous = OFF;
        PRAGMA temp_store = MEMORY;
        PRAGMA cache_size = -512000;

        CREATE TABLE meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE files (
            file_id INTEGER PRIMARY KEY,
            path TEXT NOT NULL UNIQUE,
            total_executable_lines INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE executable_lines (
            file_id INTEGER NOT NULL,
            line_number INTEGER NOT NULL,
            func_name TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (file_id, line_number)
        );

        CREATE TABLE tests (
            test_id INTEGER PRIMARY KEY,
            label TEXT NOT NULL UNIQUE,
            tier_hint INTEGER NOT NULL,
            gcda_count INTEGER NOT NULL
        );

        CREATE TABLE line_intervals (
            file_id INTEGER NOT NULL,
            test_id INTEGER NOT NULL,
            start_line INTEGER NOT NULL,
            end_line INTEGER NOT NULL,
            max_hit_count INTEGER NOT NULL
        );
        """
    )
    conn.commit()


def build_per_test_sqlite_db(
    bep_files: List[str],
    cov_build_dir: str,
    qemu_dir: str,
    ot_dir: str,
    expected_stamps: Dict[str, bytes],
    out_db_path: str,
    out_meta_path: str,
    workers: int = 32,
) -> None:
    t0 = time.time()
    candidates, _ = parse_bep_test_candidates(bep_files)
    print(
        f"[per-test-db] Indexing {len(candidates)} test targets across {workers} workers "
        f"against {len(expected_stamps)} .gcno files in {cov_build_dir}...",
        flush=True,
    )

    out_dir = os.path.dirname(os.path.abspath(out_db_path))
    os.makedirs(out_dir, exist_ok=True)
    tmp_db_path = f"{out_db_path}.tmp.{os.getpid()}"
    if os.path.exists(tmp_db_path):
        os.remove(tmp_db_path)

    conn = sqlite3.connect(tmp_db_path)
    init_sqlite_schema(conn)
    cur = conn.cursor()

    file_id_map: Dict[str, int] = {}
    recorded_exec_lines: bool = False
    valid_tests = 0
    skipped_tests = 0
    total_intervals = 0

    tasks = [
        (idx + 1, label, gcov_dir, zip_path, idx < 32)
        for idx, (label, gcov_dir, zip_path) in enumerate(candidates)
    ]

    test_rows: List[Tuple[int, str, int, int]] = []
    interval_batch: List[Tuple[int, int, int, int, int]] = []

    with multiprocessing.Pool(
        processes=workers,
        initializer=_init_per_test_worker,
        initargs=(cov_build_dir, qemu_dir, expected_stamps),
    ) as pool:
        for done_idx, (
            test_id,
            label,
            matched_cnt,
            intervals_by_file,
            exec_lines_meta,
        ) in enumerate(
            pool.imap_unordered(_process_single_test, tasks, chunksize=4), 1
        ):
            if matched_cnt == 0 or not intervals_by_file:
                skipped_tests += 1
            else:
                valid_tests += 1
                tier_hint = classify_test_tier_hint(label)
                test_rows.append((test_id, label, tier_hint, matched_cnt))

                if not recorded_exec_lines and exec_lines_meta:
                    recorded_exec_lines = True
                    exec_batch: List[Tuple[int, int, str]] = []
                    for rel_path in sorted(exec_lines_meta.keys()):
                        el_map = exec_lines_meta[rel_path]
                        if rel_path not in file_id_map:
                            fid = len(file_id_map) + 1
                            file_id_map[rel_path] = fid
                            cur.execute(
                                "INSERT INTO files "
                                "(file_id, path, total_executable_lines) "
                                "VALUES (?, ?, ?);",
                                (fid, rel_path, len(el_map)),
                            )
                        else:
                            fid = file_id_map[rel_path]
                            cur.execute(
                                "UPDATE files SET total_executable_lines = ? WHERE file_id = ?;",
                                (len(el_map), fid),
                            )
                        for ln, fn_name in sorted(el_map.items()):
                            exec_batch.append((fid, ln, fn_name))
                    cur.executemany(
                        "INSERT OR IGNORE INTO executable_lines "
                        "(file_id, line_number, func_name) VALUES (?, ?, ?);",
                        exec_batch,
                    )
                    conn.commit()

                for rel_path, ivals in intervals_by_file.items():
                    fid = file_id_map.get(rel_path)
                    if fid is None:
                        fid = len(file_id_map) + 1
                        file_id_map[rel_path] = fid
                        cur.execute(
                            "INSERT INTO files "
                            "(file_id, path, total_executable_lines) VALUES (?, ?, 0);",
                            (fid, rel_path),
                        )
                    for s_ln, e_ln, max_cnt in ivals:
                        interval_batch.append((fid, test_id, s_ln, e_ln, max_cnt))
                        total_intervals += 1

                if len(interval_batch) >= 200000:
                    cur.executemany(
                        "INSERT INTO line_intervals "
                        "(file_id, test_id, start_line, end_line, max_hit_count) "
                        "VALUES (?, ?, ?, ?, ?);",
                        interval_batch,
                    )
                    interval_batch.clear()
                    conn.commit()

            if done_idx % 200 == 0 or done_idx == len(tasks):
                elapsed = time.time() - t0
                print(
                    f"[per-test-db] Progress: {done_idx}/{len(tasks)} tests "
                    f"(valid={valid_tests}, skipped={skipped_tests}, "
                    f"intervals={total_intervals}, elapsed={elapsed:.1f}s)",
                    flush=True,
                )

    if test_rows:
        cur.executemany(
            "INSERT INTO tests (test_id, label, tier_hint, gcda_count) VALUES (?, ?, ?, ?);",
            test_rows,
        )
    if interval_batch:
        cur.executemany(
            "INSERT INTO line_intervals "
            "(file_id, test_id, start_line, end_line, max_hit_count) "
            "VALUES (?, ?, ?, ?, ?);",
            interval_batch,
        )
        interval_batch.clear()
    conn.commit()

    print("[per-test-db] Building SQLite B-tree indexes...", flush=True)
    cur.executescript(
        """
        CREATE INDEX idx_intervals_file_lines ON line_intervals (file_id, start_line, end_line);
        CREATE INDEX idx_intervals_test_file ON line_intervals (test_id, file_id);
        """
    )
    conn.commit()

    qemu_commit = get_jj_commit(qemu_dir, "mainline")
    ot_commit = get_jj_commit(ot_dir, "mainline")
    now_iso = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    elapsed_total = round(time.time() - t0, 2)

    meta_dict = {
        "generated_at": now_iso,
        "qemu_commit": qemu_commit,
        "opentitan_commit": ot_commit,
        "cov_build_dir": cov_build_dir,
        "bep_files": bep_files,
        "total_bep_tests": len(candidates),
        "valid_tests_with_coverage": valid_tests,
        "skipped_tests": skipped_tests,
        "indexed_files": len(file_id_map),
        "total_intervals": total_intervals,
        "build_time_seconds": elapsed_total,
    }
    cur.executemany(
        "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);",
        [
            (k, json.dumps(v) if isinstance(v, (list, dict)) else str(v))
            for k, v in meta_dict.items()
        ],
    )
    conn.commit()
    conn.close()

    os.replace(tmp_db_path, out_db_path)
    tmp_meta = f"{out_meta_path}.tmp.{os.getpid()}"
    with open(tmp_meta, "w") as mf:
        json.dump(meta_dict, mf, indent=2)
    os.replace(tmp_meta, out_meta_path)

    db_size_mb = os.path.getsize(out_db_path) / (1024 * 1024)
    print(
        f"[per-test-db] Completed in {elapsed_total:.2f}s: {out_db_path} ({db_size_mb:.1f} MB) | "
        f"valid_tests={valid_tests}/{len(candidates)} | "
        f"files={len(file_id_map)} | intervals={total_intervals}",
        flush=True,
    )


def infer_qemu_dir(ot_dir: str, explicit_qemu_dir: Optional[str]) -> str:
    if explicit_qemu_dir:
        return os.path.abspath(explicit_qemu_dir)
    for rc_file in (".bazelrc-site", ".bazelrc"):
        rc_path = os.path.join(ot_dir, rc_file)
        if os.path.exists(rc_path):
            with open(rc_path, "r", errors="replace") as f:
                m = re.search(r"override_repository=.*qemu_opentitan_src=(\S+)", f.read())
                if m and os.path.exists(m.group(1)):
                    return os.path.abspath(m.group(1))
    sibling = os.path.join(os.path.dirname(ot_dir), "qemu")
    if os.path.exists(sibling):
        return os.path.abspath(sibling)
    return "/root/src/workspaces/732be61e-7364-4417-93e7-d09bc7ad7a10/qemu"


def resolve_cov_build_dir(out_base: str, qemu_dir: str, explicit_dir: Optional[str]) -> str:
    if explicit_dir and os.path.exists(os.path.join(explicit_dir, "libsystem-ot-cov.a.p")):
        return os.path.abspath(explicit_dir)
    for candidate in (
        os.path.join(out_base, "external", "+qemu+qemu_opentitan", "build"),
        os.path.join(out_base, "external", "qemu_opentitan", "build"),
        os.path.join(qemu_dir, "build"),
    ):
        if os.path.exists(os.path.join(candidate, "libsystem-ot-cov.a.p")):
            return candidate
    raise SystemExit(
        f"Could not find libsystem-ot-cov.a.p/*.gcno in @qemu_opentitan or {qemu_dir}/build."
    )


def main() -> None:
    default_ot_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "Any additional arguments or flags not listed above (e.g. "
            "--test_tag_filters=qemu,-broken, --build_tag_filters=..., "
            "--test_timeout=60, --jobs=32, --target_pattern_file=..., "
            "//sw/device/tests/..., -//sw/device/tests/sim_dv/...) are passed "
            "directly through to `./bazelisk.sh test`."
        ),
    )
    parser.add_argument(
        "--ot-dir",
        default=default_ot_dir,
        help="Path to OpenTitan workspace (defaults to repo root of this script)",
    )
    parser.add_argument(
        "--qemu-dir",
        default=None,
        help="Path to QEMU source repository (auto-detected from .bazelrc-site / sibling dir)",
    )
    parser.add_argument(
        "--cov-build-dir",
        default=None,
        help="Path to build dir containing lib*-ot-cov.a.p/*.gcno",
    )
    parser.add_argument(
        "--bep-file",
        action="append",
        default=[],
        help="Optional existing Bazel Build Event JSON file(s)",
    )
    parser.add_argument(
        "--bazel-arg",
        action="append",
        default=[],
        help="Additional argument(s) to pass to ./bazelisk.sh test",
    )
    parser.add_argument(
        "--out-dir",
        default=None,
        help="Directory to write uncovered_chunks.json and summary.md",
    )
    parser.add_argument(
        "--out-json",
        default=None,
        help="Optional explicit path for uncovered_chunks.json",
    )
    parser.add_argument(
        "--out-summary",
        default=None,
        help="Optional explicit path for summary.md",
    )
    parser.add_argument(
        "--per-test-db",
        "--out-db",
        dest="per_test_db",
        default=None,
        help="Optional path to generate per-test coverage SQLite database",
    )
    parser.add_argument(
        "--per-test-meta",
        "--out-meta",
        dest="per_test_meta",
        default=None,
        help="Optional path for per-test coverage metadata JSON",
    )
    parser.add_argument(
        "--workers",
        "--per-test-workers",
        dest="workers",
        type=int,
        default=32,
        help="Number of parallel worker processes for per-test SQLite indexing (default: 32)",
    )
    parser.add_argument(
        "--skip-aggregate-report",
        action="store_true",
        help="Skip generating aggregate uncovered_chunks.json/summary.md",
    )

    raw_argv = sys.argv[1:]
    if "--" in raw_argv:
        dd_idx = raw_argv.index("--")
        args, extra_before = parser.parse_known_args(raw_argv[:dd_idx])
        extra = extra_before + ["--"] + raw_argv[dd_idx + 1:]
    else:
        args, extra = parser.parse_known_args(raw_argv)

    passthrough_flags, targets = split_bazel_args_and_targets(extra)
    all_bazel_args = list(args.bazel_arg) + passthrough_flags

    ot_dir = os.path.abspath(args.ot_dir)
    qemu_dir = infer_qemu_dir(ot_dir, args.qemu_dir)
    out_base = subprocess.check_output(
        ["./bazelisk.sh", "info", "output_base"], cwd=ot_dir, text=True
    ).strip()

    report_dir = (
        os.path.abspath(args.out_dir)
        if args.out_dir
        else os.path.join(out_base, "qemu-cov-report")
    )
    os.makedirs(report_dir, exist_ok=True)

    bep_files: List[str] = []
    for bf in args.bep_file:
        for p in bf.split(","):
            if p.strip():
                bep_files.append(os.path.abspath(p.strip()))

    if not bep_files:
        bep_path = os.path.join(report_dir, "last_coverage_bep.json")
        run_bazel_coverage_tests(ot_dir, targets, bep_path, all_bazel_args)
        bep_files = [bep_path]

    cov_build_dir = resolve_cov_build_dir(out_base, qemu_dir, args.cov_build_dir)
    expected_stamps = load_expected_gcno_stamps(cov_build_dir)

    if not args.skip_aggregate_report:
        with tempfile.TemporaryDirectory(dir=report_dir, prefix="merge_") as tmp_work:
            gcov_dirs, stats = extract_gcov_dirs_from_bep(
                bep_files, os.path.join(tmp_work, "staged"), expected_stamps
            )
            print(
                f"BEP results: {stats['bep_test_results']} target(s) "
                f"({stats['cached_test_results']} cached), "
                f"{stats['valid_gcov_dirs']} valid gcov dir(s) merged"
            )
            merged_gcda_dir = parallel_tree_merge(gcov_dirs, os.path.join(tmp_work, "tree"))

            eval_build = os.path.join(tmp_work, "eval_build")
            for sub in COV_SUBDIRS:
                src_sub = os.path.join(cov_build_dir, sub)
                dst_sub = os.path.join(eval_build, sub)
                os.makedirs(dst_sub, exist_ok=True)
                for gcno in glob.glob(os.path.join(src_sub, "*.gcno")):
                    base = os.path.basename(gcno)
                    os.symlink(gcno, os.path.join(dst_sub, base))
                    gcda_name = base[:-5] + ".gcda"
                    merged_gcda = os.path.join(merged_gcda_dir, gcda_name)
                    if os.path.exists(merged_gcda):
                        shutil.copy2(merged_gcda, os.path.join(dst_sub, gcda_name))

            obj_targets = []
            for sub in COV_SUBDIRS:
                for gcno in sorted(glob.glob(os.path.join(eval_build, sub, "*.gcno"))):
                    obj_rel = os.path.join(sub, os.path.basename(gcno)[:-5] + ".o")
                    obj_targets.append((eval_build, obj_rel, qemu_dir))

            raw_files: Dict[str, Dict] = {}
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                for file_dict in pool.map(run_gcov_on_obj, obj_targets):
                    for rel_path, entry in file_dict.items():
                        if rel_path not in raw_files:
                            raw_files[rel_path] = {
                                "line_map": dict(entry["line_map"]),
                                "func_for_line": dict(entry["func_for_line"]),
                                "funcs": dict(entry["funcs"]),
                            }
                        else:
                            cur = raw_files[rel_path]
                            for ln, cnt in entry["line_map"].items():
                                cur["line_map"][ln] = max(cur["line_map"].get(ln, 0), cnt)
                            cur["func_for_line"].update(entry["func_for_line"])
                            for fname, fn in entry["funcs"].items():
                                prev_fn = cur["funcs"].get(fname)
                                if (
                                    prev_fn is None or
                                    fn["execution_count"] > prev_fn["execution_count"]
                                ):
                                    cur["funcs"][fname] = dict(fn)

            all_files: Dict[str, Dict] = {
                rel_path: build_file_summary(rel_path, entry, qemu_dir)
                for rel_path, entry in raw_files.items()
            }

        sorted_files = sorted(
            all_files.values(),
            key=lambda x: (-x["uncovered_executable_lines"], x["file"]),
        )
        tot_exec = sum(f["total_executable_lines"] for f in sorted_files)
        tot_cov = sum(f["covered_executable_lines"] for f in sorted_files)
        tot_uncov = sum(f["uncovered_executable_lines"] for f in sorted_files)
        tot_fn = sum(f["total_functions"] for f in sorted_files)
        tot_fn_cov = sum(f["covered_functions"] for f in sorted_files)
        tot_chunks = sum(len(f["uncovered_chunks"]) for f in sorted_files)

        summary_payload = {
            "bep_test_results": stats["bep_test_results"],
            "cached_test_results": stats["cached_test_results"],
            "test_gcov_dirs_merged": stats["valid_gcov_dirs"],
            "total_files": len(sorted_files),
            "total_executable_lines": tot_exec,
            "covered_executable_lines": tot_cov,
            "uncovered_executable_lines": tot_uncov,
            "line_coverage_pct": round(100.0 * tot_cov / tot_exec, 2) if tot_exec else 100.0,
            "total_functions": tot_fn,
            "covered_functions": tot_fn_cov,
            "function_coverage_pct": round(100.0 * tot_fn_cov / tot_fn, 2) if tot_fn else 100.0,
            "total_uncovered_chunks": tot_chunks,
            "files": sorted_files,
        }

        json_path = (
            os.path.abspath(args.out_json)
            if args.out_json
            else os.path.join(report_dir, "uncovered_chunks.json")
        )
        os.makedirs(os.path.dirname(json_path), exist_ok=True)
        with open(json_path, "w") as jf:
            json.dump(summary_payload, jf, indent=2)

        bep_tot = stats["bep_test_results"]
        bep_cached = stats["cached_test_results"]
        line_pct = summary_payload["line_coverage_pct"]
        fn_pct = summary_payload["function_coverage_pct"]
        md_lines = [
            "# QEMU Coverage Summary",
            "",
            f"- **BEP Test Targets**: `{bep_tot}` (`{bep_cached}` cached)",
            f"- **Valid GCOV Directories Merged**: `{stats['valid_gcov_dirs']}`",
            f"- **Total Files**: `{len(sorted_files)}`",
            f"- **Line Coverage**: `{tot_cov} / {tot_exec}` (`{line_pct}%`)",
            f"- **Function Coverage**: `{tot_fn_cov} / {tot_fn}` (`{fn_pct}%`)",
            f"- **Total Uncovered Chunks**: `{tot_chunks}`",
            "",
            "| File | Line Coverage | Covered / Total Lines | "
            "Function Coverage | Uncovered Chunks |",
            "| :--- | :---: | :---: | :---: | :---: |",
        ]
        for f in sorted_files:
            f_fn_cov = f["covered_functions"]
            f_fn_tot = f["total_functions"]
            md_lines.append(
                f"| `{f['file']}` | **{f['line_coverage_pct']}%** | "
                f"`{f['covered_executable_lines']} / {f['total_executable_lines']}` | "
                f"{f['function_coverage_pct']}% (`{f_fn_cov}/{f_fn_tot}`) | "
                f"{len(f['uncovered_chunks'])} |"
            )

        md_path = (
            os.path.abspath(args.out_summary)
            if args.out_summary
            else os.path.join(report_dir, "summary.md")
        )
        os.makedirs(os.path.dirname(md_path), exist_ok=True)
        with open(md_path, "w") as mf:
            mf.write("\n".join(md_lines) + "\n")

        print(f"Coverage JSON written to: {json_path}")
        print(f"Coverage Summary written to: {md_path}")
        print(
            f"Overall Line Coverage: {tot_cov}/{tot_exec} ({line_pct}%) "
            f"across {len(sorted_files)} files ({tot_chunks} uncovered chunks)"
        )

    if args.per_test_db:
        out_db = os.path.abspath(args.per_test_db)
        if args.per_test_meta:
            out_meta = os.path.abspath(args.per_test_meta)
        else:
            stem = out_db[:-7] if out_db.endswith(".sqlite") else out_db
            out_meta = f"{stem}_meta.json"
        build_per_test_sqlite_db(
            bep_files=bep_files,
            cov_build_dir=cov_build_dir,
            qemu_dir=qemu_dir,
            ot_dir=ot_dir,
            expected_stamps=expected_stamps,
            out_db_path=out_db,
            out_meta_path=out_meta,
            workers=args.workers,
        )


if __name__ == "__main__":
    main()
