// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.io.ByteArrayOutputStream;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.sql.Array;
import java.sql.Blob;
import java.sql.Clob;
import java.sql.Ref;
import java.sql.RowId;
import java.sql.SQLException;
import java.sql.SQLXML;
import java.sql.Struct;
import java.sql.Time;
import java.sql.Timestamp;
import java.time.Duration;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.OffsetTime;
import java.time.Period;
import java.time.ZonedDateTime;
import java.time.ZoneOffset;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.UUID;

public final class SBTypeCodec {
    public static final int FORMAT_TEXT = 0;
    public static final int FORMAT_BINARY = 1;

    public static final int OID_BOOL = 16;
    public static final int OID_BYTEA = 17;
    public static final int OID_CHAR = 18;
    public static final int OID_INT8 = 20;
    public static final int OID_INT2 = 21;
    public static final int OID_INT4 = 23;
    public static final int OID_TEXT = 25;
    public static final int OID_JSON = 114;
    public static final int OID_XML = 142;
    public static final int OID_POINT = 600;
    public static final int OID_LSEG = 601;
    public static final int OID_PATH = 602;
    public static final int OID_BOX = 603;
    public static final int OID_POLYGON = 604;
    public static final int OID_LINE = 628;
    public static final int OID_FLOAT4 = 700;
    public static final int OID_FLOAT8 = 701;
    public static final int OID_CIRCLE = 718;
    public static final int OID_MONEY = 790;
    public static final int OID_MACADDR = 829;
    public static final int OID_CIDR = 650;
    public static final int OID_INET = 869;
    public static final int OID_MACADDR8 = 774;
    public static final int OID_BPCHAR = 1042;
    public static final int OID_VARCHAR = 1043;
    public static final int OID_DATE = 1082;
    public static final int OID_TIME = 1083;
    public static final int OID_TIMESTAMP = 1114;
    public static final int OID_TIMESTAMPTZ = 1184;
    public static final int OID_INTERVAL = 1186;
    public static final int OID_TIMETZ = 1266;
    public static final int OID_NUMERIC = 1700;
    public static final int OID_UUID = 2950;
    public static final int OID_JSONB = 3802;
    public static final int OID_RECORD = 2249;
    public static final int OID_TSVECTOR = 3614;
    public static final int OID_TSQUERY = 3615;
    public static final int OID_SB_VECTOR = 16386;

    public static final int OID_INT4RANGE = 3904;
    public static final int OID_NUMRANGE = 3906;
    public static final int OID_TSRANGE = 3908;
    public static final int OID_TSTZRANGE = 3910;
    public static final int OID_DATERANGE = 3912;
    public static final int OID_INT8RANGE = 3926;

    public static final int OID_BOOL_ARRAY = 1000;
    public static final int OID_BYTEA_ARRAY = 1001;
    public static final int OID_INT2_ARRAY = 1005;
    public static final int OID_INT4_ARRAY = 1007;
    public static final int OID_INT8_ARRAY = 1016;
    public static final int OID_FLOAT4_ARRAY = 1021;
    public static final int OID_FLOAT8_ARRAY = 1022;
    public static final int OID_TEXT_ARRAY = 1009;
    public static final int OID_VARCHAR_ARRAY = 1015;
    public static final int OID_DATE_ARRAY = 1182;
    public static final int OID_TIME_ARRAY = 1183;
    public static final int OID_TIMESTAMP_ARRAY = 1115;
    public static final int OID_TIMESTAMPTZ_ARRAY = 1185;
    public static final int OID_TIMETZ_ARRAY = 1270;
    public static final int OID_NUMERIC_ARRAY = 1231;
    public static final int OID_UUID_ARRAY = 2951;

    private static final int RANGE_EMPTY = 0x01;
    private static final int RANGE_LB_INC = 0x02;
    private static final int RANGE_UB_INC = 0x04;
    private static final int RANGE_LB_INF = 0x08;
    private static final int RANGE_UB_INF = 0x10;

    private static final Instant EPOCH_2000 = LocalDateTime.of(2000, 1, 1, 0, 0)
        .toInstant(ZoneOffset.UTC);

    private SBTypeCodec() {
    }

    public static final class ParamEncoding {
        private final int format;
        private final int oid;
        private final byte[] data;
        private final boolean isNull;

        ParamEncoding(int format, int oid, byte[] data, boolean isNull) {
            this.format = format;
            this.oid = oid;
            this.data = data;
            this.isNull = isNull;
        }

        public int getFormat() { return format; }
        public int getOid() { return oid; }
        public byte[] getData() { return data; }
        public boolean isNull() { return isNull; }
    }

    public static ParamEncoding encodeParam(Object value, Integer sqlType) throws SQLException {
        if (value == null) {
            return new ParamEncoding(FORMAT_BINARY, 0, null, true);
        }
        if (value instanceof SBRawValue) {
            SBRawValue raw = (SBRawValue) value;
            return new ParamEncoding(FORMAT_BINARY, raw.getOid(), raw.getData(), raw.getData() == null);
        }
        if (value instanceof SBJsonb) {
            SBJsonb jsonb = (SBJsonb) value;
            byte[] raw = jsonb.getRaw();
            if ((raw == null || raw.length == 0) && jsonb.getValue() != null) {
                raw = jsonb.getValue().getBytes(StandardCharsets.UTF_8);
            }
            if (raw == null || raw.length == 0) {
                throw new SQLException("JSONB requires raw payload", "22023");
            }
            return new ParamEncoding(FORMAT_BINARY, OID_JSONB, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof SBGeometry) {
            SBGeometry geom = (SBGeometry) value;
            byte[] wkb = geom.getWkb();
            if (wkb == null || wkb.length == 0) {
                throw new SQLException("Geometry requires WKB payload", "22023");
            }
            return new ParamEncoding(FORMAT_BINARY, OID_POINT, encodeLengthPrefixed(wkb), false);
        }
        if (value instanceof SBRange) {
            RangeEncoding encoded = encodeRange((SBRange<?>) value);
            return new ParamEncoding(FORMAT_BINARY, encoded.oid, encoded.data, false);
        }
        if (value instanceof Ref) {
            Object refValue = ((Ref) value).getObject();
            if (refValue == null) {
                return new ParamEncoding(FORMAT_BINARY, OID_TEXT, null, true);
            }
            return encodeParam(refValue, sqlType != null ? sqlType : java.sql.Types.REF);
        }
        if (value instanceof RowId) {
            byte[] raw = ((RowId) value).getBytes();
            if (raw == null) {
                return new ParamEncoding(FORMAT_BINARY, OID_BYTEA, null, true);
            }
            return new ParamEncoding(FORMAT_BINARY, OID_BYTEA, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof Blob) {
            Blob blob = (Blob) value;
            byte[] raw = blob.getBytes(1, (int) blob.length());
            return new ParamEncoding(FORMAT_BINARY, OID_BYTEA, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof Clob) {
            Clob clob = (Clob) value;
            String text = clob.getSubString(1, (int) clob.length());
            return new ParamEncoding(
                FORMAT_BINARY,
                OID_TEXT,
                encodeLengthPrefixed(text.getBytes(StandardCharsets.UTF_8)),
                false);
        }
        if (value instanceof SQLXML) {
            String text = ((SQLXML) value).getString();
            if (text == null) {
                return new ParamEncoding(FORMAT_BINARY, OID_XML, null, true);
            }
            return new ParamEncoding(
                FORMAT_BINARY,
                OID_XML,
                encodeLengthPrefixed(text.getBytes(StandardCharsets.UTF_8)),
                false);
        }
        if (value instanceof Struct) {
            return encodeComposite((Struct) value);
        }
        if (value instanceof Boolean) {
            return new ParamEncoding(FORMAT_BINARY, OID_BOOL,
                new byte[]{(byte) ((Boolean) value ? 1 : 0)}, false);
        }
        if (value instanceof Byte || value instanceof Short) {
            short v = ((Number) value).shortValue();
            return new ParamEncoding(FORMAT_BINARY, OID_INT2, toBytesLE(v), false);
        }
        if (value instanceof Integer) {
            int v = (Integer) value;
            return new ParamEncoding(FORMAT_BINARY, OID_INT4, toBytesLE(v), false);
        }
        if (value instanceof Long) {
            long v = (Long) value;
            return new ParamEncoding(FORMAT_BINARY, OID_INT8, toBytesLE(v), false);
        }
        if (value instanceof Float) {
            return new ParamEncoding(FORMAT_BINARY, OID_FLOAT4, toBytesLE(((Float) value).floatValue()), false);
        }
        if (value instanceof Double) {
            return new ParamEncoding(FORMAT_BINARY, OID_FLOAT8, toBytesLE(((Double) value).doubleValue()), false);
        }
        if (value instanceof BigDecimal) {
            byte[] raw = ((BigDecimal) value).toPlainString().getBytes(StandardCharsets.UTF_8);
            return new ParamEncoding(FORMAT_BINARY, OID_NUMERIC, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof byte[]) {
            return new ParamEncoding(FORMAT_BINARY, OID_BYTEA, encodeLengthPrefixed((byte[]) value), false);
        }
        if (value instanceof int[]) {
            return encodeArray(boxArray((int[]) value));
        }
        if (value instanceof long[]) {
            return encodeArray(boxArray((long[]) value));
        }
        if (value instanceof short[]) {
            return encodeArray(boxArray((short[]) value));
        }
        if (value instanceof boolean[]) {
            return encodeArray(boxArray((boolean[]) value));
        }
        if (value instanceof UUID) {
            return new ParamEncoding(FORMAT_BINARY, OID_UUID, uuidToBytes((UUID) value), false);
        }
        if (value instanceof java.sql.Date) {
            return new ParamEncoding(FORMAT_BINARY, OID_DATE, encodeDate(((java.sql.Date) value).toLocalDate()), false);
        }
        if (value instanceof Time) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIME, encodeTime(((Time) value).toLocalTime()), false);
        }
        if (value instanceof Timestamp) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMP,
                encodeTimestamp(((Timestamp) value).toInstant()), false);
        }
        if (value instanceof LocalDate) {
            return new ParamEncoding(FORMAT_BINARY, OID_DATE, encodeDate((LocalDate) value), false);
        }
        if (value instanceof LocalTime) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIME, encodeTime((LocalTime) value), false);
        }
        if (value instanceof LocalDateTime) {
            Instant instant = ((LocalDateTime) value).toInstant(ZoneOffset.UTC);
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMP, encodeTimestamp(instant), false);
        }
        if (value instanceof OffsetDateTime) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMPTZ,
                encodeTimestamp(((OffsetDateTime) value).toInstant()), false);
        }
        if (value instanceof ZonedDateTime) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMPTZ,
                encodeTimestamp(((ZonedDateTime) value).toInstant()), false);
        }
        if (value instanceof OffsetTime) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMETZ,
                encodeTimeWithTimezone((OffsetTime) value), false);
        }
        if (value instanceof Instant) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMPTZ, encodeTimestamp((Instant) value), false);
        }
        if (value instanceof java.util.Date) {
            return new ParamEncoding(FORMAT_BINARY, OID_TIMESTAMP,
                encodeTimestamp(((java.util.Date) value).toInstant()), false);
        }
        if (value instanceof Duration) {
            return new ParamEncoding(FORMAT_BINARY, OID_INTERVAL, encodeInterval((Duration) value), false);
        }
        if (value instanceof Period) {
            return new ParamEncoding(FORMAT_BINARY, OID_INTERVAL, encodeInterval((Period) value), false);
        }
        if (value instanceof Array) {
            Object[] arrayValue = (Object[]) ((Array) value).getArray();
            return encodeArray(arrayValue);
        }
        if (value instanceof float[]) {
            return new ParamEncoding(FORMAT_BINARY, OID_SB_VECTOR,
                encodeLengthPrefixed(formatVectorLiteral((float[]) value).getBytes(StandardCharsets.UTF_8)), false);
        }
        if (value instanceof double[]) {
            return new ParamEncoding(FORMAT_BINARY, OID_SB_VECTOR,
                encodeLengthPrefixed(formatVectorLiteral((double[]) value).getBytes(StandardCharsets.UTF_8)), false);
        }
        if (value instanceof Collection) {
            Object[] arrayValue = ((Collection<?>) value).toArray();
            return encodeArray(arrayValue);
        }
        if (value.getClass().isArray()) {
            if (value instanceof Object[]) {
                return encodeArray((Object[]) value);
            }
        }
        if (value instanceof String) {
            int oid = mapSqlTypeToOid(sqlType);
            if (oid != OID_BPCHAR && oid != OID_VARCHAR && oid != OID_TEXT && oid != OID_XML) {
                oid = OID_TEXT;
            }
            byte[] raw = ((String) value).getBytes(StandardCharsets.UTF_8);
            return new ParamEncoding(FORMAT_BINARY, oid, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof CharSequence) {
            byte[] raw = value.toString().getBytes(StandardCharsets.UTF_8);
            return new ParamEncoding(FORMAT_BINARY, OID_TEXT, encodeLengthPrefixed(raw), false);
        }
        if (value instanceof Enum<?>) {
            byte[] raw = ((Enum<?>) value).name().getBytes(StandardCharsets.UTF_8);
            return new ParamEncoding(FORMAT_BINARY, OID_TEXT, encodeLengthPrefixed(raw), false);
        }

        int mappedOid = mapSqlTypeToOid(sqlType);
        if (mappedOid != 0 && value instanceof Number) {
            ParamEncoding numeric = encodeNumericWithOid(mappedOid, (Number) value);
            if (numeric != null) {
                return numeric;
            }
        }
        byte[] fallback = value.toString().getBytes(StandardCharsets.UTF_8);
        int fallbackOid = mappedOid != 0 ? mappedOid : OID_TEXT;
        return new ParamEncoding(FORMAT_BINARY, fallbackOid, encodeLengthPrefixed(fallback), false);
    }

    public static Object decodeValue(int typeOid, byte[] data, int format) throws SQLException {
        if (data == null) {
            return null;
        }
        if (typeOid == 0) {
            if (format == FORMAT_TEXT) {
                return parseUnknownText(new String(data, StandardCharsets.UTF_8));
            }
            return decodeUnknownBinary(data);
        }
        if (format == FORMAT_TEXT) {
            return parseTextValue(new String(data, StandardCharsets.UTF_8), typeOid);
        }
        return parseBinaryValue(typeOid, data);
    }

    private static Object parseBinaryValue(int oid, byte[] data) throws SQLException {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        switch (oid) {
            case OID_BOOL:
                return data[0] != 0;
            case OID_INT2:
                return buf.getShort();
            case OID_INT4:
                return buf.getInt();
            case OID_INT8:
                return buf.getLong();
            case OID_FLOAT4:
                return buf.getFloat();
            case OID_FLOAT8:
                return buf.getDouble();
            case OID_NUMERIC:
                return new BigDecimal(new String(stripLengthPrefixed(data), StandardCharsets.UTF_8));
            case OID_MONEY:
                long cents = buf.getLong();
                return new BigDecimal(cents).movePointLeft(2);
            case OID_TEXT:
            case OID_VARCHAR:
            case OID_CHAR:
            case OID_BPCHAR:
            case OID_JSON:
            case OID_XML:
            case OID_TSVECTOR:
            case OID_TSQUERY:
                return new String(stripLengthPrefixed(data), StandardCharsets.UTF_8);
            case OID_JSONB:
                return new SBJsonb(stripLengthPrefixed(data));
            case OID_BYTEA:
                byte[] rawBytea = stripLengthPrefixed(data);
                if (rawBytea.length == 0) {
                    return rawBytea;
                }
                String byteaText = new String(rawBytea, StandardCharsets.UTF_8);
                if (byteaText.startsWith("\\x")
                    || byteaText.startsWith("0x")
                    || byteaText.indexOf('\\') >= 0
                    || (((byteaText.length() & 1) == 0)
                        && byteaText.matches("(?i)[0-9a-f]+"))) {
                    return decodeBytea(byteaText);
                }
                return rawBytea;
            case OID_DATE:
                return java.sql.Date.valueOf(decodeDate(data));
            case OID_TIME:
                return java.sql.Time.valueOf(decodeTime(data));
            case OID_TIMETZ:
                return decodeTimeWithTimezone(data);
            case OID_TIMESTAMP:
                return Timestamp.from(decodeTimestamp(data));
            case OID_TIMESTAMPTZ:
                return OffsetDateTime.ofInstant(decodeTimestamp(data), ZoneOffset.UTC);
            case OID_INTERVAL:
                return decodeInterval(data);
            case OID_UUID:
                return bytesToUuid(data);
            case OID_INET:
            case OID_CIDR:
            case OID_MACADDR:
            case OID_MACADDR8:
                return new String(stripLengthPrefixed(data), StandardCharsets.UTF_8);
            case OID_INT4RANGE:
            case OID_INT8RANGE:
            case OID_NUMRANGE:
            case OID_TSRANGE:
            case OID_TSTZRANGE:
            case OID_DATERANGE:
                return decodeRange(oid, data);
            case OID_SB_VECTOR:
                return parseVectorLiteral(new String(stripLengthPrefixed(data), StandardCharsets.UTF_8));
            case OID_POINT:
            case OID_LSEG:
            case OID_PATH:
            case OID_BOX:
            case OID_POLYGON:
            case OID_LINE:
            case OID_CIRCLE:
                return new SBGeometry(stripLengthPrefixed(data));
            case OID_RECORD:
                return decodeComposite(data);
            default:
                if (isArrayOid(oid)) {
                    return decodeArray(oid, data);
                }
                return data;
        }
    }

    private static Object decodeUnknownBinary(byte[] data) {
        byte[] trimmed = stripTrailingNulls(data);
        if (trimmed.length > 0 && looksLikeText(trimmed)) {
            return parseUnknownText(new String(trimmed, StandardCharsets.UTF_8));
        }
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        switch (data.length) {
            case 1:
                return (int) data[0];
            case 2:
                return buf.getShort();
            case 4:
                return buf.getInt();
            case 8:
                return buf.getLong();
            case 16:
                return bytesToUuid(data);
            default:
                return data;
        }
    }

    private static Object parseUnknownText(String text) {
        String trimmed = text.trim();
        if (trimmed.isEmpty()) {
            return text;
        }
        String lowered = trimmed.toLowerCase();
        if (lowered.equals("true")) {
            return Boolean.TRUE;
        }
        if (lowered.equals("false")) {
            return Boolean.FALSE;
        }
        if (trimmed.matches("[+-]?\\d+")) {
            try {
                long value = Long.parseLong(trimmed);
                if (value >= Integer.MIN_VALUE && value <= Integer.MAX_VALUE) {
                    return (int) value;
                }
                return value;
            } catch (NumberFormatException ex) {
                return text;
            }
        }
        if (trimmed.matches("[+-]?(?:\\d+\\.?\\d*|\\d*\\.?\\d+)(?:[eE][+-]?\\d+)?")) {
            try {
                return Double.parseDouble(trimmed);
            } catch (NumberFormatException ex) {
                return text;
            }
        }
        return text;
    }

    private static byte[] stripTrailingNulls(byte[] data) {
        int end = data.length;
        while (end > 0 && data[end - 1] == 0) {
            end -= 1;
        }
        if (end == data.length) {
            return data;
        }
        return Arrays.copyOf(data, end);
    }

    private static boolean looksLikeText(byte[] data) {
        for (byte b : data) {
            if (b == 0x09 || b == 0x0a || b == 0x0d) {
                continue;
            }
            if ((b & 0xFF) < 0x20 || (b & 0xFF) > 0x7e) {
                return false;
            }
        }
        return true;
    }

    private static Object parseTextValue(String text, int oid) throws SQLException {
        switch (oid) {
            case OID_BOOL:
                return "t".equalsIgnoreCase(text) || "true".equalsIgnoreCase(text);
            case OID_INT2:
                return Short.parseShort(text);
            case OID_INT4:
                return Integer.parseInt(text);
            case OID_INT8:
                return parseIntegerLikeText(text);
            case OID_FLOAT4:
                return Float.parseFloat(text);
            case OID_FLOAT8:
                return Double.parseDouble(text);
            case OID_NUMERIC:
                return new BigDecimal(text);
            case OID_BYTEA:
                return decodeBytea(text);
            case OID_DATE:
                return java.sql.Date.valueOf(text);
            case OID_TIME:
                return java.sql.Time.valueOf(text);
            case OID_TIMESTAMP:
                return java.sql.Timestamp.valueOf(text.replace('T', ' '));
            case OID_TIMESTAMPTZ:
                return parseOffsetDateTimeText(text);
            case OID_TIMETZ:
                return parseOffsetTimeText(text);
            case OID_UUID:
                return UUID.fromString(text);
            default:
                return text;
        }
    }

    private static Object parseIntegerLikeText(String text) {
        String trimmed = text.trim();
        if (!trimmed.matches("[+-]?\\d+")) {
            try {
                return new BigDecimal(trimmed);
            } catch (NumberFormatException ex) {
                return text;
            }
        }
        try {
            return Long.parseLong(trimmed);
        } catch (NumberFormatException ex) {
            return new BigInteger(trimmed);
        }
    }

    private static ParamEncoding encodeArray(Object[] values) {
        Object[] safe = values != null ? values : new Object[0];
        int arrayOid = inferArrayOid(safe);
        String literal = formatArrayLiteral(safe);
        byte[] raw = literal.getBytes(StandardCharsets.UTF_8);
        return new ParamEncoding(FORMAT_BINARY, arrayOid, encodeLengthPrefixed(raw), false);
    }

    private static Object decodeArray(int oid, byte[] data) {
        String baseType = arrayBaseType(oid);
        String literal = new String(stripLengthPrefixed(data), StandardCharsets.UTF_8);
        Object[] elements = parseArrayLiteral(literal);
        Object[] converted = convertArrayElements(baseType, elements);
        return new SBArray(baseType, converted);
    }

    private static String arrayBaseType(int oid) {
        switch (oid) {
            case OID_BOOL_ARRAY: return "boolean";
            case OID_BYTEA_ARRAY: return "bytea";
            case OID_INT2_ARRAY: return "smallint";
            case OID_INT4_ARRAY: return "integer";
            case OID_INT8_ARRAY: return "bigint";
            case OID_FLOAT4_ARRAY: return "real";
            case OID_FLOAT8_ARRAY: return "double precision";
            case OID_TEXT_ARRAY: return "text";
            case OID_VARCHAR_ARRAY: return "varchar";
            case OID_DATE_ARRAY: return "date";
            case OID_TIME_ARRAY: return "time";
            case OID_TIMETZ_ARRAY: return "timetz";
            case OID_TIMESTAMP_ARRAY: return "timestamp";
            case OID_TIMESTAMPTZ_ARRAY: return "timestamptz";
            case OID_NUMERIC_ARRAY: return "numeric";
            case OID_UUID_ARRAY: return "uuid";
            default: return "text";
        }
    }

    private static boolean isArrayOid(int oid) {
        return oid == OID_BOOL_ARRAY || oid == OID_BYTEA_ARRAY || oid == OID_INT2_ARRAY ||
            oid == OID_INT4_ARRAY || oid == OID_INT8_ARRAY || oid == OID_FLOAT4_ARRAY ||
            oid == OID_FLOAT8_ARRAY || oid == OID_TEXT_ARRAY || oid == OID_VARCHAR_ARRAY ||
            oid == OID_DATE_ARRAY || oid == OID_TIME_ARRAY || oid == OID_TIMETZ_ARRAY ||
            oid == OID_TIMESTAMP_ARRAY || oid == OID_TIMESTAMPTZ_ARRAY || oid == OID_NUMERIC_ARRAY ||
            oid == OID_UUID_ARRAY;
    }

    private static byte[] encodeDate(LocalDate value) {
        long days = value.toEpochDay() - LocalDate.of(2000, 1, 1).toEpochDay();
        return toBytesLE((int) days);
    }

    private static byte[] encodeTime(LocalTime value) {
        long micros = value.toNanoOfDay() / 1000;
        return toBytesLE(micros);
    }

    private static byte[] encodeTimeWithTimezone(OffsetTime value) {
        ByteBuffer buf = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        long micros = value.toLocalTime().toNanoOfDay() / 1000;
        // PostgreSQL stores timetz zone as seconds west of UTC.
        int zoneSecondsWest = -value.getOffset().getTotalSeconds();
        buf.putLong(micros);
        buf.putInt(zoneSecondsWest);
        return buf.array();
    }

    private static byte[] encodeTimestamp(Instant instant) {
        long micros = Duration.between(EPOCH_2000, instant).toNanos() / 1000;
        return toBytesLE(micros);
    }

    private static byte[] encodeInterval(Duration duration) {
        long micros = duration.toNanos() / 1000;
        ByteBuffer buf = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(0);
        buf.putInt(0);
        buf.putLong(micros);
        return buf.array();
    }

    private static byte[] encodeInterval(Period period) {
        int months = period.getYears() * 12 + period.getMonths();
        int days = period.getDays();
        ByteBuffer buf = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(months);
        buf.putInt(days);
        buf.putLong(0);
        return buf.array();
    }

    private static Object decodeInterval(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int months = buf.getInt();
        int days = buf.getInt();
        long micros = buf.getLong();
        if (months != 0 || days != 0) {
            int years = months / 12;
            int remMonths = months % 12;
            return Period.of(years, remMonths, days);
        }
        return Duration.ofNanos(micros * 1000);
    }

    private static LocalDate decodeDate(byte[] data) {
        int days = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt();
        return LocalDate.of(2000, 1, 1).plusDays(days);
    }

    private static LocalTime decodeTime(byte[] data) {
        long micros = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getLong();
        long nanos = micros * 1000;
        return LocalTime.ofNanoOfDay(nanos);
    }

    private static OffsetTime decodeTimeWithTimezone(byte[] data) throws SQLException {
        if (data == null || data.length < 8) {
            throw new SQLException("Invalid timetz binary payload", "22023");
        }

        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        long micros = buf.getLong();
        long dayMicros = 24L * 60L * 60L * 1_000_000L;
        micros = Math.floorMod(micros, dayMicros);
        LocalTime localTime = LocalTime.ofNanoOfDay(micros * 1000L);

        // Backward compatibility with older 8-byte timetz payloads (time only).
        if (buf.remaining() < 4) {
            return OffsetTime.of(localTime, ZoneOffset.UTC);
        }

        int zoneSecondsWest = buf.getInt();
        try {
            ZoneOffset offset = ZoneOffset.ofTotalSeconds(-zoneSecondsWest);
            return OffsetTime.of(localTime, offset);
        } catch (RuntimeException ex) {
            throw new SQLException("Invalid timetz zone displacement", "22023", ex);
        }
    }

    private static Instant decodeTimestamp(byte[] data) {
        long micros = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getLong();
        return EPOCH_2000.plusNanos(micros * 1000);
    }

    private static RangeEncoding encodeRange(SBRange<?> range) throws SQLException {
        int oid = range.getRangeOid() != null ? range.getRangeOid() : inferRangeOid(range);
        int flags = 0;
        if (range.isEmpty()) {
            flags |= RANGE_EMPTY;
        }
        if (range.isLowerInclusive()) {
            flags |= RANGE_LB_INC;
        }
        if (range.isUpperInclusive()) {
            flags |= RANGE_UB_INC;
        }
        if (range.isLowerInfinite()) {
            flags |= RANGE_LB_INF;
        }
        if (range.isUpperInfinite()) {
            flags |= RANGE_UB_INF;
        }
        ByteBuffer buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        buf.put((byte) flags).put((byte) 0).put((byte) 0).put((byte) 0);
        byte[] header = buf.array();

        ByteBuffer out = ByteBuffer.allocate(1024).order(ByteOrder.LITTLE_ENDIAN);
        out.put(header);
        if (!range.isEmpty() && !range.isLowerInfinite()) {
            byte[] bound = encodeRangeBound(oid, range.getLower());
            out = ensureCapacity(out, 4 + bound.length);
            out.putInt(bound.length);
            out.put(bound);
        }
        if (!range.isEmpty() && !range.isUpperInfinite()) {
            byte[] bound = encodeRangeBound(oid, range.getUpper());
            out = ensureCapacity(out, 4 + bound.length);
            out.putInt(bound.length);
            out.put(bound);
        }
        out.flip();
        byte[] data = new byte[out.remaining()];
        out.get(data);
        return new RangeEncoding(oid, data);
    }

    private static int inferRangeOid(SBRange<?> range) throws SQLException {
        Object sample = range.getLower() != null ? range.getLower() : range.getUpper();
        if (sample instanceof BigDecimal) {
            return OID_NUMRANGE;
        }
        if (sample instanceof LocalDate || sample instanceof java.sql.Date) {
            return OID_DATERANGE;
        }
        if (sample instanceof LocalDateTime || sample instanceof Timestamp) {
            return OID_TSRANGE;
        }
        if (sample instanceof OffsetDateTime || sample instanceof Instant) {
            return OID_TSTZRANGE;
        }
        if (sample instanceof Integer || sample instanceof Short || sample instanceof Byte) {
            return OID_INT4RANGE;
        }
        if (sample instanceof Long) {
            return OID_INT8RANGE;
        }
        throw new SQLException("Range type cannot be inferred", "22023");
    }

    private static byte[] encodeRangeBound(int oid, Object value) throws SQLException {
        if (value == null) {
            return new byte[0];
        }
        switch (oid) {
            case OID_INT4RANGE:
                return toBytesLE(((Number) value).intValue());
            case OID_INT8RANGE:
                return toBytesLE(((Number) value).longValue());
            case OID_NUMRANGE:
                return encodeLengthPrefixed(value.toString().getBytes(StandardCharsets.UTF_8));
            case OID_DATERANGE:
                if (value instanceof java.sql.Date) {
                    return encodeDate(((java.sql.Date) value).toLocalDate());
                }
                if (value instanceof java.util.Date) {
                    return encodeDate(((java.util.Date) value).toInstant().atZone(ZoneOffset.UTC).toLocalDate());
                }
                if (value instanceof CharSequence) {
                    return encodeDate(LocalDate.parse(value.toString().trim()));
                }
                return encodeDate((LocalDate) value);
            case OID_TSRANGE:
                if (value instanceof Timestamp) {
                    return encodeTimestamp(((Timestamp) value).toInstant());
                }
                if (value instanceof LocalDateTime) {
                    return encodeTimestamp(((LocalDateTime) value).toInstant(ZoneOffset.UTC));
                }
                if (value instanceof OffsetDateTime) {
                    return encodeTimestamp(((OffsetDateTime) value).toInstant());
                }
                if (value instanceof java.util.Date) {
                    return encodeTimestamp(((java.util.Date) value).toInstant());
                }
                if (value instanceof CharSequence) {
                    return encodeTimestamp(parseRangeTimestamp(value.toString()));
                }
                return encodeTimestamp(((Instant) value));
            case OID_TSTZRANGE:
                if (value instanceof OffsetDateTime) {
                    return encodeTimestamp(((OffsetDateTime) value).toInstant());
                }
                if (value instanceof Instant) {
                    return encodeTimestamp((Instant) value);
                }
                if (value instanceof LocalDateTime) {
                    return encodeTimestamp(((LocalDateTime) value).toInstant(ZoneOffset.UTC));
                }
                if (value instanceof java.util.Date) {
                    return encodeTimestamp(((java.util.Date) value).toInstant());
                }
                if (value instanceof CharSequence) {
                    return encodeTimestamp(parseRangeTimestamp(value.toString()));
                }
                return encodeTimestamp(((Timestamp) value).toInstant());
            default:
                throw new SQLException("Unsupported range bound type", "22023");
        }
    }

    private static SBRange<Object> decodeRange(int oid, byte[] data) {
        if (data.length < 4) {
            return new SBRange<>(null, null, false, false, false, false, false, oid);
        }
        int flags = data[0] & 0xff;
        boolean empty = (flags & RANGE_EMPTY) != 0;
        boolean lowerInc = (flags & RANGE_LB_INC) != 0;
        boolean upperInc = (flags & RANGE_UB_INC) != 0;
        boolean lowerInf = (flags & RANGE_LB_INF) != 0;
        boolean upperInf = (flags & RANGE_UB_INF) != 0;

        int offset = 4;
        Object lower = null;
        Object upper = null;
        if (!empty && !lowerInf && offset + 4 <= data.length) {
            int len = ByteBuffer.wrap(data, offset, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
            offset += 4;
            if (offset + len <= data.length) {
                lower = decodeRangeBound(oid, slice(data, offset, len));
                offset += len;
            }
        }
        if (!empty && !upperInf && offset + 4 <= data.length) {
            int len = ByteBuffer.wrap(data, offset, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
            offset += 4;
            if (offset + len <= data.length) {
                upper = decodeRangeBound(oid, slice(data, offset, len));
                offset += len;
            }
        }
        return new SBRange<>(lower, upper, lowerInc, upperInc, lowerInf, upperInf, empty, oid);
    }

    private static Object decodeRangeBound(int oid, byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        switch (oid) {
            case OID_INT4RANGE:
                return buf.getInt();
            case OID_INT8RANGE:
                return buf.getLong();
            case OID_NUMRANGE:
                return new BigDecimal(new String(stripLengthPrefixed(data), StandardCharsets.UTF_8));
            case OID_DATERANGE:
                return decodeDate(data);
            case OID_TSRANGE:
                return decodeTimestamp(data).atZone(ZoneOffset.UTC).toLocalDateTime();
            case OID_TSTZRANGE:
                return OffsetDateTime.ofInstant(decodeTimestamp(data), ZoneOffset.UTC);
            default:
                return null;
        }
    }

    private static byte[] toBytesLE(short value) {
        ByteBuffer buf = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN);
        buf.putShort(value);
        return buf.array();
    }

    private static byte[] toBytesLE(int value) {
        ByteBuffer buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(value);
        return buf.array();
    }

    private static byte[] toBytesLE(long value) {
        ByteBuffer buf = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        buf.putLong(value);
        return buf.array();
    }

    private static byte[] toBytesLE(float value) {
        ByteBuffer buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN);
        buf.putFloat(value);
        return buf.array();
    }

    private static byte[] toBytesLE(double value) {
        ByteBuffer buf = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        buf.putDouble(value);
        return buf.array();
    }

    private static byte[] encodeLengthPrefixed(byte[] data) {
        ByteBuffer buf = ByteBuffer.allocate(4 + data.length).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(data.length);
        buf.put(data);
        return buf.array();
    }

    private static byte[] stripLengthPrefixed(byte[] data) {
        if (data.length < 4) {
            return data;
        }
        int len = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt();
        if (len <= data.length - 4) {
            byte[] out = new byte[len];
            System.arraycopy(data, 4, out, 0, len);
            return out;
        }
        return data;
    }

    private static String formatVectorLiteral(float[] values) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(Float.toString(values[i]));
        }
        sb.append(']');
        return sb.toString();
    }

    private static String formatVectorLiteral(double[] values) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(Double.toString(values[i]));
        }
        sb.append(']');
        return sb.toString();
    }

    private static float[] parseVectorLiteral(String text) {
        String trimmed = text.trim();
        if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
            trimmed = trimmed.substring(1, trimmed.length() - 1);
        }
        if (trimmed.isEmpty()) {
            return new float[0];
        }
        String[] parts = trimmed.split(",");
        float[] out = new float[parts.length];
        for (int i = 0; i < parts.length; i++) {
            out[i] = Float.parseFloat(parts[i].trim());
        }
        return out;
    }

    private static String formatArrayLiteral(Object[] values) {
        StringBuilder sb = new StringBuilder("{");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(formatArrayItem(values[i]));
        }
        sb.append('}');
        return sb.toString();
    }

    private static String formatArrayItem(Object value) {
        if (value == null) {
            return "NULL";
        }
        if (value instanceof Object[]) {
            return formatArrayLiteral((Object[]) value);
        }
        if (value instanceof Collection) {
            return formatArrayLiteral(((Collection<?>) value).toArray());
        }
        if (value instanceof String) {
            return '"' + ((String) value).replace("\"", "\\\"") + '"';
        }
        if (value instanceof Boolean) {
            return ((Boolean) value) ? "true" : "false";
        }
        return value.toString();
    }

    private static Object[] parseArrayLiteral(String literal) {
        if (literal == null) {
            return new Object[0];
        }
        String trimmed = literal.trim();
        if (trimmed.equals("{}")) {
            return new Object[0];
        }
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
            return new Object[]{trimmed};
        }
        ParseResult result = parseArray(trimmed, 0);
        return result.items.toArray(new Object[0]);
    }

    private static class ParseResult {
        final List<Object> items;
        final int nextIdx;

        ParseResult(List<Object> items, int nextIdx) {
            this.items = items;
            this.nextIdx = nextIdx;
        }
    }

    private static ParseResult parseArray(String text, int start) {
        List<Object> items = new ArrayList<>();
        StringBuilder token = new StringBuilder();
        boolean inQuotes = false;
        boolean tokenActive = false;
        boolean tokenQuoted = false;
        int i = start + 1;

        while (i < text.length()) {
            char ch = text.charAt(i);
            if (inQuotes) {
                if (ch == '\\' && i + 1 < text.length()) {
                    token.append(text.charAt(i + 1));
                    i += 2;
                    continue;
                }
                if (ch == '"') {
                    inQuotes = false;
                    tokenQuoted = true;
                    i++;
                    continue;
                }
                token.append(ch);
                tokenActive = true;
                i++;
                continue;
            }

            if (ch == '"') {
                inQuotes = true;
                tokenActive = true;
                i++;
                continue;
            }
            if (ch == '{') {
                ParseResult nested = parseArray(text, i);
                items.add(nested.items.toArray(new Object[0]));
                i = nested.nextIdx;
                tokenActive = false;
                tokenQuoted = false;
                token.setLength(0);
                continue;
            }
            if (ch == '}') {
                if (tokenActive || tokenQuoted) {
                    items.add(parseArrayToken(token.toString(), tokenQuoted));
                }
                return new ParseResult(items, i + 1);
            }
            if (ch == ',') {
                if (tokenActive || tokenQuoted) {
                    items.add(parseArrayToken(token.toString(), tokenQuoted));
                } else {
                    items.add("");
                }
                token.setLength(0);
                tokenActive = false;
                tokenQuoted = false;
                i++;
                continue;
            }
            if (!Character.isWhitespace(ch) || tokenActive) {
                token.append(ch);
                tokenActive = true;
            }
            i++;
        }
        return new ParseResult(items, text.length());
    }

    private static Object parseArrayToken(String token, boolean quoted) {
        String value = token;
        if (!quoted) {
            String upper = value.trim();
            if ("NULL".equalsIgnoreCase(upper)) {
                return null;
            }
        }
        return quoted ? value : value.trim();
    }

    private static Object[] convertArrayElements(String baseType, Object[] elements) {
        Object[] converted = new Object[elements.length];
        for (int i = 0; i < elements.length; i++) {
            Object element = elements[i];
            if (element instanceof Object[]) {
                converted[i] = convertArrayElements(baseType, (Object[]) element);
                continue;
            }
            if (element == null) {
                converted[i] = null;
                continue;
            }
            String text = element.toString();
            converted[i] = convertElement(baseType, text);
        }
        return converted;
    }

    private static Object convertElement(String baseType, String text) {
        if (text == null) {
            return null;
        }
        String lower = baseType.toLowerCase();
        switch (lower) {
            case "boolean":
                return "t".equalsIgnoreCase(text) || "true".equalsIgnoreCase(text);
            case "smallint":
                return Short.parseShort(text);
            case "integer":
                return Integer.parseInt(text);
            case "bigint":
                return parseIntegerLikeText(text);
            case "real":
                return Float.parseFloat(text);
            case "double precision":
                return Double.parseDouble(text);
            case "numeric":
                return new BigDecimal(text);
            case "bytea":
                return decodeBytea(text);
            case "date":
                return java.sql.Date.valueOf(text);
            case "time":
                return java.sql.Time.valueOf(text);
            case "timestamp":
                return java.sql.Timestamp.valueOf(text.replace('T', ' '));
            case "timestamptz":
                return parseOffsetDateTimeText(text);
            case "timetz":
                return parseOffsetTimeText(text);
            case "uuid":
                return UUID.fromString(text);
            default:
                break;
        }
        return text;
    }

    private static OffsetDateTime parseOffsetDateTimeText(String text) {
        String normalized = normalizeTemporalText(text);
        try {
            return OffsetDateTime.parse(normalized);
        } catch (RuntimeException ignored) {
            try {
                return Instant.parse(normalized).atOffset(ZoneOffset.UTC);
            } catch (RuntimeException ignored2) {
                return LocalDateTime.parse(normalized).atOffset(ZoneOffset.UTC);
            }
        }
    }

    private static OffsetTime parseOffsetTimeText(String text) {
        String normalized = normalizeTemporalText(text);
        return OffsetTime.parse(normalized);
    }

    private static String normalizeTemporalText(String value) {
        String normalized = value == null ? "" : value.trim();
        if (normalized.contains(" ") && !normalized.contains("T")) {
            normalized = normalized.replace(' ', 'T');
        }
        if (normalized.matches(".*[+-]\\d{2}$")) {
            normalized = normalized + ":00";
        }
        return normalized;
    }

    private static int inferArrayOid(Object[] values) {
        if (values == null || values.length == 0) {
            return OID_TEXT_ARRAY;
        }

        int scalarOid = 0;
        for (Object value : values) {
            int candidate = inferScalarOid(value);
            if (candidate == 0) {
                continue;
            }
            if (scalarOid == 0) {
                scalarOid = candidate;
                continue;
            }
            if (scalarOid == candidate) {
                continue;
            }
            if (isNumericScalarOid(scalarOid) && isNumericScalarOid(candidate)) {
                scalarOid = widenNumericScalarOid(scalarOid, candidate);
                continue;
            }
            return OID_TEXT_ARRAY;
        }

        if (scalarOid == 0) {
            return OID_TEXT_ARRAY;
        }
        int mapped = scalarToArrayOid(scalarOid);
        return mapped == 0 ? OID_TEXT_ARRAY : mapped;
    }

    private static int inferScalarOid(Object value) {
        if (value == null) {
            return 0;
        }
        if (value instanceof Collection<?>) {
            return inferArrayOid(((Collection<?>) value).toArray());
        }
        if (value instanceof Object[]) {
            return inferArrayOid((Object[]) value);
        }
        if (value instanceof Boolean) {
            return OID_BOOL;
        }
        if (value instanceof Byte || value instanceof Short) {
            return OID_INT2;
        }
        if (value instanceof Integer) {
            return OID_INT4;
        }
        if (value instanceof Long) {
            return OID_INT8;
        }
        if (value instanceof Float) {
            return OID_FLOAT4;
        }
        if (value instanceof Double) {
            return OID_FLOAT8;
        }
        if (value instanceof BigDecimal) {
            return OID_NUMERIC;
        }
        if (value instanceof byte[]) {
            return OID_BYTEA;
        }
        if (value instanceof UUID) {
            return OID_UUID;
        }
        if (value instanceof java.sql.Date || value instanceof LocalDate) {
            return OID_DATE;
        }
        if (value instanceof Time || value instanceof LocalTime) {
            return OID_TIME;
        }
        if (value instanceof OffsetTime) {
            return OID_TIMETZ;
        }
        if (value instanceof Timestamp || value instanceof LocalDateTime) {
            return OID_TIMESTAMP;
        }
        if (value instanceof OffsetDateTime || value instanceof ZonedDateTime || value instanceof Instant) {
            return OID_TIMESTAMPTZ;
        }
        if (value instanceof java.util.Date) {
            return OID_TIMESTAMPTZ;
        }
        if (value instanceof String || value instanceof CharSequence || value instanceof Enum<?>) {
            return OID_TEXT;
        }
        if (value instanceof RowId) {
            return OID_BYTEA;
        }
        if (value instanceof Ref) {
            return OID_TEXT;
        }
        return 0;
    }

    private static int scalarToArrayOid(int scalarOid) {
        return switch (scalarOid) {
            case OID_BOOL -> OID_BOOL_ARRAY;
            case OID_BYTEA -> OID_BYTEA_ARRAY;
            case OID_INT2 -> OID_INT2_ARRAY;
            case OID_INT4 -> OID_INT4_ARRAY;
            case OID_INT8 -> OID_INT8_ARRAY;
            case OID_FLOAT4 -> OID_FLOAT4_ARRAY;
            case OID_FLOAT8 -> OID_FLOAT8_ARRAY;
            case OID_TEXT, OID_VARCHAR, OID_BPCHAR -> OID_TEXT_ARRAY;
            case OID_DATE -> OID_DATE_ARRAY;
            case OID_TIME -> OID_TIME_ARRAY;
            case OID_TIMETZ -> OID_TIMETZ_ARRAY;
            case OID_TIMESTAMP -> OID_TIMESTAMP_ARRAY;
            case OID_TIMESTAMPTZ -> OID_TIMESTAMPTZ_ARRAY;
            case OID_NUMERIC -> OID_NUMERIC_ARRAY;
            case OID_UUID -> OID_UUID_ARRAY;
            default -> 0;
        };
    }

    private static boolean isNumericScalarOid(int oid) {
        return oid == OID_INT2 || oid == OID_INT4 || oid == OID_INT8
            || oid == OID_FLOAT4 || oid == OID_FLOAT8 || oid == OID_NUMERIC;
    }

    private static int widenNumericScalarOid(int left, int right) {
        if (left == OID_NUMERIC || right == OID_NUMERIC) {
            return OID_NUMERIC;
        }
        if (left == OID_FLOAT8 || right == OID_FLOAT8) {
            return OID_FLOAT8;
        }
        if (left == OID_FLOAT4 || right == OID_FLOAT4) {
            return OID_FLOAT8;
        }
        if (left == OID_INT8 || right == OID_INT8) {
            return OID_INT8;
        }
        if (left == OID_INT4 || right == OID_INT4) {
            return OID_INT4;
        }
        return OID_INT2;
    }

    private static Object[] boxArray(int[] values) {
        Object[] out = new Object[values.length];
        for (int i = 0; i < values.length; i++) {
            out[i] = values[i];
        }
        return out;
    }

    private static Object[] boxArray(long[] values) {
        Object[] out = new Object[values.length];
        for (int i = 0; i < values.length; i++) {
            out[i] = values[i];
        }
        return out;
    }

    private static Object[] boxArray(short[] values) {
        Object[] out = new Object[values.length];
        for (int i = 0; i < values.length; i++) {
            out[i] = values[i];
        }
        return out;
    }

    private static Object[] boxArray(boolean[] values) {
        Object[] out = new Object[values.length];
        for (int i = 0; i < values.length; i++) {
            out[i] = values[i];
        }
        return out;
    }

    private static byte[] decodeBytea(String text) {
        if (text == null) {
            return null;
        }
        String hex = null;
        if (text.startsWith("\\x") || text.startsWith("0x")) {
            hex = text.substring(2);
        } else if ((text.length() & 1) == 0 && !text.isEmpty() && text.matches("(?i)[0-9a-f]+")) {
            // Some servers return plain hex text for BYTEA expressions without a \x prefix.
            hex = text;
        }
        if (hex != null) {
            int len = hex.length();
            byte[] out = new byte[len / 2];
            for (int i = 0; i < out.length; i++) {
                out[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
            }
            return out;
        }
        if (text.indexOf('\\') >= 0) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(text.length());
            int i = 0;
            while (i < text.length()) {
                char ch = text.charAt(i);
                if (ch != '\\') {
                    out.write((byte) ch);
                    i++;
                    continue;
                }
                if (i + 1 >= text.length()) {
                    out.write((byte) '\\');
                    break;
                }
                char n1 = text.charAt(i + 1);
                if (n1 == '\\') {
                    out.write((byte) '\\');
                    i += 2;
                    continue;
                }
                if (i + 3 < text.length()
                    && n1 >= '0' && n1 <= '7'
                    && text.charAt(i + 2) >= '0' && text.charAt(i + 2) <= '7'
                    && text.charAt(i + 3) >= '0' && text.charAt(i + 3) <= '7') {
                    int value = ((n1 - '0') << 6)
                        | ((text.charAt(i + 2) - '0') << 3)
                        | (text.charAt(i + 3) - '0');
                    out.write((byte) value);
                    i += 4;
                    continue;
                }
                out.write((byte) n1);
                i += 2;
            }
            return out.toByteArray();
        }
        return text.getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] uuidToBytes(UUID uuid) {
        ByteBuffer buf = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN);
        buf.putLong(uuid.getMostSignificantBits());
        buf.putLong(uuid.getLeastSignificantBits());
        return buf.array();
    }

    private static UUID bytesToUuid(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        long msb = buf.getLong();
        long lsb = buf.getLong();
        return new UUID(msb, lsb);
    }

    private static ParamEncoding encodeNumericWithOid(int oid, Number value) {
        switch (oid) {
            case OID_INT2:
                return new ParamEncoding(FORMAT_BINARY, oid, toBytesLE(value.shortValue()), false);
            case OID_INT4:
                return new ParamEncoding(FORMAT_BINARY, oid, toBytesLE(value.intValue()), false);
            case OID_INT8:
                return new ParamEncoding(FORMAT_BINARY, oid, toBytesLE(value.longValue()), false);
            case OID_FLOAT4:
                return new ParamEncoding(FORMAT_BINARY, oid, toBytesLE(value.floatValue()), false);
            case OID_FLOAT8:
                return new ParamEncoding(FORMAT_BINARY, oid, toBytesLE(value.doubleValue()), false);
            default:
                return null;
        }
    }

    private static ParamEncoding encodeComposite(Struct value) throws SQLException {
        Object[] attributes = value.getAttributes();
        int fieldCount = attributes != null ? attributes.length : 0;
        ByteBuffer buffer = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(fieldCount);
        if (attributes != null) {
            for (Object attr : attributes) {
                int fieldOid = 0;
                byte[] fieldData = null;
                if (attr != null) {
                    ParamEncoding encoded = encodeParam(attr, null);
                    fieldOid = encoded.oid;
                    fieldData = encoded.isNull ? null : encoded.data;
                }
                buffer = ensureCapacity(buffer, 8 + (fieldData != null ? fieldData.length : 0));
                buffer.putInt(fieldOid);
                if (fieldData == null) {
                    buffer.putInt(-1);
                } else {
                    buffer.putInt(fieldData.length);
                    buffer.put(fieldData);
                }
            }
        }
        buffer.flip();
        byte[] out = new byte[buffer.limit()];
        buffer.get(out);
        return new ParamEncoding(FORMAT_BINARY, OID_RECORD, out, false);
    }

    private static Struct decodeComposite(byte[] data) throws SQLException {
        if (data.length < 4) {
            return new SBStruct("record", new Object[0]);
        }
        int count = ByteBuffer.wrap(data, 0, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
        int offset = 4;
        Object[] attrs = new Object[count];
        for (int i = 0; i < count; i++) {
            if (offset + 8 > data.length) {
                break;
            }
            int oid = ByteBuffer.wrap(data, offset, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
            offset += 4;
            int length = ByteBuffer.wrap(data, offset, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
            offset += 4;
            if (length < 0) {
                attrs[i] = null;
                continue;
            }
            if (offset + length > data.length) {
                break;
            }
            byte[] raw = slice(data, offset, length);
            offset += length;
            attrs[i] = decodeValue(oid, raw, FORMAT_BINARY);
        }
        return new SBStruct("record", attrs);
    }

    private static int mapSqlTypeToOid(Integer sqlType) {
        if (sqlType == null) {
            return 0;
        }
        switch (sqlType) {
            case java.sql.Types.BOOLEAN:
            case java.sql.Types.BIT:
                return OID_BOOL;
            case java.sql.Types.TINYINT:
            case java.sql.Types.SMALLINT:
                return OID_INT2;
            case java.sql.Types.INTEGER:
                return OID_INT4;
            case java.sql.Types.BIGINT:
                return OID_INT8;
            case java.sql.Types.REAL:
                return OID_FLOAT4;
            case java.sql.Types.FLOAT:
            case java.sql.Types.DOUBLE:
                return OID_FLOAT8;
            case java.sql.Types.NUMERIC:
            case java.sql.Types.DECIMAL:
                return OID_NUMERIC;
            case java.sql.Types.CHAR:
                return OID_BPCHAR;
            case java.sql.Types.VARCHAR:
            case java.sql.Types.LONGVARCHAR:
                return OID_TEXT;
            case java.sql.Types.SQLXML:
                return OID_XML;
            case java.sql.Types.DATE:
                return OID_DATE;
            case java.sql.Types.TIME:
                return OID_TIME;
            case java.sql.Types.TIME_WITH_TIMEZONE:
                return OID_TIMETZ;
            case java.sql.Types.TIMESTAMP:
                return OID_TIMESTAMP;
            case java.sql.Types.TIMESTAMP_WITH_TIMEZONE:
                return OID_TIMESTAMPTZ;
            case java.sql.Types.BINARY:
            case java.sql.Types.VARBINARY:
            case java.sql.Types.LONGVARBINARY:
            case java.sql.Types.BLOB:
                return OID_BYTEA;
            case java.sql.Types.CLOB:
            case java.sql.Types.NCLOB:
                return OID_TEXT;
            case java.sql.Types.REF:
                return OID_TEXT;
            case java.sql.Types.ROWID:
                return OID_BYTEA;
            default:
                return 0;
        }
    }

    private static Instant parseRangeTimestamp(String text) throws SQLException {
        if (text == null) {
            throw new SQLException("Range timestamp bound is null", "22023");
        }
        String trimmed = text.trim();
        if (trimmed.isEmpty()) {
            throw new SQLException("Range timestamp bound is empty", "22023");
        }
        try {
            return Instant.parse(trimmed);
        } catch (Exception ignored) {
            // Fall through to local datetime parse.
        }
        try {
            return OffsetDateTime.parse(trimmed).toInstant();
        } catch (Exception ignored) {
            // Fall through to local datetime parse.
        }
        try {
            return LocalDateTime.parse(trimmed.replace(' ', 'T')).toInstant(ZoneOffset.UTC);
        } catch (Exception ex) {
            throw new SQLException("Unsupported range timestamp bound: " + text, "22023", ex);
        }
    }

    private static ByteBuffer ensureCapacity(ByteBuffer buffer, int additional) {
        if (buffer.remaining() >= additional) {
            return buffer;
        }
        int newSize = Math.max(buffer.capacity() * 2, buffer.capacity() + additional);
        ByteBuffer larger = ByteBuffer.allocate(newSize).order(ByteOrder.LITTLE_ENDIAN);
        buffer.flip();
        larger.put(buffer);
        return larger;
    }

    private static byte[] slice(byte[] data, int offset, int length) {
        byte[] out = new byte[length];
        System.arraycopy(data, offset, out, 0, length);
        return out;
    }

    private static final class RangeEncoding {
        private final int oid;
        private final byte[] data;

        RangeEncoding(int oid, byte[] data) {
            this.oid = oid;
            this.data = data;
        }
    }
}
