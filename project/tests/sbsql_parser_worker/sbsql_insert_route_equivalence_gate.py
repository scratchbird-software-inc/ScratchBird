#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Optimized/staged INSERT differential histories through the actual server.

The staged profile is not a whole-DML optimization-disabled reference. This
gate proves only the INSERT routes it observes, not every index/cache family.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import random
import subprocess
import tempfile
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    isql_data_lines, run_isql, start_route, stop_route,
)
from sbsql_savepoint_rollback_full_route_gate import clean_passed_database
from sbsql_whole_store_recovery_full_route_gate import clone_database
from dml_history_reducer import reduce_schedule

TABLE = "sbsfc021_stream_table"
OPTIMIZATION_PROFILES = ("cold", "uncached", "evicted", "publication_evicted", "relation_rows", "publish_cache", "scan_scalar")


@dataclass
class History:
    name: str
    sql: str
    probes: list[str]
    final_rows: list[tuple[int, str | None]]
    inserted_rows: int
    diagnostics: tuple[str, ...] = ()
    transaction_states: tuple[str, ...] = ("committed",)
    admitted: bool = True
    execution_rows: tuple[int, ...] = ()
    affected_rows: int | None = None
    schedule: list[list[dict]] | None = None


def insert(key: int, payload: str | None) -> str:
    value = "NULL" if payload is None else "'" + payload.replace("'", "''") + "'"
    return f"INSERT INTO {TABLE} (id, payload) VALUES ({key}, {value});"


def count(key: int) -> str:
    return f"SELECT COUNT(*) FROM {TABLE} WHERE id = {key};"


def histories(seed: int) -> list[History]:
    rng = random.Random(seed)
    a, b, c = rng.sample(range(100, 10000), 3)
    payload = f"seed-{seed}-'quoted'-é"
    baseline = [(6, "stream-baseline")]
    def history(name, sql, probes, rows, inserted_rows, diagnostics=(),
                transaction_states=("committed",), admitted=True, execution_rows=None):
        return History(name, "\n".join([*sql, ""]), probes,
                       sorted(baseline + rows), inserted_rows, diagnostics,
                       transaction_states, admitted,
                       (1,) * inserted_rows if execution_rows is None else execution_rows)
    return [
        history("commit", [insert(a, payload), insert(b, "second"), count(a),
                            "COMMIT;", count(b)], ["1", "1"],
                # The parser coalesces adjacent compatible INSERT statements.
                [(a, payload), (b, "second")], 2, execution_rows=(2,)),
        history("rollback_reuse", [insert(a, payload), count(a), "ROLLBACK;",
                  count(a), insert(a, "reused"), "COMMIT;", count(a)],
                ["1", "0", "1"], [(a, "reused")], 2,
                transaction_states=("rolled_back", "committed")),
        history("nested_savepoints", [insert(a, "retained"), "SAVEPOINT outer_sp;",
                  insert(b, "discarded"), "SAVEPOINT inner_sp;", insert(c, payload),
                  count(b), count(c), "ROLLBACK TO SAVEPOINT outer_sp;",
                  count(b), count(c), insert(b, "again"), count(b),
                  "ROLLBACK TO SAVEPOINT outer_sp;", count(b),
                  "RELEASE SAVEPOINT outer_sp;", "COMMIT;", count(a)],
                ["1", "1", "0", "0", "1", "0", "1"], [(a, "retained")], 4),
        history("unique_refusal", [insert(a, "retained"), "SAVEPOINT outer_sp;",
                  "SET BAIL OFF;", insert(a, "duplicate"), "SET BAIL ON;",
                  count(a), insert(b, "discarded"), "ROLLBACK TO SAVEPOINT outer_sp;",
                  count(a), count(b), "RELEASE SAVEPOINT outer_sp;", "COMMIT;"],
                ["1", "1", "0"], [(a, "retained")], 2,
                ("CLI.CONSTRAINT_UNIQUE_VIOLATION",)),
        history("multirow", [
                  f"INSERT INTO {TABLE} (id, payload) VALUES ({a}, 'first'), ({b}, 'second');",
                  count(a), count(b), "COMMIT;"], ["1", "1"],
                [(a, "first"), (b, "second")], 2, execution_rows=(2,)),
        history("multirow_existing_key_refusal", [insert(a, "retained"),
                  "SAVEPOINT outer_sp;", "SET BAIL OFF;",
                  f"INSERT INTO {TABLE} (id, payload) VALUES ({b}, 'prefix'), (6, 'duplicate'), ({c}, 'suffix');",
                  "SET BAIL ON;", count(a), count(b), count(c),
                  insert(c, "discarded"), "ROLLBACK TO SAVEPOINT outer_sp;",
                  count(c), "RELEASE SAVEPOINT outer_sp;", "COMMIT;"],
                ["1", "0", "0", "0"], [(a, "retained")], 2,
                ("CLI.CONSTRAINT_UNIQUE_VIOLATION",)),
        history("multirow_self_conflict_refusal", [insert(a, "retained"),
                  "SET BAIL OFF;",
                  f"INSERT INTO {TABLE} (id, payload) VALUES ({b}, 'first'), ({b}, 'duplicate');",
                  "SET BAIL ON;", count(a), count(b), insert(b, "reused"),
                  "COMMIT;", count(b)], ["1", "0", "1"],
                [(a, "retained"), (b, "reused")], 2,
                ("CLI.CONSTRAINT_UNIQUE_VIOLATION",)),
        # The SQL route currently refuses this surface before EngineInsertRows.
        # Keep the gap visible and protect refusal atomicity. This is NOT a
        # passing differential execution proof for the post-conflict shortcut.
        history("post_conflict_unadmitted", ["SET BAIL OFF;",
                  f"INSERT INTO {TABLE} (id, payload) VALUES ({a}, 'new-key') ON CONFLICT (id) DO NOTHING;",
                  "SET BAIL ON;", count(a), "COMMIT;"], ["0"], [], 0,
                ("SBLR.OPERATION.NONCANONICAL",), transaction_states=(), admitted=False),
        # Successful surrounding INSERTs must remain usable, but they do not
        # turn a refused conflict clause into positive conflict-route coverage.
        *[history(f"{name}_unadmitted", [insert(a, "retained"),
                  "SAVEPOINT outer_sp;", "SET BAIL OFF;",
                  f"INSERT INTO {TABLE} (id, payload) VALUES (6, 'must-not-replace') ON CONFLICT (id) {action};",
                  "SET BAIL ON;", count(a), count(6), insert(b, "discarded"),
                  "ROLLBACK TO SAVEPOINT outer_sp;", count(b),
                  "RELEASE SAVEPOINT outer_sp;", "COMMIT;"],
                ["1", "1", "0"], [(a, "retained")], 2,
                ("SBLR.OPERATION.NONCANONICAL",), admitted=False)
          for name, action in (("matched_conflict_nothing", "DO NOTHING"),
                               ("matched_conflict_update", "DO UPDATE SET payload = excluded.payload"))],
        history("null_omitted", [insert(a, None),
                  f"INSERT INTO {TABLE} (id) VALUES ({b});", "COMMIT;",
                  f"SELECT payload FROM {TABLE} WHERE id = {a};",
                  f"SELECT payload FROM {TABLE} WHERE id = {b};"],
                ["(null)", "(null)"], [(a, None), (b, None)], 2),
        history("overflow_rewind", [insert(a, "x" * 7000), "SAVEPOINT outer_sp;",
                  insert(b, "y" * 7100), count(b), "ROLLBACK TO SAVEPOINT outer_sp;",
                  count(b), "RELEASE SAVEPOINT outer_sp;", "COMMIT;"],
                ["1", "0"], [(a, "x" * 7000)], 2),
        mixed_history(seed),
    ]


def mixed_history(seed: int, schedule: list[list[dict]] | None = None) -> History:
    """A small independent row model; the engine still owns all execution.

    Reads separate mutations so parser coalescing cannot hide statement counts.
    Every transaction starts with an INSERT to correlate its native finality.
    """
    rng = random.Random(seed)
    keys = rng.sample(range(100, 10000), 4)
    if schedule is None:
        schedule = [[{"key": rng.choice(keys),
                      "action": rng.choice(("insert", "update", "delete", "savepoint", "rewind")),
                      "value": f"mixed-{seed}-{epoch}-{step}"}
                     for step in range(18)] for epoch in range(3)]
    if len(schedule) != 3:
        raise ValueError("mixed history requires three transaction epochs")
    committed = {6: "stream-baseline"}
    rows = dict(committed)
    sql, probes, diagnostics, states = [], [], [], []
    inserted = affected = 0
    for epoch in range(3):
        anchor = 20000 + epoch
        sql += [insert(anchor, f"anchor-{epoch}"), count(anchor)]
        probes += ["1"]
        rows[anchor] = f"anchor-{epoch}"
        inserted += 1
        affected += 1
        savepoint = None
        for operation in schedule[epoch]:
            key, action, value = operation["key"], operation["action"], operation["value"]
            if action == "insert":
                if key in rows:
                    sql += ["SET BAIL OFF;", insert(key, value), "SET BAIL ON;"]
                    diagnostics.append("CLI.CONSTRAINT_UNIQUE_VIOLATION")
                else:
                    sql.append(insert(key, value))
                    rows[key] = value
                    inserted += 1
                    affected += 1
            elif action == "update":
                sql.append(f"UPDATE {TABLE} SET payload = '{value}' WHERE id = {key};")
                if key in rows:
                    rows[key] = value
                    affected += 1
            elif action == "delete":
                sql.append(f"DELETE FROM {TABLE} WHERE id = {key};")
                if key in rows:
                    del rows[key]
                    affected += 1
            elif action == "savepoint":
                if savepoint is not None:
                    sql.append("RELEASE SAVEPOINT model_sp;")
                sql.append("SAVEPOINT model_sp;")
                savepoint = dict(rows)
            elif savepoint is not None:
                sql.append("ROLLBACK TO SAVEPOINT model_sp;")
                rows = dict(savepoint)
            sql.append(count(key))
            probes.append("1" if key in rows else "0")
            if key in rows:
                sql.append(f"SELECT payload FROM {TABLE} WHERE id = {key};")
                probes.append(rows[key])
        if epoch == 1:
            sql.append("ROLLBACK;")
            rows = dict(committed)
            states.append("rolled_back")
        else:
            sql.append("COMMIT;")
            committed = dict(rows)
            states.append("committed")
        sql.append(count(anchor))
        probes.append("1" if anchor in rows else "0")
    return History("mixed_model", "\n".join([*sql, ""]), probes,
                   sorted(committed.items()), inserted, tuple(diagnostics),
                   tuple(states), execution_rows=(1,) * inserted,
                   affected_rows=affected, schedule=schedule)


def failure_class(error: Exception) -> str | None:
    """Only semantic oracle failures are reducible; not startup/timeouts."""
    message = str(error)
    for marker in ("unexpected rc=", "unexpected diagnostics:", ": probes ",
                   "incorrect client completion counts", "wrong durable rows",
                   "wrong durable mutation classification", "executor sequence changed"):
        if marker in message:
            return marker
    return None


def minimize_failure(args, root: Path, template: Path, seed: int,
                     history: History, profile: str, expected_class: str) -> Path:
    def reproduces(schedule, attempt):
        candidate = mixed_history(seed, schedule)
        trial = root / f"m{attempt:02d}"
        trial.mkdir(parents=True)
        (trial / "history.json").write_text(json.dumps({"seed": seed, **vars(candidate)}, indent=2) + "\n")
        matched = False
        detail = "passed"
        try:
            result = run_profile(args, trial / "p", template, profile, candidate)
            if expected_class == "differential":
                baseline = run_profile(args, trial / "b", template, "optimized", candidate)
                matched = result != baseline
                detail = "differential mismatch" if matched else "passed"
        except Exception as error:
            matched = failure_class(error) == expected_class
            detail = str(error)
        (trial / "result.json").write_text(json.dumps(
            {"profile": profile, "failure_class": expected_class,
             "reproduced": matched, "detail": detail}, indent=2) + "\n")
        # Every candidate remains replayable from its model/SQL and the common
        # seeded baseline; keep logs, not a growing collection of databases.
        clean_passed_database(trial / "p")
        clean_passed_database(trial / "b")
        return matched
    reduced, attempts = reduce_schedule(history.schedule, reproduces, args.reduction_attempts)
    path = root / "reduced_history.json"
    path.write_text(json.dumps({"seed": seed, **vars(mixed_history(seed, reduced))}, indent=2) + "\n")
    (root / "reduction.json").write_text(json.dumps(
        {"profile": profile, "failure_class": expected_class, "attempts": attempts,
         "budget": args.reduction_attempts,
         "original_actions": sum(map(len, history.schedule)),
         "retained_actions": sum(map(len, reduced)),
         "globally_minimal": False}, indent=2) + "\n")
    return path


def observation(result, diagnostics=()) -> dict:
    errors = result.stderr.splitlines()
    if result.returncode != (1 if diagnostics else 0):
        raise RuntimeError(f"{result.case}: unexpected rc={result.returncode}: {result.stderr}")
    def has_diagnostic_code(line: str, code: str) -> bool:
        # The client may append diagnostic detail after a semicolon. Validate
        # its code here; retain and compare the entire original string below.
        prefix, separator, diagnostic = line.rpartition(" (")
        return (prefix.startswith("Error: ") and bool(separator)
                and diagnostic.endswith(")")
                and diagnostic[:-1].split(";", 1)[0] == code)
    if len(errors) != len(diagnostics) or any(
            not has_diagnostic_code(line, code)
            for line, code in zip(errors, diagnostics)):
        raise RuntimeError(f"{result.case}: unexpected diagnostics: {errors!r}")
    # Preserve actual output, completion counts and diagnostic text. No UUID,
    # affected-count or error normalization is allowed in this fixture.
    return {"returncode": result.returncode, "stdout": result.stdout,
            "stderr": result.stderr}


def require_route_evidence(text: str, profile: str, inserted_rows: int,
                           execution_rows: tuple[int, ...] | None = None) -> list[int]:
    expected_route = "direct" if profile == "optimized" else "staged"
    entries = [line.split("\t") for line in text.splitlines()]
    if (not entries and inserted_rows != 0) or any(
            len(row) != 3 or row[0] != expected_route or
            any(not value.isascii() or not value.isdigit() for value in row[1:]) or
            int(row[2]) == 0 for row in entries):
        raise RuntimeError(f"wrong actual executor evidence: {entries!r}")
    if sum(int(row[1]) for row in entries) != inserted_rows:
        raise RuntimeError("missing successful INSERT route evidence")
    if inserted_rows == 0 and entries:
        raise RuntimeError("unadmitted SQL reached an INSERT executor")
    if execution_rows is not None and tuple(int(row[1]) for row in entries) != execution_rows:
        raise RuntimeError("successful INSERT executor sequence changed")
    return list(dict.fromkeys(int(row[2]) for row in entries))


def mutation_inventory(text: str, transaction_ids: list[int],
                       expected_states: tuple[str, ...]) -> list[dict]:
    lines = text.splitlines()
    if not lines or not lines[0].startswith("next_local_transaction_id\t"):
        raise RuntimeError("missing inventory header")
    entries = {}
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 7 or int(fields[0]) in entries:
            raise RuntimeError("malformed/duplicate inventory entry")
        entries[int(fields[0])] = fields
    if len(transaction_ids) != len(expected_states):
        raise RuntimeError("mutation transaction count changed")
    output = []
    for txid, expected_state in zip(transaction_ids, expected_states):
        fields = entries.get(txid)
        if fields is None or fields[1] != expected_state or fields[4:6] != ["1", "1"]:
            raise RuntimeError(f"wrong durable mutation classification for transaction {txid}: {fields}")
        # Agent transactions may interleave and consume ordinals. Associate
        # only actual executor-observed mutation transactions with history
        # positions; retain the FULL raw inventory separately. Do not infer
        # finality from this route evidence: fields come from the native loader.
        output.append({"state": fields[1], "scope": fields[2],
                       "committed_history_ordinals_within_begin_bound": [
                           i for i, other in enumerate(transaction_ids)
                           if entries[other][1] == "committed" and other <= int(fields[3])],
                       "evidence_required": fields[4], "evidence_written": fields[5],
                       "rollback_only": fields[6]})
    return output


def require_optimization_evidence(text: str, profile: str) -> None:
    branches = set(text.splitlines())
    required = {
        "cold": {"cache_cold", "load_indexes"},
        "uncached": {"load_indexes"},
        "evicted": {"cache_evicted_before_proof", "cache_loss_scoped_reload"},
        "publication_evicted": {"cache_evicted_before_publication", "load_indexes"},
        "relation_rows": {"load_relation_rows"},
        "publish_cache": {"cache_publish"},
        "scan_scalar": {"row_scalar_append", "index_scalar_flush", "unique_row_scan"},
    }.get(profile, set())
    if not required <= branches:
        raise RuntimeError(f"missing actual optimization branches: {sorted(required - branches)}")
    if profile in ("uncached", "relation_rows") and branches & {
            "cache_unique_proof", "cache_publish", "context_cache_hit"}:
        raise RuntimeError("disabled cache was used")
    if profile == "scan_scalar" and "decoded_row_cache_hit" in branches:
        raise RuntimeError("disabled decoded-row cache was used")


def require_mixed_optimization_evidence(text: str, profile: str) -> None:
    branches = set(text.splitlines())
    required = {"delete_canonical_scan"}
    if profile == "scan_scalar":
        required |= {"update_table_scan", "hot_point_cache_disabled"}
    else:
        required.add("update_index_candidates")
        if not branches & {"hot_point_cache_miss", "hot_point_cache_hit"}:
            raise RuntimeError("mixed optimized hot-point lookup was not executed")
    if not required <= branches:
        raise RuntimeError(f"mixed optimization branch coverage missing: {sorted(required - branches)}")


def run_profile(args, root: Path, template: Path, profile: str,
                history: History) -> dict:
    database = root / "sp.sbdb"
    clone_database(template, database)
    evidence = root / "insert_routes.tsv"
    optimization_evidence = root / "optimization_branches.tsv"
    insert_profile = "staged" if profile in ("staged", "scan_scalar") else "optimized"
    env = {"SCRATCHBIRD_TEST_INSERT_ROUTE": insert_profile,
           "SCRATCHBIRD_TEST_INSERT_ROUTE_EVIDENCE": str(evidence),
           "SCRATCHBIRD_TEST_DML_OPTIMIZATION": profile if profile in OPTIMIZATION_PROFILES else "normal",
           "SCRATCHBIRD_TEST_DML_OPTIMIZATION_EVIDENCE": str(optimization_evidence)}
    route = None
    output = {}
    try:
        route = start_route(args, root / "r", database, tls_required=False, extra_env=env)
        result = run_isql(args, route, "history", history.sql,
                          timeout=300 if history.affected_rows is not None else 90)
        output["history"] = observation(result, history.diagnostics)
        probes = [line.strip() for line in result.stdout.splitlines()
                  if line.strip() and not line.startswith("Rows affected: ")
                  and line.strip() not in ("BAIL set to OFF", "BAIL set to ON")]
        if probes != history.probes:
            raise RuntimeError(f"{history.name}/{profile}: probes {probes!r} != {history.probes!r}")
        affected = [int(line.removeprefix("Rows affected: "))
                    for line in result.stdout.splitlines()
                    if line.startswith("Rows affected: ")]
        expected_affected = history.inserted_rows if history.affected_rows is None else history.affected_rows
        if not affected or sum(affected) != expected_affected:
            raise RuntimeError(f"{history.name}/{profile}: incorrect client completion counts: {affected!r}")
        # Actual successful executor exits, not requested-profile evidence.
        transaction_ids = require_route_evidence(
            evidence.read_text() if evidence.exists() else "", insert_profile,
            history.inserted_rows, history.execution_rows)
        if history.inserted_rows:
            require_optimization_evidence(
                optimization_evidence.read_text() if optimization_evidence.exists() else "", profile)
        if history.schedule is not None:
            require_mixed_optimization_evidence(optimization_evidence.read_text(), profile)
        # Heap scan and every key's value are checked independently of pairwise
        # equality. This rejects two identically wrong implementations.
        verification = f"SELECT id FROM {TABLE} ORDER BY id;\n" + "\n".join(
            f"SELECT payload FROM {TABLE} WHERE id = {key};"
            for key, _ in history.final_rows) + "\n"
        expected = [str(key) for key, _ in history.final_rows] + [
            "(null)" if value is None else value for _, value in history.final_rows]
        for phase_index, phase in enumerate(("live", "reopen", "reopen_again")):
            if phase != "live":
                stop_route(route)
                route = None
                route = start_route(args, root / f"r{phase_index}", database,
                                    tls_required=False, extra_env=env)
            result = run_isql(args, route, phase, verification, timeout=90)
            output[phase] = observation(result)
            if isql_data_lines(result) != expected:
                raise RuntimeError(f"{history.name}/{profile}/{phase}: wrong durable rows")
        stop_route(route)
        route = None
        inventory = subprocess.run([args.inventory_probe, str(database)],
                                   capture_output=True, text=True, timeout=30)
        (root / "inventory.tsv").write_text(inventory.stdout)
        (root / "inventory.stderr").write_text(inventory.stderr)
        if inventory.returncode or inventory.stderr or not inventory.stdout:
            raise RuntimeError(f"{history.name}/{profile}: durable inventory read failed")
        output["mutation_inventory"] = mutation_inventory(
            inventory.stdout, transaction_ids, history.transaction_states)
        (root / "observations.json").write_text(json.dumps(output, indent=2) + "\n")
        return output
    finally:
        stop_route(route)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("server", "listener", "parser-worker", "sb-isql",
                 "example-db-seeder", "inventory-probe", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--seed", type=int, action="append")
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--optimization-profile", choices=OPTIMIZATION_PROFILES,
                        action="append", default=[])
    parser.add_argument("--replay-history", type=Path,
                        help="Replay a preserved history.json or reduced_history.json")
    parser.add_argument("--reduction-attempts", type=int, default=16,
                        help="Real-engine reduction budget per failed mixed history; 0 disables")
    args = parser.parse_args()
    if args.reduction_attempts < 0:
        parser.error("reduction-attempts must be nonnegative")
    seeds = args.seed if args.seed is not None else [901, 902]
    replay = None
    if args.replay_history:
        if args.seed or args.case:
            parser.error("replay-history cannot be combined with seed/case selection")
        recorded = json.loads(args.replay_history.read_text())
        seeds = [recorded.pop("seed")]
        for field in ("diagnostics", "transaction_states", "execution_rows"):
            recorded[field] = tuple(recorded[field])
        recorded["final_rows"] = [tuple(row) for row in recorded["final_rows"]]
        replay = History(**recorded)
    available = {case.name for case in histories(seeds[0])}
    if set(args.case) - available:
        parser.error("unknown case: " + ",".join(sorted(set(args.case) - available)))
    work = Path(tempfile.mkdtemp(prefix="sbi9_"))
    requested = Path(args.work_dir)
    requested.mkdir(parents=True, exist_ok=True)
    (requested / "artifact_path.txt").write_text(str(work) + "\n")
    print(f"insert_equivalence_artifacts={work}", flush=True)
    template = work / "t" / "sp.sbdb"
    route = None
    try:
        # One seeded baseline, closed before either profile is cloned. Starting
        # the listener also establishes bootstrap metadata before the fork.
        route = start_route(args, work / "t" / "r", template, tls_required=False,
                            extra_env={"SCRATCHBIRD_TEST_INSERT_ROUTE": "optimized",
                                       "SCRATCHBIRD_TEST_INSERT_ROUTE_EVIDENCE": "",
                                       "SCRATCHBIRD_TEST_DML_OPTIMIZATION": "normal",
                                       "SCRATCHBIRD_TEST_DML_OPTIMIZATION_EVIDENCE": ""})
        result = run_isql(args, route, "baseline", f"SELECT id FROM {TABLE};\n")
        if isql_data_lines(result) != ["6"]:
            raise RuntimeError("unexpected baseline")
    finally:
        stop_route(route)
    failures = []
    index = 0
    execution_pairs = 0
    refusal_pairs = 0
    profiles = ["optimized", "staged", *dict.fromkeys(args.optimization_profile)]
    for seed in seeds:
        for history in ([replay] if replay is not None else histories(seed)):
            if args.case and history.name not in args.case:
                continue
            root = work / f"c{index:02d}"
            root.mkdir()
            (root / "history.json").write_text(json.dumps(
                {"seed": seed, **vars(history)}, indent=2) + "\n")
            results = {}
            reduction_target = None
            for profile_index, profile in enumerate(profiles):
                try:
                    results[profile] = run_profile(args, root / f"p{profile_index}", template,
                                                   profile, history)
                except Exception as error:
                    failures.append(f"{seed}/{history.name}/{profile}: {error}")
                    print(failures[-1], flush=True)
                    if reduction_target is None and failure_class(error) is not None:
                        reduction_target = (profile, failure_class(error))
            if len(results) == len(profiles):
                if any(result != results["optimized"] for result in results.values()):
                    failures.append(f"{seed}/{history.name}: differential observation mismatch")
                    print(failures[-1], flush=True)
                    reduction_target = (next(profile for profile in profiles
                        if results[profile] != results["optimized"]), "differential")
                else:
                    for profile_index in range(len(profiles)):
                        clean_passed_database(root / f"p{profile_index}")
                    if history.admitted:
                        execution_pairs += 1
                        print(f"{seed}/{history.name}=passed", flush=True)
                    else:
                        refusal_pairs += 1
                        print(f"{seed}/{history.name}=unsupported_surface_refused (NOT conflict execution proof)", flush=True)
            if reduction_target and history.schedule is not None and args.reduction_attempts:
                try:
                    reduced_path = minimize_failure(args, root, template, seed, history,
                                                    *reduction_target)
                    print(f"reduced_failure={reduced_path}", flush=True)
                except Exception as error:
                    failures.append(f"{seed}/{history.name}/reduction: {error}")
                    print(failures[-1], flush=True)
            index += 1
    (work / "summary.json").write_text(json.dumps(
        {"histories": index, "seeds": seeds, "profiles": profiles,
         "profile_comparisons": execution_pairs * (len(profiles) - 1),
         "execution_pairs": execution_pairs,
         "unadmitted_refusal_pairs": refusal_pairs, "failures": failures}, indent=2) + "\n")
    if failures:
        print(f"insert_equivalence=failed failures={len(failures)}", flush=True)
        return 1
    clean_passed_database(template.parent)
    print(f"insert_equivalence=passed execution_pairs={execution_pairs} "
          f"unadmitted_refusal_pairs={refusal_pairs}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
