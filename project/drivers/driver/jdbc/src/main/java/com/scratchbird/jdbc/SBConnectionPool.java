// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import java.sql.Connection;
import java.sql.SQLException;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Connection pool for ScratchBird JDBC driver.
 * 
 * <p>Provides efficient connection reuse with configurable size limits,
 * connection validation, and automatic reconnection for failed connections.</p>
 */
public class SBConnectionPool {
    private static final Logger LOGGER = Logger.getLogger(SBConnectionPool.class.getName());
    public static final long DEFAULT_LIFETIME_MS = 60 * 60 * 1000L;
    
    private final SBConnectionProperties properties;
    private final PoolConfig config;
    private final BlockingQueue<PooledConnection> availableConnections;
    private final ConcurrentHashMap<Connection, ConnectionState> connectionStates;
    private final AtomicInteger totalConnections;
    private final AtomicLong hitCount;
    private final AtomicLong missCount;
    private final ScheduledExecutorService maintenanceExecutor;
    private final KeepaliveManager keepaliveManager;
    private final LeakDetector leakDetector;
    private volatile boolean closed = false;
    
    /**
     * Pool configuration
     */
    public static class PoolConfig {
        private int minConnections = 1;
        private int maxConnections = 10;
        private long maxLifetimeMillis = 3600000; // 1 hour
        private long idleTimeoutMillis = 600000;  // 10 minutes
        private long acquireTimeoutMillis = 30000; // 30 seconds
        private boolean testOnCheckout = true;
        private long validationTimeoutMillis = 5000;
        
        public int getMinConnections() { return minConnections; }
        public void setMinConnections(int minConnections) { this.minConnections = minConnections; }
        
        public int getMaxConnections() { return maxConnections; }
        public void setMaxConnections(int maxConnections) { this.maxConnections = maxConnections; }
        
        public long getMaxLifetimeMillis() { return maxLifetimeMillis; }
        public void setMaxLifetimeMillis(long maxLifetimeMillis) { this.maxLifetimeMillis = maxLifetimeMillis; }
        
        public long getIdleTimeoutMillis() { return idleTimeoutMillis; }
        public void setIdleTimeoutMillis(long idleTimeoutMillis) { this.idleTimeoutMillis = idleTimeoutMillis; }
        
        public long getAcquireTimeoutMillis() { return acquireTimeoutMillis; }
        public void setAcquireTimeoutMillis(long acquireTimeoutMillis) { this.acquireTimeoutMillis = acquireTimeoutMillis; }
        
        public boolean isTestOnCheckout() { return testOnCheckout; }
        public void setTestOnCheckout(boolean testOnCheckout) { this.testOnCheckout = testOnCheckout; }
        
        public long getValidationTimeoutMillis() { return validationTimeoutMillis; }
        public void setValidationTimeoutMillis(long validationTimeoutMillis) { this.validationTimeoutMillis = validationTimeoutMillis; }
    }
    
    /**
     * Connection state tracking
     */
    private static class ConnectionState {
        final long createdAt;
        volatile long lastUsedAt;
        volatile int useCount;
        
        ConnectionState() {
            this.createdAt = System.currentTimeMillis();
            this.lastUsedAt = createdAt;
            this.useCount = 0;
        }
    }
    
    /**
     * Pooled connection wrapper
     */
    private class PooledConnection implements Connection {
        private final SBConnection delegate;
        private final ConnectionState state;
        private volatile boolean returned = false;
        private LeakDetectionGuard leakGuard;
        
        PooledConnection(SBConnection delegate, ConnectionState state) {
            this.delegate = delegate;
            this.state = state;
        }

        void attachLeakGuard(LeakDetectionGuard guard) {
            this.leakGuard = guard;
        }
        
        @Override
        public void close() throws SQLException {
            if (!returned) {
                returned = true;
                if (leakGuard != null) {
                    leakGuard.release();
                    leakGuard = null;
                }
                returnConnection(delegate, state);
            }
        }
        
        // Delegate all Connection methods
        @Override
        public java.sql.Statement createStatement() throws SQLException {
            return delegate.createStatement();
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException {
            return delegate.prepareStatement(sql);
        }
        
        @Override
        public java.sql.CallableStatement prepareCall(String sql) throws SQLException {
            return delegate.prepareCall(sql);
        }
        
        @Override
        public String nativeSQL(String sql) throws SQLException {
            return delegate.nativeSQL(sql);
        }
        
        @Override
        public void setAutoCommit(boolean autoCommit) throws SQLException {
            delegate.setAutoCommit(autoCommit);
        }
        
        @Override
        public boolean getAutoCommit() throws SQLException {
            return delegate.getAutoCommit();
        }
        
        @Override
        public void commit() throws SQLException {
            delegate.commit();
        }
        
        @Override
        public void rollback() throws SQLException {
            delegate.rollback();
        }
        
        @Override
        public boolean isClosed() throws SQLException {
            return returned || delegate.isClosed();
        }
        
        @Override
        public java.sql.DatabaseMetaData getMetaData() throws SQLException {
            return delegate.getMetaData();
        }
        
        @Override
        public void setReadOnly(boolean readOnly) throws SQLException {
            delegate.setReadOnly(readOnly);
        }
        
        @Override
        public boolean isReadOnly() throws SQLException {
            return delegate.isReadOnly();
        }
        
        @Override
        public void setCatalog(String catalog) throws SQLException {
            delegate.setCatalog(catalog);
        }
        
        @Override
        public String getCatalog() throws SQLException {
            return delegate.getCatalog();
        }
        
        @Override
        public void setTransactionIsolation(int level) throws SQLException {
            delegate.setTransactionIsolation(level);
        }
        
        @Override
        public int getTransactionIsolation() throws SQLException {
            return delegate.getTransactionIsolation();
        }
        
        @Override
        public java.sql.SQLWarning getWarnings() throws SQLException {
            return delegate.getWarnings();
        }
        
        @Override
        public void clearWarnings() throws SQLException {
            delegate.clearWarnings();
        }
        
        @Override
        public java.sql.Statement createStatement(int resultSetType, int resultSetConcurrency) throws SQLException {
            return delegate.createStatement(resultSetType, resultSetConcurrency);
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql, int resultSetType, int resultSetConcurrency) throws SQLException {
            return delegate.prepareStatement(sql, resultSetType, resultSetConcurrency);
        }
        
        @Override
        public java.sql.CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency) throws SQLException {
            return delegate.prepareCall(sql, resultSetType, resultSetConcurrency);
        }
        
        @Override
        public java.util.Map<String, Class<?>> getTypeMap() throws SQLException {
            return delegate.getTypeMap();
        }
        
        @Override
        public void setTypeMap(java.util.Map<String, Class<?>> map) throws SQLException {
            delegate.setTypeMap(map);
        }
        
        @Override
        public void setHoldability(int holdability) throws SQLException {
            delegate.setHoldability(holdability);
        }
        
        @Override
        public int getHoldability() throws SQLException {
            return delegate.getHoldability();
        }
        
        @Override
        public java.sql.Savepoint setSavepoint() throws SQLException {
            return delegate.setSavepoint();
        }
        
        @Override
        public java.sql.Savepoint setSavepoint(String name) throws SQLException {
            return delegate.setSavepoint(name);
        }
        
        @Override
        public void rollback(java.sql.Savepoint savepoint) throws SQLException {
            delegate.rollback(savepoint);
        }
        
        @Override
        public void releaseSavepoint(java.sql.Savepoint savepoint) throws SQLException {
            delegate.releaseSavepoint(savepoint);
        }
        
        @Override
        public java.sql.Statement createStatement(int resultSetType, int resultSetConcurrency, int resultSetHoldability) throws SQLException {
            return delegate.createStatement(resultSetType, resultSetConcurrency, resultSetHoldability);
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql, int resultSetType, int resultSetConcurrency, int resultSetHoldability) throws SQLException {
            return delegate.prepareStatement(sql, resultSetType, resultSetConcurrency, resultSetHoldability);
        }
        
        @Override
        public java.sql.CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency, int resultSetHoldability) throws SQLException {
            return delegate.prepareCall(sql, resultSetType, resultSetConcurrency, resultSetHoldability);
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql, int autoGeneratedKeys) throws SQLException {
            return delegate.prepareStatement(sql, autoGeneratedKeys);
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql, int[] columnIndexes) throws SQLException {
            return delegate.prepareStatement(sql, columnIndexes);
        }
        
        @Override
        public java.sql.PreparedStatement prepareStatement(String sql, String[] columnNames) throws SQLException {
            return delegate.prepareStatement(sql, columnNames);
        }
        
        @Override
        public java.sql.Clob createClob() throws SQLException {
            return delegate.createClob();
        }
        
        @Override
        public java.sql.Blob createBlob() throws SQLException {
            return delegate.createBlob();
        }
        
        @Override
        public java.sql.NClob createNClob() throws SQLException {
            return delegate.createNClob();
        }
        
        @Override
        public java.sql.SQLXML createSQLXML() throws SQLException {
            return delegate.createSQLXML();
        }
        
        @Override
        public boolean isValid(int timeout) throws SQLException {
            return delegate.isValid(timeout);
        }
        
        @Override
        public void setClientInfo(String name, String value) throws java.sql.SQLClientInfoException {
            delegate.setClientInfo(name, value);
        }
        
        @Override
        public void setClientInfo(java.util.Properties properties) throws java.sql.SQLClientInfoException {
            delegate.setClientInfo(properties);
        }
        
        @Override
        public String getClientInfo(String name) throws SQLException {
            return delegate.getClientInfo(name);
        }
        
        @Override
        public java.util.Properties getClientInfo() throws SQLException {
            return delegate.getClientInfo();
        }
        
        @Override
        public java.sql.Array createArrayOf(String typeName, Object[] elements) throws SQLException {
            return delegate.createArrayOf(typeName, elements);
        }
        
        @Override
        public java.sql.Struct createStruct(String typeName, Object[] attributes) throws SQLException {
            return delegate.createStruct(typeName, attributes);
        }
        
        @Override
        public void setSchema(String schema) throws SQLException {
            delegate.setSchema(schema);
        }
        
        @Override
        public String getSchema() throws SQLException {
            return delegate.getSchema();
        }
        
        @Override
        public void abort(java.util.concurrent.Executor executor) throws SQLException {
            delegate.abort(executor);
        }
        
        @Override
        public void setNetworkTimeout(java.util.concurrent.Executor executor, int milliseconds) throws SQLException {
            delegate.setNetworkTimeout(executor, milliseconds);
        }
        
        @Override
        public int getNetworkTimeout() throws SQLException {
            return delegate.getNetworkTimeout();
        }
        
        @Override
        public <T> T unwrap(Class<T> iface) throws SQLException {
            return delegate.unwrap(iface);
        }
        
        @Override
        public boolean isWrapperFor(Class<?> iface) throws SQLException {
            return delegate.isWrapperFor(iface);
        }
    }
    
    /**
     * Creates a new connection pool
     */
    public SBConnectionPool(SBConnectionProperties properties, PoolConfig config) throws SQLException {
        this.properties = properties;
        this.config = config;
        this.availableConnections = new LinkedBlockingQueue<>();
        this.connectionStates = new ConcurrentHashMap<>();
        this.totalConnections = new AtomicInteger(0);
        this.hitCount = new AtomicLong(0);
        this.missCount = new AtomicLong(0);
        this.keepaliveManager = new KeepaliveManager();
        this.leakDetector = new LeakDetector();
        this.keepaliveManager.start();
        this.leakDetector.start();
        
        // Initialize minimum connections
        for (int i = 0; i < config.getMinConnections(); i++) {
            createNewConnection();
        }
        
        // Start maintenance thread
        this.maintenanceExecutor = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "SBConnectionPool-Maintenance");
            t.setDaemon(true);
            return t;
        });
        
        this.maintenanceExecutor.scheduleWithFixedDelay(
            this::performMaintenance,
            30, 30, TimeUnit.SECONDS
        );
    }
    
    /**
     * Acquire a connection from the pool
     */
    public Connection acquire() throws SQLException {
        if (closed) {
            throw new SQLException("Pool is closed");
        }

        final long deadline = System.currentTimeMillis() + config.getAcquireTimeoutMillis();
        while (true) {
            PooledConnection pooled = availableConnections.poll();
            if (pooled != null) {
                Connection leased = checkoutIfValid(pooled);
                if (leased != null) {
                    hitCount.incrementAndGet();
                    return leased;
                }
                // Discarded invalid lease; retry immediately.
                continue;
            }

            PooledConnection created = tryCreateNewPooledConnection();
            if (created != null) {
                missCount.incrementAndGet();
                created.attachLeakGuard(leakDetector.checkout(created.delegate.getConnectionId()));
                return created;
            }

            long remaining = deadline - System.currentTimeMillis();
            if (remaining <= 0) {
                throw new SQLException("Connection pool exhausted. "
                    + "Increase MaxPoolSize or reduce concurrent usage, then retry.");
            }

            try {
                pooled = availableConnections.poll(remaining, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new SQLException("Interrupted while acquiring connection", e);
            }

            if (pooled == null) {
                throw new SQLException("Connection pool exhausted. "
                    + "Increase MaxPoolSize or reduce concurrent usage, then retry.");
            }

            Connection leased = checkoutIfValid(pooled);
            if (leased != null) {
                hitCount.incrementAndGet();
                return leased;
            }
        }
    }
    
    /**
     * Acquire with retry logic
     */
    public Connection acquireWithRetry(int maxRetries, long baseDelayMs) throws SQLException {
        SQLException lastException = null;
        long delay = baseDelayMs;
        
        for (int i = 0; i <= maxRetries; i++) {
            try {
                return acquire();
            } catch (SQLException e) {
                lastException = e;
                if (i < maxRetries) {
                    try {
                        Thread.sleep(delay);
                        delay = Math.min(delay * 2, 5000); // Exponential backoff, max 5s
                    } catch (InterruptedException ie) {
                        Thread.currentThread().interrupt();
                        throw new SQLException("Interrupted during retry", ie);
                    }
                }
            }
        }
        
        throw lastException;
    }
    
    private Connection checkoutIfValid(PooledConnection pooled) {
        ConnectionState state = connectionStates.get(pooled.delegate);
        if (state == null) {
            closeConnection(pooled.delegate);
            return null;
        }

        if (config.isTestOnCheckout() && !isConnectionValid(pooled.delegate, state)) {
            closeConnection(pooled.delegate);
            return null;
        }

        state.lastUsedAt = System.currentTimeMillis();
        state.useCount++;
        pooled.attachLeakGuard(leakDetector.checkout(pooled.delegate.getConnectionId()));
        return pooled;
    }

    private boolean reserveConnectionSlot() {
        while (true) {
            int current = totalConnections.get();
            if (current >= config.getMaxConnections()) {
                return false;
            }
            if (totalConnections.compareAndSet(current, current + 1)) {
                return true;
            }
        }
    }

    private PooledConnection tryCreateNewPooledConnection() throws SQLException {
        if (!reserveConnectionSlot()) {
            return null;
        }

        boolean success = false;
        SBConnection conn = null;
        try {
            conn = new SBConnection(properties);
            ConnectionState state = new ConnectionState();
            connectionStates.put(conn, state);
            keepaliveManager.register(conn.getConnectionId(), conn);
            success = true;
            return new PooledConnection(conn, state);
        } finally {
            if (!success) {
                if (conn != null) {
                    try {
                        conn.close();
                    } catch (SQLException e) {
                        LOGGER.log(Level.FINE, "Error closing failed connection attempt", e);
                    }
                }
                totalConnections.decrementAndGet();
            }
        }
    }

    private PooledConnection createNewPooledConnection() throws SQLException {
        PooledConnection created = tryCreateNewPooledConnection();
        if (created == null) {
            throw new SQLException("Connection pool exhausted");
        }
        return created;
    }

    private void createNewConnection() throws SQLException {
        availableConnections.offer(createNewPooledConnection());
    }
    
    private void returnConnection(SBConnection conn, ConnectionState state) {
        if (closed) {
            closeConnection(conn);
            return;
        }

        try {
            conn.resetForPoolReuse(properties);
        } catch (SQLException ex) {
            LOGGER.log(Level.FINE, "Discarding pooled connection after reset failure", ex);
            closeConnection(conn);
            return;
        }
        
        // Check if connection is still valid
        long age = System.currentTimeMillis() - state.createdAt;
        if (age > config.getMaxLifetimeMillis()) {
            closeConnection(conn);
            return;
        }
        
        availableConnections.offer(new PooledConnection(conn, state));
    }
    
    private void closeConnection(SBConnection conn) {
        try {
            conn.close();
        } catch (SQLException e) {
            LOGGER.log(Level.WARNING, "Error closing connection", e);
        }
        keepaliveManager.unregister(conn.getConnectionId());
        connectionStates.remove(conn);
        totalConnections.decrementAndGet();
    }
    
    private boolean isConnectionValid(SBConnection conn, ConnectionState state) {
        long age = System.currentTimeMillis() - state.createdAt;
        long idleTime = System.currentTimeMillis() - state.lastUsedAt;
        
        if (age > config.getMaxLifetimeMillis() || 
            idleTime > config.getIdleTimeoutMillis()) {
            return false;
        }
        
        try {
            return conn.isValid((int) (config.getValidationTimeoutMillis() / 1000));
        } catch (SQLException e) {
            return false;
        }
    }
    
    private void performMaintenance() {
        try {
            long now = System.currentTimeMillis();
            
            // Remove expired connections from available queue
            availableConnections.removeIf(pc -> {
                ConnectionState state = connectionStates.get(pc.delegate);
                if (state == null) return true;
                
                long age = now - state.createdAt;
                long idleTime = now - state.lastUsedAt;
                
                if (age > config.getMaxLifetimeMillis() || 
                    idleTime > config.getIdleTimeoutMillis()) {
                    closeConnection(pc.delegate);
                    return true;
                }
                return false;
            });
            
            // Ensure minimum connections
            int currentSize = availableConnections.size();
            int needed = config.getMinConnections() - currentSize;
            for (int i = 0; i < needed; i++) {
                try {
                    createNewConnection();
                } catch (SQLException e) {
                    LOGGER.log(Level.WARNING, "Failed to create maintenance connection", e);
                    break;
                }
            }
            
        } catch (Exception e) {
            LOGGER.log(Level.WARNING, "Maintenance task failed", e);
        }
    }
    
    /**
     * Get pool statistics
     */
    public PoolStats getStats() {
        return new PoolStats(
            availableConnections.size(),
            totalConnections.get(),
            config.getMaxConnections(),
            hitCount.get(),
            missCount.get()
        );
    }
    
    /**
     * Pool statistics
     */
    public static class PoolStats {
        public final int available;
        public final int total;
        public final int max;
        public final long hits;
        public final long misses;
        
        public PoolStats(int available, int total, int max, long hits, long misses) {
            this.available = available;
            this.total = total;
            this.max = max;
            this.hits = hits;
            this.misses = misses;
        }
        
        public double getHitRate() {
            long total = hits + misses;
            return total == 0 ? 0.0 : (double) hits / total;
        }
    }
    
    /**
     * Close the pool and all connections
     */
    public void close() {
        closed = true;
        maintenanceExecutor.shutdown();
        keepaliveManager.stop();
        leakDetector.stop();
        
        for (Connection conn : connectionStates.keySet()) {
            if (conn instanceof SBConnection) {
                closeConnection((SBConnection) conn);
            } else {
                try {
                    conn.close();
                } catch (SQLException ignored) {
                }
            }
        }
        availableConnections.clear();
        connectionStates.clear();
    }
}
