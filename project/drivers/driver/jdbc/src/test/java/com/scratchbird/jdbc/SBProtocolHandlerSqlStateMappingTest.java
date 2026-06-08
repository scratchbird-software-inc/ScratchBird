// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.sql.SQLException;
import org.junit.jupiter.api.Test;

public class SBProtocolHandlerSqlStateMappingTest {

    @Test
    public void exactSqlStateMapsToSpecificType() throws Exception {
        var ex = createSQLExceptionFromState("42P01");
        assertInstanceOf(java.sql.SQLSyntaxErrorException.class, ex);
    }

    @Test
    public void classSqlStateMapsToCategory() throws Exception {
        var ex = createSQLExceptionFromState("22000");
        assertInstanceOf(java.sql.SQLDataException.class, ex);
    }

    @Test
    public void classSqlStateMapsConnectionCategory() throws Exception {
        var ex = createSQLExceptionFromState("08012");
        assertInstanceOf(java.sql.SQLTransientConnectionException.class, ex);
    }

    @Test
    public void privilegeViolationMapsToAuthorizationCategory() throws Exception {
        var ex = createSQLExceptionFromState("42501");
        assertInstanceOf(java.sql.SQLInvalidAuthorizationSpecException.class, ex);
    }

    @Test
    public void lockNotAvailableMapsToTransientCategory() throws Exception {
        var ex = createSQLExceptionFromState("55P03");
        assertInstanceOf(java.sql.SQLTransientException.class, ex);
    }

    @Test
    public void invalidCatalogMapsToNonTransientCategory() throws Exception {
        var ex = createSQLExceptionFromState("3D000");
        assertInstanceOf(java.sql.SQLNonTransientException.class, ex);
    }

    @Test
    public void failedTransactionStateMapsToRollbackCategory() throws Exception {
        var ex = createSQLExceptionFromState("25P02");
        assertInstanceOf(java.sql.SQLTransactionRollbackException.class, ex);
    }

    @Test
    public void unknownClassReturnsGenericSqlException() throws Exception {
        var ex = createSQLExceptionFromState("ZZ123");
        assertInstanceOf(java.sql.SQLException.class, ex);
        assertFalse(ex instanceof java.sql.SQLTransientException);
    }

    @Test
    public void retryScopeClassifiesStatementAndReconnectBoundaries() {
        assertEquals(SBRetryScope.STATEMENT, SBProtocolHandler.retryScopeForSqlState("40001"));
        assertEquals(SBRetryScope.STATEMENT, SBProtocolHandler.retryScopeForSqlState("40P01"));
        assertEquals(SBRetryScope.RECONNECT, SBProtocolHandler.retryScopeForSqlState("08006"));
        assertEquals(SBRetryScope.NONE, SBProtocolHandler.retryScopeForSqlState("57014"));
        assertEquals(SBRetryScope.NONE, SBProtocolHandler.retryScopeForSqlState(null));
    }

    @Test
    public void retryableSqlStateOnlyAllowsFreshBoundaryRetries() {
        assertTrue(SBProtocolHandler.isRetryableSqlState("40001"));
        assertTrue(SBProtocolHandler.isRetryableSqlState("08003"));
        assertFalse(SBProtocolHandler.isRetryableSqlState("57014"));
        assertFalse(SBProtocolHandler.isRetryableSqlState(""));
    }

    @Test
    public void txnBeginPayloadExpandsForReadCommittedMode() throws Exception {
        Method method = SBProtocolHandler.class.getDeclaredMethod(
            "buildTxnBeginPayload",
            short.class,
            byte.class,
            byte.class,
            byte.class,
            byte.class,
            byte.class,
            byte.class,
            int.class,
            byte.class
        );
        method.setAccessible(true);
        short flags = (short) (0x0001 | 0x0100);
        byte[] payload = (byte[]) method.invoke(
            new SBProtocolHandler(new SBConnectionProperties()),
            flags,
            (byte) 0,
            (byte) 0,
            SBProtocolHandler.ISOLATION_READ_COMMITTED,
            (byte) 0,
            (byte) 0,
            (byte) 0,
            0,
            SBProtocolHandler.READ_COMMITTED_MODE_READ_CONSISTENCY
        );

        assertEquals(16, payload.length);
        assertEquals(SBProtocolHandler.READ_COMMITTED_MODE_READ_CONSISTENCY, Byte.toUnsignedInt(payload[12]));
    }

    @Test
    public void resumeSuspendedPortalRequiresExplicitSuspendedState() {
        SBProtocolHandler handler = new SBProtocolHandler(new SBConnectionProperties());

        SQLException ex = org.junit.jupiter.api.Assertions.assertThrows(
            SQLException.class,
            () -> handler.resumeSuspendedPortal(2)
        );

        assertEquals("55000", ex.getSQLState());
    }

    @Test
    public void resumeSuspendedPortalWritesExecuteAfterExplicitSuspendedState() throws Exception {
        SBProtocolHandler handler = new SBProtocolHandler(new SBConnectionProperties());
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        setPrivateField(handler, "outputStream", output);
        setPrivateField(handler, "attachmentId", new byte[16]);
        setPrivateField(handler, "txnId", 0L);

        handler.allowPortalResume();
        handler.resumeSuspendedPortal(4);

        byte[] written = output.toByteArray();
        assertTrue(written.length >= 48);
        assertEquals(0x07, written[6] & 0xff);
        assertEquals(4, java.nio.ByteBuffer.wrap(written, 44, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).getInt());
    }

    private static SQLException createSQLExceptionFromState(String state) throws Exception {
        var method = getCreateSQLExceptionMethod();
        var handler = new SBProtocolHandler(new SBConnectionProperties());
        try {
            var result = method.invoke(handler, "mapped", state);
            return (SQLException) result;
        } catch (InvocationTargetException ex) {
            var cause = ex.getTargetException();
            if (cause instanceof Exception exception) {
                throw exception;
            }
            throw ex;
        }
    }

    private static Method getCreateSQLExceptionMethod() throws NoSuchMethodException {
        var method = SBProtocolHandler.class.getDeclaredMethod("createSQLException", String.class, String.class);
        method.setAccessible(true);
        return method;
    }

    private static void setPrivateField(Object target, String fieldName, Object value) throws Exception {
        var field = target.getClass().getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(target, value);
    }
}
