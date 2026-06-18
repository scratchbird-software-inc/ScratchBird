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

import java.sql.*;
import java.util.*;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * JDBC Statement implementation for ScratchBird.
 */
public class SBStatement implements Statement {

    private static final Logger LOGGER = Logger.getLogger(SBStatement.class.getName());
    private static final ExecutorService ASYNC_EXECUTOR =
        Executors.newCachedThreadPool(r -> {
            Thread t = new Thread(r, "sb-jdbc-stmt");
            t.setDaemon(true);
            return t;
        });
    private static final ScheduledExecutorService ASYNC_TIMEOUT_SCHEDULER =
        Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "sb-jdbc-stmt-timeout");
            t.setDaemon(true);
            return t;
        });
    private static final Pattern POSITIONED_UPDATE_PATTERN = Pattern.compile(
        "(?is)^\\s*update\\s+(.+?)\\s+set\\s+(.+?)\\s+where\\s+current\\s+of\\s+([a-zA-Z_][a-zA-Z0-9_$\"]*)\\s*;?\\s*$");
    private static final Pattern POSITIONED_DELETE_PATTERN = Pattern.compile(
        "(?is)^\\s*delete\\s+from\\s+(.+?)\\s+where\\s+current\\s+of\\s+([a-zA-Z_][a-zA-Z0-9_$\"]*)\\s*;?\\s*$");
    private static final Pattern SIMPLE_IDENTIFIER_PATTERN = Pattern.compile("[A-Za-z_][A-Za-z0-9_$]*");
    private static final Set<String> RESERVED_IDENTIFIER_WORDS = Set.of(
        "select", "from", "where", "group", "order", "by", "having", "join", "inner", "left", "right",
        "full", "cross", "on", "insert", "update", "delete", "into", "values", "table", "view", "index",
        "create", "alter", "drop", "truncate", "grant", "revoke", "user", "role", "schema", "database",
        "primary", "foreign", "key", "constraint", "null", "not", "default", "check", "unique", "distinct",
        "union", "intersect", "except", "limit", "offset", "fetch", "case", "when", "then", "else", "end",
        "true", "false", "and", "or"
    );
    private static final AtomicLong CURSOR_SEQUENCE = new AtomicLong(0);

    // Parent connection
    protected final SBConnection connection;

    // ResultSet properties
    protected final int resultSetType;
    protected final int resultSetConcurrency;
    protected final int resultSetHoldability;

    // Statement state
    protected final AtomicBoolean closed = new AtomicBoolean(false);
    protected boolean poolable = false;
    protected boolean closeOnCompletion = false;
    protected int maxRows = 0;
    protected long largeMaxRows = 0;
    protected int maxFieldSize = 0;
    protected int queryTimeout = 0;
    protected int fetchDirection = ResultSet.FETCH_FORWARD;
    protected int fetchSize = 0;
    protected String cursorName;

    // Results
    protected SBResultSet currentResultSet;
    protected long updateCount = -1;
    protected boolean moreResults = false;
    protected List<String> batchStatements = new ArrayList<>();

    // Warnings
    protected SQLWarning warnings;

    // Generated keys
    protected SBResultSet generatedKeys;
    protected final Object executionLock = new Object();
    protected String lastExecutedSql;
    protected final Deque<SBQueryResult> pendingResults = new ArrayDeque<>();
    protected final List<SBResultSet> retainedResults = new ArrayList<>();

    /**
     * Creates a new statement.
     */
    public SBStatement(SBConnection connection, int resultSetType,
                       int resultSetConcurrency, int resultSetHoldability) {
        this.connection = connection;
        this.resultSetType = resultSetType;
        this.resultSetConcurrency = resultSetConcurrency;
        this.resultSetHoldability = resultSetHoldability;
        this.fetchSize = connection.getConnectionProperties().getDefaultRowFetchSize();
    }

    @Override
    public ResultSet executeQuery(String sql) throws SQLException {
        synchronized (executionLock) {
            checkClosed();
            clearResults();
            lastExecutedSql = sql;
            int pageSize = fetchSize > 0 ? fetchSize : 0;
            if (maxRows > 0 && pageSize > 0) {
                pageSize = Math.min(pageSize, maxRows);
            }

            if (pageSize > 0 && shouldUseStreamingResultSet()) {
                final int streamPageSize = pageSize;
                SBQueryResult result = connection.withResilience("query_stream", sql, () ->
                    connection.getProtocol().executeStreaming(sql, streamPageSize, queryTimeout * 1000)
                    , true
                );
                if (result.getStream() == null) {
                    throw new SQLException("Query did not return a result set", "02000");
                }
                currentResultSet = new SBResultSet(this, result.getStream(), maxRows);
                bindCurrentResultSetCursor();
                return currentResultSet;
            }

            SBQueryResult result = connection.withResilience("query", sql, () ->
                connection.getProtocol().execute(sql, maxRows, queryTimeout * 1000)
                , true
            );
            if (result.getColumns() == null || result.getColumns().isEmpty()) {
                throw new SQLException("Query did not return a result set", "02000");
            }
            currentResultSet = new SBResultSet(this, result.getColumns(), result.getRows());
            bindCurrentResultSetCursor();
            return currentResultSet;
        }
    }

    public CompletableFuture<ResultSet> executeQueryAsync(String sql) {
        return executeAsyncInternal(() -> executeQuery(sql), queryTimeout)
            .thenApply(result -> result);
    }

    public CompletableFuture<Boolean> executeAsync(String sql) {
        return executeAsyncInternal(() -> execute(sql), queryTimeout)
            .thenApply(result -> result != null && result);
    }

    @Override
    public int executeUpdate(String sql) throws SQLException {
        return (int) executeLargeUpdate(sql);
    }

    public CompletableFuture<Integer> executeUpdateAsync(String sql) {
        return executeAsyncInternal(() -> executeUpdate(sql), queryTimeout);
    }

    public CompletableFuture<Long> executeLargeUpdateAsync(String sql) {
        return executeAsyncInternal(() -> executeLargeUpdate(sql), queryTimeout);
    }

    @Override
    public long executeLargeUpdate(String sql) throws SQLException {
        synchronized (executionLock) {
            checkClosed();
            clearResults();
            lastExecutedSql = sql;
            String effectiveSql = rewritePositionedMutationSql(sql);

            SBQueryResult result = connection.withResilience("update", effectiveSql, () ->
                connection.getProtocol().execute(effectiveSql, maxRows, queryTimeout * 1000)
            );

            if (result.getColumns() != null && !result.getColumns().isEmpty()) {
                throw new SQLException("executeUpdate cannot return a ResultSet", "21000");
            }

            updateCount = result.getUpdateCount();
            return updateCount;
        }
    }

    @Override
    public boolean execute(String sql) throws SQLException {
        return executeInternal(sql, false);
    }

    private boolean executeInternal(String sql, boolean generatedKeysRequested) throws SQLException {
        synchronized (executionLock) {
            checkClosed();
            clearResults();
            lastExecutedSql = sql;

            List<String> statements = splitStatements(sql);
            if (statements.isEmpty()) {
                throw new SQLException("No executable SQL statements", "42601");
            }

            SBQueryResult result = null;
            for (String statementSql : statements) {
                String effectiveSql = rewritePositionedMutationSql(statementSql);
                SBQueryResult segment = connection.withResilience("execute", statementSql, () ->
                    connection.getProtocol().execute(effectiveSql, maxRows, queryTimeout * 1000)
                );
                if (result == null) {
                    result = segment;
                } else {
                    pendingResults.addLast(segment);
                }
            }

            if (result == null) {
                throw new SQLException("No executable SQL statements", "42601");
            }

            moreResults = !pendingResults.isEmpty();

            if (result.getColumns() != null && !result.getColumns().isEmpty()) {
                if (generatedKeysRequested && result.getUpdateCount() >= 0) {
                    generatedKeys = new SBResultSet(this, result.getColumns(), result.getRows());
                    updateCount = result.getUpdateCount();
                    return false;
                }
                currentResultSet = new SBResultSet(this, result.getColumns(), result.getRows());
                bindCurrentResultSetCursor();
                updateCount = -1;
                return true;
            } else {
                updateCount = result.getUpdateCount();
                return false;
            }
        }
    }

    @Override
    public boolean execute(String sql, int autoGeneratedKeys) throws SQLException {
        if (autoGeneratedKeys != Statement.RETURN_GENERATED_KEYS) {
            return execute(sql);
        }
        if (!sql.toUpperCase().contains("RETURNING")) {
            sql = sql + " RETURNING *";
        }
        return executeInternal(sql, true);
    }

    @Override
    public boolean execute(String sql, int[] columnIndexes) throws SQLException {
        // For generated keys by column index
        return execute(sql, Statement.RETURN_GENERATED_KEYS);
    }

    @Override
    public boolean execute(String sql, String[] columnNames) throws SQLException {
        if (columnNames != null && columnNames.length > 0) {
            StringBuilder returning = new StringBuilder(" RETURNING ");
            for (int i = 0; i < columnNames.length; i++) {
                if (i > 0) returning.append(", ");
                returning.append(columnNames[i]);
            }
            if (!sql.toUpperCase().contains("RETURNING")) {
                sql = sql + returning.toString();
            }
            return executeInternal(sql, true);
        }
        return execute(sql);
    }

    @Override
    public int executeUpdate(String sql, int autoGeneratedKeys) throws SQLException {
        execute(sql, autoGeneratedKeys);
        return (int) updateCount;
    }

    @Override
    public int executeUpdate(String sql, int[] columnIndexes) throws SQLException {
        execute(sql, columnIndexes);
        return (int) updateCount;
    }

    @Override
    public int executeUpdate(String sql, String[] columnNames) throws SQLException {
        execute(sql, columnNames);
        return (int) updateCount;
    }

    @Override
    public void close() throws SQLException {
        if (closed.compareAndSet(false, true)) {
            clearResults();
            batchStatements.clear();
        }
    }

    @Override
    public int getMaxFieldSize() throws SQLException {
        checkClosed();
        return maxFieldSize;
    }

    @Override
    public void setMaxFieldSize(int max) throws SQLException {
        checkClosed();
        if (max < 0) {
            throw new SQLException("Max field size must be >= 0", "HY024");
        }
        this.maxFieldSize = max;
    }

    @Override
    public int getMaxRows() throws SQLException {
        checkClosed();
        return maxRows;
    }

    @Override
    public void setMaxRows(int max) throws SQLException {
        checkClosed();
        if (max < 0) {
            throw new SQLException("Max rows must be >= 0", "HY024");
        }
        this.maxRows = max;
        this.largeMaxRows = max;
    }

    @Override
    public long getLargeMaxRows() throws SQLException {
        checkClosed();
        return largeMaxRows;
    }

    @Override
    public void setLargeMaxRows(long max) throws SQLException {
        checkClosed();
        if (max < 0) {
            throw new SQLException("Max rows must be >= 0", "HY024");
        }
        this.largeMaxRows = max;
        this.maxRows = max > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) max;
    }

    @Override
    public void setEscapeProcessing(boolean enable) throws SQLException {
        checkClosed();
        // JDBC escape processing is always enabled
    }

    @Override
    public int getQueryTimeout() throws SQLException {
        checkClosed();
        return queryTimeout;
    }

    @Override
    public void setQueryTimeout(int seconds) throws SQLException {
        checkClosed();
        if (seconds < 0) {
            throw new SQLException("Query timeout must be >= 0", "HY024");
        }
        this.queryTimeout = seconds;
    }

    @Override
    public void cancel() throws SQLException {
        checkClosed();
        connection.cancelQuery();
    }

    private <T> CompletableFuture<T> executeAsyncInternal(SqlTask<T> action, int timeoutSeconds) {
        CompletableFuture<T> future = new CompletableFuture<>();
        java.util.concurrent.atomic.AtomicBoolean done = new java.util.concurrent.atomic.AtomicBoolean(false);
        var worker = ASYNC_EXECUTOR.submit(() -> {
            try {
                T result = action.execute();
                if (done.compareAndSet(false, true)) {
                    future.complete(result);
                }
            } catch (SQLException e) {
                if (done.compareAndSet(false, true)) {
                    future.completeExceptionally(e);
                }
            } catch (Throwable e) {
                if (done.compareAndSet(false, true)) {
                    future.completeExceptionally(new SQLException("Async execution failed", "58000", e));
                }
            }
        });

        Runnable cancelCurrentQuery = () -> {
            if (done.compareAndSet(false, true)) {
                worker.cancel(true);
                try {
                    cancel();
                } catch (SQLException e) {
                    LOGGER.log(Level.WARNING, "Async statement cancel failed", e);
                }
            }
        };

        future.whenComplete((result, error) -> {
            if (error instanceof CancellationException) {
                cancelCurrentQuery.run();
                future.completeExceptionally(error);
            }
        });

        if (timeoutSeconds > 0) {
            long timeoutMs = Math.max(1L, timeoutSeconds) * 1000L;
            ScheduledFuture<?> timeoutTask = ASYNC_TIMEOUT_SCHEDULER.schedule(() -> {
                if (!future.isDone()) {
                    if (done.compareAndSet(false, true)) {
                        worker.cancel(true);
                        try {
                            cancel();
                        } catch (SQLException e) {
                            LOGGER.log(Level.WARNING, "Async timeout cancel failed", e);
                        }
                        future.completeExceptionally(new SQLTimeoutException(
                            "Query timeout exceeded", "57014"));
                    }
                }
            }, timeoutMs, TimeUnit.MILLISECONDS);
            future.whenComplete((r, e) -> timeoutTask.cancel(false));
        }

        return future;
    }

    @FunctionalInterface
    private interface SqlTask<T> {
        T execute() throws SQLException;
    }

    @Override
    public SQLWarning getWarnings() throws SQLException {
        checkClosed();
        return warnings;
    }

    @Override
    public void clearWarnings() throws SQLException {
        checkClosed();
        warnings = null;
    }

    @Override
    public void setCursorName(String name) throws SQLException {
        checkClosed();
        if (currentResultSet != null) {
            unbindCurrentResultSetCursor(currentResultSet);
        }
        this.cursorName = name;
        bindCurrentResultSetCursor();
    }

    @Override
    public ResultSet getResultSet() throws SQLException {
        checkClosed();
        return currentResultSet;
    }

    @Override
    public int getUpdateCount() throws SQLException {
        checkClosed();
        return updateCount > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) updateCount;
    }

    @Override
    public long getLargeUpdateCount() throws SQLException {
        checkClosed();
        return updateCount;
    }

    @Override
    public boolean getMoreResults() throws SQLException {
        return getMoreResults(Statement.CLOSE_CURRENT_RESULT);
    }

    @Override
    public boolean getMoreResults(int current) throws SQLException {
        checkClosed();

        if (current == Statement.CLOSE_ALL_RESULTS) {
            closeRetainedResults();
        }

        if (currentResultSet != null) {
            if (current == Statement.KEEP_CURRENT_RESULT) {
                retainedResults.add(currentResultSet);
            } else {
                currentResultSet.close();
            }
            currentResultSet = null;
        }

        if (pendingResults.isEmpty()) {
            updateCount = -1;
            moreResults = false;
            return false;
        }

        SBQueryResult next = pendingResults.removeFirst();
        moreResults = !pendingResults.isEmpty();
        if (next.getColumns() != null && !next.getColumns().isEmpty()) {
            currentResultSet = new SBResultSet(this, next.getColumns(), next.getRows());
            bindCurrentResultSetCursor();
            updateCount = -1;
            return true;
        }

        updateCount = next.getUpdateCount();
        return false;
    }

    @Override
    public void setFetchDirection(int direction) throws SQLException {
        checkClosed();
        if (direction != ResultSet.FETCH_FORWARD &&
            direction != ResultSet.FETCH_REVERSE &&
            direction != ResultSet.FETCH_UNKNOWN) {
            throw new SQLException("Invalid fetch direction: " + direction, "HY024");
        }
        this.fetchDirection = direction;
    }

    @Override
    public int getFetchDirection() throws SQLException {
        checkClosed();
        return fetchDirection;
    }

    @Override
    public void setFetchSize(int rows) throws SQLException {
        checkClosed();
        if (rows < 0) {
            throw new SQLException("Fetch size must be >= 0", "HY024");
        }
        this.fetchSize = rows;
    }

    @Override
    public int getFetchSize() throws SQLException {
        checkClosed();
        return fetchSize;
    }

    @Override
    public int getResultSetConcurrency() throws SQLException {
        checkClosed();
        return resultSetConcurrency;
    }

    @Override
    public int getResultSetType() throws SQLException {
        checkClosed();
        return resultSetType;
    }

    @Override
    public void addBatch(String sql) throws SQLException {
        checkClosed();
        batchStatements.add(sql);
    }

    @Override
    public void clearBatch() throws SQLException {
        checkClosed();
        batchStatements.clear();
    }

    @Override
    public int[] executeBatch() throws SQLException {
        checkClosed();

        int[] results = new int[batchStatements.size()];
        SQLException firstException = null;

        for (int i = 0; i < batchStatements.size(); i++) {
            try {
                int count = executeUpdate(batchStatements.get(i));
                results[i] = count;
            } catch (SQLException e) {
                results[i] = Statement.EXECUTE_FAILED;
                if (firstException == null) {
                    firstException = e;
                }
            }
        }

        batchStatements.clear();

        if (firstException != null) {
            throw new BatchUpdateException(firstException.getMessage(),
                firstException.getSQLState(), firstException.getErrorCode(),
                results, firstException);
        }

        return results;
    }

    @Override
    public long[] executeLargeBatch() throws SQLException {
        checkClosed();

        long[] results = new long[batchStatements.size()];
        SQLException firstException = null;

        for (int i = 0; i < batchStatements.size(); i++) {
            try {
                long count = executeLargeUpdate(batchStatements.get(i));
                results[i] = count;
            } catch (SQLException e) {
                results[i] = Statement.EXECUTE_FAILED;
                if (firstException == null) {
                    firstException = e;
                }
            }
        }

        batchStatements.clear();

        if (firstException != null) {
            int[] intResults = new int[results.length];
            for (int i = 0; i < results.length; i++) {
                intResults[i] = results[i] > Integer.MAX_VALUE ?
                    Integer.MAX_VALUE : (int) results[i];
            }
            throw new BatchUpdateException(firstException.getMessage(),
                firstException.getSQLState(), firstException.getErrorCode(),
                intResults, firstException);
        }

        return results;
    }

    @Override
    public Connection getConnection() throws SQLException {
        checkClosed();
        return connection;
    }

    @Override
    public ResultSet getGeneratedKeys() throws SQLException {
        checkClosed();
        if (generatedKeys == null) {
            // Return empty result set
            return new SBResultSet(this, Collections.emptyList(), Collections.emptyList());
        }
        return generatedKeys;
    }

    @Override
    public int getResultSetHoldability() throws SQLException {
        checkClosed();
        return resultSetHoldability;
    }

    @Override
    public boolean isClosed() throws SQLException {
        return closed.get();
    }

    @Override
    public void setPoolable(boolean poolable) throws SQLException {
        checkClosed();
        this.poolable = poolable;
    }

    @Override
    public boolean isPoolable() throws SQLException {
        checkClosed();
        return poolable;
    }

    @Override
    public void closeOnCompletion() throws SQLException {
        checkClosed();
        this.closeOnCompletion = true;
    }

    @Override
    public boolean isCloseOnCompletion() throws SQLException {
        checkClosed();
        return closeOnCompletion;
    }

    @Override
    public String enquoteLiteral(String val) throws SQLException {
        checkClosed();
        if (val == null) {
            throw new SQLException("Literal cannot be null", "HY009");
        }
        if (val.indexOf('\0') >= 0) {
            throw new SQLException("Literal contains NUL character", "22021");
        }
        return "'" + val.replace("'", "''") + "'";
    }

    @Override
    public String enquoteIdentifier(String identifier, boolean alwaysQuote) throws SQLException {
        checkClosed();
        if (identifier == null) {
            throw new SQLException("Identifier cannot be null", "HY009");
        }
        String trimmed = identifier.trim();
        if (trimmed.isEmpty()) {
            throw new SQLException("Identifier cannot be empty", "42602");
        }
        if (trimmed.indexOf('\0') >= 0) {
            throw new SQLException("Identifier contains NUL character", "22021");
        }
        if (!alwaysQuote
            && isSimpleIdentifier(trimmed)
            && trimmed.equals(trimmed.toLowerCase(Locale.ROOT))) {
            return trimmed;
        }
        return "\"" + trimmed.replace("\"", "\"\"") + "\"";
    }

    @Override
    public boolean isSimpleIdentifier(String identifier) throws SQLException {
        checkClosed();
        if (identifier == null) {
            return false;
        }
        String trimmed = identifier.trim();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (!SIMPLE_IDENTIFIER_PATTERN.matcher(trimmed).matches()) {
            return false;
        }
        return !RESERVED_IDENTIFIER_WORDS.contains(trimmed.toLowerCase(Locale.ROOT));
    }

    @Override
    public String enquoteNCharLiteral(String val) throws SQLException {
        return "N" + enquoteLiteral(val);
    }

    @Override
    public <T> T unwrap(Class<T> iface) throws SQLException {
        if (iface.isAssignableFrom(getClass())) {
            return iface.cast(this);
        }
        throw new SQLException("Cannot unwrap to " + iface.getName(), "0A000");
    }

    @Override
    public boolean isWrapperFor(Class<?> iface) throws SQLException {
        return iface.isAssignableFrom(getClass());
    }

    // ==================== Protected Methods ====================

    protected void checkClosed() throws SQLException {
        if (closed.get()) {
            throw new SQLException("Statement is closed", "HY010");
        }
        if (connection.isClosed()) {
            throw new SQLException("Connection is closed", "08003");
        }
    }

    protected void clearResults() {
        closeRetainedResults();

        if (currentResultSet != null) {
            unbindCurrentResultSetCursor(currentResultSet);
            try {
                currentResultSet.close();
            } catch (SQLException e) {
                // Ignore
            }
            currentResultSet = null;
        }
        pendingResults.clear();
        updateCount = -1;
        moreResults = false;
        if (generatedKeys != null) {
            try {
                generatedKeys.close();
            } catch (SQLException e) {
                // Ignore generated-keys cleanup errors.
            }
        }
        generatedKeys = null;
    }

    private void closeRetainedResults() {
        if (retainedResults.isEmpty()) {
            return;
        }
        List<SBResultSet> snapshot = new ArrayList<>(retainedResults);
        retainedResults.clear();
        for (SBResultSet retained : snapshot) {
            unbindCurrentResultSetCursor(retained);
            try {
                retained.close();
            } catch (SQLException e) {
                // Ignore retained result cleanup errors.
            }
        }
    }

    protected void bindCurrentResultSetCursor() {
        if (currentResultSet == null) {
            return;
        }
        String effectiveCursorName = cursorName;
        if (effectiveCursorName == null || effectiveCursorName.isBlank()) {
            effectiveCursorName = "sb_cursor_" + CURSOR_SEQUENCE.incrementAndGet();
        }
        currentResultSet.assignCursorName(effectiveCursorName);
        connection.registerNamedCursor(effectiveCursorName, currentResultSet);
    }

    protected void unbindCurrentResultSetCursor(SBResultSet resultSet) {
        if (resultSet == null) {
            return;
        }
        String boundCursorName = resultSet.assignedCursorName();
        if (boundCursorName == null || boundCursorName.isBlank()) {
            return;
        }
        connection.unregisterNamedCursor(boundCursorName, resultSet);
        resultSet.assignCursorName(null);
    }

    void onResultSetClosed(SBResultSet resultSet) {
        if (resultSet == null) {
            return;
        }
        synchronized (executionLock) {
            unbindCurrentResultSetCursor(resultSet);
            if (currentResultSet == resultSet) {
                currentResultSet = null;
            }
            retainedResults.remove(resultSet);
        }
    }

    private String rewritePositionedMutationSql(String sql) throws SQLException {
        if (sql == null || sql.isBlank()) {
            return sql;
        }
        Matcher update = POSITIONED_UPDATE_PATTERN.matcher(sql);
        if (update.matches()) {
            String targetTable = update.group(1).trim();
            String setClause = update.group(2).trim();
            String cursor = update.group(3).trim();
            SBResultSet cursorResult = requireCursorResultSet(cursor);
            String whereClause = cursorResult.positionedWhereClauseForTable(targetTable);
            return "UPDATE " + targetTable + " SET " + setClause + " WHERE " + whereClause;
        }

        Matcher delete = POSITIONED_DELETE_PATTERN.matcher(sql);
        if (delete.matches()) {
            String targetTable = delete.group(1).trim();
            String cursor = delete.group(2).trim();
            SBResultSet cursorResult = requireCursorResultSet(cursor);
            String whereClause = cursorResult.positionedWhereClauseForTable(targetTable);
            return "DELETE FROM " + targetTable + " WHERE " + whereClause;
        }

        return sql;
    }

    private SBResultSet requireCursorResultSet(String cursorName) throws SQLException {
        SBResultSet resultSet = connection.resolveNamedCursor(cursorName);
        if (resultSet == null) {
            throw new SQLException("Cursor not found: " + cursorName, "34000");
        }
        if (resultSet.isClosed()) {
            connection.unregisterNamedCursor(cursorName, resultSet);
            throw new SQLException("Cursor is closed: " + cursorName, "34000");
        }
        return resultSet;
    }

    private List<String> splitStatements(String sql) {
        // Canonical, SET TERM- and comment-aware top-level chunker shared across the
        // driver (see SBSQLParser.splitTopLevelStatements and the cross-driver
        // conformance fixture tests/conformance/drivers/chunker_conformance).
        return SBSQLParser.splitTopLevelStatements(sql);
    }

    protected void addWarning(SQLWarning warning) {
        if (warnings == null) {
            warnings = warning;
        } else {
            warnings.setNextWarning(warning);
        }
    }

    /**
     * Streaming cursors are forward-only. Scroll-insensitive statements must
     * materialize results to preserve absolute/relative cursor semantics.
     * Updatable cursors are also materialized so row mutation semantics remain
     * deterministic instead of depending on protocol streaming state.
     */
    protected boolean shouldUseStreamingResultSet() {
        return resultSetType == ResultSet.TYPE_FORWARD_ONLY
            && resultSetConcurrency == ResultSet.CONCUR_READ_ONLY;
    }

    protected String getLastExecutedSql() {
        return lastExecutedSql;
    }

    /**
     * Checks for close on completion.
     */
    void checkCloseOnCompletion() throws SQLException {
        if (closeOnCompletion && currentResultSet != null && currentResultSet.isClosed()) {
            close();
        }
    }
}
