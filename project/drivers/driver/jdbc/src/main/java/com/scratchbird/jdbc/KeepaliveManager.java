// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Keepalive Manager - Prevents connection timeouts
 * Copyright (c) 2025-2026 Dalton Calford
 */
package com.scratchbird.jdbc;

import java.sql.Connection;
import java.sql.SQLException;
import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Configuration for connection keepalive
 */
class KeepaliveConfig {
    public static final long DEFAULT_INTERVAL_MS = 120000;  // 2 minutes
    public static final long DEFAULT_MAX_IDLE_MS = 600000;  // 10 minutes
    public static final long DEFAULT_VALIDATION_TIMEOUT_MS = 5000;  // 5 seconds
    
    private long intervalMs = DEFAULT_INTERVAL_MS;
    private long maxIdleBeforeCheckMs = DEFAULT_MAX_IDLE_MS;
    private long validationTimeoutMs = DEFAULT_VALIDATION_TIMEOUT_MS;
    
    public KeepaliveConfig() {}
    
    public KeepaliveConfig interval(long ms) {
        this.intervalMs = ms;
        return this;
    }
    
    public KeepaliveConfig maxIdleBeforeCheck(long ms) {
        this.maxIdleBeforeCheckMs = ms;
        return this;
    }
    
    public KeepaliveConfig validationTimeout(long ms) {
        this.validationTimeoutMs = ms;
        return this;
    }
    
    public long getIntervalMs() { return intervalMs; }
    public long getMaxIdleBeforeCheckMs() { return maxIdleBeforeCheckMs; }
    public long getValidationTimeoutMs() { return validationTimeoutMs; }
}

/**
 * Tracks activity for a single connection
 */
class KeepaliveTracker {
    private volatile Instant lastActivity;
    private final KeepaliveConfig config;
    
    public KeepaliveTracker(KeepaliveConfig config) {
        this.config = config;
        this.lastActivity = Instant.now();
    }
    
    public void markActive() {
        this.lastActivity = Instant.now();
    }
    
    public boolean needsValidation() {
        return Duration.between(lastActivity, Instant.now()).toMillis() > config.getMaxIdleBeforeCheckMs();
    }
    
    public long getIdleDurationMs() {
        return Duration.between(lastActivity, Instant.now()).toMillis();
    }
}

/**
 * Manages keepalive for a pool of connections
 */
public class KeepaliveManager {
    private static final Logger LOGGER = Logger.getLogger(KeepaliveManager.class.getName());
    
    private final KeepaliveConfig config;
    private final Map<String, KeepaliveTracker> trackers;
    private final Map<String, Connection> connections;
    private final ScheduledExecutorService scheduler;
    private final AtomicBoolean running;
    
    public KeepaliveManager(KeepaliveConfig config) {
        this.config = config;
        this.trackers = new ConcurrentHashMap<>();
        this.connections = new ConcurrentHashMap<>();
        this.scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "sb-keepalive");
            t.setDaemon(true);
            return t;
        });
        this.running = new AtomicBoolean(false);
    }
    
    public KeepaliveManager() {
        this(new KeepaliveConfig());
    }
    
    /**
     * Start the keepalive monitoring
     */
    public void start() {
        if (running.compareAndSet(false, true)) {
            scheduler.scheduleAtFixedRate(
                this::checkConnections,
                config.getIntervalMs(),
                config.getIntervalMs(),
                TimeUnit.MILLISECONDS
            );
            LOGGER.fine("Keepalive manager started");
        }
    }
    
    /**
     * Stop the keepalive monitoring
     */
    public void stop() {
        if (running.compareAndSet(true, false)) {
            scheduler.shutdown();
            try {
                if (!scheduler.awaitTermination(5, TimeUnit.SECONDS)) {
                    scheduler.shutdownNow();
                }
            } catch (InterruptedException e) {
                scheduler.shutdownNow();
                Thread.currentThread().interrupt();
            }
            LOGGER.fine("Keepalive manager stopped");
        }
    }
    
    /**
     * Register a connection for keepalive monitoring
     */
    public KeepaliveTracker register(String connectionId, Connection connection) {
        KeepaliveTracker tracker = new KeepaliveTracker(config);
        trackers.put(connectionId, tracker);
        connections.put(connectionId, connection);
        LOGGER.finest("Registered connection for keepalive: " + connectionId);
        return tracker;
    }
    
    /**
     * Unregister a connection
     */
    public void unregister(String connectionId) {
        trackers.remove(connectionId);
        connections.remove(connectionId);
        LOGGER.finest("Unregistered connection from keepalive: " + connectionId);
    }
    
    /**
     * Check all connections and validate idle ones
     */
    private void checkConnections() {
        for (Map.Entry<String, KeepaliveTracker> entry : trackers.entrySet()) {
            String connId = entry.getKey();
            KeepaliveTracker tracker = entry.getValue();
            
            if (tracker.needsValidation()) {
                Connection conn = connections.get(connId);
                if (conn != null) {
                    validateConnection(connId, conn, tracker);
                }
            }
        }
    }
    
    /**
     * Validate a connection by sending a ping
     */
    private void validateConnection(String connId, Connection conn, KeepaliveTracker tracker) {
        try {
            // Use JDBC4 isValid if available
            if (conn.isValid((int) (config.getValidationTimeoutMs() / 1000))) {
                tracker.markActive();
                LOGGER.finest("Keepalive validation passed for: " + connId);
            } else {
                LOGGER.warning("Keepalive validation failed for: " + connId);
            }
        } catch (SQLException e) {
            LOGGER.log(Level.FINE, "Keepalive validation error for: " + connId, e);
        }
    }
    
    /**
     * Get the number of monitored connections
     */
    public int getMonitoredCount() {
        return trackers.size();
    }
}
