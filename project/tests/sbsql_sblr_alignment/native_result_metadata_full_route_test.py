#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Required native row metadata regression, including empty public results.

Does not manufacture missing metadata or accept today's output as an oracle.
All fixed cases run even when metadata assertions fail. Owned fixtures remain
for the caller to collect evidence and clean after the processes stop.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sbsql_parser_worker"))
from sbsql_copy_persistence_full_route_gate import (
    VERIFIER, execute_query, start_route, stop_route,
)
from sbsql_sbwp_tls_engine_auth_route_smoke import (
    FEATURE_STREAMING, MSG_COMMAND_COMPLETE, MSG_DATA_ROW, MSG_ERROR, MSG_QUERY,
    MSG_READY, MSG_ROW_DESCRIPTION, MSG_TERMINATE, authenticate, connect_tls,
    decode_data_row_values, decode_ready, generate_server_cert, query_payload,
    recv_frame, send_frame,
)


CASES = (
    ("heap_rows", "SELECT id FROM metadata_rows ORDER BY id;", (b"2", b"9")),
    ("heap_empty", "SELECT id FROM metadata_rows WHERE id = 99;", ()),
    ("cte_rows", "WITH r AS (SELECT * FROM metadata_rows) SELECT * FROM r;", (b"2", b"9")),
    ("cte_empty", "WITH r AS (SELECT id FROM metadata_rows WHERE id = 99) SELECT * FROM r;", ()),
)


def metadata_errors(payload):
    """Independent offset oracle for one declared nullable BIGINT column.

    This is an ordinary public profile, not trusted management. Zero/redacted
    UUIDs are legitimate; an unmarked concrete type with no UUID is not.
    """
    errors = []
    def check(ok, message):
        if not ok:
            errors.append(message)
    if len(payload) < 72 + 216 + 5:
        return ["RowDescription shorter than header, column prefix and label"]
    version, kind, flags, count = struct.unpack_from("<HBBI", payload)
    check(version == 1, "metadata layout version")
    check(kind in (0, 5), "ordinary/cursor result-set kind")
    check(flags & ~3 == 0, "reserved result flags")
    check(count == 1, "declared one-column shape")
    check(payload[8:40] == bytes(32), "ordinary public result exposes raw UUIDs")
    check(payload[40:72] != bytes(32), "descriptor snapshot hash absent")
    if count != 1:
        return errors
    column = 72
    ordinal, column_class, value_format, nullable, updatability = struct.unpack_from("<IBBBB", payload, column)
    bitmap = struct.unpack_from("<Q", payload, column + 8)[0]
    family, code, type_version, type_flags = struct.unpack_from("<HHHH", payload, column + 16)
    modifiers = struct.unpack_from("<Q", payload, column + 16 + 64)[0]
    check(ordinal == 1, "column ordinal")
    check(column_class in (0, 1, 2), "query column class")
    check(value_format in (0, 1), "scalar value format")
    check(nullable == 1, "declared nullable BIGINT metadata")
    check(updatability in (0, 1, 2, 3), "updatability discriminator")
    check(bitmap & ((1 << 0) | (1 << 10)) == ((1 << 0) | (1 << 10)),
          "label/nullability metadata presence")
    check(bitmap >> 36 == 0 and modifiers >> 15 == 0 and type_flags >> 9 == 0,
          "reserved metadata bits")
    check((family, code, type_version) == (2, 4, 1), "declared BIGINT canonical type code")
    # TypeRef UUIDs: descriptor, domain, element, charset and collation.
    type_ref = column + 16
    for offset in (8, 24, 40, 104, 120):
        check(payload[type_ref + offset:type_ref + offset + 16] == bytes(16),
              "ordinary public type metadata exposes raw UUID")
    check(payload[column + 160:column + 208] == bytes(48),
          "ordinary public source metadata exposes raw UUID")
    check(bool(modifiers & (1 << 14)), "concrete type UUID absent without policy-redacted modifier")
    check(bool(flags & 2) and bool(bitmap & (1 << 34)),
          "public identity redaction not declared in result/column metadata")
    check(struct.unpack_from("<I", payload, type_ref + 56)[0] > 0, "descriptor version absent")
    check(payload[type_ref + 142:type_ref + 144] == bytes(2) and
          payload[column + 214:column + 216] == bytes(2), "reserved descriptor bytes")
    position = column + 216
    def text():
        nonlocal position
        if position + 5 > len(payload):
            raise ValueError("truncated NullableText")
        state, length = struct.unpack_from("<BI", payload, position)
        position += 5
        if state > 3 or (state != 3 and length != 0) or length > len(payload) - position:
            raise ValueError("invalid NullableText state/length")
        value = payload[position:position + length]
        position += length
        value.decode("utf-8", errors="strict")
        return state, value
    try:
        check(text() == (3, b"id"), "actual bound column label")
        for bit in (1, 2, 3, 4, 5, 6, 9, 7, 35):
            if bitmap & (1 << bit):
                text()
        check(position == len(payload), "trailing metadata bytes")
    except (ValueError, UnicodeError) as exc:
        errors.append(str(exc))
    return errors


def observe_query(route, sql):
    descriptions, rows, errors = [], [], []
    with connect_tls(route.port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock, VERIFIER.encode(), p1_features=FEATURE_STREAMING)
        send_frame(sock, MSG_QUERY, sequence, query_payload(sql),
                   attachment=attachment, txn_id=txn_id)
        complete = False
        while True:
            kind, payload, _, frame_txn = recv_frame(sock)
            if kind == MSG_ROW_DESCRIPTION:
                descriptions.append(payload)
            elif kind == MSG_DATA_ROW:
                rows.append(decode_data_row_values(payload))
            elif kind == MSG_COMMAND_COMPLETE:
                complete = True
            elif kind == MSG_ERROR:
                errors.append("query error frame: " + payload.hex())
            elif kind == MSG_READY:
                status, ready_txn = decode_ready(payload)
                if not status or not ready_txn or not frame_txn:
                    errors.append("no active MGA transaction after query")
                send_frame(sock, MSG_TERMINATE, sequence + 1, b"",
                           attachment=attachment, txn_id=ready_txn)
                break
            else:
                errors.append(f"unexpected response frame {kind}")
        if not complete:
            errors.append("missing command completion")
    return descriptions, rows, errors


def main():
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    root = Path(args.work_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    cert, key = generate_server_cert(args.openssl, root)
    database = root / "metadata.sbdb"
    expected_cases = tuple(f"{phase}/{name}" for phase in ("live", "reopened") for name, _, _ in CASES)
    print(json.dumps({"expected_cases": expected_cases, "count": 8,
                      "manifest_sha256": hashlib.sha256(json.dumps(CASES, default=lambda b: b.decode(),
                          separators=(",", ":")).encode()).hexdigest()}), flush=True)
    observed, failures = [], 0
    route = None
    try:
        route = start_route(args, root / "r", database, tls_required=True, cert=cert, key=key)
        with connect_tls(route.port) as sock:
            attachment, sequence, txn_id = authenticate(
                sock, VERIFIER.encode(), p1_features=FEATURE_STREAMING)
            for sql in ("CREATE TABLE metadata_rows (id BIGINT);",
                        "INSERT INTO metadata_rows (id) VALUES (2);",
                        "INSERT INTO metadata_rows (id) VALUES (9);", "COMMIT;"):
                sequence, _, txn_id = execute_query(
                    sock, sequence, attachment, txn_id, sql, require_rows=False)
            send_frame(sock, MSG_TERMINATE, sequence, b"", attachment=attachment, txn_id=txn_id)
        for phase in ("live", "reopened"):
            if phase == "reopened":
                stop_route(route)
                route = None
                route = start_route(args, root / "r2", database, tls_required=True, cert=cert, key=key)
            for name, sql, expected in CASES:
                case = phase + "/" + name
                descriptions, rows, errors = observe_query(route, sql)
                if sorted(rows) != [[value] for value in expected]:
                    errors.append(f"complete row multiset mismatch: {rows!r}")
                if len(descriptions) != 1:
                    errors.append(f"expected one RowDescription including empty result; got {len(descriptions)}")
                else:
                    errors.extend(metadata_errors(descriptions[0]))
                observed.append(case)
                failures += bool(errors)
                print(json.dumps({"case": case, "errors": errors,
                    "rows": [[v.decode() if v is not None else None for v in row] for row in rows],
                    "row_descriptions_hex": [d.hex() for d in descriptions]}), flush=True)
            _, after_rows, after_errors = observe_query(
                route, "SELECT id FROM metadata_rows ORDER BY id;")
            if after_errors or after_rows != [[b"2"], [b"9"]]:
                raise RuntimeError("independent committed state changed")
            if any(not p.is_file() or not p.stat().st_size for p in route.traces.values()):
                raise RuntimeError("missing real process-route trace layer")
        if tuple(observed) != expected_cases:
            raise RuntimeError("missing, duplicate, reordered or unknown expected case")
        print(json.dumps({"expected": 8, "observed": len(observed), "failed": failures}), flush=True)
    finally:
        stop_route(route)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
