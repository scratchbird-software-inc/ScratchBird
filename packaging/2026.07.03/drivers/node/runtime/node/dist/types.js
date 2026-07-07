"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.ScratchbirdRange = exports.ScratchbirdGeometry = exports.ScratchbirdJson = exports.ScratchbirdJsonb = exports.OID_SB_VECTOR = exports.OID_TSQUERY = exports.OID_TSVECTOR = exports.OID_INT8RANGE = exports.OID_DATERANGE = exports.OID_TSTZRANGE = exports.OID_TSRANGE = exports.OID_NUMRANGE = exports.OID_INT4RANGE = exports.OID_RECORD = exports.OID_JSONB = exports.OID_UUID = exports.OID_NUMERIC = exports.OID_TIMETZ = exports.OID_INTERVAL = exports.OID_TIMESTAMPTZ = exports.OID_TIMESTAMP = exports.OID_TIME = exports.OID_DATE = exports.OID_VARCHAR = exports.OID_BPCHAR = exports.OID_MACADDR8 = exports.OID_INET = exports.OID_CIDR = exports.OID_MACADDR = exports.OID_MONEY = exports.OID_CIRCLE = exports.OID_FLOAT8 = exports.OID_FLOAT4 = exports.OID_LINE = exports.OID_POLYGON = exports.OID_BOX = exports.OID_PATH = exports.OID_LSEG = exports.OID_POINT = exports.OID_XML = exports.OID_JSON = exports.OID_TEXT = exports.OID_INT4 = exports.OID_INT2 = exports.OID_INT8 = exports.OID_CHAR = exports.OID_BYTEA = exports.OID_BOOL = exports.FORMAT_BINARY = exports.FORMAT_TEXT = void 0;
exports.ScratchbirdTypedValue = exports.ScratchbirdRaw = exports.ScratchbirdMoney = exports.ScratchbirdDecimal = exports.ScratchbirdTimestampTZ = exports.ScratchbirdTimestamp = exports.ScratchbirdTime = exports.ScratchbirdDate = exports.ScratchbirdInterval = exports.ScratchbirdComposite = void 0;
exports.oidToString = oidToString;
exports.encodeParam = encodeParam;
exports.decodeValue = decodeValue;
exports.decodeArrayLiteral = decodeArrayLiteral;
const node_buffer_1 = require("node:buffer");
exports.FORMAT_TEXT = 0;
exports.FORMAT_BINARY = 1;
exports.OID_BOOL = 16;
exports.OID_BYTEA = 17;
exports.OID_CHAR = 18;
exports.OID_INT8 = 20;
exports.OID_INT2 = 21;
exports.OID_INT4 = 23;
exports.OID_TEXT = 25;
exports.OID_JSON = 114;
exports.OID_XML = 142;
exports.OID_POINT = 600;
exports.OID_LSEG = 601;
exports.OID_PATH = 602;
exports.OID_BOX = 603;
exports.OID_POLYGON = 604;
exports.OID_LINE = 628;
exports.OID_FLOAT4 = 700;
exports.OID_FLOAT8 = 701;
exports.OID_CIRCLE = 718;
exports.OID_MONEY = 790;
exports.OID_MACADDR = 829;
exports.OID_CIDR = 650;
exports.OID_INET = 869;
exports.OID_MACADDR8 = 774;
exports.OID_BPCHAR = 1042;
exports.OID_VARCHAR = 1043;
exports.OID_DATE = 1082;
exports.OID_TIME = 1083;
exports.OID_TIMESTAMP = 1114;
exports.OID_TIMESTAMPTZ = 1184;
exports.OID_INTERVAL = 1186;
exports.OID_TIMETZ = 1266;
exports.OID_NUMERIC = 1700;
exports.OID_UUID = 2950;
exports.OID_JSONB = 3802;
exports.OID_RECORD = 2249;
exports.OID_INT4RANGE = 3904;
exports.OID_NUMRANGE = 3906;
exports.OID_TSRANGE = 3908;
exports.OID_TSTZRANGE = 3910;
exports.OID_DATERANGE = 3912;
exports.OID_INT8RANGE = 3926;
exports.OID_TSVECTOR = 3614;
exports.OID_TSQUERY = 3615;
exports.OID_SB_VECTOR = 16386;
const RANGE_EMPTY = 0x01;
const RANGE_LB_INC = 0x02;
const RANGE_UB_INC = 0x04;
const RANGE_LB_INF = 0x08;
const RANGE_UB_INF = 0x10;
class ScratchbirdJsonb {
    constructor(raw, value) {
        this.raw = raw;
        this.value = value;
    }
}
exports.ScratchbirdJsonb = ScratchbirdJsonb;
class ScratchbirdJson {
    constructor(raw, value) {
        this.raw = raw;
        this.value = value;
    }
}
exports.ScratchbirdJson = ScratchbirdJson;
class ScratchbirdGeometry {
    constructor(wkb, opts) {
        this.wkb = wkb;
        this.srid = opts?.srid;
        this.wkt = opts?.wkt;
    }
}
exports.ScratchbirdGeometry = ScratchbirdGeometry;
class ScratchbirdRange {
    constructor(init) {
        this.lowerInclusive = false;
        this.upperInclusive = false;
        this.lowerInfinite = false;
        this.upperInfinite = false;
        this.empty = false;
        if (!init)
            return;
        Object.assign(this, init);
    }
}
exports.ScratchbirdRange = ScratchbirdRange;
class ScratchbirdComposite {
    constructor(fields, typeOid = exports.OID_RECORD) {
        this.fields = fields;
        this.typeOid = typeOid;
    }
}
exports.ScratchbirdComposite = ScratchbirdComposite;
class ScratchbirdInterval {
    constructor(micros, days = 0, months = 0) {
        this.micros = micros;
        this.days = days;
        this.months = months;
    }
}
exports.ScratchbirdInterval = ScratchbirdInterval;
class ScratchbirdDate {
    constructor(value) {
        this.value = value;
    }
}
exports.ScratchbirdDate = ScratchbirdDate;
class ScratchbirdTime {
    constructor(micros) {
        this.micros = micros;
    }
}
exports.ScratchbirdTime = ScratchbirdTime;
class ScratchbirdTimestamp {
    constructor(value) {
        this.value = value;
    }
}
exports.ScratchbirdTimestamp = ScratchbirdTimestamp;
class ScratchbirdTimestampTZ {
    constructor(value) {
        this.value = value;
    }
}
exports.ScratchbirdTimestampTZ = ScratchbirdTimestampTZ;
class ScratchbirdDecimal {
    constructor(value) {
        this.value = value;
    }
}
exports.ScratchbirdDecimal = ScratchbirdDecimal;
class ScratchbirdMoney {
    constructor(cents) {
        this.cents = cents;
    }
}
exports.ScratchbirdMoney = ScratchbirdMoney;
class ScratchbirdRaw {
    constructor(oid, data) {
        this.oid = oid;
        this.data = data;
    }
}
exports.ScratchbirdRaw = ScratchbirdRaw;
class ScratchbirdTypedValue {
    constructor(oid, value) {
        this.oid = oid;
        this.value = value;
    }
}
exports.ScratchbirdTypedValue = ScratchbirdTypedValue;
const uuidRegex = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
function oidToString(oid) {
    switch (oid) {
        case exports.OID_BOOL:
            return "boolean";
        case exports.OID_INT2:
            return "int2";
        case exports.OID_INT4:
            return "int4";
        case exports.OID_INT8:
            return "int8";
        case exports.OID_FLOAT4:
            return "float4";
        case exports.OID_FLOAT8:
            return "float8";
        case exports.OID_NUMERIC:
            return "numeric";
        case exports.OID_MONEY:
            return "money";
        case exports.OID_TEXT:
            return "text";
        case exports.OID_VARCHAR:
            return "varchar";
        case exports.OID_CHAR:
        case exports.OID_BPCHAR:
            return "char";
        case exports.OID_BYTEA:
            return "bytea";
        case exports.OID_DATE:
            return "date";
        case exports.OID_TIME:
            return "time";
        case exports.OID_TIMESTAMP:
            return "timestamp";
        case exports.OID_TIMESTAMPTZ:
            return "timestamptz";
        case exports.OID_INTERVAL:
            return "interval";
        case exports.OID_UUID:
            return "uuid";
        case exports.OID_JSON:
            return "json";
        case exports.OID_JSONB:
            return "jsonb";
        case exports.OID_XML:
            return "xml";
        case exports.OID_INET:
            return "inet";
        case exports.OID_CIDR:
            return "cidr";
        case exports.OID_MACADDR:
            return "macaddr";
        case exports.OID_MACADDR8:
            return "macaddr8";
        case exports.OID_TSVECTOR:
            return "tsvector";
        case exports.OID_TSQUERY:
            return "tsquery";
        case exports.OID_INT4RANGE:
            return "int4range";
        case exports.OID_INT8RANGE:
            return "int8range";
        case exports.OID_NUMRANGE:
            return "numrange";
        case exports.OID_TSRANGE:
            return "tsrange";
        case exports.OID_TSTZRANGE:
            return "tstzrange";
        case exports.OID_DATERANGE:
            return "daterange";
        case exports.OID_SB_VECTOR:
            return "vector";
        default:
            return "unknown";
    }
}
function encodeParam(value) {
    if (value === null || value === undefined) {
        return { param: { isNull: true, format: exports.FORMAT_BINARY }, oid: 0 };
    }
    if (value instanceof ScratchbirdRaw) {
        return { param: { data: node_buffer_1.Buffer.from(value.data), format: exports.FORMAT_BINARY }, oid: value.oid };
    }
    if (value instanceof ScratchbirdTypedValue) {
        return encodeTypedValue(value);
    }
    if (value instanceof ScratchbirdJsonb) {
        let raw = value.raw;
        if ((!raw || raw.length === 0) && value.value !== undefined) {
            raw = node_buffer_1.Buffer.from(JSON.stringify(value.value), "utf8");
        }
        if (!raw || raw.length === 0) {
            throw new Error("JSONB requires raw payload");
        }
        return { param: { data: encodeLengthPrefixed(raw), format: exports.FORMAT_BINARY }, oid: exports.OID_JSONB };
    }
    if (value instanceof ScratchbirdJson) {
        let raw = value.raw;
        if ((!raw || raw.length === 0) && value.value !== undefined) {
            raw = node_buffer_1.Buffer.from(JSON.stringify(value.value), "utf8");
        }
        if (!raw) {
            throw new Error("JSON requires raw payload");
        }
        return { param: { data: encodeLengthPrefixed(raw), format: exports.FORMAT_BINARY }, oid: exports.OID_JSON };
    }
    if (value instanceof ScratchbirdComposite) {
        const encoded = encodeComposite(value);
        return { param: { data: encoded.data, format: exports.FORMAT_BINARY }, oid: encoded.oid };
    }
    if (value instanceof ScratchbirdGeometry) {
        if (!value.wkb || value.wkb.length === 0) {
            throw new Error("geometry requires WKB payload");
        }
        return { param: { data: encodeLengthPrefixed(value.wkb), format: exports.FORMAT_BINARY }, oid: exports.OID_POINT };
    }
    if (value instanceof ScratchbirdRange) {
        const encoded = encodeRange(value);
        return { param: { data: encoded.data, format: exports.FORMAT_BINARY }, oid: encoded.oid };
    }
    if (value instanceof ScratchbirdDate) {
        return { param: { data: encodeDate(value.value), format: exports.FORMAT_BINARY }, oid: exports.OID_DATE };
    }
    if (value instanceof ScratchbirdTime) {
        return { param: { data: encodeTimeMicros(value.micros), format: exports.FORMAT_BINARY }, oid: exports.OID_TIME };
    }
    if (value instanceof ScratchbirdTimestamp) {
        return { param: { data: encodeTimestamp(value.value), format: exports.FORMAT_BINARY }, oid: exports.OID_TIMESTAMP };
    }
    if (value instanceof ScratchbirdTimestampTZ) {
        return { param: { data: encodeTimestamp(value.value), format: exports.FORMAT_BINARY }, oid: exports.OID_TIMESTAMPTZ };
    }
    if (value instanceof ScratchbirdInterval) {
        return { param: { data: encodeInterval(value), format: exports.FORMAT_BINARY }, oid: exports.OID_INTERVAL };
    }
    if (value instanceof ScratchbirdDecimal) {
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(value.value, "utf8")), format: exports.FORMAT_BINARY }, oid: exports.OID_NUMERIC };
    }
    if (value instanceof ScratchbirdMoney) {
        return { param: { data: encodeInt64(value.cents), format: exports.FORMAT_BINARY }, oid: exports.OID_MONEY };
    }
    if (typeof value === "boolean") {
        return { param: { data: node_buffer_1.Buffer.from([value ? 1 : 0]), format: exports.FORMAT_BINARY }, oid: exports.OID_BOOL };
    }
    if (typeof value === "bigint") {
        return { param: { data: encodeInt64(value), format: exports.FORMAT_BINARY }, oid: exports.OID_INT8 };
    }
    if (typeof value === "number") {
        if (!Number.isFinite(value)) {
            throw new Error("numeric value must be finite");
        }
        if (Number.isInteger(value)) {
            if (value >= -2147483648 && value <= 2147483647) {
                return { param: { data: encodeInt32(value), format: exports.FORMAT_BINARY }, oid: exports.OID_INT4 };
            }
            if (Number.isSafeInteger(value)) {
                return { param: { data: encodeInt64(BigInt(value)), format: exports.FORMAT_BINARY }, oid: exports.OID_INT8 };
            }
            throw new Error("integer out of range for int64");
        }
        return { param: { data: encodeFloat64(value), format: exports.FORMAT_BINARY }, oid: exports.OID_FLOAT8 };
    }
    if (value instanceof Date) {
        return { param: { data: encodeTimestamp(value), format: exports.FORMAT_BINARY }, oid: exports.OID_TIMESTAMPTZ };
    }
    if (value instanceof node_buffer_1.Buffer) {
        return { param: { data: encodeLengthPrefixed(value), format: exports.FORMAT_BINARY }, oid: exports.OID_BYTEA };
    }
    if (value instanceof Uint8Array) {
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(value)), format: exports.FORMAT_BINARY }, oid: exports.OID_BYTEA };
    }
    if (value instanceof Float32Array || value instanceof Float64Array) {
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(formatVectorLiteral(Array.from(value)))), format: exports.FORMAT_BINARY }, oid: exports.OID_SB_VECTOR };
    }
    if (Array.isArray(value)) {
        if (value.length > 0 && value.every((item) => typeof item === "number")) {
            return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(formatVectorLiteral(value))), format: exports.FORMAT_BINARY }, oid: exports.OID_SB_VECTOR };
        }
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(formatArrayLiteral(value), "utf8")), format: exports.FORMAT_BINARY }, oid: 0 };
    }
    if (typeof value === "string") {
        if (uuidRegex.test(value)) {
            return { param: { data: node_buffer_1.Buffer.from(value.replace(/-/g, ""), "hex"), format: exports.FORMAT_BINARY }, oid: exports.OID_UUID };
        }
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(value, "utf8")), format: exports.FORMAT_BINARY }, oid: exports.OID_TEXT };
    }
    if (isIntervalObject(value)) {
        return { param: { data: encodeInterval(value), format: exports.FORMAT_BINARY }, oid: exports.OID_INTERVAL };
    }
    if (typeof value === "object") {
        return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(JSON.stringify(value), "utf8")), format: exports.FORMAT_BINARY }, oid: exports.OID_JSON };
    }
    throw new Error("unsupported parameter type");
}
function decodeValue(typeOid, data, format) {
    if (data === null) {
        return null;
    }
    if (typeOid === 0) {
        if (format === exports.FORMAT_TEXT) {
            return parseUnknownText(decodeTextValue(data));
        }
        return decodeUnknownBinary(data);
    }
    if (format === exports.FORMAT_TEXT) {
        try {
            return decodeTextTypedValue(typeOid, data);
        }
        catch {
            return decodeTextValue(data);
        }
    }
    return decodeBinaryValue(typeOid, data);
}
function decodeBinaryValue(typeOid, data) {
    const textFallback = maybeDecodeBinaryTextValue(typeOid, data);
    if (textFallback !== undefined) {
        return textFallback;
    }
    switch (typeOid) {
        case exports.OID_BOOL:
            return data.length > 0 && data[0] === 1;
        case exports.OID_INT2:
            return data.readInt16LE(0);
        case exports.OID_INT4:
            return data.readInt32LE(0);
        case exports.OID_INT8: {
            const value = data.readBigInt64LE(0);
            if (value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)) {
                return Number(value);
            }
            return value;
        }
        case exports.OID_FLOAT4:
            return data.readFloatLE(0);
        case exports.OID_FLOAT8:
            return data.readDoubleLE(0);
        case exports.OID_NUMERIC:
            return stripLengthPrefix(data).toString("utf8");
        case exports.OID_MONEY:
            return moneyToString(data.readBigInt64LE(0));
        case exports.OID_TEXT:
        case exports.OID_VARCHAR:
        case exports.OID_CHAR:
        case exports.OID_BPCHAR:
        case exports.OID_JSON:
        case exports.OID_XML:
        case exports.OID_TSVECTOR:
        case exports.OID_TSQUERY:
            return stripLengthPrefix(data).toString("utf8");
        case exports.OID_JSONB:
            return new ScratchbirdJsonb(node_buffer_1.Buffer.from(stripLengthPrefix(data)));
        case exports.OID_BYTEA:
            return node_buffer_1.Buffer.from(stripLengthPrefix(data));
        case exports.OID_DATE:
            return decodeDate(data);
        case exports.OID_TIME:
            return decodeTime(data);
        case exports.OID_TIMESTAMP:
            return decodeTimestamp(data);
        case exports.OID_TIMESTAMPTZ:
            return decodeTimestamp(data);
        case exports.OID_INTERVAL:
            return decodeInterval(data);
        case exports.OID_UUID:
            return bytesToUuid(data);
        case exports.OID_INET:
        case exports.OID_CIDR:
        case exports.OID_MACADDR:
        case exports.OID_MACADDR8:
            return stripLengthPrefix(data).toString("utf8");
        case exports.OID_INT4RANGE:
        case exports.OID_INT8RANGE:
        case exports.OID_NUMRANGE:
        case exports.OID_TSRANGE:
        case exports.OID_TSTZRANGE:
        case exports.OID_DATERANGE:
            return decodeRange(typeOid, data);
        case exports.OID_SB_VECTOR:
            return parseVectorLiteral(stripLengthPrefix(data).toString("utf8"));
        case exports.OID_POINT:
        case exports.OID_LSEG:
        case exports.OID_PATH:
        case exports.OID_BOX:
        case exports.OID_POLYGON:
        case exports.OID_LINE:
        case exports.OID_CIRCLE:
            return new ScratchbirdGeometry(node_buffer_1.Buffer.from(stripLengthPrefix(data)));
        case exports.OID_RECORD:
            return decodeComposite(data);
        default:
            return node_buffer_1.Buffer.from(data);
    }
}
function decodeTextTypedValue(typeOid, data) {
    const text = decodeTextValue(data);
    const stripped = text.trim();
    switch (typeOid) {
        case exports.OID_BOOL:
            if (!/^(t|true|1|f|false|0)$/i.test(stripped)) {
                throw new Error("invalid boolean text payload");
            }
            return /^(t|true|1)$/i.test(stripped);
        case exports.OID_INT2:
        case exports.OID_INT4: {
            if (!/^[+-]?\d+$/.test(stripped)) {
                throw new Error("invalid integer text payload");
            }
            const parsed = Number.parseInt(stripped, 10);
            if (Number.isNaN(parsed)) {
                throw new Error("invalid integer text payload");
            }
            return parsed;
        }
        case exports.OID_INT8: {
            if (!/^[+-]?\d+$/.test(stripped)) {
                throw new Error("invalid bigint text payload");
            }
            try {
                const parsed = BigInt(stripped);
                if (parsed >= BigInt(Number.MIN_SAFE_INTEGER) && parsed <= BigInt(Number.MAX_SAFE_INTEGER)) {
                    return Number(parsed);
                }
                return parsed;
            }
            catch {
                throw new Error("invalid bigint text payload");
            }
        }
        case exports.OID_FLOAT4:
        case exports.OID_FLOAT8: {
            if (!/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/.test(stripped)) {
                throw new Error("invalid floating text payload");
            }
            const parsed = Number(stripped);
            if (Number.isNaN(parsed)) {
                throw new Error("invalid floating text payload");
            }
            return parsed;
        }
        case exports.OID_NUMERIC:
        case exports.OID_MONEY:
        case exports.OID_TEXT:
        case exports.OID_VARCHAR:
        case exports.OID_CHAR:
        case exports.OID_BPCHAR:
        case exports.OID_JSON:
        case exports.OID_XML:
        case exports.OID_TSVECTOR:
        case exports.OID_TSQUERY:
        case exports.OID_INET:
        case exports.OID_CIDR:
        case exports.OID_MACADDR:
        case exports.OID_MACADDR8:
            return text;
        case exports.OID_JSONB:
            return new ScratchbirdJsonb(node_buffer_1.Buffer.from(text, "utf8"));
        case exports.OID_BYTEA:
            return decodeTextByteaValue(stripped);
        case exports.OID_DATE:
            return new Date(`${stripped}T00:00:00.000Z`);
        case exports.OID_TIME:
            return decodeTimeText(stripped);
        case exports.OID_TIMESTAMP:
            return decodeTimestampText(stripped, false);
        case exports.OID_TIMESTAMPTZ:
            return decodeTimestampText(stripped, true);
        case exports.OID_UUID:
            return stripped;
        case exports.OID_SB_VECTOR:
            return parseVectorLiteral(stripped);
        default:
            return parseUnknownText(text);
    }
}
function maybeDecodeBinaryTextValue(typeOid, data) {
    switch (typeOid) {
        case exports.OID_NUMERIC:
        case exports.OID_TEXT:
        case exports.OID_VARCHAR:
        case exports.OID_CHAR:
        case exports.OID_BPCHAR:
        case exports.OID_JSON:
        case exports.OID_XML:
        case exports.OID_TSVECTOR:
        case exports.OID_TSQUERY:
        case exports.OID_JSONB:
        case exports.OID_BYTEA:
        case exports.OID_DATE:
        case exports.OID_TIME:
        case exports.OID_TIMESTAMP:
        case exports.OID_TIMESTAMPTZ:
        case exports.OID_UUID:
        case exports.OID_SB_VECTOR:
        case exports.OID_INET:
        case exports.OID_CIDR:
        case exports.OID_MACADDR:
        case exports.OID_MACADDR8:
            break;
        default:
            return undefined;
    }
    const candidates = [];
    const stripped = stripTrailingNulls(data);
    if (stripped.length > 0 && looksLikeText(stripped)) {
        candidates.push(stripped);
    }
    if (data.length >= 4) {
        const maybePrefixed = stripLengthPrefix(data);
        if (maybePrefixed.length > 0 && maybePrefixed.length !== data.length && looksLikeText(maybePrefixed)) {
            candidates.push(maybePrefixed);
        }
    }
    for (const candidate of candidates) {
        try {
            return decodeTextTypedValue(typeOid, candidate);
        }
        catch {
            // Fall through to the regular binary decoder on malformed text fallbacks.
        }
    }
    return undefined;
}
function encodeTypedValue(typed) {
    const oid = typed.oid;
    const value = typed.value;
    if (value === null || value === undefined) {
        return { param: { isNull: true, format: exports.FORMAT_BINARY }, oid };
    }
    switch (oid) {
        case exports.OID_BOOL:
            if (typeof value !== "boolean") {
                throw new Error("typed boolean requires boolean value");
            }
            return { param: { data: node_buffer_1.Buffer.from([value ? 1 : 0]), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_INT2: {
            if (typeof value !== "number" || !Number.isInteger(value)) {
                throw new Error("typed int2 requires integer value");
            }
            if (value < -32768 || value > 32767) {
                throw new Error("typed int2 out of range");
            }
            return { param: { data: encodeInt16(value), format: exports.FORMAT_BINARY }, oid };
        }
        case exports.OID_INT4: {
            if (typeof value !== "number" || !Number.isInteger(value)) {
                throw new Error("typed int4 requires integer value");
            }
            if (value < -2147483648 || value > 2147483647) {
                throw new Error("typed int4 out of range");
            }
            return { param: { data: encodeInt32(value), format: exports.FORMAT_BINARY }, oid };
        }
        case exports.OID_INT8:
            return { param: { data: encodeInt64(toBigInt(value, "int8")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_FLOAT4:
            return { param: { data: encodeFloat32(toFiniteNumber(value, "float4")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_FLOAT8:
            return { param: { data: encodeFloat64(toFiniteNumber(value, "float8")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_NUMERIC:
            return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(String(value), "utf8")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_MONEY:
            return { param: { data: encodeInt64(toBigInt(value, "money")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_UUID:
            return { param: { data: encodeUuid(value), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_JSON:
            return { param: { data: encodeLengthPrefixed(encodeJsonPayload(value)), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_JSONB:
            return { param: { data: encodeLengthPrefixed(encodeJsonPayload(value)), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_BYTEA:
            return { param: { data: encodeLengthPrefixed(toBuffer(value, "bytea")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_DATE:
            return { param: { data: encodeDate(toDate(value, "date")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_TIME: {
            const micros = value instanceof ScratchbirdTime
                ? value.micros
                : typeof value === "number"
                    ? value
                    : Number.NaN;
            if (!Number.isFinite(micros)) {
                throw new Error("typed time requires microsecond number or ScratchbirdTime");
            }
            return { param: { data: encodeTimeMicros(micros), format: exports.FORMAT_BINARY }, oid };
        }
        case exports.OID_TIMESTAMP:
        case exports.OID_TIMESTAMPTZ:
            return { param: { data: encodeTimestamp(toDate(value, oid === exports.OID_TIMESTAMP ? "timestamp" : "timestamptz")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_INTERVAL:
            if (value instanceof ScratchbirdInterval || isIntervalObject(value)) {
                return { param: { data: encodeInterval(value), format: exports.FORMAT_BINARY }, oid };
            }
            throw new Error("typed interval requires ScratchbirdInterval or interval object");
        case exports.OID_SB_VECTOR:
            return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(formatVectorLiteral(toNumericArray(value)), "utf8")), format: exports.FORMAT_BINARY }, oid };
        case exports.OID_INT4RANGE:
        case exports.OID_INT8RANGE:
        case exports.OID_NUMRANGE:
        case exports.OID_TSRANGE:
        case exports.OID_TSTZRANGE:
        case exports.OID_DATERANGE: {
            if (!(value instanceof ScratchbirdRange)) {
                throw new Error("typed range requires ScratchbirdRange value");
            }
            const encoded = encodeRange(new ScratchbirdRange({ ...value, rangeOid: oid }));
            return { param: { data: encoded.data, format: exports.FORMAT_BINARY }, oid };
        }
        case exports.OID_RECORD: {
            if (!(value instanceof ScratchbirdComposite)) {
                throw new Error("typed record requires ScratchbirdComposite value");
            }
            const encoded = encodeComposite(new ScratchbirdComposite(value.fields, exports.OID_RECORD));
            return { param: { data: encoded.data, format: exports.FORMAT_BINARY }, oid: exports.OID_RECORD };
        }
        case exports.OID_POINT:
        case exports.OID_LSEG:
        case exports.OID_PATH:
        case exports.OID_BOX:
        case exports.OID_POLYGON:
        case exports.OID_LINE:
        case exports.OID_CIRCLE: {
            if (!(value instanceof ScratchbirdGeometry)) {
                throw new Error("typed geometry requires ScratchbirdGeometry value");
            }
            if (!value.wkb || value.wkb.length === 0) {
                throw new Error("typed geometry requires WKB payload");
            }
            return { param: { data: encodeLengthPrefixed(value.wkb), format: exports.FORMAT_BINARY }, oid };
        }
        case exports.OID_TEXT:
        case exports.OID_CHAR:
        case exports.OID_BPCHAR:
        case exports.OID_VARCHAR:
        case exports.OID_XML:
        case exports.OID_INET:
        case exports.OID_CIDR:
        case exports.OID_MACADDR:
        case exports.OID_MACADDR8:
        case exports.OID_TSVECTOR:
        case exports.OID_TSQUERY:
            if (typeof value !== "string") {
                throw new Error(`typed ${oidToString(oid)} requires string value`);
            }
            return { param: { data: encodeLengthPrefixed(node_buffer_1.Buffer.from(value, "utf8")), format: exports.FORMAT_BINARY }, oid };
        default:
            throw new Error(`typed OID ${oid} is not supported for parameter encoding`);
    }
}
function encodeComposite(value) {
    const chunks = [];
    const fields = value.fields ?? [];
    const header = node_buffer_1.Buffer.alloc(4);
    header.writeInt32LE(fields.length, 0);
    chunks.push(header);
    for (const field of fields) {
        let oid = field.oid ?? 0;
        let data = null;
        if (field.raw !== undefined) {
            data = field.raw;
        }
        else if (field.value !== undefined) {
            const encoded = encodeParam(field.value);
            if (!oid) {
                oid = encoded.oid;
            }
            if (encoded.param.isNull) {
                data = null;
            }
            else {
                data = encoded.param.data ?? node_buffer_1.Buffer.alloc(0);
            }
        }
        if (!oid) {
            throw new Error("composite field OID is required");
        }
        const oidBuf = node_buffer_1.Buffer.alloc(4);
        oidBuf.writeUInt32LE(oid, 0);
        const lenBuf = node_buffer_1.Buffer.alloc(4);
        if (data === null) {
            lenBuf.writeInt32LE(-1, 0);
            chunks.push(oidBuf, lenBuf);
            continue;
        }
        lenBuf.writeInt32LE(data.length, 0);
        chunks.push(oidBuf, lenBuf, data);
    }
    const typeOid = value.typeOid || exports.OID_RECORD;
    return { data: node_buffer_1.Buffer.concat(chunks), oid: typeOid };
}
function decodeComposite(data) {
    if (data.length < 4) {
        return new ScratchbirdComposite([]);
    }
    const count = data.readInt32LE(0);
    let offset = 4;
    const fields = [];
    for (let i = 0; i < count; i++) {
        if (offset + 8 > data.length)
            break;
        const oid = data.readUInt32LE(offset);
        offset += 4;
        const length = data.readInt32LE(offset);
        offset += 4;
        if (length < 0) {
            fields.push({ oid, value: null, raw: null });
            continue;
        }
        if (offset + length > data.length) {
            break;
        }
        const raw = data.subarray(offset, offset + length);
        offset += length;
        const value = decodeBinaryValue(oid, raw);
        fields.push({ oid, value, raw: node_buffer_1.Buffer.from(raw) });
    }
    return new ScratchbirdComposite(fields);
}
function decodeTextValue(data) {
    if (data.length >= 4) {
        const length = data.readUInt32LE(0);
        if (length <= data.length - 4) {
            return data.subarray(4, 4 + length).toString("utf8");
        }
    }
    return data.toString("utf8");
}
function decodeUnknownBinary(data) {
    if (data.length >= 4) {
        const length = data.readUInt32LE(0);
        if (length === data.length - 4) {
            const text = data.subarray(4).toString("utf8");
            if (looksLikeArrayLiteral(text)) {
                return parseArrayLiteral(text);
            }
            return parseUnknownText(text);
        }
    }
    const trimmed = stripTrailingNulls(data);
    if (trimmed.length > 0 && looksLikeText(trimmed)) {
        const text = trimmed.toString("utf8");
        if (looksLikeArrayLiteral(text)) {
            return parseArrayLiteral(text);
        }
        return parseUnknownText(text);
    }
    switch (data.length) {
        case 1:
            return data[0];
        case 2:
            return data.readInt16LE(0);
        case 4:
            return data.readInt32LE(0);
        case 8:
            return data.readBigInt64LE(0);
        case 16:
            return bytesToUuid(data);
        default:
            return node_buffer_1.Buffer.from(data);
    }
}
function parseUnknownText(text) {
    const trimmed = text.trim();
    if (trimmed === "") {
        return text;
    }
    if (looksLikeArrayLiteral(trimmed)) {
        return parseArrayLiteral(trimmed);
    }
    if (/^(true|false)$/i.test(trimmed)) {
        return trimmed.toLowerCase() === "true";
    }
    if (/^[+-]?\d+$/.test(trimmed)) {
        const asNum = Number(trimmed);
        if (Number.isSafeInteger(asNum) && String(asNum) === trimmed) {
            return asNum;
        }
        try {
            return BigInt(trimmed);
        }
        catch {
            return trimmed;
        }
    }
    if (/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/.test(trimmed)) {
        const asNum = Number(trimmed);
        if (!Number.isNaN(asNum)) {
            return asNum;
        }
    }
    return text;
}
function stripTrailingNulls(data) {
    let end = data.length;
    while (end > 0 && data[end - 1] === 0) {
        end -= 1;
    }
    return data.subarray(0, end);
}
function decodeTextByteaValue(text) {
    if (/^(\\x|0x)/i.test(text)) {
        const hex = text.slice(2);
        return /^[0-9a-f]*$/i.test(hex) ? node_buffer_1.Buffer.from(hex, "hex") : node_buffer_1.Buffer.from(text, "utf8");
    }
    if (/^[0-9a-f]+$/i.test(text) && text.length % 2 === 0) {
        return node_buffer_1.Buffer.from(text, "hex");
    }
    return node_buffer_1.Buffer.from(text, "utf8");
}
function decodeTimeText(text) {
    return new Date(`2000-01-01T${normalizeTemporalText(text)}`);
}
function decodeTimestampText(text, forceUtc) {
    const normalized = normalizeTemporalText(text);
    if (forceUtc) {
        return new Date(normalized.includes("+") || /z$/i.test(normalized) ? normalized : `${normalized}Z`);
    }
    return new Date(normalized.replace(" ", "T"));
}
function normalizeTemporalText(text) {
    let normalized = text.trim();
    if (normalized.includes(" ") && !normalized.includes("T")) {
        normalized = normalized.replace(" ", "T");
    }
    if (/z$/i.test(normalized)) {
        normalized = `${normalized.slice(0, -1)}+00:00`;
    }
    if (/[+-]\d{2}$/.test(normalized)) {
        normalized = `${normalized}:00`;
    }
    return normalized;
}
function looksLikeText(data) {
    for (const byte of data) {
        if (byte === 0x09 || byte === 0x0a || byte === 0x0d) {
            continue;
        }
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}
function looksLikeArrayLiteral(text) {
    return text.startsWith("{") && text.endsWith("}");
}
function encodeLengthPrefixed(data) {
    const out = node_buffer_1.Buffer.alloc(4 + data.length);
    out.writeUInt32LE(data.length, 0);
    data.copy(out, 4);
    return out;
}
function stripLengthPrefix(data) {
    if (data.length < 4) {
        return data;
    }
    const length = data.readUInt32LE(0);
    if (length <= data.length - 4) {
        return data.subarray(4, 4 + length);
    }
    return data;
}
function toFiniteNumber(value, label) {
    if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new Error(`typed ${label} requires finite numeric value`);
    }
    return value;
}
function toBigInt(value, label) {
    if (typeof value === "bigint") {
        return value;
    }
    if (typeof value === "number") {
        if (!Number.isSafeInteger(value)) {
            throw new Error(`typed ${label} requires safe integer value`);
        }
        return BigInt(value);
    }
    if (typeof value === "string" && /^[+-]?\d+$/.test(value.trim())) {
        return BigInt(value.trim());
    }
    throw new Error(`typed ${label} requires integer-compatible value`);
}
function toDate(value, label) {
    if (value instanceof Date) {
        return value;
    }
    if (value instanceof ScratchbirdDate) {
        return value.value;
    }
    if (value instanceof ScratchbirdTimestamp) {
        return value.value;
    }
    if (value instanceof ScratchbirdTimestampTZ) {
        return value.value;
    }
    throw new Error(`typed ${label} requires Date-compatible value`);
}
function toBuffer(value, label) {
    if (node_buffer_1.Buffer.isBuffer(value)) {
        return value;
    }
    if (value instanceof Uint8Array) {
        return node_buffer_1.Buffer.from(value);
    }
    throw new Error(`typed ${label} requires Buffer or Uint8Array value`);
}
function toNumericArray(value) {
    if (value instanceof Float32Array || value instanceof Float64Array) {
        return Array.from(value);
    }
    if (Array.isArray(value) && value.every((item) => typeof item === "number" && Number.isFinite(item))) {
        return value;
    }
    throw new Error("typed vector requires numeric array value");
}
function encodeUuid(value) {
    if (node_buffer_1.Buffer.isBuffer(value)) {
        if (value.length !== 16) {
            throw new Error("typed uuid Buffer must be 16 bytes");
        }
        return node_buffer_1.Buffer.from(value);
    }
    if (typeof value === "string" && uuidRegex.test(value)) {
        return node_buffer_1.Buffer.from(value.replace(/-/g, ""), "hex");
    }
    throw new Error("typed uuid requires UUID string or 16-byte Buffer");
}
function encodeJsonPayload(value) {
    if (node_buffer_1.Buffer.isBuffer(value)) {
        return value;
    }
    if (typeof value === "string") {
        return node_buffer_1.Buffer.from(value, "utf8");
    }
    return node_buffer_1.Buffer.from(JSON.stringify(value), "utf8");
}
function encodeInt16(value) {
    const out = node_buffer_1.Buffer.alloc(2);
    out.writeInt16LE(value, 0);
    return out;
}
function encodeInt32(value) {
    const out = node_buffer_1.Buffer.alloc(4);
    out.writeInt32LE(value, 0);
    return out;
}
function encodeInt64(value) {
    const out = node_buffer_1.Buffer.alloc(8);
    out.writeBigInt64LE(value, 0);
    return out;
}
function encodeFloat64(value) {
    const out = node_buffer_1.Buffer.alloc(8);
    out.writeDoubleLE(value, 0);
    return out;
}
function encodeFloat32(value) {
    const out = node_buffer_1.Buffer.alloc(4);
    out.writeFloatLE(value, 0);
    return out;
}
function encodeDate(value) {
    const base = Date.UTC(2000, 0, 1);
    const days = Math.trunc((value.getTime() - base) / 86400000);
    const out = node_buffer_1.Buffer.alloc(4);
    out.writeInt32LE(days, 0);
    return out;
}
function encodeTimeMicros(micros) {
    const out = node_buffer_1.Buffer.alloc(8);
    out.writeBigInt64LE(BigInt(micros), 0);
    return out;
}
function encodeTimestamp(value) {
    const base = Date.UTC(2000, 0, 1);
    const micros = BigInt(value.getTime() - base) * 1000n;
    const out = node_buffer_1.Buffer.alloc(8);
    out.writeBigInt64LE(micros, 0);
    return out;
}
function encodeInterval(interval) {
    const out = node_buffer_1.Buffer.alloc(16);
    out.writeBigInt64LE(BigInt(interval.micros), 0);
    out.writeInt32LE(interval.days ?? 0, 8);
    out.writeInt32LE(interval.months ?? 0, 12);
    return out;
}
function decodeDate(data) {
    if (data.length < 4) {
        return new Date(0);
    }
    const days = data.readInt32LE(0);
    const base = Date.UTC(2000, 0, 1);
    const millis = base + days * 86400000;
    return new Date(millis);
}
function decodeTime(data) {
    if (data.length < 8) {
        return new Date(Date.UTC(2000, 0, 1));
    }
    const micros = data.readBigInt64LE(0);
    const base = Date.UTC(2000, 0, 1);
    const millis = Number(micros / 1000n);
    return new Date(base + millis);
}
function decodeTimestamp(data) {
    if (data.length < 8) {
        return new Date(0);
    }
    const micros = data.readBigInt64LE(0);
    const base = Date.UTC(2000, 0, 1);
    const millis = Number(micros / 1000n);
    return new Date(base + millis);
}
function decodeInterval(data) {
    if (data.length < 16) {
        return { months: 0, days: 0, micros: 0 };
    }
    const micros = Number(data.readBigInt64LE(0));
    const days = data.readInt32LE(8);
    const months = data.readInt32LE(12);
    return { months, days, micros };
}
function moneyToString(cents) {
    const negative = cents < 0n;
    const abs = negative ? -cents : cents;
    const units = abs / 100n;
    const fraction = abs % 100n;
    const value = `${units.toString()}.${fraction.toString().padStart(2, "0")}`;
    return negative ? `-${value}` : value;
}
function bytesToUuid(buf) {
    const hex = buf.toString("hex");
    if (hex.length !== 32) {
        return hex;
    }
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
function encodeRange(range) {
    const oid = resolveRangeOid(range);
    const flags = (range.empty ? RANGE_EMPTY : 0) |
        (range.lowerInclusive ? RANGE_LB_INC : 0) |
        (range.upperInclusive ? RANGE_UB_INC : 0) |
        (range.lowerInfinite ? RANGE_LB_INF : 0) |
        (range.upperInfinite ? RANGE_UB_INF : 0);
    const parts = [node_buffer_1.Buffer.from([flags, 0, 0, 0])];
    if (!range.empty && !range.lowerInfinite) {
        const bound = encodeRangeBound(oid, range.lower);
        parts.push(encodeInt32(bound.length));
        parts.push(bound);
    }
    if (!range.empty && !range.upperInfinite) {
        const bound = encodeRangeBound(oid, range.upper);
        parts.push(encodeInt32(bound.length));
        parts.push(bound);
    }
    return { data: node_buffer_1.Buffer.concat(parts), oid };
}
function resolveRangeOid(range) {
    if (range.rangeOid) {
        return range.rangeOid;
    }
    const sample = range.lower ?? range.upper;
    if (sample === undefined || sample === null) {
        throw new Error("range type cannot be inferred from empty bounds");
    }
    if (sample instanceof ScratchbirdDate) {
        return exports.OID_DATERANGE;
    }
    if (sample instanceof ScratchbirdTimestamp) {
        return exports.OID_TSRANGE;
    }
    if (sample instanceof ScratchbirdTimestampTZ || sample instanceof Date) {
        return exports.OID_TSTZRANGE;
    }
    if (sample instanceof ScratchbirdDecimal) {
        return exports.OID_NUMRANGE;
    }
    if (typeof sample === "bigint") {
        return exports.OID_INT8RANGE;
    }
    if (typeof sample === "number") {
        return sample >= -2147483648 && sample <= 2147483647 ? exports.OID_INT4RANGE : exports.OID_INT8RANGE;
    }
    throw new Error("unsupported range bound type");
}
function encodeRangeBound(rangeOid, value) {
    switch (rangeOid) {
        case exports.OID_INT4RANGE: {
            if (typeof value !== "number")
                throw new Error("int4range requires number bounds");
            return encodeInt32(value);
        }
        case exports.OID_INT8RANGE: {
            if (typeof value === "number") {
                if (!Number.isSafeInteger(value))
                    throw new Error("int8range requires safe integer bounds");
                return encodeInt64(BigInt(value));
            }
            if (typeof value === "bigint") {
                return encodeInt64(value);
            }
            throw new Error("int8range requires bigint bounds");
        }
        case exports.OID_NUMRANGE: {
            if (value instanceof ScratchbirdDecimal) {
                return encodeLengthPrefixed(node_buffer_1.Buffer.from(value.value, "utf8"));
            }
            if (typeof value === "string") {
                return encodeLengthPrefixed(node_buffer_1.Buffer.from(value, "utf8"));
            }
            throw new Error("numrange requires decimal bounds");
        }
        case exports.OID_DATERANGE: {
            if (value instanceof ScratchbirdDate) {
                return encodeDate(value.value);
            }
            if (value instanceof Date) {
                return encodeDate(value);
            }
            throw new Error("daterange requires date bounds");
        }
        case exports.OID_TSRANGE: {
            if (value instanceof ScratchbirdTimestamp) {
                return encodeTimestamp(value.value);
            }
            if (value instanceof Date) {
                return encodeTimestamp(value);
            }
            throw new Error("tsrange requires timestamp bounds");
        }
        case exports.OID_TSTZRANGE: {
            if (value instanceof ScratchbirdTimestampTZ) {
                return encodeTimestamp(value.value);
            }
            if (value instanceof Date) {
                return encodeTimestamp(value);
            }
            throw new Error("tstzrange requires timestamptz bounds");
        }
        default:
            throw new Error("unsupported range type");
    }
}
function decodeRange(rangeOid, data) {
    if (data.length < 4) {
        return new ScratchbirdRange();
    }
    const flags = data[0];
    let offset = 4;
    const range = new ScratchbirdRange({
        empty: (flags & RANGE_EMPTY) !== 0,
        lowerInclusive: (flags & RANGE_LB_INC) !== 0,
        upperInclusive: (flags & RANGE_UB_INC) !== 0,
        lowerInfinite: (flags & RANGE_LB_INF) !== 0,
        upperInfinite: (flags & RANGE_UB_INF) !== 0,
        rangeOid,
    });
    if (range.empty) {
        return range;
    }
    if (!range.lowerInfinite) {
        if (offset + 4 > data.length) {
            return range;
        }
        const length = data.readInt32LE(offset);
        offset += 4;
        if (offset + length > data.length) {
            return range;
        }
        const bound = data.subarray(offset, offset + length);
        offset += length;
        range.lower = decodeRangeBound(rangeOid, bound);
    }
    if (!range.upperInfinite) {
        if (offset + 4 > data.length) {
            return range;
        }
        const length = data.readInt32LE(offset);
        offset += 4;
        if (offset + length > data.length) {
            return range;
        }
        const bound = data.subarray(offset, offset + length);
        range.upper = decodeRangeBound(rangeOid, bound);
    }
    return range;
}
function decodeRangeBound(rangeOid, data) {
    switch (rangeOid) {
        case exports.OID_INT4RANGE:
            return data.length >= 4 ? data.readInt32LE(0) : 0;
        case exports.OID_INT8RANGE:
            return data.length >= 8 ? data.readBigInt64LE(0) : 0n;
        case exports.OID_NUMRANGE:
            return stripLengthPrefix(data).toString("utf8");
        case exports.OID_DATERANGE:
            return decodeDate(data);
        case exports.OID_TSRANGE:
        case exports.OID_TSTZRANGE:
            return decodeTimestamp(data);
        default:
            return null;
    }
}
function isIntervalObject(value) {
    return value && typeof value === "object" && typeof value.micros === "number";
}
function formatArrayLiteral(values) {
    const items = values.map((value) => formatArrayItem(value));
    return `{${items.join(",")}}`;
}
function formatArrayItem(value) {
    if (value === null || value === undefined) {
        return "NULL";
    }
    if (Array.isArray(value)) {
        return formatArrayLiteral(value);
    }
    if (typeof value === "string") {
        return `"${value.replace(/"/g, "\\\"")}"`;
    }
    if (typeof value === "boolean") {
        return value ? "true" : "false";
    }
    return String(value);
}
function parseArrayLiteral(text) {
    let trimmed = text.trim();
    if (trimmed === "{}" || trimmed === "") {
        return [];
    }
    if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
        trimmed = trimmed.slice(1, -1);
    }
    return splitArrayItems(trimmed);
}
function splitArrayItems(text) {
    const items = [];
    let depth = 0;
    let buf = "";
    for (let i = 0; i < text.length; i++) {
        const ch = text[i];
        if (ch === "{") {
            depth++;
            buf += ch;
        }
        else if (ch === "}") {
            depth = Math.max(0, depth - 1);
            buf += ch;
        }
        else if (ch === "," && depth === 0) {
            items.push(parseArrayItem(buf));
            buf = "";
        }
        else {
            buf += ch;
        }
    }
    if (buf.length || text.length) {
        items.push(parseArrayItem(buf));
    }
    return items;
}
function parseArrayItem(raw) {
    const token = raw.trim();
    if (token === "") {
        return "";
    }
    if (token.toUpperCase() === "NULL") {
        return null;
    }
    if (token.startsWith("{") && token.endsWith("}")) {
        return parseArrayLiteral(token);
    }
    if (token.startsWith("[") && token.endsWith("]")) {
        return parseVectorLiteral(token);
    }
    if (token === "true" || token === "false") {
        return token === "true";
    }
    const num = Number(token);
    if (!Number.isNaN(num)) {
        return num;
    }
    return token;
}
function parseVectorLiteral(text) {
    let trimmed = text.trim();
    if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
        trimmed = trimmed.slice(1, -1);
    }
    if (!trimmed) {
        return [];
    }
    return trimmed
        .split(",")
        .map((part) => Number(part.trim()))
        .filter((val) => !Number.isNaN(val));
}
function formatVectorLiteral(values) {
    const parts = values.map((value) => Number.isFinite(value) ? String(value) : "0");
    return `[${parts.join(",")}]`;
}
function decodeArrayLiteral(text) {
    return parseArrayLiteral(text);
}
