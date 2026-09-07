#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public SBWP/TLS CREATE PROCEDURE and PROCEDURE INVOKE lifecycle gate."""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import sys
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    StartedRoute,
    authenticate_tls,
    commit_txn,
    dump_logs,
    generate_server_cert,
    make_work_dir,
    rollback_txn,
    start_route,
    stop_route,
)
from sbsql_sbwp_tls_engine_auth_route_smoke import (
    BENCHMARK_PASSWORD,
    MSG_COMMAND_COMPLETE,
    MSG_DATA_ROW,
    MSG_ERROR,
    MSG_QUERY,
    MSG_READY,
    MSG_ROW_DESCRIPTION,
    MSG_TERMINATE,
    RESTRICTED_SCHEMA_PRINCIPAL,
    RouteError,
    authenticate,
    connect_tls,
    decode_ready,
    query_payload,
    recv_frame,
    send_frame,
)


COMMITTED_NAME = "users.public.tls_null_procedure"
ROLLED_BACK_NAME = "users.public.tls_rolled_back_procedure"
UNAUTHORIZED_NAME = "users.public.tls_unauthorized_procedure"
PROVEN_SURFACE_IDS = (
    "SBSQL-13F5A8364A50",  # create_procedure_stmt
    "SBSQL-B5E9C0943E63",  # procedure_signature (empty V1 signature)
    "SBSQL-F3006C91D952",  # call
    "SBSQL-FAC34DDEAC9D",  # call_stmt
    "SBSQL-5AFD1BFCCEC8",  # psql_null_stmt inside the bound procedure body
)
PROVEN_CORE_ROUTE_IDENTITIES = {
    "SBSQL-13F5A8364A50": (
        "SBLR_DDL_CREATE_PROCEDURE",
        "engine.op.ddl_create_procedure",
    ),
    "SBSQL-F3006C91D952": (
        "SBLR_PROCEDURE_INVOKE",
        "engine.op.procedure_invoke",
    ),
    "SBSQL-FAC34DDEAC9D": (
        "SBLR_PROCEDURE_INVOKE",
        "engine.op.procedure_invoke",
    ),
    "SBSQL-5AFD1BFCCEC8": (
        "sblr.psql.node.psql_null_stmt.v1",
        "engine.psql.ir.psql_null_stmt",
    ),
}


class ProcedureLifecycleError(RuntimeError):
    pass


def durable_api_authority_rows(raw_journal: bytes) -> tuple[bytes, ...]:
    """Project exact catalog/name authority out of the multiplexed journal."""
    authority_prefixes = (b"SBAPI1\t", b"SBNAME1\t")
    telemetry_prefix = b"SBAGENTHOOK1\t"
    rows = raw_journal.splitlines(keepends=True)
    unexpected = [
        row
        for row in rows
        if not row.startswith(authority_prefixes)
        and not row.startswith(telemetry_prefix)
    ]
    if unexpected:
        raise ProcedureLifecycleError(
            "procedural lifecycle encountered an unknown durable API "
            f"journal record: {unexpected[0]!r}"
        )
    return tuple(row for row in rows if row.startswith(authority_prefixes))


def create_sql(name: str) -> str:
    return f"CREATE PROCEDURE {name} AS BEGIN NULL; END;"


def invoke_sql(name: str) -> str:
    return f"EXECUTE PROCEDURE {name};"


def call_sql(name: str) -> str:
    return f"CALL {name}();"


def decode_command_complete(payload: bytes) -> tuple[int, bytes]:
    if (
        len(payload) < 21
        or payload[:4] != b"\x01\x00\x00\x00"
        or struct.unpack_from("<Q", payload, 12)[0] != 0
        or payload[-1] != 0
        or b"\x00" in payload[20:-1]
    ):
        raise RouteError(f"invalid COMMAND_COMPLETE carrier: {payload!r}")
    return struct.unpack_from("<Q", payload, 4)[0], payload[20:-1]


def execute_command(
    sock,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    sql: str,
    expected_tag: bytes,
) -> tuple[int, int]:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(sql),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    saw_complete = False
    while True:
        msg_type, payload, _, frame_txn = recv_frame(sock)
        if msg_type in (MSG_ROW_DESCRIPTION, MSG_DATA_ROW):
            raise RouteError(f"{sql} fabricated a rowset: {payload!r}")
        if msg_type == MSG_COMMAND_COMPLETE:
            if saw_complete:
                raise RouteError(f"{sql} emitted duplicate COMMAND_COMPLETE")
            rows, tag = decode_command_complete(payload)
            if rows != 0 or tag != expected_tag:
                raise RouteError(
                    f"{sql} completion mismatch: rows={rows} tag={tag!r}"
                )
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            status, ready_txn = decode_ready(payload)
            if (
                not saw_complete
                or status == 0
                or ready_txn != txn_id
                or frame_txn == 0
            ):
                raise RouteError(
                    f"{sql} did not preserve its active MGA transaction"
                )
            return sequence, ready_txn
        if msg_type == MSG_ERROR:
            raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
        raise RouteError(
            f"{sql} returned unexpected frame 0x{msg_type:02x}: {payload!r}"
        )


def execute_refusal(
    sock,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    sql: str,
    diagnostic: bytes,
) -> tuple[int, int]:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(sql),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    msg_type, payload, _, _ = recv_frame(sock)
    if msg_type != MSG_ERROR or diagnostic not in payload:
        raise RouteError(
            f"{sql} did not return exact {diagnostic!r} refusal: "
            f"type=0x{msg_type:02x} payload={payload!r}"
        )
    msg_type, ready_payload, _, frame_txn = recv_frame(sock)
    if msg_type != MSG_READY:
        raise RouteError(
            f"{sql} refusal was not followed by READY: 0x{msg_type:02x}"
        )
    status, ready_txn = decode_ready(ready_payload)
    if status == 0 or ready_txn != txn_id or frame_txn == 0:
        raise RouteError(f"{sql} refusal changed its active MGA transaction")
    return sequence, ready_txn


def terminate(sock, sequence: int, attachment: bytes) -> None:
    send_frame(sock, MSG_TERMINATE, sequence, attachment=attachment)


def run_committed_create(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, txn_id = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            create_sql(COMMITTED_NAME),
            b"CREATE",
        )
        sequence, _ = commit_txn(
            sock, sequence, attachment, txn_id, "TLS CREATE PROCEDURE COMMIT"
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()


def run_committed_invoke(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, txn_id = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            invoke_sql(COMMITTED_NAME),
            b"EXECUTE",
        )
        sequence, _ = commit_txn(
            sock, sequence, attachment, txn_id, "TLS PROCEDURE INVOKE COMMIT"
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()


def run_committed_call(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, txn_id = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            call_sql(COMMITTED_NAME),
            b"CALL",
        )
        sequence, _ = commit_txn(
            sock, sequence, attachment, txn_id, "TLS CALL PROCEDURE COMMIT"
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()


def run_restricted_refusals(port: int, database: Path) -> None:
    durable_paths = (
        Path(f"{database}.sb.catalog_object_events"),
        Path(f"{database}.sb.api_events"),
        Path(f"{database}.sb.executable_object_events"),
    )
    before = {path: path.read_bytes() for path in durable_paths}
    api_event_path = Path(f"{database}.sb.api_events")
    api_authority_before = durable_api_authority_rows(before[api_event_path])
    sock = connect_tls(port)
    try:
        attachment, sequence, txn_id = authenticate(
            sock,
            BENCHMARK_PASSWORD,
            user=RESTRICTED_SCHEMA_PRINCIPAL,
        )
        sequence, txn_id = execute_refusal(
            sock,
            sequence,
            attachment,
            txn_id,
            create_sql(UNAUTHORIZED_NAME),
            b"SECURITY.ACCESS_DENIED",
        )
        sequence, txn_id = execute_refusal(
            sock,
            sequence,
            attachment,
            txn_id,
            call_sql(COMMITTED_NAME),
            b"SECURITY.ACCESS_DENIED",
        )
        sequence, _ = rollback_txn(
            sock,
            sequence,
            attachment,
            txn_id,
            "TLS restricted PROCEDURE authorization ROLLBACK",
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()
    changed = [
        str(path)
        for path in durable_paths
        if (
            durable_api_authority_rows(path.read_bytes()) != api_authority_before
            if path == api_event_path
            else path.read_bytes() != before[path]
        )
    ]
    if changed:
        raise ProcedureLifecycleError(
            "procedural authorization refusal changed durable state: "
            + ", ".join(changed)
        )


def run_rolled_back_create(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, txn_id = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            create_sql(ROLLED_BACK_NAME),
            b"CREATE",
        )
        sequence, _ = rollback_txn(
            sock,
            sequence,
            attachment,
            txn_id,
            "TLS CREATE PROCEDURE ROLLBACK",
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()


def run_absent_observer(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, txn_id = execute_refusal(
            sock,
            sequence,
            attachment,
            txn_id,
            invoke_sql(ROLLED_BACK_NAME),
            b"CATALOG.NAME.NOT_FOUND",
        )
        sequence, _ = rollback_txn(
            sock,
            sequence,
            attachment,
            txn_id,
            "TLS absent PROCEDURE observer ROLLBACK",
        )
        terminate(sock, sequence, attachment)
    finally:
        sock.close()


def require_durable_procedure_state(database: Path) -> None:
    required = (
        Path(f"{database}.sb.catalog_object_events"),
        Path(f"{database}.sb.api_events"),
        Path(f"{database}.sb.executable_object_events"),
    )
    missing = [str(path) for path in required if not path.is_file() or path.stat().st_size == 0]
    if missing:
        raise ProcedureLifecycleError(
            "CREATE PROCEDURE did not publish all durable lifecycle journals: "
            + ", ".join(missing)
        )


def sbps_pairs(route: StartedRoute) -> list[tuple[int, int]]:
    path = route.traces["sbps"]
    if not path.is_file() or path.stat().st_size == 0:
        raise ProcedureLifecycleError(f"missing SBPS trace {path}")
    pairs: list[tuple[int, int]] = []
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        message = re.search(r"(?:^|\t)message_type=(\d+)(?:\t|$)", line)
        schema = re.search(r"(?:^|\t)schema_id=(\d+)(?:\t|$)", line)
        if message and schema:
            pairs.append((int(message.group(1)), int(schema.group(1))))
    return pairs


def require_route_evidence(
    route: StartedRoute,
    expected_pairs: list[tuple[int, int]],
    *,
    create_executions: int,
    invoke_executions: int,
) -> None:
    selected = [
        pair
        for pair in sbps_pairs(route)
        if pair in {(200, 7195), (132, 7127)}
    ]
    if selected != expected_pairs:
        raise ProcedureLifecycleError(
            "procedural SBPS bind ordering mismatch: "
            f"actual={selected!r} expected={expected_pairs!r}"
        )
    dispatch_path = route.traces["dispatch"]
    if not dispatch_path.is_file() or dispatch_path.stat().st_size == 0:
        raise ProcedureLifecycleError(f"missing dispatch trace {dispatch_path}")
    dispatch = dispatch_path.read_text(encoding="utf-8", errors="strict")
    create_marker = (
        "layer=ddl_create_procedure_executor\t"
        "executor_id=engine.op.ddl_create_procedure\t"
        "opcode=SBLR_DDL_CREATE_PROCEDURE\t"
        "opcode_code=1554\t"
    )
    invoke_marker = (
        "layer=procedure_invoke_executor\t"
        "executor_id=engine.op.procedure_invoke\t"
        "opcode=SBLR_PROCEDURE_INVOKE\t"
        "opcode_code=1030\t"
    )
    if dispatch.count(create_marker) != create_executions:
        raise ProcedureLifecycleError(
            "CREATE PROCEDURE exact executor count mismatch"
        )
    if dispatch.count(invoke_marker) != invoke_executions:
        raise ProcedureLifecycleError("PROCEDURE INVOKE exact executor count mismatch")


def run_gate(args: argparse.Namespace, work: Path) -> None:
    if len(PROVEN_SURFACE_IDS) != len(set(PROVEN_SURFACE_IDS)):
        raise ProcedureLifecycleError("procedural surface evidence identities collide")
    if set(PROVEN_CORE_ROUTE_IDENTITIES) != set(PROVEN_SURFACE_IDS) - {
        "SBSQL-B5E9C0943E63"
    }:
        raise ProcedureLifecycleError("procedural Core route evidence is incomplete")
    root = work / "tls"
    database = root / "procedure.sbdb"
    root.mkdir(parents=True, exist_ok=True)
    cert, key = generate_server_cert(args.openssl, root)
    route: StartedRoute | None = None
    try:
        route = start_route(
            args, root, database, tls_required=True, cert=cert, key=key
        )
        run_committed_create(route.port)
        require_durable_procedure_state(database)
        run_committed_invoke(route.port)
        run_committed_call(route.port)
        run_restricted_refusals(route.port, database)
        run_rolled_back_create(route.port)
        run_absent_observer(route.port)
        require_route_evidence(
            route,
            [
                (200, 7195),
                (132, 7127),
                (132, 7127),
                (200, 7195),
                (132, 7127),
                (200, 7195),
                (132, 7127),
            ],
            create_executions=2,
            invoke_executions=2,
        )
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
        run_committed_invoke(route.port)
        run_absent_observer(route.port)
        require_route_evidence(
            route,
            [(132, 7127), (132, 7127)],
            create_executions=0,
            invoke_executions=1,
        )
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
        print("sbsql_procedure_lifecycle_full_route_gate=passed")
        return 0
    except Exception as exc:  # noqa: BLE001 - preserve concrete route evidence.
        print(
            f"sbsql_procedure_lifecycle_full_route_gate=failed: {exc}",
            file=sys.stderr,
        )
        dump_logs(work)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
