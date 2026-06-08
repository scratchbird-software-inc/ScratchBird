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
import java.sql.JDBCType;
import java.sql.ResultSet;
import java.sql.SQLFeatureNotSupportedException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import sun.misc.Unsafe;

import org.junit.jupiter.api.Test;

public class SBResultSetUpdatableTest {

    @Test
    public void updateInsertDeletePathsIssueMutatingSqlAndUpdateRowBuffer() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, name FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {1, "old"});
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());
        rs.updateString("name", "new");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertEquals("new", rs.getString("name"));

        rs.moveToInsertRow();
        rs.updateInt(1, 2);
        rs.updateString(2, "inserted");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertEquals(2, rs.getInt(1));
        assertEquals("inserted", rs.getString(2));

        assertTrue(rs.absolute(1));
        rs.deleteRow();
        assertTrue(rs.rowDeleted());
        assertFalse(rs.isAfterLast());

        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("UPDATE demo")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("INSERT INTO demo")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("DELETE FROM demo")));
    }

    @Test
    public void cancelRowUpdatesRestoresOriginalValues() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, name FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before"})));

        assertTrue(rs.next());
        rs.updateString("name", "after");
        assertEquals("after", rs.getString("name"));
        rs.cancelRowUpdates();
        assertEquals("before", rs.getString("name"));
    }

    @Test
    public void forwardOnlyStreamingResultSetSupportsMutationsWhenStatementRequestsUpdatable() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, name FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        SBResultSet rs = new SBResultSet(statement,
            new SingleRowStream(columns, new Object[] {1, "stream-old"}), 0);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("name", "stream-new");
        rs.updateRow();
        assertTrue(rs.rowUpdated());

        rs.moveToInsertRow();
        rs.updateInt(1, 2);
        rs.updateString(2, "stream-inserted");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertEquals("stream-inserted", rs.getString(2));

        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("UPDATE demo")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("INSERT INTO demo")));
    }

    @Test
    public void streamingResultSetBecomesUpdatableAfterDeferredColumnMetadataArrives() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, name FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        SBResultSet rs = new SBResultSet(statement,
            new DeferredColumnsSingleRowStream(columns, new Object[] {1, "before"}), 0);

        // SQL fallback can resolve the base table before deferred metadata is available.
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        assertTrue(rs.next());
        // Once the stream exposes columns, the update target resolves and server mutations are enabled.
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("name", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());

        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("UPDATE demo")));
    }

    @Test
    public void metadataResolvedTargetSupportsAliasedColumnsAndRejectsDerivedColumnMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT d.id AS identifier, d.payload AS payload_alias, d.payload || '-x' AS derived "
                + "FROM public.meta_demo d WHERE d.id = 1";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo identifier = new SBColumnInfo();
        identifier.setName("identifier");
        identifier.setTableOid(4242);
        identifier.setColumnNumber((short) 1);
        columns.add(identifier);

        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload_alias");
        payload.setTableOid(4242);
        payload.setColumnNumber((short) 2);
        columns.add(payload);

        SBColumnInfo derived = new SBColumnInfo();
        derived.setName("derived");
        derived.setTableOid(0);
        derived.setColumnNumber((short) 0);
        columns.add(derived);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {1, "before", "before-x"});
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());
        rs.updateString("payload_alias", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());

        assertThrows(SQLFeatureNotSupportedException.class, () -> rs.updateString("derived", "derived-local"));

        rs.moveToInsertRow();
        rs.updateInt("identifier", 2);
        rs.updateString("payload_alias", "inserted");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertEquals("inserted", rs.getString("payload_alias"));

        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"public\".\"meta_demo\"")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("INSERT INTO \"public\".\"meta_demo\"")));
        assertTrue(protocol.executedSql.stream().noneMatch(sql ->
            sql.contains("\"derived\"")));
    }

    @Test
    public void sqlFallbackProjectionMappingUsesSourceColumnsAndRejectsDerivedMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT id AS identifier, payload AS payload_alias, payload || '-x' AS derived FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo identifier = new SBColumnInfo();
        identifier.setName("identifier");
        columns.add(identifier);

        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload_alias");
        columns.add(payload);

        SBColumnInfo derived = new SBColumnInfo();
        derived.setName("derived");
        columns.add(derived);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {1, "before", "before-x"});
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload_alias", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.contains("\"payload\" = 'after'")));

        assertThrows(SQLFeatureNotSupportedException.class, () -> rs.updateString("derived", "derived-local"));
    }

    @Test
    public void metadataTargetUsesDominantBaseTableWhenResultSetIncludesSecondaryTableColumns() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT l.id AS left_id, l.payload AS left_payload, "
                + "r.id AS right_id, r.payload AS right_payload "
                + "FROM public.left_demo l JOIN public.right_demo r ON r.id = l.id";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        leftId.setTableOid(100);
        leftId.setColumnNumber((short) 1);
        columns.add(leftId);

        SBColumnInfo leftPayload = new SBColumnInfo();
        leftPayload.setName("left_payload");
        leftPayload.setTableOid(100);
        leftPayload.setColumnNumber((short) 2);
        columns.add(leftPayload);

        SBColumnInfo rightId = new SBColumnInfo();
        rightId.setName("right_id");
        rightId.setTableOid(200);
        rightId.setColumnNumber((short) 1);
        columns.add(rightId);

        SBColumnInfo rightPayload = new SBColumnInfo();
        rightPayload.setName("right_payload");
        rightPayload.setTableOid(200);
        rightPayload.setColumnNumber((short) 2);
        columns.add(rightPayload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "left-before", 1, "right-before"})));

        assertTrue(rs.next());
        rs.updateString("left_payload", "left-after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("UPDATE \"public\".\"left_demo\"") || sql.startsWith("UPDATE left_demo"))
                && sql.contains("'left-after'")), "executed SQL: " + protocol.executedSql);

        rs.updateString("right_payload", "right-after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("UPDATE \"public\".\"right_demo\"") || sql.startsWith("UPDATE right_demo"))
                && sql.contains("'right-after'")), "executed SQL: " + protocol.executedSql);

        rs.updateString("left_payload", "left-mixed");
        rs.updateString("right_payload", "right-mixed");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("UPDATE \"public\".\"left_demo\"") || sql.startsWith("UPDATE left_demo"))
                && sql.contains("'left-mixed'")), "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("UPDATE \"public\".\"right_demo\"") || sql.startsWith("UPDATE right_demo"))
                && sql.contains("'right-mixed'")), "executed SQL: " + protocol.executedSql);

        rs.moveToInsertRow();
        rs.updateInt("left_id", 2);
        rs.updateString("left_payload", "left-insert");
        rs.updateInt("right_id", 2);
        rs.updateString("right_payload", "right-insert");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("INSERT INTO \"public\".\"left_demo\"") || sql.startsWith("INSERT INTO left_demo"))
                && sql.contains("'left-insert'")), "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("INSERT INTO \"public\".\"right_demo\"") || sql.startsWith("INSERT INTO right_demo"))
                && sql.contains("'right-insert'")), "executed SQL: " + protocol.executedSql);

        assertTrue(rs.absolute(1));
        rs.deleteRow();
        assertTrue(rs.rowDeleted());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("DELETE FROM \"public\".\"left_demo\"") || sql.startsWith("DELETE FROM left_demo"))
                && sql.contains("'left-mixed'")), "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            (sql.startsWith("DELETE FROM \"public\".\"right_demo\"") || sql.startsWith("DELETE FROM right_demo"))
                && sql.contains("'right-mixed'")), "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackWithoutWritableProjectionColumnsIsReadOnlyAndDoesNotEmitServerSql() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT payload || '-x' AS derived FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo derived = new SBColumnInfo();
        derived.setName("derived");
        columns.add(derived);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {"before-x"});
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_READ_ONLY, rs.getConcurrency());
        assertThrows(SQLFeatureNotSupportedException.class, () -> rs.updateString("derived", "changed"));
        assertThrows(SQLFeatureNotSupportedException.class, rs::moveToInsertRow);
        assertTrue(protocol.executedSql.stream().noneMatch(sql ->
            sql.startsWith("UPDATE ") || sql.startsWith("INSERT ") || sql.startsWith("DELETE ")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void expressionAliasMatchingBaseColumnUsesCatalogFallbackForServerMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT payload || '' AS payload FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {"before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE demo") && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackJoinProjectionMapsPerAliasWithoutMetadataOids() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT l.id AS left_id, l.payload AS left_payload, "
                + "r.id AS right_id, r.payload AS right_payload "
                + "FROM left_demo l JOIN right_demo r ON r.id = l.id";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        columns.add(leftId);
        SBColumnInfo leftPayload = new SBColumnInfo();
        leftPayload.setName("left_payload");
        columns.add(leftPayload);
        SBColumnInfo rightId = new SBColumnInfo();
        rightId.setName("right_id");
        columns.add(rightId);
        SBColumnInfo rightPayload = new SBColumnInfo();
        rightPayload.setName("right_payload");
        columns.add(rightPayload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "left-old", 1, "right-old"})));

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("left_payload", "left-new");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE left_demo") && sql.contains("\"payload\" = 'left-new'")),
            "executed SQL: " + protocol.executedSql);

        rs.updateString("right_payload", "right-new");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("\"payload\" = 'right-new'")),
            "executed SQL: " + protocol.executedSql);

        rs.moveToInsertRow();
        rs.updateInt("left_id", 2);
        rs.updateString("left_payload", "left-ins");
        rs.updateInt("right_id", 2);
        rs.updateString("right_payload", "right-ins");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("INSERT INTO left_demo") && sql.contains("'left-ins'")),
            "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("INSERT INTO right_demo") && sql.contains("'right-ins'")),
            "executed SQL: " + protocol.executedSql);

        assertTrue(rs.absolute(1));
        rs.deleteRow();
        assertTrue(rs.rowDeleted());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("DELETE FROM left_demo")),
            "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("DELETE FROM right_demo")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackJoinMappingHandlesAsAliasesAndTrailingClauses() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT l.id AS left_id, r.payload AS right_payload "
                + "FROM left_demo AS l LEFT JOIN right_demo AS r ON r.id = l.id "
                + "WHERE l.id > 0 ORDER BY l.id LIMIT 10";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        columns.add(leftId);
        SBColumnInfo rightPayload = new SBColumnInfo();
        rightPayload.setName("right_payload");
        columns.add(rightPayload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "right-old"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("right_payload", "right-updated");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("'right-updated'")),
            "executed SQL: " + protocol.executedSql);

        rs.updateInt("left_id", 7);
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE left_demo") && sql.contains("\"id\" = 7")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackJoinMappingWithoutAliasesIgnoresJoinModifiers() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT left_demo.id AS left_id, right_demo.payload AS right_payload "
                + "FROM left_demo LEFT JOIN right_demo ON right_demo.id = left_demo.id "
                + "WHERE left_demo.id > 0";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo leftId = new SBColumnInfo();
        leftId.setName("left_id");
        columns.add(leftId);
        SBColumnInfo rightPayload = new SBColumnInfo();
        rightPayload.setName("right_payload");
        columns.add(rightPayload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "right-old"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("right_payload", "right-updated");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("'right-updated'")),
            "executed SQL: " + protocol.executedSql);

        rs.updateInt("left_id", 5);
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE left_demo") && sql.contains("\"id\" = 5")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackPrimaryTableSkipsSubqueryReferencesWhenChoosingDefaultTarget() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT payload FROM (SELECT 1 AS x) s JOIN right_demo ON true";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {"before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackSupportsRecursiveQualifiedSchemaTargets() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT r.payload AS payload_alias "
                + "FROM \"emulated\".\"mysql\".\"mymain\".\"left_demo\" AS r";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload_alias");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {"before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload_alias", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"mysql\".\"mymain\".\"left_demo\"")
                && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackSupportsOnlyQualifiedTableReferences() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT d.payload FROM ONLY \"emulated\".\"postgresql\".\"sb\".\"demo\" d";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {"before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"postgresql\".\"sb\".\"demo\"")
                && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackQualifiedStarMapsColumnsToSelectedAliasTable() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT r.* FROM left_demo l JOIN right_demo r ON r.id = l.id";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().noneMatch(sql -> sql.startsWith("UPDATE left_demo")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackQualifiedStarMapsColumnsToQuotedAliasTable() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT \"R\".* FROM left_demo l JOIN right_demo \"R\" ON \"R\".id = l.id";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before-quoted"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after-quoted");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE right_demo") && sql.contains("\"payload\" = 'after-quoted'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackResolvesFullRecursiveQualifierWhenTableNamesRepeat() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT \"emulated\".\"mysql\".\"mymain\".\"left_demo\".payload AS payload_a, "
                + "\"emulated\".\"mysql\".\"mymain1\".\"left_demo\".payload AS payload_b "
                + "FROM \"emulated\".\"mysql\".\"mymain\".\"left_demo\" "
                + "JOIN \"emulated\".\"mysql\".\"mymain1\".\"left_demo\" ON true";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo payloadA = new SBColumnInfo();
        payloadA.setName("payload_a");
        columns.add(payloadA);
        SBColumnInfo payloadB = new SBColumnInfo();
        payloadB.setName("payload_b");
        columns.add(payloadB);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {"a-before", "b-before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload_a", "a-after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"mysql\".\"mymain\".\"left_demo\"")
                && sql.contains("\"payload\" = 'a-after'")),
            "executed SQL: " + protocol.executedSql);

        rs.updateString("payload_b", "b-after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"mysql\".\"mymain1\".\"left_demo\"")
                && sql.contains("\"payload\" = 'b-after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlFallbackQualifiedStarUsesFullRecursiveQualifierWhenTableNamesRepeat() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql =
            "SELECT \"emulated\".\"mysql\".\"mymain1\".\"left_demo\".* "
                + "FROM \"emulated\".\"mysql\".\"mymain\".\"left_demo\" "
                + "JOIN \"emulated\".\"mysql\".\"mymain1\".\"left_demo\" ON true";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"mysql\".\"mymain1\".\"left_demo\"")
                && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
        assertTrue(protocol.executedSql.stream().noneMatch(sql ->
            sql.startsWith("UPDATE \"emulated\".\"mysql\".\"mymain\".\"left_demo\"")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void sqlTypeUpdateObjectOverloadsMutateRowsAndRejectNullSqlType() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT id, name FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before"})));

        assertTrue(rs.next());
        rs.updateObject(2, "typed-index", JDBCType.VARCHAR);
        rs.updateRow();
        assertEquals("typed-index", rs.getString("name"));

        rs.updateObject("name", "typed-label", JDBCType.VARCHAR, 0);
        rs.updateRow();
        assertEquals("typed-label", rs.getString("name"));

        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.contains("\"name\" = 'typed-index'") || sql.contains("\"name\" = 'typed-label'")));
        assertThrows(java.sql.SQLException.class, () -> rs.updateObject(2, "bad", (java.sql.SQLType) null));
        assertThrows(java.sql.SQLException.class, () -> rs.updateObject("name", "bad", (java.sql.SQLType) null));
    }

    @Test
    public void nestedSimpleSubqueryResolvesBaseTableForServerSideUpdatableMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT t.id, t.name FROM (SELECT id, name FROM demo) t";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo name = new SBColumnInfo();
        name.setName("name");
        columns.add(name);

        List<Object[]> rows = new ArrayList<>();
        rows.add(new Object[] {2, "OLD"});
        SBResultSet rs = new SBResultSet(statement, columns, rows);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("name", "NEW");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertEquals("NEW", rs.getString("name"));

        rs.moveToInsertRow();
        rs.updateInt("id", 3);
        rs.updateString("name", "INSERTED");
        rs.insertRow();
        assertTrue(rs.rowInserted());
        assertEquals("INSERTED", rs.getString("name"));

        assertTrue(rs.absolute(1));
        rs.deleteRow();
        assertTrue(rs.rowDeleted());

        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("UPDATE demo")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("INSERT INTO demo")));
        assertTrue(protocol.executedSql.stream().anyMatch(sql -> sql.startsWith("DELETE FROM demo")));
    }

    @Test
    public void withCteProjectionResolvesBaseTableForServerSideMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "WITH base AS (SELECT id, payload FROM demo) SELECT id, payload FROM base";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {1, "before"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE demo") && sql.contains("\"payload\" = 'after'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void nestedWithCteProjectionResolvesTransitiveBaseTableForMutations() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_SCROLL_INSENSITIVE,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "WITH base AS (SELECT id, payload FROM demo), "
            + "projected AS (SELECT id, payload FROM base) "
            + "SELECT id, payload FROM projected";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo id = new SBColumnInfo();
        id.setName("id");
        columns.add(id);
        SBColumnInfo payload = new SBColumnInfo();
        payload.setName("payload");
        columns.add(payload);

        SBResultSet rs = new SBResultSet(statement, columns,
            new ArrayList<>(Collections.singletonList(new Object[] {2, "before-nested"})));
        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_UPDATABLE, rs.getConcurrency());

        rs.updateString("payload", "after-nested");
        rs.updateRow();
        assertTrue(rs.rowUpdated());
        assertTrue(protocol.executedSql.stream().anyMatch(sql ->
            sql.startsWith("UPDATE demo") && sql.contains("\"payload\" = 'after-nested'")),
            "executed SQL: " + protocol.executedSql);
    }

    @Test
    public void streamingResultSetIsReadOnlyWhenBaseTableCannotBeResolved() throws Exception {
        CaptureMutationProtocol protocol = new CaptureMutationProtocol();
        SBConnection connection = newConnectionForTest(protocol);
        SBStatement statement = new SBStatement(connection, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_UPDATABLE, ResultSet.CLOSE_CURSORS_AT_COMMIT);
        statement.lastExecutedSql = "SELECT payload || '-x' AS derived FROM demo";

        List<SBColumnInfo> columns = new ArrayList<>();
        SBColumnInfo derived = new SBColumnInfo();
        derived.setName("derived");
        columns.add(derived);

        SBResultSet rs = new SBResultSet(statement,
            new SingleRowStream(columns, new Object[] {"before-x"}), 0);

        assertTrue(rs.next());
        assertEquals(ResultSet.CONCUR_READ_ONLY, rs.getConcurrency());
        assertThrows(SQLFeatureNotSupportedException.class, () -> rs.updateString("derived", "after-x"));
        assertThrows(SQLFeatureNotSupportedException.class, rs::moveToInsertRow);
        assertTrue(protocol.executedSql.stream().noneMatch(sql ->
            sql.startsWith("UPDATE ") || sql.startsWith("INSERT ") || sql.startsWith("DELETE ")),
            "executed SQL: " + protocol.executedSql);
    }

    private static SBConnection newConnectionForTest(SBProtocolHandler protocol) throws Exception {
        SBConnection connection = (SBConnection) getUnsafe().allocateInstance(SBConnection.class);
        setField(connection, "protocol", protocol);
        setField(connection, "properties", new SBConnectionProperties());
        setField(connection, "closed", new java.util.concurrent.atomic.AtomicBoolean(false));
        setField(connection, "circuitBreaker", new CircuitBreaker());
        setField(connection, "telemetry", new TelemetryCollector());
        setField(connection, "readOnly", false);
        setField(connection, "autoCommit", true);
        setField(connection, "schema", "public");
        return connection;
    }

    private static Unsafe getUnsafe() throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        return (Unsafe) field.get(null);
    }

    private static void setField(Object object, String fieldName, Object value) throws Exception {
        Field field = SBConnection.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static final class CaptureMutationProtocol extends SBProtocolHandler {
        private final List<String> executedSql = new ArrayList<>();

        CaptureMutationProtocol() {
            super(new SBConnectionProperties());
        }

        @Override
        public SBQueryResult execute(String sql, int maxRows, int timeoutMs) {
            executedSql.add(sql);
            SBQueryResult result = new SBQueryResult();
            result.setUpdateCount(1);
            if (sql.contains("FROM information_schema.columns")) {
                result.setColumns(Collections.singletonList(new SBColumnInfo()));
                result.setRows(Arrays.asList(
                    new Object[] {"id"},
                    new Object[] {"payload"},
                    new Object[] {"name"}
                ));
                return result;
            }
            if (sql.contains("FROM pg_catalog.pg_class")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                int oid = extractNumericSuffix(sql, "WHERE c.oid = ");
                String tableName = switch (oid) {
                    case 100 -> "left_demo";
                    case 200 -> "right_demo";
                    default -> "meta_demo";
                };
                result.setRows(Collections.singletonList(new Object[] {"public", tableName}));
                return result;
            }
            if (sql.contains("FROM pg_catalog.pg_attribute")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                result.setRows(Arrays.asList(
                    new Object[] {1, "id"},
                    new Object[] {2, "payload"}
                ));
                return result;
            }
            if (sql.startsWith("SELECT") && sql.contains("LIMIT 1")) {
                result.setColumns(Arrays.asList(new SBColumnInfo(), new SBColumnInfo()));
                result.setRows(Collections.singletonList(new Object[] {1, "refreshed"}));
            }
            return result;
        }

        private static int extractNumericSuffix(String sql, String marker) {
            int markerIndex = sql.indexOf(marker);
            if (markerIndex < 0) {
                return -1;
            }
            int start = markerIndex + marker.length();
            int end = start;
            while (end < sql.length() && Character.isDigit(sql.charAt(end))) {
                end++;
            }
            if (end <= start) {
                return -1;
            }
            try {
                return Integer.parseInt(sql.substring(start, end));
            } catch (NumberFormatException ex) {
                return -1;
            }
        }
    }

    private static final class SingleRowStream implements SBRowStream {
        private final List<SBColumnInfo> columns;
        private final Object[] row;
        private boolean consumed;

        private SingleRowStream(List<SBColumnInfo> columns, Object[] row) {
            this.columns = columns;
            this.row = row;
        }

        @Override
        public Object[] nextRow() {
            if (consumed) {
                return null;
            }
            consumed = true;
            return row.clone();
        }

        @Override
        public List<SBColumnInfo> getColumns() {
            return columns;
        }

        @Override
        public long getUpdateCount() {
            return -1;
        }

        @Override
        public String getCommandTag() {
            return "SELECT";
        }

        @Override
        public boolean isDone() {
            return consumed;
        }
    }

    private static final class DeferredColumnsSingleRowStream implements SBRowStream {
        private final List<SBColumnInfo> columns;
        private final Object[] row;
        private boolean consumed;
        private boolean columnsReady;

        private DeferredColumnsSingleRowStream(List<SBColumnInfo> columns, Object[] row) {
            this.columns = columns;
            this.row = row;
        }

        @Override
        public Object[] nextRow() {
            if (consumed) {
                return null;
            }
            columnsReady = true;
            consumed = true;
            return row.clone();
        }

        @Override
        public List<SBColumnInfo> getColumns() {
            if (!columnsReady) {
                return Collections.emptyList();
            }
            return columns;
        }

        @Override
        public long getUpdateCount() {
            return -1;
        }

        @Override
        public String getCommandTag() {
            return "SELECT";
        }

        @Override
        public boolean isDone() {
            return consumed;
        }
    }
}
