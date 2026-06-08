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
import org.junit.jupiter.api.Test;

class SBProtocolHandlerStartupFeaturesTest {
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

    private static long startupFeatureBits(boolean binaryTransfer, String compression) throws Exception {
        SBConnectionProperties properties = new SBConnectionProperties();
        properties.setBinaryTransfer(binaryTransfer);
        properties.setCompression(compression);
        properties.setDatabase("main");
        properties.setUser("scratchbird");

        byte[] message = startupFrame(properties);
        int marker = indexOf(message, "database".getBytes(StandardCharsets.UTF_8));
        assertTrue(marker >= 8, "startup frame did not include expected parameter block");
        int featureOffset = marker - 8;
        ByteBuffer payload = ByteBuffer.wrap(message).order(ByteOrder.LITTLE_ENDIAN);
        payload.position(featureOffset);
        return payload.getLong();
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

    private static int indexOf(byte[] value, byte[] needle) {
        if (value == null || needle == null || needle.length == 0 || value.length < needle.length) {
            return -1;
        }
        for (int i = 0; i <= value.length - needle.length; i++) {
            boolean match = true;
            for (int j = 0; j < needle.length; j++) {
                if (value[i + j] != needle[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return i;
            }
        }
        return -1;
    }

    private static void setField(Object target, String fieldName, Object value) throws Exception {
        Field field = SBProtocolHandler.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(target, value);
    }
}
