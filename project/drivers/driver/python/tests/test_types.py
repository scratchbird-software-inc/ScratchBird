# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import datetime as dt
import decimal
import enum
import struct
import uuid

import pytest

from scratchbird.types import (
    Blob,
    Clob,
    Composite,
    CompositeField,
    FORMAT_TEXT,
    OID_BOOL,
    OID_BOOL_ARRAY,
    OID_BYTEA,
    OID_BYTEA_ARRAY,
    OID_DATE_ARRAY,
    OID_DATE,
    OID_DATERANGE,
    OID_FLOAT8_ARRAY,
    OID_FLOAT8,
    OID_INT4_ARRAY,
    OID_INT4,
    OID_INT4RANGE,
    OID_INT8,
    OID_INTERVAL,
    OID_JSONB,
    OID_NUMERIC,
    OID_NUMERIC_ARRAY,
    OID_RECORD,
    OID_SB_VECTOR,
    OID_TEXT,
    OID_TEXT_ARRAY,
    OID_TIME,
    OID_TIME_ARRAY,
    OID_TIMETZ,
    OID_TIMETZ_ARRAY,
    OID_TIMESTAMP_ARRAY,
    OID_TSRANGE,
    OID_TIMESTAMP,
    OID_TSTZRANGE,
    OID_TIMESTAMPTZ_ARRAY,
    OID_TIMESTAMPTZ,
    OID_UUID_ARRAY,
    OID_UUID,
    OID_XML,
    FORMAT_BINARY,
    decode_value,
    encode_param,
    Jsonb,
    Ref,
    Range,
    RowId,
    SqlXml,
    type_name,
)


def test_decode_int4():
    raw = (42).to_bytes(4, byteorder="little", signed=True)
    assert decode_value(OID_INT4, raw, FORMAT_BINARY) == 42


def test_decode_unknown_text_integer_outside_int64_returns_text():
    assert decode_value(0, b"9223372036854775808", FORMAT_TEXT) == "9223372036854775808"
    assert decode_value(0, b"-9223372036854775809", FORMAT_TEXT) == "-9223372036854775809"


def test_decode_integer_text_hex_payload_recovers_binary_value():
    assert decode_value(OID_INT8, b"00000001930a", FORMAT_TEXT) == bytes.fromhex("00000001930a")


def test_decode_jsonb():
    payload = b"{\"a\":1}"
    raw = len(payload).to_bytes(4, byteorder="little") + payload
    value = decode_value(OID_JSONB, raw, FORMAT_BINARY)
    assert isinstance(value, Jsonb)
    assert value.raw == payload


def test_decode_unknown_binary_single_byte_is_signed():
    assert decode_value(0, b"\xff", FORMAT_BINARY) == -1
    assert decode_value(0, b"\x7f", FORMAT_BINARY) == 127


def test_encode_blob_wrapper_uses_bytea_oid():
    payload = b"\x01\x02\x03"
    param, oid = encode_param(Blob(raw=payload))
    assert oid == OID_BYTEA
    assert decode_value(oid, param.data, FORMAT_BINARY) == payload


def test_encode_clob_wrapper_uses_text_oid():
    param, oid = encode_param(Clob(text="scratchbird-clob"))
    assert oid == OID_TEXT
    assert decode_value(oid, param.data, FORMAT_BINARY) == "scratchbird-clob"


def test_encode_rowid_wrapper_uses_bytea_oid():
    payload = b"rid-42"
    param, oid = encode_param(RowId(raw=payload))
    assert oid == OID_BYTEA
    assert decode_value(oid, param.data, FORMAT_BINARY) == payload


def test_decode_bytea_binary_prefixed_hex_payload():
    payload = b"\\x616263"
    raw = len(payload).to_bytes(4, byteorder="little") + payload
    assert decode_value(OID_BYTEA, raw, FORMAT_BINARY) == b"abc"


def test_decode_bytea_binary_escaped_octal_payload():
    payload = b"\\141\\142\\\\c"
    raw = len(payload).to_bytes(4, byteorder="little") + payload
    assert decode_value(OID_BYTEA, raw, FORMAT_BINARY) == b"ab\\c"


def test_decode_bytea_text_payload():
    assert decode_value(OID_BYTEA, b"\\x616263", FORMAT_TEXT) == b"abc"


def test_encode_ref_wrapper_uses_text_oid():
    param, oid = encode_param(Ref(type_name="demo_ref", value="rid-100"))
    assert oid == OID_TEXT
    assert decode_value(oid, param.data, FORMAT_BINARY) == "rid-100"


def test_encode_sqlxml_wrapper_uses_xml_oid():
    param, oid = encode_param(SqlXml(value="<doc/>"))
    assert oid == OID_XML
    assert decode_value(oid, param.data, FORMAT_BINARY) == "<doc/>"


def test_encode_enum_parameter_uses_text_name():
    class WorkerState(enum.Enum):
        RUNNABLE = 1

    param, oid = encode_param(WorkerState.RUNNABLE)
    assert oid == OID_TEXT
    assert decode_value(oid, param.data, FORMAT_BINARY) == "RUNNABLE"


def test_encode_custom_object_falls_back_to_text():
    class CustomParameter:
        def __str__(self) -> str:
            return "custom-parameter"

    param, oid = encode_param(CustomParameter())
    assert oid == OID_TEXT
    assert decode_value(oid, param.data, FORMAT_BINARY) == "custom-parameter"


def test_decode_vector_literal():
    text = b"[1, 2.5]"
    raw = len(text).to_bytes(4, byteorder="little") + text
    assert decode_value(OID_SB_VECTOR, raw, FORMAT_BINARY) == [1.0, 2.5]


def test_encode_timedelta_uses_interval_binary_layout():
    param, oid = encode_param(dt.timedelta(days=2, seconds=3, microseconds=4))

    assert oid == OID_INTERVAL
    months, days, micros = struct.unpack("<iiq", param.data)
    assert months == 0
    assert days == 0
    assert micros == 172803000004


def test_decode_interval_binary_payload():
    raw = struct.pack("<iiq", 2, 3, 5_000_000)
    assert decode_value(OID_INTERVAL, raw, FORMAT_BINARY) == {
        "months": 2,
        "days": 3,
        "micros": 5_000_000,
    }


def test_encode_timetz_uses_binary_layout_and_zone_seconds_west():
    value = dt.time(12, 34, 56, 123000, tzinfo=dt.timezone(dt.timedelta(hours=5, minutes=30)))
    param, oid = encode_param(value)

    assert oid == OID_TIMETZ
    micros, zone_seconds_west = struct.unpack("<qi", param.data)
    assert micros == 45_296_123_000
    assert zone_seconds_west == -19_800


def test_decode_timetz_binary_payload_roundtrip():
    raw = struct.pack("<qi", 45_296_123_000, -19_800)
    decoded = decode_value(OID_TIMETZ, raw, FORMAT_BINARY)
    assert decoded == dt.time(12, 34, 56, 123000, tzinfo=dt.timezone(dt.timedelta(hours=5, minutes=30)))


def test_decode_timetz_binary_payload_supports_legacy_8byte_form():
    raw = struct.pack("<q", 3_661_000_000)
    decoded = decode_value(OID_TIMETZ, raw, FORMAT_BINARY)
    assert decoded == dt.time(1, 1, 1, tzinfo=dt.timezone.utc)


def test_decode_timestamp_binary_payload_to_naive_datetime():
    base = dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)
    target = dt.datetime(2026, 3, 1, 12, 34, 56, tzinfo=dt.timezone.utc)
    micros = int((target - base).total_seconds() * 1_000_000)
    raw = struct.pack("<q", micros)
    decoded = decode_value(OID_TIMESTAMP, raw, FORMAT_BINARY)
    assert decoded == dt.datetime(2026, 3, 1, 12, 34, 56)
    assert decoded.tzinfo is None


def test_decode_timestamptz_binary_payload_to_aware_utc_datetime():
    base = dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)
    target = dt.datetime(2026, 3, 1, 12, 34, 56, tzinfo=dt.timezone.utc)
    micros = int((target - base).total_seconds() * 1_000_000)
    raw = struct.pack("<q", micros)
    decoded = decode_value(OID_TIMESTAMPTZ, raw, FORMAT_BINARY)
    assert decoded == target
    assert decoded.tzinfo == dt.timezone.utc


def test_decode_timetz_text_payload_to_offset_time():
    decoded = decode_value(OID_TIMETZ, b"08:09:10+03", FORMAT_TEXT)
    assert decoded == dt.time(8, 9, 10, tzinfo=dt.timezone(dt.timedelta(hours=3)))


def test_decode_date_text_payload_to_date():
    decoded = decode_value(OID_DATE, b"2026-03-01", FORMAT_TEXT)
    assert decoded == dt.date(2026, 3, 1)


def test_decode_numeric_scalar_text_payloads():
    assert decode_value(OID_BOOL, b"t", FORMAT_TEXT) is True
    assert decode_value(OID_INT4, b"42", FORMAT_TEXT) == 42
    assert decode_value(OID_FLOAT8, b"3.5", FORMAT_TEXT) == 3.5
    assert decode_value(OID_NUMERIC, b"12.34", FORMAT_TEXT) == decimal.Decimal("12.34")


def test_decode_text_null_sentinel_and_decimal_integer_payloads():
    assert decode_value(OID_INT4, b"<NULL>", FORMAT_TEXT) is None
    assert decode_value(OID_INT4, b"", FORMAT_TEXT) is None
    assert decode_value(OID_INT8, b"226.00", FORMAT_TEXT) == decimal.Decimal("226.00")


def test_decode_bool_scalar_text_non_true_tokens_map_to_false():
    assert decode_value(OID_BOOL, b"f", FORMAT_TEXT) is False
    assert decode_value(OID_BOOL, b"not-a-bool", FORMAT_TEXT) is False


def test_decode_typed_text_payloads_raise_on_invalid_parse():
    with pytest.raises(ValueError):
        decode_value(OID_INT4, b"bad-int", FORMAT_TEXT)
    with pytest.raises(ValueError):
        decode_value(OID_FLOAT8, b"bad-float", FORMAT_TEXT)
    with pytest.raises(decimal.InvalidOperation):
        decode_value(OID_NUMERIC, b"bad-numeric", FORMAT_TEXT)
    with pytest.raises(ValueError):
        decode_value(OID_DATE, b"bad-date", FORMAT_TEXT)


def test_decode_time_text_payload_to_time():
    decoded = decode_value(OID_TIME, b"12:34:56.123000", FORMAT_TEXT)
    assert decoded == dt.time(12, 34, 56, 123000)


def test_decode_time_text_payload_with_offset_raises():
    with pytest.raises(ValueError):
        decode_value(OID_TIME, b"12:34:56+02", FORMAT_TEXT)


def test_decode_timestamp_text_payload_to_naive_datetime():
    decoded = decode_value(OID_TIMESTAMP, b"2026-03-01 12:34:56", FORMAT_TEXT)
    assert decoded == dt.datetime(2026, 3, 1, 12, 34, 56)


def test_decode_timestamp_text_payload_with_offset_raises():
    with pytest.raises(ValueError):
        decode_value(OID_TIMESTAMP, b"2026-03-01 12:34:56+02", FORMAT_TEXT)


def test_decode_timestamptz_text_payload_to_aware_datetime():
    decoded = decode_value(OID_TIMESTAMPTZ, b"2026-03-01 12:34:56+02", FORMAT_TEXT)
    assert decoded == dt.datetime(2026, 3, 1, 12, 34, 56, tzinfo=dt.timezone(dt.timedelta(hours=2)))


def test_decode_timestamptz_text_payload_with_z_suffix_to_aware_datetime():
    decoded = decode_value(OID_TIMESTAMPTZ, b"2026-03-01T12:34:56Z", FORMAT_TEXT)
    assert decoded == dt.datetime(2026, 3, 1, 12, 34, 56, tzinfo=dt.timezone.utc)


def test_decode_timetz_text_payload_without_offset_raises():
    with pytest.raises(ValueError):
        decode_value(OID_TIMETZ, b"08:09:10", FORMAT_TEXT)


def test_decode_uuid_text_payload_to_uuid():
    decoded = decode_value(OID_UUID, b"12345678-1234-1234-1234-123456789abc", FORMAT_TEXT)
    assert decoded == uuid.UUID("12345678-1234-1234-1234-123456789abc")


def test_type_name_includes_timetz():
    assert type_name(OID_TIMETZ) == "timetz"


def test_encode_string_array_infers_text_array_oid():
    param, oid = encode_param(["alpha", "beta"])
    assert oid == OID_TEXT_ARRAY
    assert decode_value(oid, param.data, FORMAT_BINARY) == ["alpha", "beta"]


def test_encode_bool_array_infers_boolean_array_oid():
    param, oid = encode_param([True, False, True])
    assert oid == OID_BOOL_ARRAY
    assert decode_value(oid, param.data, FORMAT_BINARY) == [True, False, True]


def test_encode_mixed_numeric_array_widens_to_float8_array_oid():
    param, oid = encode_param([1, 2, 3.5])
    assert oid == OID_FLOAT8_ARRAY


def test_decode_int4_array_literal_payload():
    literal = b"{1,2,3}"
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_INT4_ARRAY, raw, FORMAT_BINARY) == [1, 2, 3]


def test_decode_bool_array_literal_accepts_t_f_tokens():
    literal = b"{t,f,true,false,yes}"
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_BOOL_ARRAY, raw, FORMAT_BINARY) == [True, False, True, False, False]


def test_decode_text_array_literal_with_quotes_and_nested_arrays():
    literal = b'{{"a,b","c\\\"d"},{"x","y"}}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_TEXT_ARRAY, raw, FORMAT_BINARY) == [["a,b", 'c"d'], ["x", "y"]]


def test_decode_text_array_literal_preserves_unquoted_scalar_tokens():
    literal = b"{true,False,001,3.50}"
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_TEXT_ARRAY, raw, FORMAT_BINARY) == ["true", "False", "001", "3.50"]


def test_type_name_includes_array_names():
    assert type_name(OID_TEXT_ARRAY) == "text[]"


def test_encode_timetz_array_infers_timetz_array_oid():
    values = [
        dt.time(1, 2, 3, tzinfo=dt.timezone(dt.timedelta(hours=2))),
        dt.time(4, 5, 6, tzinfo=dt.timezone(dt.timedelta(hours=-5))),
    ]
    param, oid = encode_param(values)
    assert oid == OID_TIMETZ_ARRAY
    decoded = decode_value(oid, param.data, FORMAT_BINARY)
    assert decoded == values


def test_decode_date_array_to_date_values():
    literal = b'{"2026-01-10","2026-02-11"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_DATE_ARRAY, raw, FORMAT_BINARY) == [
        dt.date(2026, 1, 10),
        dt.date(2026, 2, 11),
    ]


def test_decode_time_array_with_offset_payload_raises():
    literal = b'{"12:34:56+02"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    with pytest.raises(ValueError):
        decode_value(OID_TIME_ARRAY, raw, FORMAT_BINARY)


def test_decode_timetz_array_without_offset_payload_raises():
    literal = b'{"08:09:10"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    with pytest.raises(ValueError):
        decode_value(OID_TIMETZ_ARRAY, raw, FORMAT_BINARY)


def test_decode_timestamp_array_with_offset_payload_raises():
    literal = b'{"2026-03-01 12:34:56+02"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    with pytest.raises(ValueError):
        decode_value(OID_TIMESTAMP_ARRAY, raw, FORMAT_BINARY)


def test_decode_numeric_array_to_decimal_values():
    literal = b"{1,2.5,3.75}"
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_NUMERIC_ARRAY, raw, FORMAT_BINARY) == [
        decimal.Decimal("1"),
        decimal.Decimal("2.5"),
        decimal.Decimal("3.75"),
    ]


def test_decode_uuid_array_to_uuid_values():
    literal = b'{"12345678-1234-1234-1234-123456789abc","aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_UUID_ARRAY, raw, FORMAT_BINARY) == [
        uuid.UUID("12345678-1234-1234-1234-123456789abc"),
        uuid.UUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
    ]


def test_decode_timestamptz_array_to_aware_datetimes():
    literal = b'{"2026-03-01 12:34:56+02","2026-03-02 01:02:03-05"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_TIMESTAMPTZ_ARRAY, raw, FORMAT_BINARY) == [
        dt.datetime(2026, 3, 1, 12, 34, 56, tzinfo=dt.timezone(dt.timedelta(hours=2))),
        dt.datetime(2026, 3, 2, 1, 2, 3, tzinfo=dt.timezone(dt.timedelta(hours=-5))),
    ]


def test_decode_bytea_array_to_bytes_values():
    literal = b'{"\\\\x6162","\\\\x63"}'
    raw = len(literal).to_bytes(4, byteorder="little") + literal
    assert decode_value(OID_BYTEA_ARRAY, raw, FORMAT_BINARY) == [b"ab", b"c"]


def test_encode_decode_range_roundtrip():
    param, oid = encode_param(
        Range(
            lower=1,
            upper=10,
            lower_inclusive=True,
            upper_inclusive=False,
            range_oid=OID_INT4RANGE,
        )
    )

    assert oid == OID_INT4RANGE
    value = decode_value(oid, param.data, FORMAT_BINARY)
    assert isinstance(value, Range)
    assert value.lower == 1
    assert value.upper == 10
    assert value.lower_inclusive is True
    assert value.upper_inclusive is False


def test_encode_daterange_with_string_bounds_roundtrip():
    param, oid = encode_param(
        Range(
            lower="2026-01-10",
            upper="2026-02-11",
            lower_inclusive=True,
            upper_inclusive=False,
            range_oid=OID_DATERANGE,
        )
    )

    assert oid == OID_DATERANGE
    value = decode_value(oid, param.data, FORMAT_BINARY)
    assert isinstance(value, Range)
    assert value.lower == dt.date(2026, 1, 10)
    assert value.upper == dt.date(2026, 2, 11)


def test_encode_tstzrange_with_string_bounds_roundtrip():
    param, oid = encode_param(
        Range(
            lower="2026-03-01T00:00:00Z",
            upper="2026-03-02 12:30:00+02",
            lower_inclusive=True,
            upper_inclusive=False,
            range_oid=OID_TSTZRANGE,
        )
    )

    assert oid == OID_TSTZRANGE
    value = decode_value(oid, param.data, FORMAT_BINARY)
    assert isinstance(value, Range)
    assert value.lower == dt.datetime(2026, 3, 1, 0, 0, tzinfo=dt.timezone.utc)
    assert value.upper == dt.datetime(2026, 3, 2, 10, 30, tzinfo=dt.timezone.utc)


def test_encode_tsrange_with_string_bounds_roundtrip():
    param, oid = encode_param(
        Range(
            lower="2026-03-01 10:00:00+02",
            upper="2026-03-01 12:00:00+02",
            lower_inclusive=True,
            upper_inclusive=False,
            range_oid=OID_TSRANGE,
        )
    )

    assert oid == OID_TSRANGE
    value = decode_value(oid, param.data, FORMAT_BINARY)
    assert isinstance(value, Range)
    assert value.lower == dt.datetime(2026, 3, 1, 8, 0, 0)
    assert value.upper == dt.datetime(2026, 3, 1, 10, 0, 0)


def test_encode_decode_composite_roundtrip():
    param, oid = encode_param(
        Composite(
            fields=[
                CompositeField(oid=OID_INT4, value=7),
                CompositeField(oid=OID_JSONB, value=Jsonb(raw=b'{"a":1}')),
            ],
            type_oid=OID_RECORD,
        )
    )

    assert oid == OID_RECORD
    value = decode_value(oid, param.data, FORMAT_BINARY)
    assert isinstance(value, Composite)
    assert len(value.fields) == 2
    assert value.fields[0].oid == OID_INT4
    assert value.fields[0].value == 7
    assert value.fields[1].oid == OID_JSONB
    assert isinstance(value.fields[1].value, Jsonb)
    assert value.fields[1].value.raw == b'{"a":1}'


def test_encode_vector_candidate_roundtrip():
    param, oid = encode_param([1.0, 2.5, 3.0])

    assert oid == OID_SB_VECTOR
    assert decode_value(oid, param.data, FORMAT_BINARY) == [1.0, 2.5, 3.0]
