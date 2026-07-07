// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

const test = require("node:test");
const assert = require("node:assert/strict");

const {
  FORMAT_BINARY,
  FORMAT_TEXT,
  OID_BOOL,
  OID_BYTEA,
  OID_CHAR,
  OID_CIDR,
  OID_DATE,
  OID_FLOAT4,
  OID_FLOAT8,
  OID_INET,
  OID_INT2,
  OID_INT4,
  OID_INT8,
  OID_INT4RANGE,
  OID_INTERVAL,
  OID_LINE,
  OID_MACADDR,
  OID_NUMERIC,
  OID_MONEY,
  OID_JSON,
  OID_JSONB,
  OID_TSQUERY,
  OID_TSVECTOR,
  OID_TIMESTAMP,
  OID_TIME,
  OID_UUID,
  OID_VARCHAR,
  OID_XML,
  OID_INT8RANGE,
  OID_SB_VECTOR,
  ScratchbirdGeometry,
  ScratchbirdJsonb,
  ScratchbirdRange,
  ScratchbirdTypedValue,
  encodeParam,
  decodeValue,
} = require("../dist/index.js");

function lengthPrefixed(value) {
  const data = Buffer.isBuffer(value) ? value : Buffer.from(value, "utf8");
  const out = Buffer.alloc(4 + data.length);
  out.writeUInt32LE(data.length, 0);
  data.copy(out, 4);
  return out;
}

test("encodeParam covers representative primitive, structured, and typed OID inputs", () => {
  {
    const encoded = encodeParam(true);
    assert.equal(encoded.oid, OID_BOOL);
    assert.equal(encoded.param.format, FORMAT_BINARY);
    assert.deepEqual(Array.from(encoded.param.data ?? Buffer.alloc(0)), [1]);
  }
  {
    const encoded = encodeParam(42);
    assert.equal(encoded.oid, OID_INT4);
    assert.equal(encoded.param.data.readInt32LE(0), 42);
  }
  {
    const encoded = encodeParam(2147483648);
    assert.equal(encoded.oid, OID_INT8);
    assert.equal(encoded.param.data.readBigInt64LE(0), 2147483648n);
  }
  {
    const encoded = encodeParam({ role: "admin", active: true });
    assert.equal(encoded.oid, OID_JSON);
    assert.equal(encoded.param.data.readUInt32LE(0) > 0, true);
  }
  {
    const encoded = encodeParam([1, 2, 3]);
    assert.equal(encoded.oid, OID_SB_VECTOR);
    assert.equal(encoded.param.data.subarray(4).toString("utf8"), "[1,2,3]");
  }
  {
    const encoded = encodeParam(new ScratchbirdTypedValue(OID_INT2, 123));
    assert.equal(encoded.oid, OID_INT2);
    assert.equal(encoded.param.data.readInt16LE(0), 123);
  }
  {
    const encoded = encodeParam(new ScratchbirdTypedValue(OID_FLOAT4, 12.5));
    assert.equal(encoded.oid, OID_FLOAT4);
    assert.equal(encoded.param.data.readFloatLE(0), 12.5);
  }
  {
    const encoded = encodeParam(new ScratchbirdTypedValue(OID_XML, "<root/>"));
    assert.equal(encoded.oid, OID_XML);
    assert.equal(encoded.param.data.subarray(4).toString("utf8"), "<root/>");
  }
  {
    const encoded = encodeParam(new ScratchbirdTypedValue(OID_UUID, "00112233-4455-6677-8899-aabbccddeeff"));
    assert.equal(encoded.oid, OID_UUID);
    assert.equal(encoded.param.data.length, 16);
  }
  {
    const range = new ScratchbirdRange({ lower: 1, upper: 7, rangeOid: OID_INT4RANGE });
    const encoded = encodeParam(new ScratchbirdTypedValue(OID_INT4RANGE, range));
    assert.equal(encoded.oid, OID_INT4RANGE);
    assert.ok(encoded.param.data.length >= 4);
  }
});

test("encodeParam rejects unsupported numeric, range, and typed-value inputs", () => {
  assert.throws(() => encodeParam(Number.POSITIVE_INFINITY), /must be finite/);
  assert.throws(() => encodeParam(new ScratchbirdRange({ empty: true })), /cannot be inferred/);
  assert.throws(() => encodeParam(new ScratchbirdTypedValue(OID_INT2, 40000)), /out of range/);
  assert.throws(() => encodeParam(new ScratchbirdTypedValue(OID_FLOAT8, "12.5")), /requires finite numeric/);
  assert.throws(() => encodeParam(new ScratchbirdTypedValue(OID_UUID, "not-a-uuid")), /requires UUID string/);
});

test("decodeValue decodes jsonb wrapper and bytea payload", () => {
  const jsonb = decodeValue(OID_JSONB, lengthPrefixed('{"k":1}'), FORMAT_BINARY);
  assert.ok(jsonb instanceof ScratchbirdJsonb);
  assert.equal(jsonb.raw.toString("utf8"), '{"k":1}');

  const bytes = decodeValue(OID_BYTEA, lengthPrefixed(Buffer.from([1, 2, 3, 4])), FORMAT_BINARY);
  assert.ok(Buffer.isBuffer(bytes));
  assert.deepEqual(Array.from(bytes), [1, 2, 3, 4]);
});

test("decodeValue decodes numeric, money, uuid, and vector", () => {
  const numeric = decodeValue(OID_NUMERIC, lengthPrefixed("12345.678"), FORMAT_BINARY);
  assert.equal(numeric, "12345.678");

  const moneyBuf = Buffer.alloc(8);
  moneyBuf.writeBigInt64LE(12345n, 0);
  const money = decodeValue(OID_MONEY, moneyBuf, FORMAT_BINARY);
  assert.equal(money, "123.45");

  const uuidBytes = Buffer.from("00112233445566778899aabbccddeeff", "hex");
  const uuid = decodeValue(OID_UUID, uuidBytes, FORMAT_BINARY);
  assert.equal(uuid, "00112233-4455-6677-8899-aabbccddeeff");

  const vector = decodeValue(OID_SB_VECTOR, lengthPrefixed("[0.5, 1.5, 2.5]"), FORMAT_BINARY);
  assert.deepEqual(vector, [0.5, 1.5, 2.5]);
});

test("decodeValue decodes date/time/timestamp/interval and text-like wire families", () => {
  const datePayload = Buffer.alloc(4);
  datePayload.writeInt32LE(1, 0);
  const dateValue = decodeValue(OID_DATE, datePayload, FORMAT_BINARY);
  assert.equal(dateValue.toISOString().slice(0, 10), "2000-01-02");

  const timePayload = Buffer.alloc(8);
  timePayload.writeBigInt64LE(90_000_000n, 0);
  const timeValue = decodeValue(OID_TIME, timePayload, FORMAT_BINARY);
  assert.equal(timeValue.toISOString(), "2000-01-01T00:01:30.000Z");

  const tsPayload = Buffer.alloc(8);
  tsPayload.writeBigInt64LE(1_500_000n, 0);
  const tsValue = decodeValue(OID_TIMESTAMP, tsPayload, FORMAT_BINARY);
  assert.equal(tsValue.toISOString(), "2000-01-01T00:00:01.500Z");

  const intervalPayload = Buffer.alloc(16);
  intervalPayload.writeBigInt64LE(1_250_000n, 0);
  intervalPayload.writeInt32LE(3, 8);
  intervalPayload.writeInt32LE(2, 12);
  const interval = decodeValue(OID_INTERVAL, intervalPayload, FORMAT_BINARY);
  assert.deepEqual(interval, { months: 2, days: 3, micros: 1250000 });

  assert.equal(decodeValue(OID_VARCHAR, lengthPrefixed("typed_text"), FORMAT_BINARY), "typed_text");
  assert.equal(decodeValue(OID_CHAR, lengthPrefixed("c"), FORMAT_BINARY), "c");
  assert.equal(decodeValue(OID_XML, lengthPrefixed("<x/>"), FORMAT_BINARY), "<x/>");
  assert.equal(decodeValue(OID_INET, lengthPrefixed("127.0.0.1"), FORMAT_BINARY), "127.0.0.1");
  assert.equal(decodeValue(OID_CIDR, lengthPrefixed("127.0.0.0/8"), FORMAT_BINARY), "127.0.0.0/8");
  assert.equal(decodeValue(OID_MACADDR, lengthPrefixed("aa:bb:cc:dd:ee:ff"), FORMAT_BINARY), "aa:bb:cc:dd:ee:ff");
  assert.equal(decodeValue(OID_TSVECTOR, lengthPrefixed("'cat':1"), FORMAT_BINARY), "'cat':1");
  assert.equal(decodeValue(OID_TSQUERY, lengthPrefixed("'cat' & 'dog'"), FORMAT_BINARY), "'cat' & 'dog'");
});

test("decodeValue decodes geometry payloads as ScratchbirdGeometry wrappers", () => {
  const geometry = decodeValue(OID_LINE, lengthPrefixed(Buffer.from([0xde, 0xad, 0xbe, 0xef])), FORMAT_BINARY);
  assert.ok(geometry instanceof ScratchbirdGeometry);
  assert.deepEqual(Array.from(geometry.wkb), [0xde, 0xad, 0xbe, 0xef]);
});

test("typed textual OIDs encode with explicit wire OID selection", () => {
  const typedTextOids = [OID_CHAR, OID_VARCHAR, OID_XML, OID_INET, OID_CIDR, OID_MACADDR, OID_TSVECTOR, OID_TSQUERY];
  for (const oid of typedTextOids) {
    const encoded = encodeParam(new ScratchbirdTypedValue(oid, "value"));
    assert.equal(encoded.oid, oid);
    assert.equal(encoded.param.data.readUInt32LE(0), 5);
    assert.equal(encoded.param.data.subarray(4).toString("utf8"), "value");
  }
});

test("decodeValue decodes int8range boundaries", () => {
  const payload = Buffer.alloc(4 + 4 + 8 + 4 + 8);
  payload[0] = 0;
  payload.writeInt32LE(8, 4);
  payload.writeBigInt64LE(10n, 8);
  payload.writeInt32LE(8, 16);
  payload.writeBigInt64LE(20n, 20);

  const range = decodeValue(OID_INT8RANGE, payload, FORMAT_BINARY);
  assert.ok(range instanceof ScratchbirdRange);
  assert.equal(range.lower, 10n);
  assert.equal(range.upper, 20n);
  assert.equal(range.empty, false);
  assert.equal(range.lowerInclusive, false);
  assert.equal(range.upperInclusive, false);
});

test("decodeValue decodes composite row payload", () => {
  const OID_RECORD = 2249;
  const payload = Buffer.alloc(4 + 4 + 4 + 4);
  payload.writeInt32LE(1, 0);
  payload.writeUInt32LE(OID_INT4, 4);
  payload.writeInt32LE(4, 8);
  payload.writeInt32LE(77, 12);

  const composite = decodeValue(OID_RECORD, payload, FORMAT_BINARY);
  assert.equal(composite.fields.length, 1);
  assert.equal(composite.fields[0].oid, OID_INT4);
  assert.equal(composite.fields[0].value, 77);
});

test("decodeValue unknown-type heuristics parse text and arrays", () => {
  assert.equal(decodeValue(0, Buffer.from("true", "utf8"), FORMAT_TEXT), true);
  assert.equal(decodeValue(0, Buffer.from("42", "utf8"), FORMAT_TEXT), 42);
  assert.equal(decodeValue(0, Buffer.from("9007199254740993", "utf8"), FORMAT_TEXT), 9007199254740993n);
  assert.deepEqual(decodeValue(0, lengthPrefixed("{1,2,3}"), FORMAT_BINARY), [1, 2, 3]);
});

test("decodeValue preserves server text payload when typed text metadata is imprecise", () => {
  assert.equal(decodeValue(OID_INT8, Buffer.from("1251.00", "utf8"), FORMAT_TEXT), "1251.00");
});
