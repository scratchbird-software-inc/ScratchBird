#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Column-reference refusals through the real client/listener/parser/server route.

SEARCH_KEY: SB_NATIVE_BINDING_COLUMN_NOT_FOUND (owner Q193, Q197, Q198, Q199 = 1,
2026-11-14/15). A column reference that no column of the bound relation answers
is refused with SBSQL.NATIVE_BINDING.COLUMN_NOT_FOUND and SQLSTATE 42703; one
that more than one visible join column answers with
SBSQL.NATIVE_BINDING.COLUMN_AMBIGUOUS (SQLSTATE 42702; owner Q202 = 1,
2026-11-15; was 42000); neither is the generic
SBSQL.NATIVE_BINDING.CONTEXT_INVALID. Controls that resolve return their rows.

SEARCH_KEY: SB_NATIVE_BINDING_QUALIFIER_NOT_FOUND (owner Q203 = 1, 2026-11-15).
A column qualifier that names no source of the query is refused with
SBSQL.NATIVE_BINDING.QUALIFIER_NOT_FOUND (SQLSTATE 42000) instead of the
generic refusal. Qualifiers follow the Q177 case rules: quoted "X" equals the
fold of unquoted x and resolves; quoted "x" does not and is refused. The
structured fields (qualifier, qualifier_quoted) are not carried by the SBWP
error frame and are not observed here. The qualifier cases run on the
two-source CROSS JOIN route; one single-source filter case covers the
single-source site.

Evidence scope: the error frame's SQLSTATE (C) and diagnostic code (D) as the
client receives them. The structured diagnostic fields (column_reference,
column_reference_quoted, relation_uuid) are not carried by the SBWP error frame
and are not observed here. Column-name matching itself (Q186/Q189) is not what
this gate pins: it uses spellings that match exactly or not at all.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sbsql_sblr_alignment"))
from native_result_metadata_full_route_test import observe_query  # noqa: E402
from sbsql_copy_persistence_full_route_gate import (  # noqa: E402
    VERIFIER, execute_query, start_route, stop_route,
)
from sbsql_sbwp_tls_engine_auth_route_smoke import (  # noqa: E402
    FEATURE_STREAMING, MSG_TERMINATE, authenticate, connect_tls,
    generate_server_cert, send_frame,
)

NOT_FOUND = ("42703", "SBSQL.NATIVE_BINDING.COLUMN_NOT_FOUND")
# Owner Q202 = 1 (2026-11-15): 42702. (Was: "42000".)
AMBIGUOUS = ("42702", "SBSQL.NATIVE_BINDING.COLUMN_AMBIGUOUS")
# Owner Q203 = 1 (2026-11-15): new code; 42000 (no qualifier SQLSTATE).
QUALIFIER_NOT_FOUND = ("42000", "SBSQL.NATIVE_BINDING.QUALIFIER_NOT_FOUND")

SETUP = (
    "CREATE TABLE crg_left (id BIGINT, left_only BIGINT);",
    "CREATE TABLE crg_right (id BIGINT, right_only BIGINT);",
    "INSERT INTO crg_left (id, left_only) VALUES (1, 10);",
    "INSERT INTO crg_right (id, right_only) VALUES (2, 20);",
    "COMMIT;",
)

# (name, sql, expected rows or None, expected (sqlstate, code) or None)
CASES = (
    ("single_control", "SELECT left_only FROM crg_left;", [["10"]], None),
    ("single_not_found", "SELECT missing_col FROM crg_left;", None, NOT_FOUND),
    ("single_quoted_not_found", 'SELECT "Missing" FROM crg_left;', None, NOT_FOUND),
    ("join_filter_control",
     "SELECT * FROM crg_left AS a CROSS JOIN crg_right AS b WHERE a.id = 1;",
     [["1", "10", "2", "20"]], None),
    ("join_filter_not_found",
     "SELECT * FROM crg_left AS a CROSS JOIN crg_right AS b WHERE nope = 1;",
     None, NOT_FOUND),
    ("join_filter_qualified_not_found",
     "SELECT * FROM crg_left AS a CROSS JOIN crg_right AS b WHERE a.nope = 1;",
     None, NOT_FOUND),
    ("join_filter_ambiguous",
     "SELECT * FROM crg_left AS a CROSS JOIN crg_right AS b WHERE id = 1;",
     None, AMBIGUOUS),
    ("join_project_not_found",
     "SELECT nope FROM crg_left AS a CROSS JOIN crg_right AS b WHERE a.id = 1;",
     None, NOT_FOUND),
    ("join_project_ambiguous",
     "SELECT id FROM crg_left AS a CROSS JOIN crg_right AS b WHERE a.id = 1;",
     None, AMBIGUOUS),
    # Owner Q203 = 1 (2026-11-15). Before the change, x.id with no source x
    # was refused by the parser as QOW-DIAG-QRY-001-AST-MALFORMED (join) or
    # as COLUMN_NOT_FOUND / 42703 (single source); "X".id already resolved.
    ("qualifier_unqualified_control",
     "SELECT * FROM crg_left AS x CROSS JOIN crg_right AS b WHERE left_only = 10;",
     [["1", "10", "2", "20"]], None),
    ("qualifier_join_filter_not_found",
     "SELECT * FROM crg_left AS a CROSS JOIN crg_right AS b WHERE x.id = 1;",
     None, QUALIFIER_NOT_FOUND),
    ("qualifier_join_project_not_found",
     "SELECT x.id FROM crg_left AS a CROSS JOIN crg_right AS b WHERE a.id = 1;",
     None, QUALIFIER_NOT_FOUND),
    ("qualifier_quoted_upper_resolves",
     'SELECT * FROM crg_left AS x CROSS JOIN crg_right AS b WHERE "X".id = 1;',
     [["1", "10", "2", "20"]], None),
    ("qualifier_quoted_lower_not_found",
     'SELECT * FROM crg_left AS x CROSS JOIN crg_right AS b WHERE "x".id = 1;',
     None, QUALIFIER_NOT_FOUND),
    ("qualifier_quoted_lower_project_not_found",
     'SELECT "x".id FROM crg_left AS x CROSS JOIN crg_right AS b WHERE x.id = 1;',
     None, QUALIFIER_NOT_FOUND),
    ("qualifier_single_filter_not_found",
     "SELECT left_only FROM crg_left WHERE x.id = 1;",
     None, QUALIFIER_NOT_FOUND),
)


def make_work_dir(preferred_root: Path) -> Path:
    """A fresh directory short enough for the route's local socket names."""
    roots = (preferred_root, Path(tempfile.gettempdir()))
    if os.name == "posix":
        roots += (Path("/tmp"),)
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="crg_", dir=root))
        endpoint = candidate / "r/lc" / ("sbsql_" + "0" * 32 + ".management.sock")
        if len(os.fsencode(endpoint)) < 100:
            return candidate
        candidate.rmdir()
    raise RuntimeError("unable to allocate a short route workspace")


def error_frames(errors: list[str]) -> list[dict[str, str]]:
    """The SBWP error frames among `errors`, as {field letter: text}."""
    frames = []
    for error in errors:
        prefix = "query error frame: "
        if not error.startswith(prefix):
            continue
        payload = bytes.fromhex(error[len(prefix):].split()[0])
        fields = {}
        for part in payload.split(b"\x00"):
            if part:
                fields[chr(part[0])] = part[1:].decode("utf-8", "replace")
        frames.append(fields)
    return frames


def check_case(route, name, sql, expected_rows, expected_error) -> list[str]:
    _, rows, errors = observe_query(route, sql)
    rows = [[v.decode() if v is not None else None for v in row] for row in rows]
    frames = error_frames(errors)
    problems = []
    if expected_error is None:
        if frames or sorted(rows) != sorted(expected_rows):
            problems.append(f"control expected rows {expected_rows!r}; rows {rows!r} errors {errors!r}")
        return problems
    sqlstate, code = expected_error
    if rows:
        problems.append(f"refusal returned rows {rows!r}")
    if len(frames) != 1:
        problems.append(f"expected exactly one error frame; got {errors!r}")
        return problems
    frame = frames[0]
    if frame.get("D") != code:
        problems.append(f"diagnostic code {frame.get('D')!r}, expected {code!r}")
    if frame.get("C") != sqlstate:
        problems.append(f"SQLSTATE {frame.get('C')!r}, expected {sqlstate!r}")
    if "CONTEXT_INVALID" in "".join(frame.values()):
        problems.append("generic CONTEXT_INVALID reached the client")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    work = make_work_dir(Path(args.work_dir).resolve())
    cert, key = generate_server_cert(args.openssl, work)
    route = None
    failures = 0
    observed = []
    try:
        route = start_route(args, work / "r", work / "crg.sbdb", tls_required=True, cert=cert, key=key)
        with connect_tls(route.port) as sock:
            attachment, sequence, txn_id = authenticate(
                sock, VERIFIER.encode(), p1_features=FEATURE_STREAMING)
            for sql in SETUP:
                sequence, _, txn_id = execute_query(
                    sock, sequence, attachment, txn_id, sql, require_rows=False)
            send_frame(sock, MSG_TERMINATE, sequence, b"", attachment=attachment, txn_id=txn_id)
        for name, sql, expected_rows, expected_error in CASES:
            problems = check_case(route, name, sql, expected_rows, expected_error)
            observed.append(name)
            failures += bool(problems)
            print(json.dumps({"case": name, "sql": sql, "problems": problems}), flush=True)
        if observed != [case[0] for case in CASES]:
            raise RuntimeError("missing, duplicate or reordered case")
        print(json.dumps({"expected": len(CASES), "observed": len(observed), "failed": failures}),
              flush=True)
    finally:
        stop_route(route)
    if failures == 0:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"artifacts={work}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
