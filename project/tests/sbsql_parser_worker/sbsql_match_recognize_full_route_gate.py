#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public SBWP/TLS prepared MATCH_RECOGNIZE full-route gate."""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    StartedRoute,
    authenticate_tls,
    dump_logs,
    generate_server_cert,
    make_work_dir,
    start_route,
    stop_route,
)
from sbsql_sbwp_tls_engine_auth_route_smoke import (
    FEATURE_STREAMING,
    MSG_COMMAND_COMPLETE,
    MSG_DATA_ROW,
    MSG_ERROR,
    MSG_READY,
    MSG_ROW_DESCRIPTION,
    MSG_TERMINATE,
    RouteError,
    authenticate,
    connect_tls,
    decode_data_row_values,
    decode_ready,
    recv_frame,
    send_frame,
)


MSG_PARSE = 0x04
MSG_BIND = 0x05
MSG_EXECUTE = 0x07
MSG_SYNC = 0x09
MSG_PARSE_COMPLETE = 0x4A
MSG_BIND_COMPLETE = 0x4B
OID_INT8 = 20
EXECUTE_FLAG_AUTOCOMMIT = 0x01

def match_recognize_sql(parameter_count: int, row_mode: str = "ALL ROWS") -> str:
    if parameter_count not in (2, 3):
        raise MatchRecognizeGateError("the bounded MATCH profile requires 2 or 3 parameters")
    arguments = ", ".join("?" for _ in range(parameter_count))
    return (
        f"SELECT * FROM generate_series({arguments}) "
        "MATCH_RECOGNIZE ("
        "PARTITION BY generate_series "
        "ORDER BY generate_series ASC "
        f"{row_mode} PER MATCH "
        "AFTER MATCH SKIP PAST LAST ROW "
        "PATTERN (A+) "
        "DEFINE A AS TRUE"
        ");"
    )


class MatchRecognizeGateError(RuntimeError):
    pass


def lpstr(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def parse_payload(statement_name: str, sql: str, parameter_oids: tuple[int, ...]) -> bytes:
    payload = bytearray(lpstr(statement_name))
    payload += lpstr(sql)
    payload += struct.pack("<HH", len(parameter_oids), 0)
    for oid in parameter_oids:
        payload += struct.pack("<I", oid)
    return bytes(payload)


def bind_payload(
    portal_name: str,
    statement_name: str,
    values: tuple[int | None, ...],
) -> bytes:
    payload = bytearray(lpstr(portal_name))
    payload += lpstr(statement_name)
    payload += struct.pack("<HH", 1, 1)  # One binary format applies to all values.
    payload += struct.pack("<HH", len(values), 0)
    for value in values:
        if value is None:
            payload += struct.pack("<i", -1)
        else:
            encoded = struct.pack("<q", value)
            payload += struct.pack("<i", len(encoded)) + encoded
    return bytes(payload)


def execute_payload(portal_name: str, *, autocommit: bool) -> bytes:
    flags = EXECUTE_FLAG_AUTOCOMMIT if autocommit else 0
    return lpstr(portal_name) + struct.pack("<II", 0, flags)


def expect_exact_frame(sock, expected: int, label: str) -> bytes:
    msg_type, payload, _, _ = recv_frame(sock)
    if msg_type != expected:
        raise RouteError(
            f"{label}: expected frame 0x{expected:02x}, got "
            f"0x{msg_type:02x}, payload={payload!r}"
        )
    return payload


def decode_error_fields(payload: bytes) -> dict[str, str]:
    fields: dict[str, str] = {}
    for encoded in payload.split(b"\x00"):
        if not encoded:
            continue
        text = encoded.decode("utf-8", errors="replace")
        if len(text) >= 2:
            fields[text[0]] = text[1:]
    return fields


def expect_error(
    sock,
    label: str,
    *,
    sqlstate: str,
    diagnostic: str,
) -> dict[str, str]:
    payload = expect_exact_frame(sock, MSG_ERROR, label)
    fields = decode_error_fields(payload)
    if fields.get("C") != sqlstate or fields.get("M") != diagnostic:
        raise RouteError(
            f"{label}: wrong diagnostic fields {fields!r}; "
            f"expected C={sqlstate!r}, M={diagnostic!r}"
        )
    return fields


def parse_prepared_match(
    sock,
    attachment: bytes,
    sequence: int,
    txn_id: int,
    statement_name: str,
    sql: str,
    parameter_count: int,
) -> int:
    send_frame(
        sock,
        MSG_PARSE,
        sequence,
        parse_payload(statement_name, sql, (OID_INT8,) * parameter_count),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    if expect_exact_frame(sock, MSG_PARSE_COMPLETE, f"{statement_name} Parse"):
        raise RouteError(f"{statement_name} ParseComplete carried an unexpected payload")
    return sequence


def bind_prepared_match(
    sock,
    attachment: bytes,
    sequence: int,
    txn_id: int,
    statement_name: str,
    portal_name: str,
    values: tuple[int | None, ...],
) -> int:
    send_frame(
        sock,
        MSG_BIND,
        sequence,
        bind_payload(portal_name, statement_name, values),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    if expect_exact_frame(sock, MSG_BIND_COMPLETE, f"{statement_name} Bind"):
        raise RouteError(f"{statement_name} BindComplete carried an unexpected payload")
    return sequence


def execute_bound_match(
    sock,
    attachment: bytes,
    sequence: int,
    txn_id: int,
    portal_name: str,
    expected_rows: list[list[str]],
) -> int:
    send_frame(
        sock,
        MSG_EXECUTE,
        sequence,
        execute_payload(portal_name, autocommit=False),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    send_frame(
        sock,
        MSG_SYNC,
        sequence,
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1

    rows: list[list[str]] = []
    saw_description = False
    saw_complete = False
    while True:
        msg_type, payload, _, frame_txn = recv_frame(sock)
        if msg_type == MSG_ROW_DESCRIPTION:
            if saw_description:
                raise RouteError("MATCH_RECOGNIZE emitted duplicate ROW_DESCRIPTION")
            saw_description = True
            continue
        if msg_type == MSG_DATA_ROW:
            values_raw = decode_data_row_values(payload)
            rows.append(
                [
                    "<NULL>" if value is None else value.decode("utf-8")
                    for value in values_raw
                ]
            )
            continue
        if msg_type == MSG_COMMAND_COMPLETE:
            if saw_complete:
                raise RouteError("MATCH_RECOGNIZE emitted duplicate COMMAND_COMPLETE")
            saw_complete = True
            continue
        if msg_type == MSG_ERROR:
            raise RouteError(f"MATCH_RECOGNIZE Execute failed: {payload!r}")
        if msg_type == MSG_READY:
            status, ready_txn = decode_ready(payload)
            if (
                status == 0
                or ready_txn != txn_id
                or frame_txn == 0
                or frame_txn != txn_id
            ):
                raise RouteError(
                    "MATCH_RECOGNIZE did not preserve the exact active MGA transaction"
                )
            break
        raise RouteError(
            f"MATCH_RECOGNIZE returned unexpected frame "
            f"0x{msg_type:02x}: {payload!r}"
        )
    if not saw_description or not saw_complete:
        raise RouteError("MATCH_RECOGNIZE did not publish a complete row result")
    if rows != expected_rows:
        raise RouteError(
            f"MATCH_RECOGNIZE rows mismatch: actual={rows!r} "
            f"expected={expected_rows!r}"
        )
    return sequence


def execute_prepared_match(
    port: int,
    *,
    case: str,
    values: tuple[int, ...],
    expected_rows: list[list[str]],
) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        statement_name = f"match_recognize_{case}_stmt"
        portal_name = f"match_recognize_{case}_portal"
        sequence = parse_prepared_match(
            sock,
            attachment,
            sequence,
            txn_id,
            statement_name,
            match_recognize_sql(len(values)),
            len(values),
        )
        sequence = bind_prepared_match(
            sock,
            attachment,
            sequence,
            txn_id,
            statement_name,
            portal_name,
            values,
        )
        sequence = execute_bound_match(
            sock,
            attachment,
            sequence,
            txn_id,
            portal_name,
            expected_rows,
        )
        send_frame(sock, MSG_TERMINATE, sequence, attachment=attachment)
    finally:
        sock.close()


def require_bind_shape_refusal(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        statement_name = "match_recognize_bad_bind_stmt"
        sequence = parse_prepared_match(
            sock,
            attachment,
            sequence,
            txn_id,
            statement_name,
            match_recognize_sql(3),
            3,
        )
        send_frame(
            sock,
            MSG_BIND,
            sequence,
            bind_payload("bad_bind_portal", statement_name, (5, 1)),
            attachment=attachment,
            txn_id=txn_id,
        )
        expect_error(
            sock,
            "MATCH_RECOGNIZE malformed Bind",
            sqlstate="08P01",
            diagnostic="invalid BIND payload",
        )
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)
    finally:
        sock.close()


def require_profile_refusal(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        send_frame(
            sock,
            MSG_PARSE,
            sequence,
            parse_payload(
                "match_recognize_unimplemented_profile",
                match_recognize_sql(3, "ONE ROW"),
                (OID_INT8,) * 3,
            ),
            attachment=attachment,
            txn_id=txn_id,
        )
        expect_error(
            sock,
            "MATCH_RECOGNIZE unimplemented profile Parse",
            sqlstate="42000",
            diagnostic="QOW-DIAG-QRY-001-AST-MALFORMED",
        )
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)
    finally:
        sock.close()


def require_standalone_child_refusal(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        statement_name = "match_recognize_standalone_child"
        portal_name = "match_recognize_standalone_child_portal"
        send_frame(
            sock,
            MSG_PARSE,
            sequence,
            parse_payload(statement_name, "MATCH_RECOGNIZE;", ()),
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        if expect_exact_frame(sock, MSG_PARSE_COMPLETE, "standalone MATCH_RECOGNIZE Parse"):
            raise RouteError("standalone MATCH_RECOGNIZE ParseComplete carried a payload")

        send_frame(
            sock,
            MSG_BIND,
            sequence,
            bind_payload(portal_name, statement_name, ()),
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        if expect_exact_frame(sock, MSG_BIND_COMPLETE, "standalone MATCH_RECOGNIZE Bind"):
            raise RouteError("standalone MATCH_RECOGNIZE BindComplete carried a payload")

        send_frame(
            sock,
            MSG_EXECUTE,
            sequence,
            execute_payload(portal_name, autocommit=False),
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        send_frame(
            sock,
            MSG_SYNC,
            sequence,
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        fields = decode_error_fields(
            expect_exact_frame(sock, MSG_ERROR, "standalone MATCH_RECOGNIZE Execute")
        )
        expected = {
            "C": "42000",
            "M": "statement family is not recognized by the vertical-slice parser",
            "D": "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN",
        }
        if any(fields.get(key) != value for key, value in expected.items()):
            raise RouteError(
                "standalone MATCH_RECOGNIZE lost its child-only refusal: "
                f"actual={fields!r} expected={expected!r}"
            )
        ready_payload = expect_exact_frame(sock, MSG_READY, "standalone MATCH_RECOGNIZE Sync")
        status, ready_txn = decode_ready(ready_payload)
        if status == 0 or ready_txn != txn_id:
            raise RouteError(
                "standalone MATCH_RECOGNIZE refusal did not preserve the active transaction"
            )
        send_frame(sock, MSG_TERMINATE, sequence, attachment=attachment)
    finally:
        sock.close()


def require_authentication_refusal(port: int) -> None:
    with connect_tls(port) as sock:
        try:
            authenticate(
                sock,
                b"ScratchBird-E2E-wrong-password",
                p1_features=FEATURE_STREAMING,
            )
        except RouteError as exc:
            if "SECURITY.AUTHENTICATION" not in str(exc):
                raise MatchRecognizeGateError(
                    f"wrong-password refusal lost its authentication diagnostic: {exc}"
                ) from exc
            return
    raise MatchRecognizeGateError("wrong-password authentication was accepted")


def require_exact_trace(route: StartedRoute, minimum_query_observations: int) -> None:
    dispatch = route.traces["dispatch"].read_text(
        encoding="utf-8", errors="replace"
    )
    exact = "preflight_observe op=query.execute opcode=SBLR_QUERY_EXECUTE code=4615"
    if dispatch.count(exact) < minimum_query_observations:
        raise MatchRecognizeGateError(
            "MATCH_RECOGNIZE did not publish enough exact query.execute trace evidence: "
            f"count={dispatch.count(exact)} expected_at_least={minimum_query_observations}"
        )
    forbidden = (
        "op=query.plan_operation",
        "op=dml.",
        "op=engine.op.insert",
        "op=engine.op.update",
        "op=engine.op.delete",
        "op=engine.op.ddl_",
    )
    leaked = [marker for marker in forbidden if marker in dispatch]
    if leaked:
        raise MatchRecognizeGateError(
            f"source-free MATCH_RECOGNIZE entered a mutation/legacy route: {leaked!r}"
        )


def run_gate(args: argparse.Namespace, work: Path) -> None:
    root = work / "tls"
    database = root / "match_recognize.sbdb"
    root.mkdir(parents=True, exist_ok=True)
    cert, key = generate_server_cert(args.openssl, root)
    route: StartedRoute | None = None
    try:
        route = start_route(
            args, root, database, tls_required=True, cert=cert, key=key
        )
        execute_prepared_match(
            route.port,
            case="descending_step",
            values=(5, 1, -2),
            expected_rows=[["1"], ["3"], ["5"]],
        )
        execute_prepared_match(
            route.port,
            case="default_step",
            values=(1, 3),
            expected_rows=[["1"], ["2"], ["3"]],
        )
        require_bind_shape_refusal(route.port)
        require_profile_refusal(route.port)
        require_standalone_child_refusal(route.port)
        require_authentication_refusal(route.port)
        require_exact_trace(route, minimum_query_observations=4)
        stop_route(route)
        route = None

        route = start_route(
            args,
            root / "restart",
            database,
            tls_required=True,
            cert=cert,
            key=key,
        )
        execute_prepared_match(
            route.port,
            case="restart",
            values=(2, 4),
            expected_rows=[["2"], ["3"], ["4"]],
        )
        require_exact_trace(route, minimum_query_observations=2)
    finally:
        stop_route(route)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--example-db-seeder", required=True)
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    work = make_work_dir(Path(args.work_dir))
    try:
        run_gate(args, work)
        print(f"sbsql_match_recognize_full_route_gate=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - emit concrete E2E route evidence.
        print(
            f"sbsql_match_recognize_full_route_gate=failed work={work}: {exc}",
            file=sys.stderr,
        )
        dump_logs(work)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
