#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Authenticated public SHOW ACCELERATION full-route conformance gate."""

from __future__ import annotations

import argparse
import hashlib
import shutil
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
    MSG_QUERY,
    MSG_READY,
    MSG_ROW_DESCRIPTION,
    MSG_TERMINATE,
    RouteError,
    authenticate,
    connect_tls,
    decode_data_row_values,
    decode_ready,
    query_payload,
    recv_frame,
    send_frame,
)


SQL = "SHOW ACCELERATION"
EXPECTED_PROVIDER_IDS = (
    "compiler.llvm",
    "execution.interpreter",
    "gpu.cuda",
    "gpu.hip",
    "gpu.opencl",
)
ROW_FIELDS = (
    "provider_id",
    "provider_class",
    "state",
    "requirement",
    "provider",
    "diagnostic_code",
    "provider_generation",
    "snapshot_generation",
    "snapshot_sha256",
    "node_uuid",
)


class AccelerationGateError(RuntimeError):
    pass


def decode_command_complete(payload: bytes) -> tuple[int, bytes]:
    if (
        len(payload) < 21
        or payload[:4] != b"\x01\x00\x00\x00"
        or payload[-1] != 0
        or b"\x00" in payload[20:-1]
    ):
        raise AccelerationGateError(
            f"SHOW ACCELERATION returned a malformed completion: {payload!r}"
        )
    return int.from_bytes(payload[4:12], "little"), payload[20:-1]


def execute_show(port: int) -> tuple[tuple[str, ...], ...]:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        send_frame(
            sock,
            MSG_QUERY,
            sequence,
            query_payload(SQL),
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        saw_description = False
        saw_complete = False
        rows: list[tuple[str, ...]] = []
        while True:
            msg_type, payload, _, frame_txn = recv_frame(sock)
            if msg_type == MSG_ROW_DESCRIPTION:
                if saw_description:
                    raise AccelerationGateError(
                        "SHOW ACCELERATION emitted duplicate row descriptions"
                    )
                saw_description = True
                continue
            if msg_type == MSG_DATA_ROW:
                raw = decode_data_row_values(payload)
                if len(raw) != len(ROW_FIELDS) or any(value is None for value in raw):
                    raise AccelerationGateError(
                        f"SHOW ACCELERATION returned a noncanonical row: {raw!r}"
                    )
                rows.append(tuple(value.decode("utf-8") for value in raw if value is not None))
                continue
            if msg_type == MSG_COMMAND_COMPLETE:
                if saw_complete:
                    raise AccelerationGateError(
                        "SHOW ACCELERATION emitted duplicate completions"
                    )
                row_count, tag = decode_command_complete(payload)
                if row_count != len(rows) or tag != f"SELECT {len(rows)}".encode("ascii"):
                    raise AccelerationGateError(
                        "SHOW ACCELERATION completion did not bind the published rows: "
                        f"count={row_count} tag={tag!r} rows={len(rows)}"
                    )
                saw_complete = True
                continue
            if msg_type == MSG_READY:
                status, ready_txn = decode_ready(payload)
                if (
                    not saw_description
                    or not saw_complete
                    or status == 0
                    or ready_txn != txn_id
                    or frame_txn != txn_id
                ):
                    raise AccelerationGateError(
                        "SHOW ACCELERATION did not preserve its exact active MGA transaction"
                    )
                break
            if msg_type == MSG_ERROR:
                raise AccelerationGateError(
                    f"SHOW ACCELERATION failed with ERROR payload {payload!r}"
                )
            raise AccelerationGateError(
                f"SHOW ACCELERATION returned unexpected frame 0x{msg_type:02x}: {payload!r}"
            )
        send_frame(sock, MSG_TERMINATE, sequence, attachment=attachment)
    finally:
        sock.close()

    if tuple(row[0] for row in rows) != EXPECTED_PROVIDER_IDS:
        raise AccelerationGateError(
            "SHOW ACCELERATION did not publish the exact engine provider registry: "
            f"{[row[0] for row in rows]!r}"
        )
    by_id = {row[0]: dict(zip(ROW_FIELDS, row, strict=True)) for row in rows}
    if by_id["execution.interpreter"]["provider_class"] != "interpreter":
        raise AccelerationGateError("interpreter fallback lost its engine-owned class")
    if by_id["compiler.llvm"]["provider_class"] != "compiler":
        raise AccelerationGateError("LLVM capability lost its compiler class")
    if any(by_id[provider]["provider_class"] != "gpu" for provider in EXPECTED_PROVIDER_IDS[2:]):
        raise AccelerationGateError("GPU capabilities lost their engine-owned class")
    generations = {row[6] for row in rows} | {row[7] for row in rows}
    if len(generations) != 1 or next(iter(generations)) in ("", "0"):
        raise AccelerationGateError(
            f"provider rows did not share a nonzero registry generation: {generations!r}"
        )
    snapshot_hashes = {row[8] for row in rows}
    if (
        len(snapshot_hashes) != 1
        or len(next(iter(snapshot_hashes))) != 71
        or not next(iter(snapshot_hashes)).startswith("sha256:")
    ):
        raise AccelerationGateError(
            f"provider rows did not share an exact SHA-256 snapshot: {snapshot_hashes!r}"
        )
    allowed_redactions = {"scratchbird_engine", "engine_detected", "not_present", "disabled", "unknown"}
    if any(row[4] not in allowed_redactions for row in rows):
        raise AccelerationGateError("SHOW ACCELERATION exposed an unredacted provider identity")
    return tuple(rows)


def require_authentication_refusal(port: int) -> None:
    sock = connect_tls(port)
    try:
        try:
            authenticate(
                sock,
                b"ScratchBird-acceleration-wrong-password",
                p1_features=FEATURE_STREAMING,
            )
        except RouteError as exc:
            if "SECURITY.AUTHENTICATION" not in str(exc):
                raise AccelerationGateError(
                    f"wrong-password refusal lost its authentication diagnostic: {exc}"
                ) from exc
            return
    finally:
        sock.close()
    raise AccelerationGateError("wrong-password authentication was accepted")


def durable_mutation_fingerprint(database: Path) -> dict[str, str]:
    suffixes = (
        ".sb.catalog_object_events",
        ".sb.crud_events",
        ".sb.executable_object_events",
        ".sb.mga_relation_metadata",
        ".sb.mga_index_entries",
        ".sb.mga_relation_descriptors",
    )
    fingerprint: dict[str, str] = {}
    for suffix in suffixes:
        path = Path(f"{database}{suffix}")
        fingerprint[suffix] = (
            hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "absent"
        )
    return fingerprint


def operational_journal_snapshot(database: Path) -> dict[str, bytes]:
    return {
        suffix: path.read_bytes() if path.exists() else b""
        for suffix in (".sb.api_events", ".sb.mga_row_versions")
        for path in (Path(f"{database}{suffix}"),)
    }


def require_no_application_journal_mutation(
    before: dict[str, bytes], after: dict[str, bytes]
) -> None:
    allowed_prefixes = {
        ".sb.api_events": b"SBAGENTHOOK1\t",
        ".sb.mga_row_versions": b"SBMGA1\tROW_VERSION\t",
    }
    for suffix, prior in before.items():
        current = after[suffix]
        if current == prior:
            continue
        if not current.startswith(prior):
            raise AccelerationGateError(
                f"SHOW ACCELERATION rewrote operational journal {suffix}"
            )
        appended = current[len(prior):].splitlines()
        if not appended:
            raise AccelerationGateError(
                f"SHOW ACCELERATION changed operational journal {suffix} without records"
            )
        for record in appended:
            if not record.startswith(allowed_prefixes[suffix]):
                raise AccelerationGateError(
                    "SHOW ACCELERATION published a non-agent operational record: "
                    f"journal={suffix} record={record[:120]!r}"
                )
            if (
                suffix == ".sb.mga_row_versions"
                and b"\tagent-catalog-runtime-root\t" not in record
            ):
                raise AccelerationGateError(
                    "SHOW ACCELERATION published an application MGA row version: "
                    f"record={record[:120]!r}"
                )


def require_exact_trace(route: StartedRoute, expected_observations: int) -> None:
    dispatch = route.traces["dispatch"].read_text(
        encoding="utf-8", errors="replace"
    )
    exact = (
        "preflight_observe op=observability.show_acceleration "
        "opcode=SBLR_OBSERVABILITY_SHOW_ACCELERATION code=3365"
    )
    observed = dispatch.count(exact)
    if observed != expected_observations:
        raise AccelerationGateError(
            "SHOW ACCELERATION did not publish exact SBLR dispatch evidence: "
            f"count={observed} expected={expected_observations}"
        )
    forbidden = (
        "op=query.plan_operation",
        "op=query.execute",
        "op=dml.",
        "op=engine.op.insert",
        "op=engine.op.update",
        "op=engine.op.delete",
        "op=engine.op.ddl_",
        "op=index.",
    )
    leaked = [marker for marker in forbidden if marker in dispatch]
    if leaked:
        raise AccelerationGateError(
            f"SHOW ACCELERATION entered a mutation, query, or index route: {leaked!r}"
        )


def run_gate(args: argparse.Namespace, work: Path) -> None:
    root = work / "tls"
    database = root / "acceleration.sbdb"
    root.mkdir(parents=True, exist_ok=True)
    cert, key = generate_server_cert(args.openssl, root)
    route: StartedRoute | None = None
    first_rows: tuple[tuple[str, ...], ...]
    before: dict[str, str]
    operational_before: dict[str, bytes]
    try:
        route = start_route(
            args, root, database, tls_required=True, cert=cert, key=key
        )
        before = durable_mutation_fingerprint(database)
        operational_before = operational_journal_snapshot(database)
        first_rows = execute_show(route.port)
        if execute_show(route.port) != first_rows:
            raise AccelerationGateError(
                "independent authenticated sessions observed different acceleration snapshots"
            )
        require_authentication_refusal(route.port)
        require_exact_trace(route, expected_observations=2)
        after = durable_mutation_fingerprint(database)
        if after != before:
            changed = sorted(key for key in before if before[key] != after[key])
            raise AccelerationGateError(
                "SHOW ACCELERATION mutated catalog, API, executable, or MGA relation state: "
                f"changed={changed!r}"
            )
        require_no_application_journal_mutation(
            operational_before, operational_journal_snapshot(database)
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
        if execute_show(route.port) != first_rows:
            raise AccelerationGateError(
                "process restart changed the immutable acceleration snapshot"
            )
        require_exact_trace(route, expected_observations=1)
        after_restart = durable_mutation_fingerprint(database)
        if after_restart != before:
            changed = sorted(
                key for key in before if before[key] != after_restart[key]
            )
            raise AccelerationGateError(
                "restart SHOW ACCELERATION mutated durable relation or catalog state: "
                f"changed={changed!r}"
            )
        require_no_application_journal_mutation(
            operational_before, operational_journal_snapshot(database)
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
        print(f"sbsql_acceleration_full_route_gate=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - retain public-route evidence.
        print(
            f"sbsql_acceleration_full_route_gate=failed work={work}: {exc}",
            file=sys.stderr,
        )
        dump_logs(work)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
