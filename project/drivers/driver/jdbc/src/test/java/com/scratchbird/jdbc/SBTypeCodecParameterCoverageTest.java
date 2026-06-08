// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.nio.charset.StandardCharsets;
import java.sql.Types;
import java.time.LocalDate;
import java.time.OffsetDateTime;
import java.time.OffsetTime;
import java.time.ZoneOffset;
import org.junit.jupiter.api.Test;

class SBTypeCodecParameterCoverageTest {

    @Test
    void encodesExtendedJdbcParameterObjects() throws Exception {
        byte[] rowIdBytes = "rid-42".getBytes(StandardCharsets.UTF_8);
        SBTypeCodec.ParamEncoding rowId = SBTypeCodec.encodeParam(new SBRowId(rowIdBytes), Types.ROWID);
        assertEquals(SBTypeCodec.OID_BYTEA, rowId.getOid());
        assertArrayEquals(
            rowIdBytes,
            assertInstanceOf(byte[].class,
                SBTypeCodec.decodeValue(rowId.getOid(), rowId.getData(), rowId.getFormat())));

        SBTypeCodec.ParamEncoding ref = SBTypeCodec.encodeParam(new SBRef("demo_ref", "rid-100"), Types.REF);
        assertEquals(SBTypeCodec.OID_TEXT, ref.getOid());
        assertEquals("rid-100", SBTypeCodec.decodeValue(ref.getOid(), ref.getData(), ref.getFormat()));

        SBTypeCodec.ParamEncoding xml = SBTypeCodec.encodeParam(new SBSQLXML("<doc/>"), Types.SQLXML);
        assertEquals(SBTypeCodec.OID_XML, xml.getOid());
        assertEquals("<doc/>", SBTypeCodec.decodeValue(xml.getOid(), xml.getData(), xml.getFormat()));

        SBTypeCodec.ParamEncoding blob = SBTypeCodec.encodeParam(new SBBlob(new byte[]{1, 2, 3}), Types.BLOB);
        assertEquals(SBTypeCodec.OID_BYTEA, blob.getOid());
        assertArrayEquals(
            new byte[]{1, 2, 3},
            assertInstanceOf(byte[].class,
                SBTypeCodec.decodeValue(blob.getOid(), blob.getData(), blob.getFormat())));

        SBTypeCodec.ParamEncoding clob = SBTypeCodec.encodeParam(new SBClob("hello"), Types.CLOB);
        assertEquals(SBTypeCodec.OID_TEXT, clob.getOid());
        assertEquals("hello", SBTypeCodec.decodeValue(clob.getOid(), clob.getData(), clob.getFormat()));

        SBTypeCodec.ParamEncoding enumValue = SBTypeCodec.encodeParam(Thread.State.RUNNABLE, Types.VARCHAR);
        assertEquals(SBTypeCodec.OID_TEXT, enumValue.getOid());
        assertEquals("RUNNABLE", SBTypeCodec.decodeValue(
            enumValue.getOid(), enumValue.getData(), enumValue.getFormat()));

        Object customObject = new Object() {
            @Override
            public String toString() {
                return "custom-parameter";
            }
        };
        SBTypeCodec.ParamEncoding custom = SBTypeCodec.encodeParam(customObject, Types.OTHER);
        assertEquals(SBTypeCodec.OID_TEXT, custom.getOid());
        assertEquals("custom-parameter", SBTypeCodec.decodeValue(
            custom.getOid(), custom.getData(), custom.getFormat()));
    }

    @Test
    void acceptsStringTemporalBoundsForRanges() throws Exception {
        SBRange<String> dateRange = new SBRange<>(
            "2026-01-10",
            "2026-02-11",
            true,
            false,
            false,
            false,
            false,
            SBTypeCodec.OID_DATERANGE);
        SBTypeCodec.ParamEncoding encodedDateRange = SBTypeCodec.encodeParam(dateRange, null);
        SBRange<?> decodedDateRange = assertInstanceOf(SBRange.class, SBTypeCodec.decodeValue(
            encodedDateRange.getOid(),
            encodedDateRange.getData(),
            encodedDateRange.getFormat()));
        assertEquals(LocalDate.parse("2026-01-10"), decodedDateRange.getLower());
        assertEquals(LocalDate.parse("2026-02-11"), decodedDateRange.getUpper());

        SBRange<String> tsTzRange = new SBRange<>(
            "2026-03-01T00:00:00Z",
            "2026-03-02T12:30:00Z",
            true,
            false,
            false,
            false,
            false,
            SBTypeCodec.OID_TSTZRANGE);
        SBTypeCodec.ParamEncoding encodedTsTzRange = SBTypeCodec.encodeParam(tsTzRange, null);
        SBRange<?> decodedTsTzRange = assertInstanceOf(SBRange.class, SBTypeCodec.decodeValue(
            encodedTsTzRange.getOid(),
            encodedTsTzRange.getData(),
            encodedTsTzRange.getFormat()));
        assertNotNull(decodedTsTzRange.getLower());
        assertNotNull(decodedTsTzRange.getUpper());
        assertInstanceOf(OffsetDateTime.class, decodedTsTzRange.getLower());
        assertInstanceOf(OffsetDateTime.class, decodedTsTzRange.getUpper());
    }

    @Test
    void encodesAndDecodesTimeWithTimeZone() throws Exception {
        OffsetTime value = OffsetTime.of(12, 34, 56, 123_000_000, ZoneOffset.ofHoursMinutes(5, 30));
        SBTypeCodec.ParamEncoding encoded = SBTypeCodec.encodeParam(value, Types.TIME_WITH_TIMEZONE);
        assertEquals(SBTypeCodec.OID_TIMETZ, encoded.getOid());

        Object decoded = SBTypeCodec.decodeValue(encoded.getOid(), encoded.getData(), encoded.getFormat());
        assertInstanceOf(OffsetTime.class, decoded);
        assertEquals(value, decoded);
    }

    @Test
    void decodesTemporalTextPayloadsUsingOffsetAwareTypes() throws Exception {
        Object timetz = SBTypeCodec.decodeValue(
            SBTypeCodec.OID_TIMETZ,
            "08:09:10+03".getBytes(StandardCharsets.UTF_8),
            SBTypeCodec.FORMAT_TEXT);
        assertEquals(OffsetTime.parse("08:09:10+03:00"), timetz);

        Object timestamptz = SBTypeCodec.decodeValue(
            SBTypeCodec.OID_TIMESTAMPTZ,
            "2026-03-01 12:34:56+02".getBytes(StandardCharsets.UTF_8),
            SBTypeCodec.FORMAT_TEXT);
        assertEquals(OffsetDateTime.parse("2026-03-01T12:34:56+02:00"), timestamptz);
    }

    @Test
    void infersArrayOidsForTemporalAndNumericCollections() throws Exception {
        SBTypeCodec.ParamEncoding timetzArray = SBTypeCodec.encodeParam(
            new Object[] {
                OffsetTime.parse("01:02:03+02:00"),
                OffsetTime.parse("09:10:11-05:00")
            },
            Types.ARRAY
        );
        assertEquals(SBTypeCodec.OID_TIMETZ_ARRAY, timetzArray.getOid());
        SBArray decodedTimetz = assertInstanceOf(SBArray.class,
            SBTypeCodec.decodeValue(timetzArray.getOid(), timetzArray.getData(), timetzArray.getFormat()));
        assertEquals("timetz", decodedTimetz.getBaseTypeName());

        SBTypeCodec.ParamEncoding numericArray = SBTypeCodec.encodeParam(
            new Object[] {1, 2L, 3.5d},
            Types.ARRAY
        );
        assertEquals(SBTypeCodec.OID_FLOAT8_ARRAY, numericArray.getOid());
    }
}
