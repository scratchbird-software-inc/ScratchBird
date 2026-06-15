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
import java.util.Map;
import org.junit.jupiter.api.Test;

class SBProtocolHandlerStartupFeaturesTest {
    private static final int HEADER_SIZE = 40;
    private static final int PROTOCOL_VERSION = 0x0101;
    private static final long FEATURE_COMPRESSION = 1L << 0;
    private static final long FEATURE_STREAMING = 1L << 1;

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

    private static void putLengthPrefixedString(ByteArrayOutputStream out, String value) throws Exception {
        byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
        out.write(ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(encoded.length)
            .array());
        out.write(encoded);
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
}
