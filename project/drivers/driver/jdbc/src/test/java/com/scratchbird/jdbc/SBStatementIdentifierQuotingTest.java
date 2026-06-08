// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBStatementIdentifierQuotingTest {

    @Test
    void enquoteLiteralEscapesSingleQuotes() throws Exception {
        SBStatement statement = newStatementForTest();
        assertEquals("'o''hare'", statement.enquoteLiteral("o'hare"));
        assertEquals("N'alpha''beta'", statement.enquoteNCharLiteral("alpha'beta"));
    }

    @Test
    void simpleIdentifierDetectionRejectsKeywordsAndInvalidShapes() throws Exception {
        SBStatement statement = newStatementForTest();
        assertTrue(statement.isSimpleIdentifier("customer_1"));
        assertFalse(statement.isSimpleIdentifier("select"));
        assertFalse(statement.isSimpleIdentifier("has-dash"));
        assertFalse(statement.isSimpleIdentifier("1prefix"));
    }

    @Test
    void enquoteIdentifierHonorsAlwaysQuoteAndEscapesEmbeddedQuotes() throws Exception {
        SBStatement statement = newStatementForTest();
        assertEquals("customer_1", statement.enquoteIdentifier("customer_1", false));
        assertEquals("\"Select\"", statement.enquoteIdentifier("Select", false));
        assertEquals("\"x\"\"y\"", statement.enquoteIdentifier("x\"y", true));
    }

    @Test
    void enquoteMethodsRejectNullAndInvalidInputs() throws Exception {
        SBStatement statement = newStatementForTest();
        assertThrows(SQLException.class, () -> statement.enquoteLiteral(null));
        assertThrows(SQLException.class, () -> statement.enquoteIdentifier(null, true));
        assertThrows(SQLException.class, () -> statement.enquoteIdentifier("   ", false));
    }

    private static SBStatement newStatementForTest() throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setConnectionField(connection, "closed", new AtomicBoolean(false));
        setConnectionField(connection, "properties", new SBConnectionProperties());
        return new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, ResultSet.CLOSE_CURSORS_AT_COMMIT);
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static void setConnectionField(SBConnection connection, String fieldName, Object value)
            throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(connection, value);
    }
}
