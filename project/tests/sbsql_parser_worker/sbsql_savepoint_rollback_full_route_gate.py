#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Named savepoint rollback via the real client/listener/parser/server route.

This is row/index visibility and process-restart evidence, not comprehensive
mutation-capability or crash-during-compensation coverage.
"""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    isql_data_lines, require_isql_success, run_isql, start_route, stop_route,
)

TABLE = "sbsfc021_stream_table"


def clean_passed_database(root: Path) -> None:
    """Discard owned database fixtures only; preserve scripts and route logs."""
    for artifact in root.glob("sp.sbdb*"):
        if artifact.is_dir() and not artifact.is_symlink():
            shutil.rmtree(artifact)
        else:
            artifact.unlink()
    # Filespace growth produces sibling segments, not sp.sbdb companions.
    # Each caller supplies its isolated, stopped case directory.
    for pattern in ("scratchbird_filespace_growth_*.sbfs",
                    "scratchbird_filespace_growth_*.sbfs.sb.owner.lock"):
        for artifact in root.glob(pattern):
            if artifact.is_file() or artifact.is_symlink():
                artifact.unlink()


def run_case(args: argparse.Namespace, work: Path, name: str,
             statements: list[str], expected_probes: list[str]) -> None:
    root = work / name
    database = root / "sp.sbdb"
    route = None
    try:
        route = start_route(args, root / "r", database, tls_required=False)
        result = run_isql(args, route, "mutate", "\n".join([
            f"INSERT INTO {TABLE} (id, payload) VALUES (1, 'seed');",
            "COMMIT;", *statements, "COMMIT;", "",
        ]))
        require_isql_success(result)
        probes = [line for line in isql_data_lines(result)
                  if not line.startswith("Rows affected: ")]
        if probes != expected_probes:
            raise RuntimeError(f"{name}: mutation/rollback probes {probes!r}, "
                               f"expected {expected_probes!r}")
        for phase in ("live", "reopened"):
            if phase == "reopened":
                stop_route(route)
                route = None
                route = start_route(args, root / "r2", database, tls_required=False)
            rows = run_isql(args, route, phase, "\n".join([
                f"SELECT id FROM {TABLE} ORDER BY id;",
                f"SELECT payload FROM {TABLE} WHERE id = 1;", "",
            ]))
            actual = isql_data_lines(rows)
            # The approved database fixture already contains id 6.
            if actual != ["1", "6", "seed"]:
                raise RuntimeError(f"{name}/{phase}: expected baseline rows, got {actual!r}")
            # Neither a rolled-back insert nor an UPDATE's new key may leave
            # an index/uniqueness ghost.
            reuse_key = 3 if name in ("update", "nested_update") else 2
            require_isql_success(run_isql(args, route, phase + "_reuse_key",
                f"INSERT INTO {TABLE} (id, payload) VALUES ({reuse_key}, 'reuse');\nROLLBACK;\n"))
            count = run_isql(args, route, phase + "_after_reuse",
                             f"SELECT COUNT(*) FROM {TABLE};\n")
            if isql_data_lines(count) != ["2"]:
                raise RuntimeError(f"{name}/{phase}: rollback after key reuse changed rows")
    finally:
        stop_route(route)


def run_all_rows_case(args: argparse.Namespace, work: Path) -> None:
    """Exercise empty, mutating and no-effect TRUE predicates without a bypass."""
    root = work / "all_rows_update"
    database = root / "sp.sbdb"
    table = "savepoint_all_rows_update"
    route = None
    try:
        route = start_route(args, root / "r", database, tls_required=False)
        require_isql_success(run_isql(args, route, "create", "\n".join([
            f"CREATE TABLE {table} (id BIGINT, account_balance BIGINT);",
            "COMMIT;", "",
        ])))
        result = run_isql(args, route, "mutate", "\n".join([
            f"UPDATE {table} SET account_balance = 7;",
            f"SELECT COUNT(*) FROM {table};",
            f"INSERT INTO {table} (id, account_balance) VALUES (11, 1);",
            f"INSERT INTO {table} (id, account_balance) VALUES (12, 3);",
            "COMMIT;", "SAVEPOINT outer_sp;",
            f"UPDATE {table} SET account_balance = 7;",
            f"UPDATE {table} SET account_balance = 7;",
            f"SELECT COUNT(*) FROM {table} WHERE account_balance = 7;",
            f"SELECT account_balance FROM {table} ORDER BY id;",
            "ROLLBACK TO SAVEPOINT outer_sp;",
            f"SELECT account_balance FROM {table} ORDER BY id;",
            f"UPDATE {table} SET account_balance = 9;",
            f"SELECT account_balance FROM {table} ORDER BY id;",
            "ROLLBACK TO SAVEPOINT outer_sp;",
            "RELEASE SAVEPOINT outer_sp;", "COMMIT;", "",
        ]))
        probes = [line for line in isql_data_lines(result)
                  if not line.startswith("Rows affected: ")]
        if probes != ["0", "2", "7", "7", "1", "3", "9", "9"]:
            raise RuntimeError(f"all_rows_update: wrong mutation/rollback probes: {result}")
        completion_lines = [line.strip() for line in result.stdout.splitlines()
                            if line.strip().startswith("Rows affected: ")]
        if (not completion_lines or completion_lines[0] != "Rows affected: 0" or
                completion_lines.count("Rows affected: 2") < 2):
            raise RuntimeError(f"all_rows_update: incorrect engine completion counts: {completion_lines}")
        for phase in ("committed", "reopened"):
            if phase == "reopened":
                stop_route(route)
                route = None
                route = start_route(args, root / "r2", database, tls_required=False)
            rows = run_isql(args, route, phase, "\n".join([
                f"SELECT id FROM {table} ORDER BY id;",
                f"SELECT account_balance FROM {table} ORDER BY id;", "",
            ]))
            if isql_data_lines(rows) != ["11", "12", "1", "3"]:
                raise RuntimeError(f"all_rows_update/{phase}: baseline not restored")
    finally:
        stop_route(route)


def run_omitted_columns_case(args: argparse.Namespace, work: Path) -> None:
    root = work / "omitted_columns"
    database = root / "sp.sbdb"
    table = "savepoint_omitted_columns"
    route = None
    try:
        route = start_route(args, root / "r", database, tls_required=False)
        require_isql_success(run_isql(args, route, "create", "\n".join([
            # Constraint-bearing CREATE TABLE is not yet an admitted parser
            # route. Literal defaults are covered by the engine constraint
            # gate; this route proves omitted nullable columns on real DML.
            f"CREATE TABLE {table} (id BIGINT, payload TEXT, balance BIGINT);",
            "COMMIT;", "",
        ])))
        result = run_isql(args, route, "mutate", "\n".join([
            f"INSERT INTO {table} (id) VALUES (11);", "COMMIT;",
            "SAVEPOINT outer_sp;",
            f"INSERT INTO {table} (id) VALUES (12);",
            f"SELECT payload FROM {table} ORDER BY id;",
            f"SELECT balance FROM {table} ORDER BY id;",
            "ROLLBACK TO SAVEPOINT outer_sp;", "ROLLBACK TO SAVEPOINT outer_sp;",
            f"SELECT id FROM {table} ORDER BY id;",
            "RELEASE SAVEPOINT outer_sp;", "COMMIT;", "",
        ]))
        probes = [line for line in isql_data_lines(result) if not line.startswith("Rows affected: ")]
        if probes != ["(null)", "(null)", "(null)", "(null)", "11"]:
            raise RuntimeError(f"omitted columns/defaults/rewind: {probes!r}")
        stop_route(route)
        route = None
        route = start_route(args, root / "r2", database, tls_required=False)
        result = run_isql(args, route, "reopened", "\n".join([
            f"SELECT id FROM {table};", f"SELECT payload FROM {table};",
            f"SELECT balance FROM {table};", "",
        ]))
        if isql_data_lines(result) != ["11", "(null)", "(null)"]:
            raise RuntimeError("omitted-column storage shape changed on restart")
    finally:
        stop_route(route)


def main() -> int:
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "sb-isql", "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--case", action="append", default=[],
                        help="Run selected cases; omitting this runs the complete matrix")
    args = parser.parse_args()
    # Keep UNIX-domain socket paths short, while retaining the actual evidence
    # location beneath the requested build directory for discovery.
    work = Path(tempfile.mkdtemp(prefix="sbsp8_"))
    requested = Path(args.work_dir)
    requested.mkdir(parents=True, exist_ok=True)
    (requested / "artifact_path.txt").write_text(str(work) + "\n", encoding="utf-8")
    def count(predicate: str) -> str:
        return f"SELECT COUNT(*) FROM {TABLE} WHERE {predicate};"

    cases = {
        "distinct_nested": (["SAVEPOINT outer_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'outer_later');",
                   "SAVEPOINT inner_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (3, 'inner_later');",
                   count("id = 2"), count("id = 3"),
                   "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2"), count("id = 3"),
                   "RELEASE SAVEPOINT outer_sp;"], ["1", "1", "0", "0"]),
        "release_parent": (["SAVEPOINT outer_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'outer_later');",
                   "SAVEPOINT inner_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (3, 'inner_later');",
                   "RELEASE SAVEPOINT outer_sp;",
                   "ROLLBACK TO SAVEPOINT inner_sp;",
                   count("id = 2"), count("id = 3"),
                   "RELEASE SAVEPOINT inner_sp;", "ROLLBACK;",
                   count("id = 2")], ["1", "0", "0"]),
        "same_name_replacement": (["SAVEPOINT outer_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'outer_later');",
                   "SAVEPOINT OUTER_SP;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (3, 'inner_later');",
                   "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2"), count("id = 3"),
                   "RELEASE SAVEPOINT OUTER_SP;", "ROLLBACK;",
                   count("id = 2")], ["1", "0", "0"]),
        "quoted_distinct": (['SAVEPOINT "Outer_SP";',
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'outer_later');",
                   "SAVEPOINT outer_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (3, 'inner_later');",
                   'ROLLBACK TO SAVEPOINT "Outer_SP";',
                   count("id = 2"), count("id = 3"),
                   'RELEASE SAVEPOINT "Outer_SP";'], ["0", "0"]),
        "insert": (["SAVEPOINT outer_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'discard');",
                   count("id = 2"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2")], ["1", "0"]),
        "update": (["SAVEPOINT outer_sp;",
                   f"UPDATE {TABLE} SET id = 3 WHERE id = 1;",
                   count("id = 3"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 3"), count("id = 1")], ["1", "0", "1"]),
        "update_text": (["SAVEPOINT outer_sp;",
                   f"UPDATE {TABLE} SET payload = 'discard' WHERE id = 1;",
                   f"SELECT payload FROM {TABLE} WHERE id = 1;",
                   "ROLLBACK TO SAVEPOINT outer_sp;",
                   f"SELECT payload FROM {TABLE} WHERE id = 1;",
                   f"UPDATE {TABLE} SET payload = 'discard_again' WHERE id = 1;",
                   f"SELECT payload FROM {TABLE} WHERE id = 1;",
                   "ROLLBACK TO SAVEPOINT outer_sp;",
                   f"SELECT payload FROM {TABLE} WHERE id = 1;"],
                   ["discard", "seed", "discard_again", "seed"]),
        "delete": (["SAVEPOINT outer_sp;", f"DELETE FROM {TABLE} WHERE id = 1;",
                   count("id = 1"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 1")], ["0", "1"]),
        "nested_insert": (["SAVEPOINT outer_sp;", "SAVEPOINT inner_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'discard');",
                   count("id = 2"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2"), "SAVEPOINT inner_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'discard_again');",
                   count("id = 2"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2"), "RELEASE SAVEPOINT outer_sp;"], ["1", "0", "1", "0"]),
        "nested_update": (["SAVEPOINT outer_sp;", "SAVEPOINT inner_sp;",
                   f"UPDATE {TABLE} SET id = 3 WHERE id = 1;",
                   count("id = 3"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 1"), "SAVEPOINT inner_sp;",
                   f"UPDATE {TABLE} SET id = 3 WHERE id = 1;",
                   count("id = 3"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 3"), "RELEASE SAVEPOINT outer_sp;"], ["1", "1", "1", "0"]),
        "nested_delete": (["SAVEPOINT outer_sp;", "SAVEPOINT inner_sp;",
                   f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'discard');",
                   count("id = 2"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 2"), "SAVEPOINT inner_sp;",
                   f"DELETE FROM {TABLE} WHERE id = 1;",
                   count("id = 1"), "ROLLBACK TO SAVEPOINT outer_sp;",
                   count("id = 1"), "RELEASE SAVEPOINT outer_sp;"], ["1", "0", "0", "1"]),
    }
    for name, value in (("update_text_empty", ""),
                        ("update_text_unicode", "e\u0301\U0001f600"),
                        ("update_text_overflow", "x" * 12000)):
        # Probe by the unaffected integer key: this is assignment/rewind
        # coverage, not admission of TEXT comparison semantics. The id probe
        # distinguishes an empty payload from a missing row in CLI rendering.
        cases[name] = (["SAVEPOINT outer_sp;",
                       f"UPDATE {TABLE} SET payload = '{value}' WHERE id = 1;",
                       f"SELECT id FROM {TABLE} WHERE id = 1;",
                       f"SELECT payload FROM {TABLE} WHERE id = 1;",
                       "ROLLBACK TO SAVEPOINT outer_sp;",
                       f"SELECT payload FROM {TABLE} WHERE id = 1;"],
                      ["1", *([value] if value else []), "seed"])
    cases["update_text_null"] = (["SAVEPOINT outer_sp;",
        f"UPDATE {TABLE} SET payload = NULL WHERE id = 1;",
        f"SELECT payload FROM {TABLE} WHERE id = 1;",
        "ROLLBACK TO SAVEPOINT outer_sp;",
        f"SELECT payload FROM {TABLE} WHERE id = 1;"], ["(null)", "seed"])
    cases["delete_all_rows"] = (["SAVEPOINT outer_sp;",
        f"DELETE FROM {TABLE};", f"SELECT COUNT(*) FROM {TABLE};",
        f"DELETE FROM {TABLE};", f"SELECT COUNT(*) FROM {TABLE};",
        "ROLLBACK TO SAVEPOINT outer_sp;",
        f"SELECT COUNT(*) FROM {TABLE};"], ["0", "0", "2"])
    cases["delete_no_match"] = (["SAVEPOINT outer_sp;",
        f"DELETE FROM {TABLE} WHERE id = 999999;",
        f"SELECT COUNT(*) FROM {TABLE};", "ROLLBACK TO SAVEPOINT outer_sp;",
        f"SELECT COUNT(*) FROM {TABLE};"], ["2", "2"])
    cases["delete_overflow"] = (["SAVEPOINT outer_sp;",
        f"UPDATE {TABLE} SET payload = '{'x' * 12000}' WHERE id = 1;",
        "SAVEPOINT overflow_sp;", f"DELETE FROM {TABLE} WHERE id = 1;",
        count("id = 1"), "ROLLBACK TO SAVEPOINT overflow_sp;", count("id = 1"),
        f"SELECT payload FROM {TABLE} WHERE id = 1;",
        "ROLLBACK TO SAVEPOINT outer_sp;"], ["0", "1", "x" * 12000])
    dedicated = {"all_rows_update": run_all_rows_case,
                 "omitted_columns": run_omitted_columns_case}
    selected = set(args.case) if args.case else set(cases) | set(dedicated)
    if selected - (set(cases) | set(dedicated)):
        parser.error("unknown savepoint case")
    failures = []
    for name, run in dedicated.items():
        if name not in selected:
            continue
        try:
            run(args, work)
            clean_passed_database(work / name)
            print(f"{name}=passed", flush=True)
        except Exception as error:
            failures.append(name)
            print(f"{name}=failed {error}", flush=True)
    for name, (statements, expected_probes) in cases.items():
        if name not in selected:
            continue
        try:
            run_case(args, work, name, statements, expected_probes)
            clean_passed_database(work / name)
            print(f"{name}=passed", flush=True)
        except Exception as error:
            failures.append(name)
            print(f"{name}=failed {error}", flush=True)
    print(f"savepoint_full_route cases={len(selected)} failures={len(failures)} artifacts={work}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
