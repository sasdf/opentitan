#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
"""Coverage test runner & collector for QEMU Earlgrey hardware models.

Given one or more Bazel test target patterns (e.g. //sw/device/tests/...), this script:
1. Expands wildcard patterns to runnable 'qemu'-tagged test targets via Bazel query.
2. Runs `./bazelisk.sh test --config=qemu_coverage --build_event_json_file=<bep.json> <targets>`
   so Bazel selects the bundled `@qemu_opentitan//:build/qemu-system-riscv32-cov` binary inside the
   sandbox and preserves full Bazel test caching across runs.
3. Parses `<bep.json>` from that exact Bazel invocation to locate the `testActionOutput`
   (`test.outputs/gcov/*.gcda` or `outputs.zip`) ONLY for the requested targets (both cached and
   freshly executed), completely avoiding leftover `bazel-testlogs` from previous runs or old commits.
4. Merges the `.gcda` files against the bundled `.gcno` files in `@qemu_opentitan`
   (`libsystem-ot-cov.a.p` and `libqemu-riscv32-softmmu-ot-cov.a.p`) and generates
   `uncovered_chunks.json` and `summary.md`.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.parse
import zipfile
from typing import Dict, List, Optional, Set, Tuple

EARLGREY_SOURCE_PATTERNS = [
    "hw/opentitan/ot_",
    "hw/riscv/ot_earlgrey.c",
    "hw/riscv/ibex_",
    "hw/misc/pulp_rv_dm.c",
    "hw/jtag/tap_ctrl_rbb.c",
    "hw/block/m25p80",
    "hw/nvram/eeprom_at24c.c",
    "hw/intc/sifive_plic.c",
]

EXCLUDE_PATTERNS = [
    "_dj.c",
    "ot_keymgr_dpe.c",
    "ot_mbx.c",
    "ot_dma.c",
    "ot_soc_proxy.c",
    "ot_soc_dbg_ctrl.c",
]

COV_SUBDIRS = (
    "libsystem-ot-cov.a.p",
    "libqemu-riscv32-softmmu-ot-cov.a.p",
)


def is_earlgrey_model_source(src_path: str) -> bool:
    if any(ex in src_path for ex in EXCLUDE_PATTERNS):
        return False
    return any(pat in src_path for pat in EARLGREY_SOURCE_PATTERNS)


def load_expected_gcno_stamps(cov_build_dir: str) -> Dict[str, bytes]:
    """Return map of '<stem>.gcda' -> 8-byte (version, stamp) from .gcno header[4:12]."""
    stamps: Dict[str, bytes] = {}
    for sub in COV_SUBDIRS:
        for gcno_path in glob.glob(os.path.join(cov_build_dir, sub, "*.gcno")):
            with open(gcno_path, "rb") as f:
                hdr = f.read(12)
            if len(hdr) == 12 and hdr[:4] == b"oncg":
                gcda_name = os.path.basename(gcno_path)[:-5] + ".gcda"
                stamps[gcda_name] = hdr[4:12]
    return stamps


def split_bazel_args_and_targets(extra: List[str]) -> Tuple[List[str], List[str]]:
    """Partition unconsumed CLI arguments into Bazel flags and target patterns."""
    bazel_flags: List[str] = []
    targets: List[str] = []
    bool_short_flags = {"-k", "-s", "-q", "-i"}
    i = 0
    while i < len(extra):
        tok = extra[i]
        if tok == "--":
            targets.extend(extra[i + 1 :])
            break
        if tok.startswith(("//", "@", ":", "-//", "-@", "-:")):
            targets.append(tok)
            i += 1
        elif tok.startswith("-"):
            bazel_flags.append(tok)
            if (
                "=" not in tok
                and tok not in bool_short_flags
                and not tok.startswith("--no")
                and i + 1 < len(extra)
                and not extra[i + 1].startswith(("-", "//", "@", ":"))
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
    has_target_pattern_file = any(
        a == "--target_pattern_file" or a.startswith("--target_pattern_file=")
        for a in bazel_args
    )
    if not targets and not has_target_pattern_file:
        targets = ["//sw/device/tests/...", "-//sw/device/tests/sim_dv/..."]

    default_flags: List[str] = []
    if not has_tag_filter:
        default_flags.append("--test_tag_filters=qemu,-broken")

    cmd = (
        [
            "./bazelisk.sh",
            "test",
            "--config=qemu_coverage",
            f"--build_event_json_file={bep_path}",
            "--keep_going",
        ]
        + default_flags
        + bazel_args
        + (["--"] + targets if targets else [])
    )
    print(f"Running Bazel coverage test command: {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=ot_dir)
    return res.returncode


def extract_gcov_dirs_from_bep(
    bep_path: str,
    work_dir: str,
    expected_stamps: Dict[str, bytes],
) -> Tuple[List[str], Dict[str, int]]:
    """Extract and validate per-test .gcda directories strictly from the BEP JSON file
    of the current Bazel invocation, ignoring any leftover directories in bazel-testlogs.
    """
    stats = {
        "bep_test_results": 0,
        "cached_test_results": 0,
        "valid_gcov_dirs": 0,
        "skipped_missing_outputs": 0,
        "skipped_stamp_mismatch_files": 0,
    }
    raw_candidates: List[Tuple[str, Optional[str], Optional[str]]] = []
    seen_labels: Set[str] = set()

    with open(bep_path, "r", errors="replace") as bf:
        for line in bf:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except Exception:
                continue
            if "testResult" not in ev:
                continue
            tr_id = ev.get("id", {}).get("testResult", {})
            label = tr_id.get("label", "")
            shard = tr_id.get("shard", 0)
            run = tr_id.get("run", 0)
            attempt = tr_id.get("attempt", 0)
            key = f"{label}#{shard}:{run}:{attempt}"
            if key in seen_labels:
                continue
            seen_labels.add(key)

            tr = ev["testResult"]
            stats["bep_test_results"] += 1
            if tr.get("cachedLocally") or tr.get("executionInfo", {}).get("cachedRemotely"):
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
                    gcov_dir = os.path.dirname(path)
                    break
                if name == "test.outputs__outputs.zip":
                    zip_uri = path

            if not gcov_dir and zip_uri:
                cand_gcov = os.path.join(os.path.dirname(zip_uri), "gcov")
                if os.path.isdir(cand_gcov):
                    gcov_dir = cand_gcov

            if not gcov_dir and not zip_uri:
                stats["skipped_missing_outputs"] += 1
                continue

            raw_candidates.append((
                key,
                gcov_dir if (gcov_dir and os.path.isdir(gcov_dir)) else None,
                zip_uri if (zip_uri and os.path.isfile(zip_uri)) else None,
            ))

    staged_dirs: List[str] = []
    for idx, (_key, gcov_dir, zip_path) in enumerate(raw_candidates):
        dest = os.path.join(work_dir, f"test_{idx}")
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
                        shutil.copy2(src_f, os.path.join(dest, fn))
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
    for f_entry in data.get("files", []):
        f_path = f_entry.get("file", "")
        rel_path = f_path[3:] if f_path.startswith("../") else f_path
        if rel_path.startswith(qemu_dir + "/"):
            rel_path = rel_path[len(qemu_dir) + 1:]
        if not is_earlgrey_model_source(rel_path):
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

    return {
        "file": rel_path,
        "total_executable_lines": len(exec_lines),
        "covered_executable_lines": len(covered_lines),
        "uncovered_executable_lines": len(uncovered_lines),
        "line_coverage_pct": round(100.0 * len(covered_lines) / len(exec_lines), 2) if exec_lines else 100.0,
        "total_functions": len(funcs),
        "covered_functions": sum(1 for fn in funcs if fn["execution_count"] > 0),
        "uncovered_functions": [fn["name"] for fn in funcs if fn["execution_count"] == 0],
        "function_coverage_pct": round(
            100.0 * sum(1 for fn in funcs if fn["execution_count"] > 0) / len(funcs), 2
        ) if funcs else 100.0,
        "uncovered_chunks": formatted_chunks,
    }


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
    return "/root/src/qemu"


def resolve_cov_build_dir(out_base: str, qemu_dir: str, explicit_dir: Optional[str]) -> str:
    if explicit_dir:
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
    parser.add_argument("--ot-dir", default=default_ot_dir,
                        help="Path to OpenTitan workspace (defaults to repo root of this script)")
    parser.add_argument("--qemu-dir", default=None,
                        help="Path to QEMU source repository (auto-detected from .bazelrc-site / sibling qemu dir)")
    parser.add_argument("--cov-build-dir", default=None,
                        help="Path to build dir containing lib*-ot-cov.a.p/*.gcno (defaults to @qemu_opentitan/build)")
    parser.add_argument("--bep-file", default=None,
                        help="Optional existing Bazel Build Event JSON file (skips running bazelisk.sh test if provided)")
    parser.add_argument("--bazel-arg", action="append", default=[],
                        help="Additional argument(s) to pass to ./bazelisk.sh test")
    parser.add_argument("--out-dir", default=None,
                        help="Directory to write uncovered_chunks.json and summary.md (defaults to <output_base>/qemu-cov-report)")
    parser.add_argument("--out-json", default=None,
                        help="Optional explicit path for uncovered_chunks.json")
    parser.add_argument("--out-summary", default=None,
                        help="Optional explicit path for summary.md")

    import sys
    raw_argv = sys.argv[1:]
    if "--" in raw_argv:
        dd_idx = raw_argv.index("--")
        args, extra_before = parser.parse_known_args(raw_argv[:dd_idx])
        extra = extra_before + ["--"] + raw_argv[dd_idx + 1 :]
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

    if args.bep_file:
        bep_path = os.path.abspath(args.bep_file)
    else:
        bep_path = os.path.join(report_dir, "last_coverage_bep.json")
        run_bazel_coverage_tests(ot_dir, targets, bep_path, all_bazel_args)

    cov_build_dir = resolve_cov_build_dir(out_base, qemu_dir, args.cov_build_dir)
    expected_stamps = load_expected_gcno_stamps(cov_build_dir)

    with tempfile.TemporaryDirectory(dir=report_dir, prefix="merge_") as tmp_work:
        gcov_dirs, stats = extract_gcov_dirs_from_bep(
            bep_path, os.path.join(tmp_work, "staged"), expected_stamps
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
                            if fname not in cur["funcs"] or fn["execution_count"] > cur["funcs"][fname]["execution_count"]:
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

    json_path = os.path.abspath(args.out_json) if args.out_json else os.path.join(report_dir, "uncovered_chunks.json")
    md_path = os.path.abspath(args.out_summary) if args.out_summary else os.path.join(report_dir, "summary.md")
    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    os.makedirs(os.path.dirname(md_path), exist_ok=True)

    with open(json_path, "w") as jf:
        json.dump(summary_payload, jf, indent=2)

    md_lines = [
        "# QEMU Earlgrey Hardware Model Coverage Summary",
        "",
        f"- **BEP Targets Evaluated**: `{stats['bep_test_results']}` (`{stats['cached_test_results']}` cached, `{stats['valid_gcov_dirs']}` gcov outputs merged)",
        f"- **Total Earlgrey Model Files**: `{len(sorted_files)}`",
        f"- **Executable Lines**: `{tot_cov} / {tot_exec}` (`{summary_payload['line_coverage_pct']}%`), Uncovered: `{tot_uncov}`",
        f"- **Functions**: `{tot_fn_cov} / {tot_fn}` (`{summary_payload['function_coverage_pct']}%`)",
        f"- **Total Uncovered Chunks**: `{tot_chunks}`",
        "",
        "| File | Line Coverage | Covered / Total Lines | Uncovered Lines | Function Coverage | Uncovered Chunks |",
        "| :--- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for f in sorted_files:
        md_lines.append(
            f"| `{f['file']}` | `{f['line_coverage_pct']}%` | "
            f"`{f['covered_executable_lines']} / {f['total_executable_lines']}` | "
            f"`{f['uncovered_executable_lines']}` | "
            f"`{f['covered_functions']} / {f['total_functions']} ({f['function_coverage_pct']}%)` | "
            f"`{len(f['uncovered_chunks'])}` |"
        )
    with open(md_path, "w") as mf:
        mf.write("\n".join(md_lines) + "\n")

    print(f"Wrote JSON report:    {json_path}")
    print(f"Wrote Markdown table: {md_path}")
    print(f"Overall Coverage:     {tot_cov}/{tot_exec} lines ({summary_payload['line_coverage_pct']}%), "
          f"{tot_fn_cov}/{tot_fn} functions ({summary_payload['function_coverage_pct']}%), "
          f"{tot_chunks} uncovered chunks")


if __name__ == "__main__":
    main()
