#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Engine-API conflict/snapshot/index/restart differential proof.

This supplements, but does not claim closure of, public SQL ON CONFLICT
transport. Each isolated process starts from the identical closed baseline.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from sbsql_savepoint_rollback_full_route_gate import clean_passed_database
from sbsql_whole_store_recovery_full_route_gate import clone_database
from sbsql_insert_route_equivalence_gate import require_optimization_evidence


def run(command, root, phase, env, expected_codes=(0,)):
    (root / f"{phase}.command.json").write_text(json.dumps(command, indent=2) + "\n")
    try:
        result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=180)
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills/waits for the probe on timeout. Preserve its
        # partial action schedule before the caller records this lane's failure.
        for stream, value in (("stdout", error.stdout), ("stderr", error.stderr)):
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            (root / f"{phase}.{stream}").write_text(value or "")
        raise
    (root / f"{phase}.stdout").write_text(result.stdout)
    (root / f"{phase}.stderr").write_text(result.stderr)
    if result.returncode not in expected_codes or result.stderr:
        raise RuntimeError(f"{phase}: rc={result.returncode}: {result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--inventory-probe", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--case", choices=("history", "bulk", "random", "crash_before_commit", "crash_after_commit"), default="history")
    parser.add_argument("--seed", type=int, default=901)
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be an unsigned 32-bit integer")
    root = Path(tempfile.mkdtemp(prefix="sbi9_snap_"))
    pointer = Path(args.work_dir)
    pointer.mkdir(parents=True, exist_ok=True)
    (pointer / "artifact_path.txt").write_text(str(root) + "\n")
    print(f"snapshot_equivalence_artifacts={root}", flush=True)
    template = root / "t" / "sp.sbdb"
    template.parent.mkdir()
    env = dict(os.environ, SCRATCHBIRD_TEST_INSERT_ROUTE="optimized",
               SCRATCHBIRD_TEST_DML_OPTIMIZATION="normal",
               SCRATCHBIRD_TEST_INSERT_ROUTE_EVIDENCE="",
               SCRATCHBIRD_TEST_DML_OPTIMIZATION_EVIDENCE="")
    run([args.probe, "create", str(template)], root, "create", env)
    profiles = ("optimized", "staged", "cold", "uncached", "evicted", "publication_evicted", "relation_rows", "publish_cache", "scan_scalar")
    observations, failures = {}, []
    (root / "replay.json").write_text(json.dumps({"case": args.case, "seed": args.seed,
        "profiles": profiles, "probe": args.probe}, indent=2) + "\n")
    for profile in profiles:
        lane = root / profile
        database = lane / "sp.sbdb"
        clone_database(template, database)
        ids = lane / "transactions.tsv"
        branches = lane / "branches.tsv"
        lane_env = dict(env,
            SCRATCHBIRD_TEST_INSERT_ROUTE="staged" if profile in ("staged", "scan_scalar") else "optimized",
            SCRATCHBIRD_TEST_DML_OPTIMIZATION=profile if profile not in ("optimized", "staged") else "normal",
            SCRATCHBIRD_TEST_INSERT_ROUTE_EVIDENCE=str(lane / "insert_routes.tsv"),
            SCRATCHBIRD_TEST_DML_OPTIMIZATION_EVIDENCE=str(branches))
        try:
            crash = args.case.startswith("crash_")
            command = [args.probe, args.case, str(database), str(ids)]
            if args.case == "random":
                command.append(str(args.seed))
            history = run(command, lane, "history", lane_env,
                          expected_codes=((-9,) if os.name == "posix" else (137,)) if crash else (0,))
            observe_mode = "observe" if args.case == "history" else "observe_" + args.case
            observe_command = [args.probe, observe_mode, str(database)]
            if args.case == "random":
                observe_command.append(str(ids) + ".expected")
            reopens = [run(observe_command, lane, f"reopen{i}", lane_env) for i in range(2)]
            inventory = run([args.inventory_probe, str(database)], lane, "inventory", lane_env)
            records = {int(row.split("\t")[0]): row.split("\t") for row in inventory.splitlines()[1:]}
            classifications = []
            for row in ids.read_text().splitlines():
                role, txid, expected = row.split("\t")
                record = records[int(txid)]
                if record[1] != expected or (role != "old-reader" and record[4:6] != ["1", "1"]):
                    raise RuntimeError(f"wrong native finality for {role}: {record}")
                classifications.append([role, record[1], record[2], *record[4:]])
            coverage = set(branches.read_text().splitlines()) if branches.exists() else set()
            if args.case == "random":
                require_optimization_evidence("\n".join(coverage), profile)
                if profile == "optimized":
                    required = {"context_cache_hit", "cache_unique_proof", "decoded_row_cache_hit",
                                "hot_point_cache_hit", "unique_index_probe", "single_window_cache_skip"}
                elif profile == "scan_scalar":
                    required = {"native_bulk_staged_fallback", "hot_point_cache_disabled"}
                elif profile == "publish_cache":
                    required = {"single_window_cache_forced_publish"}
                else:
                    required = set()
                if not required <= coverage:
                    raise RuntimeError(f"random optimized/reference branch missing: {sorted(required - coverage)}")
            if crash:
                entries = (lane / "insert_routes.tsv").read_text().splitlines()
                expected_route = "staged" if profile in ("staged", "scan_scalar") else "direct"
                if len(entries) != 1 or entries[0].split("\t")[:2] != [expected_route, "1"]:
                    raise RuntimeError("crash did not follow the expected actual INSERT executor")
            elif args.case == "bulk":
                required = ("native_bulk_staged_fallback" if profile == "scan_scalar" else
                            "single_window_cache_forced_publish" if profile == "publish_cache" else
                            "single_window_cache_skip")
                if required not in coverage:
                    raise RuntimeError(f"bulk branch not executed: {required}")
                if profile == "publish_cache" and "cache_publish" not in coverage:
                    raise RuntimeError("forced bulk cache publication not executed")
            elif profile in ("staged", "scan_scalar"):
                if "post_conflict_direct" in coverage:
                    raise RuntimeError("staged profile used post-conflict direct shortcut")
            elif "post_conflict_direct" not in coverage:
                raise RuntimeError("post-conflict direct shortcut was not executed")
            observations[profile] = {"history": history, "inventory": classifications, "reopens": reopens}
            (lane / "observations.json").write_text(json.dumps(observations[profile], indent=2) + "\n")
            if profile != "optimized" and observations[profile] != observations.get("optimized"):
                raise RuntimeError("differential observations differ")
            clean_passed_database(lane)
            print(f"{profile}=passed", flush=True)
        except Exception as error:
            failures.append(f"{profile}: {error}")
            print(failures[-1], flush=True)
    (root / "summary.json").write_text(json.dumps(
            {"case": args.case, "seed": args.seed, "profiles": profiles, "failures": failures,
         "public_conflict_transport_proof": False}, indent=2) + "\n")
    if not failures:
        # The baseline shares the root with logs; cleanup recognizes only DB
        # companions and filespace segments and preserves diagnostic evidence.
        clean_passed_database(template.parent)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
