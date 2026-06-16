// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class SBProtocolHandlerStartupFeaturesTest {
    private static final int HEADER_SIZE = 40;
    private static final int PROTOCOL_VERSION = 0x0101;
    private static final long FEATURE_COMPRESSION = 1L << 0;
    private static final long FEATURE_STREAMING = 1L << 1;
    private static final int P1_CANONICAL_TYPE_REF_BYTES = 144;

    @Test
    void startupPayloadEncodesFeatureFlagsFromConnectionOptions() throws Exception {
        assertEquals(FEATURE_STREAMING, startupFeatureBits(true, "off"));
        assertEquals(0L, startupFeatureBits(false, "off"));
        assertEquals(FEATURE_COMPRESSION, startupFeatureBits(false, "zstd"));
        assertEquals(FEATURE_COMPRESSION | FEATURE_STREAMING, startupFeatureBits(true, "zstd"));
    }

    @Test
    void startupPayloadUsesP1VersionWindowAndTypedParams() throws Exception {
        SBConnectionProperties properties = new SBConnectionProperties();
        properties.setDatabase("main");
        properties.setUser("scratchbird");

        byte[] message = startupFrame(properties);
        ByteBuffer payload = ByteBuffer.wrap(message).order(ByteOrder.LITTLE_ENDIAN);

        assertEquals(PROTOCOL_VERSION, payload.getShort(HEADER_SIZE) & 0xffff);
        assertEquals(PROTOCOL_VERSION, payload.getShort(HEADER_SIZE + 2) & 0xffff);
        assertEquals(0, payload.getInt(HEADER_SIZE + 4));
        assertEquals(0L, payload.getLong(HEADER_SIZE + 16));
        assertEquals(0L, payload.getLong(HEADER_SIZE + 24));
        assertTrue(payload.getInt(HEADER_SIZE + 80) >= 3);
    }

    @Test
    void startupPayloadIncludesAuthPluginAndPinningParams() throws Exception {
        SBConnectionProperties properties = new SBConnectionProperties();
        properties.setDatabase("main");
        properties.setUser("scratchbird");
        properties.setAuthMethodId("scratchbird.auth.proxy_assertion");
        properties.setAuthMethodPayload("opaque");
        properties.setAuthPayloadJson("{\"subject\":\"alice\"}");
        properties.setAuthPayloadB64("YWJj");
        properties.setAuthProviderProfile("corp_primary");
        properties.setAuthRequiredMethods("SCRAM_SHA_256,TOKEN");
        properties.setAuthForbiddenMethods("MD5");
        properties.setAuthRequireChannelBinding(true);
        properties.setWorkloadIdentityToken("jwt-token");
        properties.setProxyPrincipalAssertion("signed-assertion");

        byte[] message = startupFrame(properties);
        String text = new String(message, StandardCharsets.UTF_8);

        assertTrue(text.contains("auth_method_id"));
        assertTrue(text.contains("auth_method_payload"));
        assertTrue(text.contains("auth_payload_json"));
        assertTrue(text.contains("auth_payload_b64"));
        assertTrue(text.contains("auth_provider_profile"));
        assertTrue(text.contains("auth_required_methods"));
        assertTrue(text.contains("auth_forbidden_methods"));
        assertTrue(text.contains("auth_require_channel_binding"));
        assertTrue(text.contains("workload_identity_token"));
        assertTrue(text.contains("proxy_principal_assertion"));
    }

    @Test
    void parameterStatusAcceptsP1BatchedTypedStatusPayload() throws Exception {
        SBProtocolHandler protocol = new SBProtocolHandler(new SBConnectionProperties());
        Method handleParameterStatusPayload = SBProtocolHandler.class
            .getDeclaredMethod("handleParameterStatusPayload", byte[].class);
        handleParameterStatusPayload.setAccessible(true);

        handleParameterStatusPayload.invoke(protocol, p1ParameterStatusPayload(
            "protocol.selected_version", "1.1",
            "language.tag", "en-US"
        ));

        @SuppressWarnings("unchecked")
        Map<String, String> serverParameters =
            (Map<String, String>) getField(protocol, "serverParameters");
        assertEquals("1.1", serverParameters.get("protocol.selected_version"));
        assertEquals("en-US", serverParameters.get("language.tag"));
    }

    @Test
    void readyStatusAcceptsP1PayloadAndUpdatesRuntimeTransaction() throws Exception {
        SBProtocolHandler protocol = new SBProtocolHandler(new SBConnectionProperties());
        Method parseReady = SBProtocolHandler.class.getDeclaredMethod("parseReady", byte[].class);
        parseReady.setAccessible(true);
        Object ready = parseReady.invoke(protocol, p1ReadyPayload(12345L));

        Method applyReady = SBProtocolHandler.class
            .getDeclaredMethod("applyRuntimeReadyState", ready.getClass());
        applyReady.setAccessible(true);
        applyReady.invoke(protocol, ready);

        assertEquals(12345L, getField(protocol, "txnId"));
        assertTrue((Boolean) getField(protocol, "runtimeTxnActive"));
    }

    @Test
    void rowDescriptionAcceptsP1PayloadAndDecodesMatchingDataRows() throws Exception {
        SBProtocolHandler protocol = new SBProtocolHandler(new SBConnectionProperties());
        Method parseRowDescription = SBProtocolHandler.class
            .getDeclaredMethod("parseRowDescription", byte[].class);
        parseRowDescription.setAccessible(true);
        Method parseDataRow = SBProtocolHandler.class
            .getDeclaredMethod("parseDataRow", byte[].class, List.class);
        parseDataRow.setAccessible(true);

        @SuppressWarnings("unchecked")
        List<SBColumnInfo> columns = (List<SBColumnInfo>) parseRowDescription.invoke(
            protocol,
            p1RowDescriptionPayload(
                new P1Column("node_id", SBTypeCodec.OID_TEXT),
                new P1Column("sort_ordinal", SBTypeCodec.OID_INT4),
                new P1Column("node_name", SBTypeCodec.OID_TEXT)));

        assertEquals(3, columns.size());
        assertEquals("node_id", columns.get(0).getName());
        assertEquals(SBTypeCodec.OID_INT4, columns.get(1).getTypeOid());
        assertEquals(SBTypeCodec.FORMAT_TEXT, columns.get(1).getFormatCode());

        Object[] row = (Object[]) parseDataRow.invoke(
            protocol,
            dataRowPayload("018f", "42", "default"),
            columns);
        assertEquals("018f", row[0]);
        assertEquals(42, row[1]);
        assertEquals("default", row[2]);
    }

    private static long startupFeatureBits(boolean binaryTransfer, String compression) throws Exception {
        SBConnectionProperties properties = new SBConnectionProperties();
        properties.setBinaryTransfer(binaryTransfer);
        properties.setCompression(compression);
        properties.setDatabase("main");
        properties.setUser("scratchbird");

        byte[] message = startupFrame(properties);
        ByteBuffer payload = ByteBuffer.wrap(message).order(ByteOrder.LITTLE_ENDIAN);
        return payload.getLong(HEADER_SIZE + 8);
    }

    private static byte[] startupFrame(SBConnectionProperties properties) throws Exception {
        SBProtocolHandler protocol = new SBProtocolHandler(properties);
        ByteArrayOutputStream networkBuffer = new ByteArrayOutputStream();
        setField(protocol, "outputStream", networkBuffer);

        Method sendStartup = SBProtocolHandler.class.getDeclaredMethod("sendStartupMessage");
        sendStartup.setAccessible(true);
        sendStartup.invoke(protocol);

        return networkBuffer.toByteArray();
    }

    private static byte[] p1ParameterStatusPayload(String... keyValues) throws Exception {
        ByteArrayOutputStream payload = new ByteArrayOutputStream();
        payload.write(ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(keyValues.length / 2)
            .array());
        for (int i = 0; i < keyValues.length; i += 2) {
            putLengthPrefixedString(payload, keyValues[i]);
            payload.write(ByteBuffer.allocate(2)
                .order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) 0x01)
                .array());
            payload.write(0);
            byte[] value = keyValues[i + 1].getBytes(StandardCharsets.UTF_8);
            payload.write(ByteBuffer.allocate(4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .putInt(value.length)
                .array());
            payload.write(value);
        }
        return payload.toByteArray();
    }

    private static byte[] p1ReadyPayload(long txnId) {
        byte[] payload = new byte[76];
        ByteBuffer buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putLong(48, txnId);
        payload[56] = 0x54;
        payload[57] = 0x00;
        buffer.putShort(58, (short) PROTOCOL_VERSION);
        return payload;
    }

    private static byte[] p1RowDescriptionPayload(P1Column... columns) throws Exception {
        ByteArrayOutputStream payload = new ByteArrayOutputStream();
        putShort(payload, 1);
        payload.write(0);
        payload.write(1);
        putInt(payload, columns.length);
        writeZeros(payload, 16);
        writeZeros(payload, 16);
        writeZeros(payload, 32);
        for (int i = 0; i < columns.length; i++) {
            P1Column column = columns[i];
            putInt(payload, i + 1);
            payload.write(0);
            payload.write(1);
            payload.write(1);
            payload.write(0);
            putLong(payload, (1L << 0) | (1L << 10));
            putCanonicalTypeRef(payload, column.typeOid);
            writeZeros(payload, 16 * 3);
            putInt(payload, 0);
            putShort(payload, 0);
            putShort(payload, 0);
            putNullableText(payload, column.name);
        }
        return payload.toByteArray();
    }

    private static byte[] dataRowPayload(String... values) throws Exception {
        ByteArrayOutputStream payload = new ByteArrayOutputStream();
        putShort(payload, values.length);
        putShort(payload, 0);
        for (String value : values) {
            byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
            putInt(payload, encoded.length);
            payload.write(encoded);
        }
        return payload.toByteArray();
    }

    private static void putLengthPrefixedString(ByteArrayOutputStream out, String value) throws Exception {
        byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
        out.write(ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(encoded.length)
                .array());
        out.write(encoded);
    }

    private static void putNullableText(ByteArrayOutputStream out, String value) throws Exception {
        byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
        out.write(3);
        putInt(out, encoded.length);
        out.write(encoded);
    }

    private static void putCanonicalTypeRef(ByteArrayOutputStream out, int oid) throws Exception {
        int family = switch (oid) {
            case SBTypeCodec.OID_BOOL -> 1;
            case SBTypeCodec.OID_INT4, SBTypeCodec.OID_INT8 -> 2;
            case SBTypeCodec.OID_NUMERIC -> 4;
            case SBTypeCodec.OID_FLOAT8 -> 6;
            default -> oid == 0 ? 0 : 8;
        };
        int code = switch (oid) {
            case SBTypeCodec.OID_BOOL, SBTypeCodec.OID_NUMERIC, SBTypeCodec.OID_TEXT -> 1;
            case SBTypeCodec.OID_INT4 -> 3;
            case SBTypeCodec.OID_INT8 -> 4;
            case SBTypeCodec.OID_FLOAT8 -> 2;
            default -> oid == 0 ? 0 : 1;
        };
        int before = out.size();
        putShort(out, family);
        putShort(out, code);
        putShort(out, family == 0 ? 0 : 1);
        putShort(out, 0);
        writeZeros(out, 16 * 3);
        putInt(out, family == 0 ? 0 : 1);
        putInt(out, 0);
        putLong(out, 0);
        putLong(out, 0);
        putInt(out, 0);
        putInt(out, 0);
        putLong(out, 0);
        putLong(out, 0);
        writeZeros(out, 16 * 2);
        putShort(out, 0);
        putShort(out, 0);
        putShort(out, 0);
        putShort(out, 0);
        assertEquals(P1_CANONICAL_TYPE_REF_BYTES, out.size() - before);
    }

    private static void putShort(ByteArrayOutputStream out, int value) throws Exception {
        out.write(ByteBuffer.allocate(2)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putShort((short) value)
            .array());
    }

    private static void putInt(ByteArrayOutputStream out, int value) throws Exception {
        out.write(ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(value)
            .array());
    }

    private static void putLong(ByteArrayOutputStream out, long value) throws Exception {
        out.write(ByteBuffer.allocate(8)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putLong(value)
            .array());
    }

    private static void writeZeros(ByteArrayOutputStream out, int count) {
        out.writeBytes(new byte[count]);
    }

    private static void setField(Object target, String fieldName, Object value) throws Exception {
        Field field = SBProtocolHandler.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static Object getField(Object target, String fieldName) throws Exception {
        Field field = SBProtocolHandler.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        return field.get(target);
    }

    private static final class P1Column {
        final String name;
        final int typeOid;

        P1Column(String name, int typeOid) {
            this.name = name;
            this.typeOid = typeOid;
        }
    }
}
