#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real CTE child-DAG route regression; this does NOT qualify recursive execution."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sbsql_parser_worker"))
from sbsql_copy_persistence_full_route_gate import (
    isql_data_lines, require_isql_success, run_isql, start_route, stop_route,
)


def main():
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "sb-isql", "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    root = Path(args.work_dir).resolve()
    # Refuse reuse: evidence and database state belong to exactly this run.
    root.mkdir(parents=True, exist_ok=False)
    database = root / "cte.sbdb"
    route = None
    calls = 0
    try:
        route = start_route(args, root / "r", database, tls_required=False)
        setup = run_isql(args, route, "setup",
            "CREATE TABLE cte_identity_rows (id BIGINT);\n"
            "INSERT INTO cte_identity_rows (id) VALUES (2);\n"
            "INSERT INTO cte_identity_rows (id) VALUES (9);\nCOMMIT;\n")
        calls += 1
        require_isql_success(setup)
        for phase in ("live", "reopened"):
            if phase == "reopened":
                stop_route(route)
                route = None
                route = start_route(args, root / "r2", database, tls_required=False)
            for name, query in (
                ("plain", "WITH r AS (SELECT * FROM cte_identity_rows) SELECT * FROM r;"),
                ("quoted", 'WITH "R" AS (SELECT * FROM cte_identity_rows) SELECT * FROM "R";'),
                ("comments", "WITH /*scope*/ r AS (SELECT * FROM cte_identity_rows) /*body*/ SELECT * FROM r;"),
                ("independent_read", "SELECT id FROM cte_identity_rows ORDER BY id;"),
            ):
                result = run_isql(args, route, phase + "_" + name, query + "\n")
                calls += 1
                require_isql_success(result)
                actual = isql_data_lines(result)
                # CTE identity has no ORDER BY; assert its complete multiset.
                if (actual if name == "independent_read" else sorted(actual)) != ["2", "9"]:
                    raise RuntimeError(f"{phase}/{name}: expected complete rows 2,9; got {actual!r}")
                print(f"PASS {phase}/{name} rows={actual!r}")
            # Fixed expected cases, independent of what the parser accepts.
            # The keyword does not make a definition without self-reference recursive.
            for name, body, expected in (
                ("values", "VALUES(2),(9)", ["2", "9"]),
                ("scalar", "SELECT 7", ["7"]),
                ("filter", "SELECT id FROM cte_identity_rows WHERE id = 2", ["2"]),
                ("count", "SELECT COUNT(*) FROM cte_identity_rows", ["2"]),
                ("limit", "SELECT * FROM cte_identity_rows LIMIT 1", None),
                ("nested", "WITH inner_cte AS (VALUES(2),(9)) SELECT * FROM inner_cte", ["2", "9"]),
            ):
                for keyword in ("", "RECURSIVE "):
                    case = phase + "_" + ("keyword_" if keyword else "") + name
                    result = run_isql(args, route, case,
                        f"WITH {keyword}r AS ({body}) SELECT * FROM r;\n")
                    calls += 1
                    require_isql_success(result)
                    actual = isql_data_lines(result)
                    if expected is None:
                        valid = len(actual) == 1 and actual[0] in ("2", "9")
                    else:
                        valid = sorted(actual) == expected
                    if not valid:
                        raise RuntimeError(f"{case}: expected {expected!r}; got {actual!r}")
                    print(f"PASS {case} rows={actual!r}")
            # Every invocation opens an independent client session. A missing
            # recursive route must neither return fake rows nor alter the table.
            refused = run_isql(args, route, phase + "_recursive_pending",
                "WITH RECURSIVE r(n) AS (VALUES(1) UNION ALL SELECT n + 2 FROM r WHERE n < 7) SELECT * FROM r;\n")
            calls += 1
            errors = refused.stderr.splitlines()
            if (refused.returncode != 1 or len(errors) != 2 or
                    not errors[0].startswith("Error: ") or
                    not errors[0].endswith("(SBSQL.IMPL.NOT_AVAILABLE)") or
                    errors[1] != "Stopping due to error (SET BAIL is ON)"):
                raise RuntimeError(f"unexpected recursive boundary: {refused!r}")
            if refused.stdout:
                raise RuntimeError(f"recursive refusal published output: {refused.stdout!r}")
            after = run_isql(args, route, phase + "_after_refusal",
                             "SELECT id FROM cte_identity_rows ORDER BY id;\n")
            calls += 1
            require_isql_success(after)
            if isql_data_lines(after) != ["2", "9"]:
                raise RuntimeError("recursive refusal changed independent table state")
            for layer, trace in route.traces.items():
                if not trace.is_file() or not trace.stat().st_size:
                    raise RuntimeError(f"missing actual {phase}/{layer} route trace")
            print(f"PASS {phase}/recursive_pending exact refusal and unchanged independent state")
        if calls != 37:
            raise RuntimeError(f"expected 37 real client invocations; observed {calls}")
        print(f"PASS invocations={calls} phases=2; child DAGs; recursion remains required")
    finally:
        stop_route(route)
    # The caller retains compact evidence before removing this exact owned
    # root. No passing/failed fixture is silently erased by a result collector.


if __name__ == "__main__":
    main()
