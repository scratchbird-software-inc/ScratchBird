// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.sql.ResultSet;
import java.sql.RowIdLifetime;
import java.sql.SQLException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import sun.misc.Unsafe;

class SBDatabaseMetaDataCapabilitiesTest {

    @Test
    void reportsValidatedCapabilitySurfaceForGrammarAndMultiResultSupport() throws SQLException {
        SBDatabaseMetaData meta = new SBDatabaseMetaData(null);

        assertTrue(meta.allProceduresAreCallable());
        assertTrue(meta.allTablesAreSelectable());
        assertTrue(meta.supportsMultipleTransactions());
        assertTrue(meta.supportsMultipleResultSets());
        assertTrue(meta.supportsMultipleOpenResults());
        assertTrue(meta.supportsPositionedUpdate());
        assertTrue(meta.supportsPositionedDelete());
        assertTrue(meta.supportsSelectForUpdate());
        assertTrue(meta.supportsConvert());
        assertTrue(meta.supportsConvert(java.sql.Types.INTEGER, java.sql.Types.BIGINT));
        assertTrue(meta.supportsConvert(java.sql.Types.VARCHAR, java.sql.Types.INTEGER));
        assertTrue(meta.supportsConvert(java.sql.Types.TIMESTAMP, java.sql.Types.DATE));
        assertFalse(meta.supportsConvert(java.sql.Types.ARRAY, java.sql.Types.STRUCT));
        assertTrue(meta.supportsMinimumSQLGrammar());
        assertTrue(meta.supportsCoreSQLGrammar());
        assertTrue(meta.supportsExtendedSQLGrammar());
        assertTrue(meta.supportsANSI92EntryLevelSQL());
        assertTrue(meta.supportsANSI92IntermediateSQL());
        assertTrue(meta.supportsANSI92FullSQL());
        assertTrue(meta.supportsCatalogsInDataManipulation());
        assertTrue(meta.supportsCatalogsInProcedureCalls());
        assertTrue(meta.supportsCatalogsInTableDefinitions());
        assertTrue(meta.supportsCatalogsInIndexDefinitions());
        assertTrue(meta.supportsCatalogsInPrivilegeDefinitions());
        assertTrue(meta.locatorsUpdateCopy());
        assertTrue(meta.supportsStatementPooling());
        assertTrue(meta.supportsNamedParameters());
        assertTrue(meta.supportsStoredFunctionsUsingCallSyntax());
        assertTrue(meta.supportsRefCursors());
        assertTrue(meta.supportsSharding());
        assertEquals(1073741823L, meta.getMaxLogicalLobSize());
        assertTrue(meta.generatedKeyAlwaysReturned());
        assertTrue(meta.supportsResultSetType(ResultSet.TYPE_SCROLL_INSENSITIVE));
        assertTrue(meta.supportsResultSetType(ResultSet.TYPE_SCROLL_SENSITIVE));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_SENSITIVE, ResultSet.CONCUR_READ_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_SCROLL_SENSITIVE, ResultSet.CONCUR_UPDATABLE));
        assertTrue(meta.othersUpdatesAreVisible(ResultSet.TYPE_SCROLL_SENSITIVE));
        assertTrue(meta.othersDeletesAreVisible(ResultSet.TYPE_SCROLL_SENSITIVE));
        assertTrue(meta.othersInsertsAreVisible(ResultSet.TYPE_SCROLL_SENSITIVE));
        assertFalse(meta.othersUpdatesAreVisible(ResultSet.TYPE_SCROLL_INSENSITIVE));
        assertFalse(meta.othersDeletesAreVisible(ResultSet.TYPE_SCROLL_INSENSITIVE));
        assertFalse(meta.othersInsertsAreVisible(ResultSet.TYPE_SCROLL_INSENSITIVE));
    }

    @Test
    void keepsKnownSupportedCapabilities() throws SQLException {
        SBDatabaseMetaData meta = new SBDatabaseMetaData(null);

        assertTrue(meta.supportsBatchUpdates());
        assertTrue(meta.supportsSavepoints());
        assertTrue(meta.supportsGetGeneratedKeys());
        assertTrue(meta.supportsOpenCursorsAcrossCommit());
        assertTrue(meta.supportsOpenCursorsAcrossRollback());
        assertTrue(meta.supportsOpenStatementsAcrossCommit());
        assertTrue(meta.supportsOpenStatementsAcrossRollback());
        assertTrue(meta.supportsResultSetType(ResultSet.TYPE_FORWARD_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY));
        assertTrue(meta.supportsResultSetConcurrency(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_UPDATABLE));
        assertTrue(meta.supportsResultSetHoldability(ResultSet.HOLD_CURSORS_OVER_COMMIT));
        assertTrue(meta.supportsResultSetHoldability(ResultSet.CLOSE_CURSORS_AT_COMMIT));
        assertTrue(meta.getRowIdLifetime() == RowIdLifetime.ROWID_VALID_OTHER);
    }

    @Test
    void exposesDeterministicIdentifierCaseAndNullOrderingCapabilities() throws Exception {
        SBDatabaseMetaData defaults = new SBDatabaseMetaData(null);
        assertTrue(defaults.nullsAreSortedHigh());
        assertTrue(defaults.nullsAreSortedAtEnd());
        assertFalse(defaults.nullsAreSortedLow());
        assertFalse(defaults.nullsAreSortedAtStart());

        assertFalse(defaults.supportsMixedCaseIdentifiers());
        assertFalse(defaults.storesUpperCaseIdentifiers());
        assertTrue(defaults.storesLowerCaseIdentifiers());
        assertFalse(defaults.storesMixedCaseIdentifiers());
        assertFalse(defaults.storesUpperCaseQuotedIdentifiers());
        assertFalse(defaults.storesLowerCaseQuotedIdentifiers());
        assertTrue(defaults.storesMixedCaseQuotedIdentifiers());

        StubProtocol overriddenProtocol = new StubProtocol(Map.of(
            "identifier_case", "mixed",
            "quoted_identifier_case", "upper"
        ));
        SBDatabaseMetaData overridden = new SBDatabaseMetaData(newConnection(overriddenProtocol));
        assertTrue(overridden.supportsMixedCaseIdentifiers());
        assertFalse(overridden.storesUpperCaseIdentifiers());
        assertFalse(overridden.storesLowerCaseIdentifiers());
        assertTrue(overridden.storesMixedCaseIdentifiers());
        assertTrue(overridden.storesUpperCaseQuotedIdentifiers());
        assertFalse(overridden.storesLowerCaseQuotedIdentifiers());
        assertFalse(overridden.storesMixedCaseQuotedIdentifiers());
    }

    @Test
    void exposesClientInfoPropertiesForDriverAndToolingDiscovery() throws SQLException {
        SBDatabaseMetaData meta = new SBDatabaseMetaData(null);

        try (ResultSet rs = meta.getClientInfoProperties()) {
            Map<String, Integer> maxLenByName = new HashMap<>();
            while (rs.next()) {
                maxLenByName.put(rs.getString("NAME"), rs.getInt("MAX_LEN"));
                assertTrue(rs.getString("DESCRIPTION") != null && !rs.getString("DESCRIPTION").isBlank());
            }

            assertEquals(5, maxLenByName.size());
            assertEquals(1024, maxLenByName.get("ApplicationName"));
            assertEquals(256, maxLenByName.get("ClientUser"));
            assertEquals(256, maxLenByName.get("ClientHostname"));
            assertEquals(32, maxLenByName.get("ClientPid"));
            assertEquals(256, maxLenByName.get("TraceTag"));
        }
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
