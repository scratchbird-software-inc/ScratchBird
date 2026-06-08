// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.reflect.Field;
import java.sql.SQLException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBDatabaseMetaDataVersionTest {

    @Test
    void fallsBackToStaticVersionWhenServerParametersUnavailable() throws SQLException {
        SBDatabaseMetaData meta = new SBDatabaseMetaData(null);
        assertEquals("1.0.0", meta.getDatabaseProductVersion());
        assertEquals(1, meta.getDatabaseMajorVersion());
        assertEquals(0, meta.getDatabaseMinorVersion());
    }

    @Test
    void usesNegotiatedServerVersionWhenAvailable() throws Exception {
        StubProtocol protocol = new StubProtocol(Map.of("server_version", "4.7.19-beta1"));
        SBDatabaseMetaData meta = new SBDatabaseMetaData(newConnection(protocol));

        assertEquals("4.7.19-beta1", meta.getDatabaseProductVersion());
        assertEquals(4, meta.getDatabaseMajorVersion());
        assertEquals(7, meta.getDatabaseMinorVersion());
    }

    @Test
    void derivesSemanticVersionFromServerVersionNumWhenNeeded() throws Exception {
        StubProtocol protocol = new StubProtocol(Map.of("server_version_num", "120003"));
        SBDatabaseMetaData meta = new SBDatabaseMetaData(newConnection(protocol));

        assertEquals("12.0.3", meta.getDatabaseProductVersion());
        assertEquals(12, meta.getDatabaseMajorVersion());
        assertEquals(0, meta.getDatabaseMinorVersion());
    }

    private static SBConnection newConnection(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new AtomicBoolean(false));
        return connection;
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static void setField(Object target, String fieldName, Object value) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static final class StubProtocol extends SBProtocolHandler {
        private final Map<String, String> parameters = new HashMap<>();

        StubProtocol(Map<String, String> parameters) {
            super(new SBConnectionProperties());
            if (parameters != null) {
                this.parameters.putAll(parameters);
            }
        }

        @Override
        public String getServerParameter(String name) {
            return parameters.get(name);
        }
    }
}
