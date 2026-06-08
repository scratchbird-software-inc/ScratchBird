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

import java.nio.charset.StandardCharsets;
import java.sql.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * JDBC Connection implementation for ScratchBird.
 *
 * <p>This class manages a connection to a ScratchBird database server,
 * providing methods for creating statements, managing transactions,
 * and querying database metadata.</p>
 */
public class SBConnection implements Connection {

    private static final Logger LOGGER = Logger.getLogger(SBConnection.class.getName());

    /** Snapshot isolation level (ScratchBird extension) */
    public static final int TRANSACTION_SNAPSHOT = 5;
    public static final int READ_COMMITTED_MODE_DEFAULT = SBProtocolHandler.READ_COMMITTED_MODE_DEFAULT;
    public static final int READ_COMMITTED_MODE_READ_CONSISTENCY = SBProtocolHandler.READ_COMMITTED_MODE_READ_CONSISTENCY;
    public static final int READ_COMMITTED_MODE_RECORD_VERSION = SBProtocolHandler.READ_COMMITTED_MODE_RECORD_VERSION;
    public static final int READ_COMMITTED_MODE_NO_RECORD_VERSION = SBProtocolHandler.READ_COMMITTED_MODE_NO_RECORD_VERSION;

    @FunctionalInterface
    public interface NotificationListener {
        void onNotification(Notification notification);
    }

    public static final class Notification {
        private final int processId;
        private final String channel;
        private final byte[] payload;
        private final Character changeType;
        private final Long rowId;

        private Notification(int processId, String channel, byte[] payload, Character changeType, Long rowId) {
            this.processId = processId;
            this.channel = channel;
            this.payload = payload == null ? new byte[0] : Arrays.copyOf(payload, payload.length);
            this.changeType = changeType;
            this.rowId = rowId;
        }

        public int getProcessId() {
            return processId;
        }

        public String getChannel() {
            return channel;
        }

        public byte[] getPayload() {
            return Arrays.copyOf(payload, payload.length);
        }

        public String getPayloadText() {
            return new String(payload, StandardCharsets.UTF_8);
        }

        public Character getChangeType() {
            return changeType;
        }

        public Long getRowId() {
            return rowId;
        }
    }

    // Connection properties
    private final SBConnectionProperties properties;
    private final String connectionId;
    private static final AtomicInteger connectionCounter = new AtomicInteger(0);

    // Connection state
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private boolean autoCommit = true;
    private boolean readOnly = false;
    private int transactionIsolation = TRANSACTION_READ_COMMITTED;
    private byte readCommittedMode = (byte) READ_COMMITTED_MODE_DEFAULT;
    private String catalog;
    private String schema;
    private int holdability = ResultSet.HOLD_CURSORS_OVER_COMMIT;
    private Map<String, Class<?>> typeMap = new HashMap<>();
    private ShardingKey shardingKey;
    private ShardingKey superShardingKey;
    private boolean requestScopeActive = false;

    // Network protocol handler
    private SBProtocolHandler protocol;

    // Warnings
    private SQLWarning warnings;

    // Client info
    private Properties clientInfo = new Properties();

    // Savepoint counter
    private int savepointCounter = 0;

    // Resilience and telemetry
    private final CircuitBreaker circuitBreaker = new CircuitBreaker();
    private final TelemetryCollector telemetry = new TelemetryCollector();
    private final KeepaliveManager keepaliveManager = new KeepaliveManager();
    private KeepaliveTracker keepaliveTracker;
    private final Map<String, SBResultSet> namedCursors = new ConcurrentHashMap<>();
    private Queue<Notification> notificationQueue;
    private CopyOnWriteArrayList<NotificationListener> notificationListeners;
    private AtomicBoolean notificationBridgeInstalled;
    private SBProtocolHandler.NotificationListener notificationBridgeListener;

    @FunctionalInterface
    interface SqlSupplier<T> {
        T get() throws SQLException;
    }

    /**
     * Creates a new connection with the given properties.
     *
     * @param props connection properties
     * @throws SQLException if connection fails
     */
    public SBConnection(SBConnectionProperties props) throws SQLException {
        this.properties = props;
        this.connectionId = "conn-" + connectionCounter.incrementAndGet();

        // Initialize from properties
        this.autoCommit = props.isAutoCommit();
        this.readOnly = props.isReadOnly();
        this.schema = normalizeSchemaValue(props.getCurrentSchema());

        if (props.getApplicationName() != null) {
            clientInfo.setProperty("ApplicationName", props.getApplicationName());
        }

        // Connect to server
        connect();

        LOGGER.log(Level.FINE, "Connection {0} established to {1}:{2}/{3}",
            new Object[]{connectionId, props.getHost(), props.getPort(), props.getDatabase()});
    }

    /**
     * Establishes connection to the server.
     */
    private void connect() throws SQLException {
        try {
            properties.setProtocol(properties.getProtocol());
            protocol = new SBProtocolHandler(properties);
            protocol.connect();
            String compression = properties.getCompression();
            if ("zstd".equalsIgnoreCase(compression)) {
                String negotiated = protocol.getServerParameter("compression");
                if (negotiated == null
                    || negotiated.isBlank()
                    || !"zstd".equalsIgnoreCase(negotiated)) {
                    appendWarning(new SQLWarning(
                        "compression=zstd requested but was not negotiated by server; continuing without compression",
                        "01S02"
                    ));
                }
            }

            // Preserve the server-derived current schema unless the caller explicitly overrides it.
            applySchemaSetting(schema);
            protocol.execute("SET AUTOCOMMIT " + (autoCommit ? "ON" : "OFF"));
            if (!autoCommit && !protocol.hasActiveTransaction()) {
                beginManagedTransaction();
            }
            if (schema == null || schema.isBlank()) {
                schema = discoverCurrentSchema();
            }

            catalog = properties.getDatabase();
            keepaliveManager.start();
            keepaliveTracker = keepaliveManager.register(connectionId, this);

        } catch (SQLException e) {
            throw e;
        } catch (Exception e) {
            throw new SQLException("Failed to connect to " + properties.getHost() +
                ":" + properties.getPort() + "/" + properties.getDatabase() +
                ": " + e.getMessage(), "08001", e);
        }
    }

    private void appendWarning(SQLWarning warning) {
        if (warning == null) {
            return;
        }
        if (warnings == null) {
            warnings = warning;
            return;
        }
        warnings.setNextWarning(warning);
    }

    private Queue<Notification> notificationQueue() {
        Queue<Notification> queue = notificationQueue;
        if (queue == null) {
            synchronized (this) {
                queue = notificationQueue;
                if (queue == null) {
                    queue = new ConcurrentLinkedQueue<>();
                    notificationQueue = queue;
                }
            }
        }
        return queue;
    }

    private CopyOnWriteArrayList<NotificationListener> notificationListenerRegistry() {
        CopyOnWriteArrayList<NotificationListener> listeners = notificationListeners;
        if (listeners == null) {
            synchronized (this) {
                listeners = notificationListeners;
                if (listeners == null) {
                    listeners = new CopyOnWriteArrayList<>();
                    notificationListeners = listeners;
                }
            }
        }
        return listeners;
    }

    private AtomicBoolean notificationBridgeFlag() {
        AtomicBoolean bridge = notificationBridgeInstalled;
        if (bridge == null) {
            synchronized (this) {
                bridge = notificationBridgeInstalled;
                if (bridge == null) {
                    bridge = new AtomicBoolean(false);
                    notificationBridgeInstalled = bridge;
                }
            }
        }
        return bridge;
    }

    private SBProtocolHandler.NotificationListener notificationBridgeListener() {
        SBProtocolHandler.NotificationListener bridge = notificationBridgeListener;
        if (bridge == null) {
            synchronized (this) {
                bridge = notificationBridgeListener;
                if (bridge == null) {
                    bridge = this::acceptNotification;
                    notificationBridgeListener = bridge;
                }
            }
        }
        return bridge;
    }

    private void ensureNotificationBridge() throws SQLException {
        SBProtocolHandler handler = protocol;
        if (handler == null) {
            throw new SQLException("Connection protocol is not initialized", "08003");
        }
        AtomicBoolean bridgeInstalled = notificationBridgeFlag();
        if (bridgeInstalled.compareAndSet(false, true)) {
            handler.addNotificationListener(notificationBridgeListener());
        }
    }

    private void acceptNotification(SBProtocolHandler.NotificationMessage message) {
        if (message == null) {
            return;
        }
        Notification notification = new Notification(
            message.processId,
            message.channel,
            message.payload,
            message.changeType,
            message.rowId
        );
        notificationQueue().add(notification);
        CopyOnWriteArrayList<NotificationListener> listeners = notificationListeners;
        if (listeners == null) {
            return;
        }
        for (NotificationListener listener : listeners) {
            try {
                listener.onNotification(notification);
            } catch (RuntimeException ex) {
                LOGGER.log(Level.FINE, "Notification listener failed for connection " + connectionId, ex);
            }
        }
    }

    public void addNotificationListener(NotificationListener listener) throws SQLException {
        checkClosed();
        if (listener == null) {
            throw new SQLException("Notification listener cannot be null", "HY024");
        }
        ensureNotificationBridge();
        CopyOnWriteArrayList<NotificationListener> listeners = notificationListenerRegistry();
        if (!listeners.contains(listener)) {
            listeners.add(listener);
        }
    }

    public boolean removeNotificationListener(NotificationListener listener) throws SQLException {
        checkClosed();
        if (listener == null) {
            return false;
        }
        CopyOnWriteArrayList<NotificationListener> listeners = notificationListeners;
        return listeners != null && listeners.remove(listener);
    }

    public Notification getNotification() throws SQLException {
        checkClosed();
        ensureNotificationBridge();
        return notificationQueue().poll();
    }

    public List<Notification> getNotifications() throws SQLException {
        checkClosed();
        ensureNotificationBridge();
        Queue<Notification> queue = notificationQueue();
        if (queue.isEmpty()) {
            return Collections.emptyList();
        }
        List<Notification> drained = new ArrayList<>();
        Notification notice;
        while ((notice = queue.poll()) != null) {
            drained.add(notice);
        }
        return Collections.unmodifiableList(drained);
    }

    public void clearNotifications() throws SQLException {
        checkClosed();
        ensureNotificationBridge();
        notificationQueue().clear();
    }

    public void listen(String channel) throws SQLException {
        checkClosed();
        ensureNotificationBridge();
        String normalizedChannel = normalizeNotificationChannel(channel);
        String sql = "LISTEN " + quoteIdentifier(normalizedChannel);
        withResilience("listen", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void unlisten(String channel) throws SQLException {
        checkClosed();
        String normalizedChannel = normalizeNotificationChannel(channel);
        String sql = "UNLISTEN " + quoteIdentifier(normalizedChannel);
        withResilience("unlisten", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void unlistenAll() throws SQLException {
        checkClosed();
        final String sql = "UNLISTEN *";
        withResilience("unlisten", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void notifyChannel(String channel) throws SQLException {
        notifyChannel(channel, (String) null);
    }

    public void notifyChannel(String channel, byte[] payload) throws SQLException {
        if (payload == null) {
            notifyChannel(channel, (String) null);
            return;
        }
        notifyChannel(channel, new String(payload, StandardCharsets.UTF_8));
    }

    public void notifyChannel(String channel, String payload) throws SQLException {
        checkClosed();
        String normalizedChannel = normalizeNotificationChannel(channel);
        String sql = "NOTIFY " + quoteIdentifier(normalizedChannel);
        if (payload != null) {
            if (payload.indexOf('\0') >= 0) {
                throw new SQLException("Notification payload cannot contain NUL bytes", "HY024");
            }
            sql += ", " + quoteSqlLiteral(payload);
        }
        final String command = sql;
        withResilience("notify", command, () -> {
            protocol.execute(command);
            return null;
        });
    }

    @Override
    public Statement createStatement() throws SQLException {
        checkClosed();
        return new SBStatement(this, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, holdability);
    }

    @Override
    public Statement createStatement(int resultSetType, int resultSetConcurrency) throws SQLException {
        checkClosed();
        return new SBStatement(this, resultSetType, resultSetConcurrency, holdability);
    }

    @Override
    public Statement createStatement(int resultSetType, int resultSetConcurrency,
                                     int resultSetHoldability) throws SQLException {
        checkClosed();
        return new SBStatement(this, resultSetType, resultSetConcurrency, resultSetHoldability);
    }

    @Override
    public PreparedStatement prepareStatement(String sql) throws SQLException {
        checkClosed();
        return new SBPreparedStatement(this, sql, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, holdability);
    }

    @Override
    public PreparedStatement prepareStatement(String sql, int resultSetType,
                                              int resultSetConcurrency) throws SQLException {
        checkClosed();
        return new SBPreparedStatement(this, sql, resultSetType, resultSetConcurrency, holdability);
    }

    @Override
    public PreparedStatement prepareStatement(String sql, int resultSetType,
                                              int resultSetConcurrency, int resultSetHoldability)
            throws SQLException {
        checkClosed();
        return new SBPreparedStatement(this, sql, resultSetType, resultSetConcurrency,
            resultSetHoldability);
    }

    @Override
    public PreparedStatement prepareStatement(String sql, int autoGeneratedKeys) throws SQLException {
        checkClosed();
        SBPreparedStatement stmt = new SBPreparedStatement(this, sql,
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
        stmt.setReturnGeneratedKeys(autoGeneratedKeys == Statement.RETURN_GENERATED_KEYS);
        return stmt;
    }

    @Override
    public PreparedStatement prepareStatement(String sql, int[] columnIndexes) throws SQLException {
        checkClosed();
        SBPreparedStatement stmt = new SBPreparedStatement(this, sql,
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
        stmt.setGeneratedKeyColumnIndexes(columnIndexes);
        return stmt;
    }

    @Override
    public PreparedStatement prepareStatement(String sql, String[] columnNames) throws SQLException {
        checkClosed();
        SBPreparedStatement stmt = new SBPreparedStatement(this, sql,
            ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, holdability);
        stmt.setGeneratedKeyColumnNames(columnNames);
        return stmt;
    }

    @Override
    public CallableStatement prepareCall(String sql) throws SQLException {
        checkClosed();
        return new SBCallableStatement(this, sql, ResultSet.TYPE_FORWARD_ONLY,
            ResultSet.CONCUR_READ_ONLY, holdability);
    }

    @Override
    public CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency)
            throws SQLException {
        checkClosed();
        return new SBCallableStatement(this, sql, resultSetType, resultSetConcurrency, holdability);
    }

    @Override
    public CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency,
                                         int resultSetHoldability) throws SQLException {
        checkClosed();
        return new SBCallableStatement(this, sql, resultSetType, resultSetConcurrency,
            resultSetHoldability);
    }

    @Override
    public String nativeSQL(String sql) throws SQLException {
        checkClosed();
        // Convert JDBC escape sequences to native SQL
        return SBSQLParser.convertToNativeSQL(sql);
    }

    public boolean supportsPreparedTransactions() {
        return true;
    }

    public boolean supportsDormantReattach() {
        return false;
    }

    public void prepareTransaction(String globalTransactionId) throws SQLException {
        checkClosed();
        String sql = buildPreparedTransactionSql("PREPARE TRANSACTION", globalTransactionId);
        withResilience("prepareTransaction", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void commitPrepared(String globalTransactionId) throws SQLException {
        checkClosed();
        String sql = buildPreparedTransactionSql("COMMIT PREPARED", globalTransactionId);
        withResilience("commitPrepared", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void rollbackPrepared(String globalTransactionId) throws SQLException {
        checkClosed();
        String sql = buildPreparedTransactionSql("ROLLBACK PREPARED", globalTransactionId);
        withResilience("rollbackPrepared", sql, () -> {
            protocol.execute(sql);
            return null;
        });
    }

    public void detachToDormant() throws SQLFeatureNotSupportedException {
        throw new SQLFeatureNotSupportedException(
            "dormant detach/reattach is not yet exposed by the public JDBC driver surface",
            "0A000");
    }

    public void reattachDormant(String dormantId, String authToken) throws SQLFeatureNotSupportedException {
        Objects.requireNonNull(dormantId, "dormantId");
        Objects.requireNonNull(authToken, "authToken");
        throw new SQLFeatureNotSupportedException(
            "dormant detach/reattach is not yet exposed by the public JDBC driver surface",
            "0A000");
    }

    static String buildPreparedTransactionSql(String verb, String globalTransactionId) throws SQLSyntaxErrorException {
        if (globalTransactionId == null || globalTransactionId.trim().isEmpty()) {
            throw new SQLSyntaxErrorException("global transaction id is required", "42601");
        }
        String escaped = globalTransactionId.trim().replace("'", "''");
        return verb + " '" + escaped + "'";
    }

    @Override
    public void setAutoCommit(boolean autoCommit) throws SQLException {
        checkClosed();
        if (this.autoCommit != autoCommit) {
            if (!this.autoCommit && protocol.hasActiveTransaction()) {
                // Commit any pending transaction before changing mode
                commit();
            }
            this.autoCommit = autoCommit;
            protocol.execute("SET AUTOCOMMIT " + (autoCommit ? "ON" : "OFF"));
            if (!autoCommit && !protocol.hasActiveTransaction()) {
                beginManagedTransaction();
            }
        }
    }

    @Override
    public boolean getAutoCommit() throws SQLException {
        checkClosed();
        return autoCommit;
    }

    @Override
    public void commit() throws SQLException {
        checkClosed();
        if (autoCommit) {
            throw new SQLException("Cannot commit when autoCommit is enabled", "25000");
        }
        if (!protocol.hasActiveTransaction()) {
            return;
        }
        protocol.commitTransaction((byte) 0);
    }

    @Override
    public void rollback() throws SQLException {
        checkClosed();
        if (autoCommit) {
            throw new SQLException("Cannot rollback when autoCommit is enabled", "25000");
        }
        if (!protocol.hasActiveTransaction()) {
            return;
        }
        protocol.rollbackTransaction((byte) 0);
    }

    @Override
    public void rollback(Savepoint savepoint) throws SQLException {
        checkClosed();
        if (autoCommit) {
            throw new SQLException("Cannot rollback when autoCommit is enabled", "25000");
        }
        if (savepoint == null) {
            throw new SQLException("Savepoint cannot be null", "HY000");
        }
        String name = ((SBSavepoint) savepoint).getSavepointName();
        protocol.rollbackToSavepoint(name);
    }

    @Override
    public void close() throws SQLException {
        if (closed.compareAndSet(false, true)) {
            try {
                if (!autoCommit) {
                    try {
                        rollback();
                    } catch (SQLException e) {
                        // Ignore rollback errors during close
                    }
                }
                if (protocol != null) {
                    AtomicBoolean bridgeInstalled = notificationBridgeInstalled;
                    SBProtocolHandler.NotificationListener bridgeListener = notificationBridgeListener;
                    if (bridgeInstalled != null && bridgeInstalled.get() && bridgeListener != null) {
                        protocol.removeNotificationListener(bridgeListener);
                    }
                    protocol.close();
                }
                if (keepaliveTracker != null) {
                    keepaliveManager.unregister(connectionId);
                    keepaliveTracker = null;
                }
                keepaliveManager.stop();
                Queue<Notification> pending = notificationQueue;
                if (pending != null) {
                    pending.clear();
                }
                CopyOnWriteArrayList<NotificationListener> listeners = notificationListeners;
                if (listeners != null) {
                    listeners.clear();
                }
            } finally {
                LOGGER.log(Level.FINE, "Connection {0} closed", connectionId);
            }
        }
    }

    @Override
    public boolean isClosed() throws SQLException {
        return closed.get();
    }

    @Override
    public DatabaseMetaData getMetaData() throws SQLException {
        checkClosed();
        return new SBDatabaseMetaData(this);
    }

    @Override
    public void setReadOnly(boolean readOnly) throws SQLException {
        checkClosed();
        this.readOnly = readOnly;
        protocol.execute("SET TRANSACTION READ " + (readOnly ? "ONLY" : "WRITE"));
    }

    @Override
    public boolean isReadOnly() throws SQLException {
        checkClosed();
        return readOnly;
    }

    @Override
    public void setCatalog(String catalog) throws SQLException {
        checkClosed();
        // ScratchBird doesn't support changing database after connection
        // Just store the value, actual database is set at connection time
        this.catalog = catalog;
    }

    @Override
    public String getCatalog() throws SQLException {
        checkClosed();
        return catalog;
    }

    @Override
    public void setTransactionIsolation(int level) throws SQLException {
        checkClosed();
        applyTransactionIsolation(level, null);
    }

    @Override
    public int getTransactionIsolation() throws SQLException {
        checkClosed();
        return transactionIsolation;
    }

    public void setTransactionIsolation(int level, int readCommittedMode) throws SQLException {
        checkClosed();
        applyTransactionIsolation(level, normalizeReadCommittedMode(readCommittedMode));
    }

    public void setReadCommittedMode(int readCommittedMode) throws SQLException {
        checkClosed();
        if (!supportsReadCommittedMode(transactionIsolation)) {
            throw new SQLFeatureNotSupportedException(
                "readCommittedMode requires a READ COMMITTED isolation alias",
                "0A000"
            );
        }
        byte normalized = normalizeReadCommittedMode(readCommittedMode);
        this.readCommittedMode = normalized;
        protocol.execute("SET TRANSACTION ISOLATION LEVEL "
            + buildIsolationLevelSql(transactionIsolation, normalized));
    }

    public int getReadCommittedMode() throws SQLException {
        checkClosed();
        return Byte.toUnsignedInt(readCommittedMode);
    }

    public static String canonicalReadCommittedModeLabel(int readCommittedMode) {
        return SBProtocolHandler.canonicalReadCommittedModeLabel(readCommittedMode);
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
    public Map<String, Class<?>> getTypeMap() throws SQLException {
        checkClosed();
        return new HashMap<>(typeMap);
    }

    @Override
    public void setTypeMap(Map<String, Class<?>> map) throws SQLException {
        checkClosed();
        this.typeMap = new HashMap<>(map);
    }

    @Override
    public void setHoldability(int holdability) throws SQLException {
        checkClosed();
        if (holdability != ResultSet.HOLD_CURSORS_OVER_COMMIT &&
            holdability != ResultSet.CLOSE_CURSORS_AT_COMMIT) {
            throw new SQLException("Invalid holdability: " + holdability, "HY024");
        }
        this.holdability = holdability;
    }

    @Override
    public int getHoldability() throws SQLException {
        checkClosed();
        return holdability;
    }

    @Override
    public Savepoint setSavepoint() throws SQLException {
        checkClosed();
        if (autoCommit) {
            throw new SQLException("Cannot set savepoint when autoCommit is enabled", "25000");
        }
        String name = "sp_" + (++savepointCounter);
        protocol.savepoint(name);
        return new SBSavepoint(savepointCounter, name);
    }

    @Override
    public Savepoint setSavepoint(String name) throws SQLException {
        checkClosed();
        if (autoCommit) {
            throw new SQLException("Cannot set savepoint when autoCommit is enabled", "25000");
        }
        if (name == null || name.isEmpty()) {
            throw new SQLException("Savepoint name cannot be null or empty", "HY000");
        }
        protocol.savepoint(name);
        return new SBSavepoint(0, name);
    }

    @Override
    public void releaseSavepoint(Savepoint savepoint) throws SQLException {
        checkClosed();
        if (savepoint == null) {
            throw new SQLException("Savepoint cannot be null", "HY000");
        }
        String name = ((SBSavepoint) savepoint).getSavepointName();
        protocol.releaseSavepoint(name);
    }

    @Override
    public Clob createClob() throws SQLException {
        checkClosed();
        return new SBClob();
    }

    @Override
    public Blob createBlob() throws SQLException {
        checkClosed();
        return new SBBlob();
    }

    @Override
    public NClob createNClob() throws SQLException {
        checkClosed();
        return new SBNClob();
    }

    @Override
    public SQLXML createSQLXML() throws SQLException {
        checkClosed();
        return new SBSQLXML();
    }

    @Override
    public boolean isValid(int timeout) throws SQLException {
        if (timeout < 0) {
            throw new SQLException("Timeout must be >= 0", "HY024");
        }
        if (closed.get()) {
            return false;
        }
        try {
            return protocol.isAlive(timeout);
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    public void setClientInfo(String name, String value) throws SQLClientInfoException {
        if (closed.get()) {
            Map<String, ClientInfoStatus> failures = new HashMap<>();
            failures.put(name, ClientInfoStatus.REASON_UNKNOWN);
            throw new SQLClientInfoException("Connection is closed", failures);
        }
        if (value != null) {
            clientInfo.setProperty(name, value);
        } else {
            clientInfo.remove(name);
        }
        try {
            if ("ApplicationName".equals(name)) {
                protocol.execute("SET APPLICATION_NAME = '" + (value != null ? value.replace("'", "''") : "") + "'");
            }
        } catch (SQLException e) {
            Map<String, ClientInfoStatus> failures = new HashMap<>();
            failures.put(name, ClientInfoStatus.REASON_UNKNOWN);
            throw new SQLClientInfoException("Failed to set client info", failures, e);
        }
    }

    @Override
    public void setClientInfo(Properties properties) throws SQLClientInfoException {
        if (closed.get()) {
            Map<String, ClientInfoStatus> failures = new HashMap<>();
            for (String key : properties.stringPropertyNames()) {
                failures.put(key, ClientInfoStatus.REASON_UNKNOWN);
            }
            throw new SQLClientInfoException("Connection is closed", failures);
        }
        Map<String, ClientInfoStatus> failures = new HashMap<>();
        for (String key : properties.stringPropertyNames()) {
            try {
                setClientInfo(key, properties.getProperty(key));
            } catch (SQLClientInfoException e) {
                failures.putAll(e.getFailedProperties());
            }
        }
        if (!failures.isEmpty()) {
            throw new SQLClientInfoException("Failed to set some client info properties", failures);
        }
    }

    @Override
    public String getClientInfo(String name) throws SQLException {
        checkClosed();
        return clientInfo.getProperty(name);
    }

    @Override
    public Properties getClientInfo() throws SQLException {
        checkClosed();
        return new Properties(clientInfo);
    }

    @Override
    public Array createArrayOf(String typeName, Object[] elements) throws SQLException {
        checkClosed();
        return new SBArray(typeName, elements);
    }

    @Override
    public Struct createStruct(String typeName, Object[] attributes) throws SQLException {
        checkClosed();
        return new SBStruct(typeName, attributes);
    }

    @Override
    public void setSchema(String schema) throws SQLException {
        checkClosed();
        applySchemaSetting(schema);
    }

    @Override
    public String getSchema() throws SQLException {
        checkClosed();
        return schema;
    }

    @Override
    public void abort(Executor executor) throws SQLException {
        if (executor == null) {
            throw new SQLException("Executor cannot be null", "HY000");
        }
        executor.execute(() -> {
            try {
                if (!closed.get()) {
                    protocol.abort();
                    closed.set(true);
                }
            } catch (Exception e) {
                LOGGER.log(Level.WARNING, "Error during connection abort", e);
            }
        });
    }

    @Override
    public void setNetworkTimeout(Executor executor, int milliseconds) throws SQLException {
        checkClosed();
        if (executor == null) {
            throw new SQLException("Executor cannot be null", "HY000");
        }
        if (milliseconds < 0) {
            throw new SQLException("Network timeout must be >= 0", "HY024");
        }
        protocol.setNetworkTimeout(milliseconds);
    }

    @Override
    public int getNetworkTimeout() throws SQLException {
        checkClosed();
        return protocol.getNetworkTimeout();
    }

    @Override
    public void beginRequest() throws SQLException {
        checkClosed();
        requestScopeActive = true;
    }

    @Override
    public void endRequest() throws SQLException {
        checkClosed();
        requestScopeActive = false;
    }

    public ShardingKeyBuilder createShardingKeyBuilder() throws SQLException {
        checkClosed();
        return new SBShardingKeyBuilder();
    }

    @Override
    public boolean setShardingKeyIfValid(ShardingKey shardingKey, int timeout) throws SQLException {
        checkClosed();
        if (timeout < 0) {
            throw new SQLException("Timeout must be >= 0", "HY024");
        }
        if (shardingKey == null) {
            return false;
        }
        this.shardingKey = shardingKey;
        this.superShardingKey = null;
        return true;
    }

    @Override
    public boolean setShardingKeyIfValid(ShardingKey shardingKey, ShardingKey superShardingKey, int timeout)
            throws SQLException {
        checkClosed();
        if (timeout < 0) {
            throw new SQLException("Timeout must be >= 0", "HY024");
        }
        if (shardingKey == null) {
            return false;
        }
        this.shardingKey = shardingKey;
        this.superShardingKey = superShardingKey;
        return true;
    }

    @Override
    public void setShardingKey(ShardingKey shardingKey) throws SQLException {
        checkClosed();
        if (shardingKey == null) {
            throw new SQLException("Sharding key cannot be null", "HY024");
        }
        this.shardingKey = shardingKey;
        this.superShardingKey = null;
    }

    @Override
    public void setShardingKey(ShardingKey shardingKey, ShardingKey superShardingKey) throws SQLException {
        checkClosed();
        if (shardingKey == null) {
            throw new SQLException("Sharding key cannot be null", "HY024");
        }
        this.shardingKey = shardingKey;
        this.superShardingKey = superShardingKey;
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

    private static final class SBShardingKey implements ShardingKey {
        private final List<KeyPart> parts;

        private SBShardingKey(List<KeyPart> parts) {
            this.parts = Collections.unmodifiableList(new ArrayList<>(parts));
        }
    }

    private static final class KeyPart {
        private final Object value;
        private final int sqlType;

        private KeyPart(Object value, int sqlType) {
            this.value = value;
            this.sqlType = sqlType;
        }
    }

    private static final class SBShardingKeyBuilder implements ShardingKeyBuilder {
        private final List<KeyPart> parts = new ArrayList<>();

        @Override
        public ShardingKeyBuilder subkey(Object subkey, SQLType sqlType) {
            if (sqlType == null) {
                throw new IllegalArgumentException("SQLType cannot be null");
            }
            Integer vendorType = sqlType.getVendorTypeNumber();
            parts.add(new KeyPart(subkey, vendorType != null ? vendorType : java.sql.Types.OTHER));
            return this;
        }

        @Override
        public ShardingKey build() throws SQLException {
            return new SBShardingKey(parts);
        }
    }

    // ==================== ScratchBird Extensions ====================

    /**
     * Cancels the currently executing query.
     *
     * @throws SQLException if cancellation fails
     */
    public void cancelQuery() throws SQLException {
        checkClosed();
        protocol.cancelCurrentQuery();
    }

    /**
     * Gets connection properties.
     *
     * @return connection properties
     */
    public SBConnectionProperties getConnectionProperties() {
        return properties;
    }

    public SBResolvedAuthContext getResolvedAuthContext() {
        return protocol != null
            ? protocol.getResolvedAuthContext()
            : new SBResolvedAuthContext();
    }

    public static SBAuthProbeResult probeAuthSurface(SBConnectionProperties props) throws SQLException {
        if (props == null) {
            throw new SQLException("Connection properties cannot be null", "08001");
        }
        SBProtocolHandler handler = new SBProtocolHandler(props);
        return handler.probeAuthSurface();
    }

    /**
     * Gets the protocol handler.
     *
     * @return protocol handler
     */
    SBProtocolHandler getProtocol() {
        return protocol;
    }

    /**
     * Gets the connection ID.
     *
     * @return connection ID
     */
    public String getConnectionId() {
        return connectionId;
    }

    /**
     * Adds a warning to the warning chain.
     *
     * @param warning warning to add
     */
    void addWarning(SQLWarning warning) {
        if (warnings == null) {
            warnings = warning;
        } else {
            warnings.setNextWarning(warning);
        }
    }

    <T> T withResilience(String operation, String sql, SqlSupplier<T> supplier) throws SQLException {
        return withResilience(operation, sql, supplier, false);
    }

    <T> T withResilience(String operation, String sql, SqlSupplier<T> supplier,
            boolean allowFailoverReplay) throws SQLException {
        if (!circuitBreaker.allowRequest()) {
            throw new SQLTransientConnectionException("Circuit breaker is OPEN", "08006");
        }

        int attempts = 0;
        while (true) {
            if (keepaliveTracker != null && keepaliveTracker.needsValidation()) {
                protocol.ping();
                keepaliveTracker.markActive();
            }

            SpanContext span = telemetry.startSpan(operation);
            if (span != null && sql != null) {
                String sanitized = TelemetryCollector.sanitizeQuery(sql);
                if (sanitized != null) {
                    span.withAttribute("db.statement", sanitized);
                }
            }

            boolean success = false;
            try {
                T result = supplier.get();
                success = true;
                circuitBreaker.recordSuccess();
                if (keepaliveTracker != null) {
                    keepaliveTracker.markActive();
                }
                return result;
            } catch (SQLException e) {
                circuitBreaker.recordFailure();
                boolean shouldReplay = allowFailoverReplay
                    && attempts == 0
                    && isFailoverReplayCandidate(e);
                attempts++;

                if (!shouldReplay) {
                    throw e;
                }

                try {
                    reconnectForFailover();
                } catch (SQLException reconnectFailure) {
                    reconnectFailure.addSuppressed(e);
                    throw reconnectFailure;
                }
                continue;
            } catch (RuntimeException e) {
                circuitBreaker.recordFailure();
                throw e;
            } finally {
                telemetry.endSpan(span, success);
            }
        }
    }

    void registerNamedCursor(String cursorName, SBResultSet resultSet) {
        String key = normalizeCursorName(cursorName);
        if (key == null || resultSet == null) {
            return;
        }
        Map<String, SBResultSet> cursorMap = namedCursors;
        if (cursorMap == null) {
            return;
        }
        resultSet.assignCursorName(key);
        cursorMap.put(key, resultSet);
    }

    void unregisterNamedCursor(String cursorName, SBResultSet resultSet) {
        String key = normalizeCursorName(cursorName);
        if (key == null) {
            return;
        }
        Map<String, SBResultSet> cursorMap = namedCursors;
        if (cursorMap == null) {
            return;
        }
        if (resultSet == null) {
            cursorMap.remove(key);
            return;
        }
        if (cursorMap.remove(key, resultSet)) {
            resultSet.assignCursorName(null);
        }
    }

    SBResultSet resolveNamedCursor(String cursorName) {
        String key = normalizeCursorName(cursorName);
        if (key == null) {
            return null;
        }
        Map<String, SBResultSet> cursorMap = namedCursors;
        if (cursorMap == null) {
            return null;
        }
        return cursorMap.get(key);
    }

    // ==================== Private Methods ====================

    private static String normalizeCursorName(String cursorName) {
        if (cursorName == null) {
            return null;
        }
        String trimmed = cursorName.trim();
        if (trimmed.isEmpty()) {
            return null;
        }
        if (trimmed.startsWith("\"") && trimmed.endsWith("\"") && trimmed.length() >= 2) {
            trimmed = trimmed.substring(1, trimmed.length() - 1).replace("\"\"", "\"");
            return "\"" + trimmed + "\"";
        }
        return trimmed.toLowerCase(Locale.ROOT);
    }

    private static String normalizeNotificationChannel(String channel) throws SQLException {
        if (channel == null) {
            throw new SQLException("Notification channel cannot be null", "HY024");
        }
        String normalized = channel.trim();
        if (normalized.isEmpty()) {
            throw new SQLException("Notification channel cannot be empty", "HY024");
        }
        if (normalized.indexOf('\0') >= 0) {
            throw new SQLException("Notification channel cannot contain NUL bytes", "HY024");
        }
        return normalized;
    }

    private void reconnectForFailover() throws SQLException {
        if (protocol == null) {
            throw new SQLException("Protocol handler is not available", "08003");
        }

        // MGA recovery rule: failover reconnect repairs the wire/session surface only.
        // Cursors bound to the abandoned session must not survive as resumable local state.
        invalidateNamedCursorsAfterReconnect();
        protocol.close();
        protocol.connect();

        applySchemaSetting(schema);

        if (!autoCommit) {
            // ScratchBird remains always-in-transaction after reconnect, but this is a new
            // session transaction, not a resurrection of the abandoned in-flight one.
            protocol.execute("SET AUTOCOMMIT OFF");
            beginManagedTransaction();
        }

        protocol.execute("SET TRANSACTION READ " + (readOnly ? "ONLY" : "WRITE"));

        if (keepaliveTracker != null) {
            keepaliveTracker.markActive();
        }
    }

    private void invalidateNamedCursorsAfterReconnect() {
        for (SBResultSet resultSet : namedCursors.values()) {
            if (resultSet != null) {
                resultSet.assignCursorName(null);
            }
        }
        namedCursors.clear();
    }

    private boolean isFailoverReplayCandidate(SQLException ex) {
        String state = ex.getSQLState();
        if (state == null || state.isEmpty()) {
            return false;
        }
        return state.startsWith("08")
            || ex instanceof SQLTransientConnectionException;
    }

    private void applySchemaSetting(String schemaValue) throws SQLException {
        String normalized = normalizeSchemaValue(schemaValue);
        if (normalized == null) {
            return;
        }
        String statement = buildSchemaStatement(normalized);
        if (statement.isBlank()) {
            return;
        }
        protocol.execute(statement);
        this.schema = normalized;
    }

    private String discoverCurrentSchema() {
        try {
            SBQueryResult result = protocol.execute("SHOW current_schema");
            return extractTrailingTextCell(result);
        } catch (SQLException ignored) {
            return null;
        }
    }

    private static String extractTrailingTextCell(SBQueryResult result) {
        if (result == null || result.getRows() == null || result.getRows().isEmpty()) {
            return null;
        }
        Object[] row = result.getRows().get(0);
        if (row == null) {
            return null;
        }
        for (int index = row.length - 1; index >= 0; index--) {
            String value = normalizeSchemaValue(row[index] == null ? null : row[index].toString());
            if (value != null) {
                return value;
            }
        }
        return null;
    }

    private static String normalizeSchemaValue(String schemaValue) {
        if (schemaValue == null) {
            return null;
        }
        String normalized = schemaValue.trim();
        return normalized.isEmpty() ? null : normalized;
    }

    void resetForPoolReuse(SBConnectionProperties baseline) throws SQLException {
        checkClosed();
        clearWarnings();

        if (!autoCommit && protocol.hasActiveTransaction()) {
            rollback();
        }

        if (autoCommit != baseline.isAutoCommit()) {
            setAutoCommit(baseline.isAutoCommit());
        } else if (!autoCommit && !protocol.hasActiveTransaction()) {
            beginManagedTransaction();
        }

        if (transactionIsolation != TRANSACTION_READ_COMMITTED
            || readCommittedMode != (byte) READ_COMMITTED_MODE_DEFAULT) {
            readCommittedMode = (byte) READ_COMMITTED_MODE_DEFAULT;
            setTransactionIsolation(TRANSACTION_READ_COMMITTED);
        }

        if (readOnly != baseline.isReadOnly()) {
            setReadOnly(baseline.isReadOnly());
        }

        String desiredSchema = normalizeSchemaValue(baseline.getCurrentSchema());
        if (desiredSchema == null) {
            protocol.execute("RESET search_path");
            schema = discoverCurrentSchema();
        } else if (schema == null || !desiredSchema.equalsIgnoreCase(schema)) {
            applySchemaSetting(desiredSchema);
        }

        // A pooled borrower should not inherit the previous borrower's
        // failure quarantine once the connection has been reset successfully.
        circuitBreaker.reset();
    }

    static String buildSchemaStatement(String schemaValue) {
        if (schemaValue == null) {
            return "";
        }
        String trimmed = schemaValue.trim();
        if (trimmed.isEmpty()) {
            return "";
        }

        List<String> paths = splitTopLevel(trimmed, ',');
        if (paths.size() > 1) {
            StringBuilder sql = new StringBuilder("SET SEARCH_PATH TO ");
            boolean first = true;
            for (String path : paths) {
                if (path == null || path.isBlank()) {
                    continue;
                }
                if (!first) {
                    sql.append(", ");
                }
                first = false;
                sql.append(formatSchemaPath(path.trim()));
            }
            return first ? "" : sql.toString();
        }
        return "SET SCHEMA " + formatSchemaPath(trimmed);
    }

    private void applyTransactionIsolation(int level, Byte explicitReadCommittedMode) throws SQLException {
        byte effectiveReadCommittedMode = readCommittedMode;
        if (explicitReadCommittedMode != null) {
            if (!supportsReadCommittedMode(level)) {
                throw new SQLFeatureNotSupportedException(
                    "readCommittedMode requires a READ COMMITTED isolation alias",
                    "0A000"
                );
            }
            effectiveReadCommittedMode = explicitReadCommittedMode;
        } else if (!supportsReadCommittedMode(level)) {
            effectiveReadCommittedMode = (byte) READ_COMMITTED_MODE_DEFAULT;
        }

        protocol.execute("SET TRANSACTION ISOLATION LEVEL "
            + buildIsolationLevelSql(level, effectiveReadCommittedMode));
        this.transactionIsolation = level;
        this.readCommittedMode = effectiveReadCommittedMode;
    }

    private static boolean supportsReadCommittedMode(int level) {
        return level == TRANSACTION_READ_UNCOMMITTED || level == TRANSACTION_READ_COMMITTED;
    }

    private static byte normalizeReadCommittedMode(int readCommittedMode) throws SQLException {
        switch (readCommittedMode) {
            case READ_COMMITTED_MODE_DEFAULT:
            case READ_COMMITTED_MODE_READ_CONSISTENCY:
            case READ_COMMITTED_MODE_RECORD_VERSION:
            case READ_COMMITTED_MODE_NO_RECORD_VERSION:
                return (byte) readCommittedMode;
            default:
                throw new SQLException("Invalid READ COMMITTED mode: " + readCommittedMode, "HY024");
        }
    }

    private static String buildIsolationLevelSql(int level, byte readCommittedMode) throws SQLException {
        switch (level) {
            case TRANSACTION_READ_UNCOMMITTED:
                if (readCommittedMode == (byte) READ_COMMITTED_MODE_DEFAULT) {
                    return "READ UNCOMMITTED";
                }
                return canonicalReadCommittedModeLabel(Byte.toUnsignedInt(readCommittedMode));
            case TRANSACTION_READ_COMMITTED:
                return canonicalReadCommittedModeLabel(Byte.toUnsignedInt(readCommittedMode));
            case TRANSACTION_REPEATABLE_READ:
                return "REPEATABLE READ";
            case TRANSACTION_SERIALIZABLE:
                return "SERIALIZABLE";
            case TRANSACTION_SNAPSHOT:
                return "SNAPSHOT";
            default:
                throw new SQLException("Invalid transaction isolation level: " + level, "HY024");
        }
    }

    private static byte mapIsolationToWireLevel(int level) throws SQLException {
        switch (level) {
            case TRANSACTION_READ_UNCOMMITTED:
                return SBProtocolHandler.ISOLATION_READ_UNCOMMITTED;
            case TRANSACTION_READ_COMMITTED:
                return SBProtocolHandler.ISOLATION_READ_COMMITTED;
            case TRANSACTION_REPEATABLE_READ:
            case TRANSACTION_SNAPSHOT:
                return SBProtocolHandler.ISOLATION_REPEATABLE_READ;
            case TRANSACTION_SERIALIZABLE:
                return SBProtocolHandler.ISOLATION_SERIALIZABLE;
            default:
                throw new SQLException("Invalid transaction isolation level: " + level, "HY024");
        }
    }

    private void beginManagedTransaction() throws SQLException {
        byte wireIsolation = mapIsolationToWireLevel(transactionIsolation);
        byte effectiveReadCommittedMode = supportsReadCommittedMode(transactionIsolation)
            ? readCommittedMode
            : (byte) READ_COMMITTED_MODE_DEFAULT;
        protocol.beginTransaction(
            wireIsolation,
            (byte) 0,
            false,
            false,
            0,
            (byte) 0,
            (byte) 0,
            effectiveReadCommittedMode
        );
    }

    private static String formatSchemaPath(String schemaPath) {
        List<String> segments = splitTopLevel(schemaPath, '.');
        if (segments.isEmpty()) {
            return quoteIdentifier(schemaPath);
        }
        StringBuilder out = new StringBuilder();
        boolean first = true;
        for (String rawSegment : segments) {
            String segment = rawSegment == null ? "" : rawSegment.trim();
            if (segment.isEmpty()) {
                continue;
            }
            if (!first) {
                out.append('.');
            }
            first = false;
            out.append(normalizeIdentifierSegment(segment));
        }
        return out.length() == 0 ? quoteIdentifier(schemaPath.trim()) : out.toString();
    }

    private static String normalizeIdentifierSegment(String segment) {
        if (segment.startsWith("\"") && segment.endsWith("\"") && segment.length() >= 2) {
            return segment;
        }
        return quoteIdentifier(segment);
    }

    private static String quoteIdentifier(String identifier) {
        return "\"" + identifier.replace("\"", "\"\"") + "\"";
    }

    private static String quoteSqlLiteral(String value) {
        return "'" + value.replace("'", "''") + "'";
    }

    private static List<String> splitTopLevel(String value, char delimiter) {
        List<String> tokens = new ArrayList<>();
        if (value == null || value.isBlank()) {
            return tokens;
        }
        StringBuilder current = new StringBuilder();
        boolean inDouble = false;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"') {
                current.append(c);
                if (inDouble && i + 1 < value.length() && value.charAt(i + 1) == '"') {
                    current.append('"');
                    i++;
                    continue;
                }
                inDouble = !inDouble;
                continue;
            }
            if (!inDouble && c == delimiter) {
                tokens.add(current.toString());
                current.setLength(0);
                continue;
            }
            current.append(c);
        }
        tokens.add(current.toString());
        return tokens;
    }

    /**
     * Checks if connection is closed and throws exception if so.
     */
    private void checkClosed() throws SQLException {
        if (closed.get()) {
            throw new SQLException("Connection is closed", "08003");
        }
    }
}
