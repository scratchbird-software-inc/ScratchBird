# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import datetime
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def test_parse_array_literal_nested_and_null() -> None:
    parsed = scratchbird.parse_array_literal('{1,{2,3},NULL,"quoted"}')
    _require(parsed == ["1", ["2", "3"], None, "quoted"], "array literal parse mismatch")


def test_parse_vector_literal_and_decode_vector_oid() -> None:
    parsed = scratchbird.parse_vector_literal("[1.5, 2, -3]")
    _require(parsed == [1.5, 2.0, -3.0], "vector literal parse mismatch")

    decoded = scratchbird.decode_value(scratchbird.OID_SB_VECTOR, b"[4,5,6]")
    _require(decoded == [4.0, 5.0, 6.0], "vector decode mismatch")


def test_parse_range_literal_edges() -> None:
    r1 = scratchbird.parse_range_literal("[1,10)")
    _require(r1.lower == "1" and r1.upper == "10", "range bounds mismatch")
    _require(r1.lower_inclusive is True and r1.upper_inclusive is False, "range inclusivity mismatch")

    r2 = scratchbird.parse_range_literal("(,10]")
    _require(r2.lower is None and r2.upper == "10", "range infinite lower mismatch")
    _require(r2.lower_infinite is True and r2.upper_infinite is False, "range infinite flags mismatch")

    empty = scratchbird.parse_range_literal("empty")
    _require(empty.empty is True, "empty range mismatch")


def test_parse_composite_literal_and_decode_record_oid() -> None:
    parsed = scratchbird.parse_composite_literal('(1,"two",NULL)')
    _require(parsed == ["1", "two", None], "composite literal parse mismatch")

    decoded = scratchbird.decode_value(scratchbird.OID_RECORD, b'(1,"two",NULL)')
    _require(isinstance(decoded, scratchbird.ScratchBirdComposite), "record decode should return ScratchBirdComposite")
    _require(decoded.fields == ["1", "two", None], "record decode field mismatch")


def test_decode_geometry_and_network_types() -> None:
    point = scratchbird.decode_value(scratchbird.OID_POINT, b"(1.5,-2)")
    _require(isinstance(point, scratchbird.ScratchBirdGeometry), "point decode should return ScratchBirdGeometry")
    _require(point.wkt == "POINT(1.5 -2.0)", "point WKT mismatch")

    inet = scratchbird.decode_value(scratchbird.OID_INET, b"10.0.0.1/32")
    _require(isinstance(inet, scratchbird.ScratchBirdNetwork), "inet decode should return ScratchBirdNetwork")
    _require(inet.kind == "inet" and inet.address == "10.0.0.1/32", "inet decode mismatch")

    cidr = scratchbird.decode_value(scratchbird.OID_CIDR, b"10.0.0.0/24")
    _require(cidr.kind == "cidr" and cidr.address == "10.0.0.0/24", "cidr decode mismatch")

    mac = scratchbird.decode_value(scratchbird.OID_MACADDR, b"08:00:2b:01:02:03")
    _require(mac.kind == "macaddr" and mac.address == "08:00:2b:01:02:03", "mac decode mismatch")


def test_decode_unknown_oid_returns_raw() -> None:
    raw = scratchbird.decode_value(99999, b"abc")
    _require(isinstance(raw, scratchbird.ScratchBirdRaw), "unknown oid should return ScratchBirdRaw")
    _require(raw.oid == 99999 and raw.data == b"abc", "raw fallback content mismatch")


def test_decode_json_jsonb_and_uuid() -> None:
    json_value = scratchbird.decode_value(scratchbird.OID_JSON, b'{"k":1,"v":"x"}')
    _require(isinstance(json_value, scratchbird.ScratchBirdJson), "json decode wrapper mismatch")
    _require(json_value.value == {"k": 1, "v": "x"}, "json decode value mismatch")

    jsonb_value = scratchbird.decode_value(scratchbird.OID_JSONB, b'\x01{"ok":true}')
    _require(isinstance(jsonb_value, scratchbird.ScratchBirdJsonb), "jsonb decode wrapper mismatch")
    _require(jsonb_value.value == {"ok": True}, "jsonb decode value mismatch")

    uuid_text = scratchbird.decode_value(scratchbird.OID_UUID, b"550e8400-e29b-41d4-a716-446655440000")
    _require(uuid_text == "550e8400-e29b-41d4-a716-446655440000", "uuid text decode mismatch")

    uuid_binary = scratchbird.decode_value(
        scratchbird.OID_UUID,
        bytes.fromhex("550e8400e29b41d4a716446655440000"),
    )
    _require(uuid_binary == "550e8400-e29b-41d4-a716-446655440000", "uuid binary decode mismatch")


def test_decode_temporal_and_interval_types() -> None:
    date_value = scratchbird.decode_value(scratchbird.OID_DATE, b"2026-03-05")
    _require(isinstance(date_value, scratchbird.ScratchBirdDate), "date decode wrapper mismatch")
    _require(date_value.value == datetime.date(2026, 3, 5), "date decode value mismatch")

    time_value = scratchbird.decode_value(scratchbird.OID_TIME, b"12:34:56.123456")
    _require(isinstance(time_value, scratchbird.ScratchBirdTime), "time decode wrapper mismatch")
    _require(time_value.value == datetime.time(12, 34, 56, 123456), "time decode value mismatch")

    ts_value = scratchbird.decode_value(scratchbird.OID_TIMESTAMP, b"2026-03-05 12:34:56.123456")
    _require(isinstance(ts_value, scratchbird.ScratchBirdTimestamp), "timestamp decode wrapper mismatch")
    _require(ts_value.value == datetime.datetime(2026, 3, 5, 12, 34, 56, 123456), "timestamp decode value mismatch")

    tstz_value = scratchbird.decode_value(scratchbird.OID_TIMESTAMPTZ, b"2026-03-05 12:34:56+00:00")
    _require(isinstance(tstz_value, scratchbird.ScratchBirdTimestampTZ), "timestamptz decode wrapper mismatch")
    _require(
        tstz_value.value == datetime.datetime(2026, 3, 5, 12, 34, 56, tzinfo=datetime.timezone.utc),
        "timestamptz decode value mismatch",
    )

    interval_value = scratchbird.decode_value(
        scratchbird.OID_INTERVAL,
        struct.pack("<qii", 1_500_000, 2, 3),
    )
    _require(isinstance(interval_value, scratchbird.ScratchBirdInterval), "interval decode wrapper mismatch")
    _require(
        interval_value.micros == 1_500_000 and interval_value.days == 2 and interval_value.months == 3,
        "interval decode value mismatch",
    )


def test_decode_array_variants() -> None:
    int_array = scratchbird.decode_value(scratchbird.OID_INT4_ARRAY, b"{1,2,3}")
    _require(int_array == [1, 2, 3], "int4 array decode mismatch")

    text_array = scratchbird.decode_value(scratchbird.OID_TEXT_ARRAY, b'{"a","b",NULL}')
    _require(text_array == ["a", "b", None], "text array decode mismatch")

    record_array = scratchbird.decode_value(scratchbird.OID_RECORD_ARRAY, b'{"(1,\\"two\\")","(3,\\"four\\")"}')
    _require(len(record_array) == 2, "record array decode length mismatch")
    _require(isinstance(record_array[0], scratchbird.ScratchBirdComposite), "record array item wrapper mismatch")
    _require(record_array[0].fields == ["1", "two"], "record array first item mismatch")


def test_decode_int4_truncation_error() -> None:
    try:
        scratchbird.decode_value(scratchbird.OID_INT4, b"\x01")
        raise RuntimeError("expected truncation failure")
    except RuntimeError as exc:
        _require("row data truncated" in str(exc), "truncation message mismatch")


def test_encode_value_shapes() -> None:
    encoded_int = scratchbird.encode_value(7)
    _require(encoded_int == struct.pack("<i", 7), "int encode mismatch")

    encoded_vec = scratchbird.encode_value([1, 2.5, -3])
    _require(encoded_vec == b"[1.0,2.5,-3.0]", "vector encode mismatch")

    encoded_range = scratchbird.encode_value(
        scratchbird.ScratchBirdRange(
            lower="1",
            upper="9",
            lower_inclusive=True,
            upper_inclusive=False,
        )
    )
    _require(encoded_range == b"[1,9)", "range encode mismatch")

    encoded_composite = scratchbird.encode_value(
        scratchbird.ScratchBirdComposite(["1", "two,three", None])
    )
    _require(encoded_composite == b'(1,"two,three",NULL)', "composite encode mismatch")

    encoded_point = scratchbird.encode_value(scratchbird.ScratchBirdGeometry(raw="(1,2)", wkt="POINT(1 2)"))
    _require(encoded_point == b"(1,2)", "geometry encode mismatch")

    encoded_inet = scratchbird.encode_value(scratchbird.ScratchBirdNetwork(kind="inet", address="127.0.0.1/32"))
    _require(encoded_inet == b"127.0.0.1/32", "network encode mismatch")

    encoded_json = scratchbird.encode_value(scratchbird.ScratchBirdJson(raw=b"", value={"ok": True}))
    _require(encoded_json == b'{"ok": true}', "json encode mismatch")

    encoded_jsonb = scratchbird.encode_value(scratchbird.ScratchBirdJsonb(raw=b"", value={"ok": True}))
    _require(encoded_jsonb == b'\x01{"ok": true}', "jsonb encode mismatch")

    encoded_date = scratchbird.encode_value(scratchbird.ScratchBirdDate(datetime.date(2026, 3, 5)))
    _require(encoded_date == b"2026-03-05", "date encode mismatch")

    encoded_interval = scratchbird.encode_value(scratchbird.ScratchBirdInterval(micros=2_000_000, days=1, months=0))
    _require(encoded_interval == struct.pack("<qii", 2_000_000, 1, 0), "interval encode mismatch")

    encoded_array = scratchbird.encode_value([scratchbird.ScratchBirdComposite(["1", "two"]), None, "x"])
    _require(encoded_array == b'{"(1,two)",NULL,x}', "array-of-composite encode mismatch")


def main() -> None:
    test_parse_array_literal_nested_and_null()
    test_parse_vector_literal_and_decode_vector_oid()
    test_parse_range_literal_edges()
    test_parse_composite_literal_and_decode_record_oid()
    test_decode_geometry_and_network_types()
    test_decode_unknown_oid_returns_raw()
    test_decode_json_jsonb_and_uuid()
    test_decode_temporal_and_interval_types()
    test_decode_array_variants()
    test_decode_int4_truncation_error()
    test_encode_value_shapes()
    print("Mojo type codec tests OK")


if __name__ == "__main__":
    main()
