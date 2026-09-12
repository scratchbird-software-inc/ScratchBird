#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Prove refusal preserves the outer transaction and target savepoint.

This companion does not replace positive mutation/rollback coverage. Unsupported
mutation profiles remain fail-closed. Ordinary DELETE and TEXT assignment
have positive coverage separately; TEXT predicate authority is independently refused.
"""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    isql_data_lines, run_isql, start_route, stop_route,
)
from sbsql_savepoint_rollback_full_route_gate import clean_passed_database

TABLE = "sbsfc021_stream_table"


def run_case(args: argparse.Namespace, work: Path, name: str,
             rejected_sql: str, diagnostic: str) -> None:
    root = work / name
    database = root / "sp.sbdb"
    route = None
    try:
        route = start_route(args, root / "r", database, tls_required=False)
        result = run_isql(args, route, "refuse_then_continue", "\n".join([
            f"INSERT INTO {TABLE} (id, payload) VALUES (1, 'seed');",
            "COMMIT;",
            f"INSERT INTO {TABLE} (id, payload) VALUES (4, 'outer');",
            "SAVEPOINT outer_sp;",
            f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'later');",
            # Continue in this exact client connection: reconnecting would
            # conceal an accidentally aborted outer transaction.
            "SET BAIL OFF;", rejected_sql, "SET BAIL ON;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 4;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 2;",
            f"SELECT payload FROM {TABLE} WHERE id = 1;",
            "ROLLBACK TO SAVEPOINT outer_sp;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 4;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 2;",
            f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'again');",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 2;",
            "ROLLBACK TO SAVEPOINT outer_sp;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 2;",
            "RELEASE SAVEPOINT outer_sp;", "COMMIT;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 4;", "",
        ]))
        errors = result.stderr.splitlines()
        unexpected_diagnostic = (result.returncode != 1 or len(errors) != 1 or
                                 not errors[0].startswith("Error: ") or
                                 not errors[0].endswith(f"({diagnostic})"))
        # Do not call the success-only normalizer on an intentional SQL error.
        ignored = ("Rows affected: ", "Transaction ", "COPY ")
        probes = [line.strip() for line in result.stdout.splitlines()
                  if line.strip() and not line.strip().startswith(ignored)
                  and line.strip() not in ("BAIL set to OFF", "BAIL set to ON")]
        expected = ["1", "1", "seed", "1", "0", "1", "0", "1"]
        if probes != expected:
            raise RuntimeError(f"{name}: same-connection probes {probes!r}, "
                               f"expected {expected!r}")
        for phase in ("committed", "reopened"):
            if phase == "reopened":
                stop_route(route)
                route = None
                route = start_route(args, root / "r2", database, tls_required=False)
            rows = run_isql(args, route, phase, "\n".join([
                f"SELECT id FROM {TABLE} ORDER BY id;",
                f"SELECT payload FROM {TABLE} WHERE id = 1;",
                f"SELECT payload FROM {TABLE} WHERE id = 4;", "",
            ]))
            actual = isql_data_lines(rows)
            # id 6 belongs to the approved database seed fixture.
            if actual != ["1", "4", "6", "seed", "outer"]:
                raise RuntimeError(f"{name}/{phase}: retained outer work {actual!r}")
        # Collect state evidence even when the diagnostic is unexpected, but
        # do not turn an unexplained execution failure into a passing case.
        if unexpected_diagnostic:
            raise RuntimeError(f"{name}: live/committed/reopened state preserved; "
                               f"expected one {diagnostic} refusal, "
                               f"rc={result.returncode} stderr={result.stderr!r}")
    finally:
        stop_route(route)


def main() -> int:
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "sb-isql",
                 "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="sbspr_"))
    requested = Path(args.work_dir)
    requested.mkdir(parents=True, exist_ok=True)
    (requested / "artifact_path.txt").write_text(str(work) + "\n", encoding="utf-8")
    cases = {
        "catalog_mutation": ("CREATE TABLE issue8_refused_catalog (id INTEGER);",
                             "SBLR.OPERATION_UNSUPPORTED"),
        # These syntaxes have no admitted canonical full route yet. They
        # prove preservation on parser/envelope refusal, NOT that the engine
        # savepoint capability registry was reached. Its lower-level producer
        # checks are exercised separately by the engine component gate.
        "security_unadmitted_route": ("CREATE ROLE issue8_refused_role;",
                                      "SBLR.OPERATION.NONCANONICAL"),
        "maintenance_unadmitted_route": (f"TRUNCATE TABLE {TABLE};",
                                         "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN"),
        "text_predicate": (f"UPDATE {TABLE} SET payload = 'discard' WHERE payload = 'seed';",
                        "DATATYPE.DESCRIPTOR.INVALID"),
        # Native Query carries NUL-terminated SQL; embedded U+0000 VALUE is
        # exercised by the binary-length engine binder, not by raw SQL text.
        "raw_nul_sql": (f"UPDATE {TABLE} SET payload = 'a\0b' WHERE id = 1;",
                        "SBSQL.LEXER.STRING_UNCLOSED"),
        "delete_text_predicate": (f"DELETE FROM {TABLE} WHERE payload = 'seed';",
                                  # A well-formed TEXT predicate exceeds the
                                  # DELETE binder's admitted integer profile.
                                  "SBLR.OPERATION_UNSUPPORTED"),
        "unique_insert": (f"INSERT INTO {TABLE} (id, payload) VALUES (1, 'conflict');",
                          "CLI.CONSTRAINT_UNIQUE_VIOLATION"),
        "unique_update": (f"UPDATE {TABLE} SET id = 6 WHERE id = 1;",
                          # The current UPDATE index validator emits this
                          # generic diagnostic for unique_index_duplicate.
                          # This gate proves state preservation, not parity
                          # with INSERT's specific constraint diagnostic.
                          "SB_ENGINE_API_INVALID_REQUEST"),
        "missing_rollback_label": ("ROLLBACK TO SAVEPOINT absent_sp;",
                                   "MGA.SAVEPOINT.HANDLE_REQUIRED"),
        "missing_release_label": ("RELEASE SAVEPOINT absent_sp;",
                                  "MGA.SAVEPOINT.HANDLE_REQUIRED"),
        "unique_multirow_update": (f"UPDATE {TABLE} SET id = 5;",
                                   "SB_ENGINE_API_INVALID_REQUEST"),
        "rolled_back_descendant_label": (
            "SAVEPOINT child_sp;\nROLLBACK TO SAVEPOINT outer_sp;\n"
            f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'later');\n"
            "ROLLBACK TO SAVEPOINT child_sp;", "MGA.SAVEPOINT.HANDLE_REQUIRED"),
        "released_label": (
            "SAVEPOINT child_sp;\nRELEASE SAVEPOINT child_sp;\n"
            "ROLLBACK TO SAVEPOINT child_sp;", "MGA.SAVEPOINT.HANDLE_REQUIRED"),
        "released_replacement_label": (
            "SAVEPOINT reused_sp;\nSAVEPOINT reused_sp;\n"
            "RELEASE SAVEPOINT reused_sp;\nROLLBACK TO SAVEPOINT reused_sp;",
            "MGA.SAVEPOINT.HANDLE_REQUIRED"),
    }
    failures = []
    for name, (sql, diagnostic) in cases.items():
        try:
            run_case(args, work, name, sql, diagnostic)
            clean_passed_database(work / name)
            print(f"{name}=passed", flush=True)
        except Exception as error:
            failures.append(name)
            print(f"{name}=failed {error}", flush=True)
    print(f"savepoint_refusal_full_route cases={len(cases)} "
          f"failures={len(failures)} artifacts={work}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
