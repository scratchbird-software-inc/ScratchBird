// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Connection Leak Detector
 * Copyright (c) 2025-2026 Dalton Calford
 */
package com.scratchbird.jdbc;

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
 * Configuration for leak detection
 */
class LeakDetectionConfig {
    public static final long DEFAULT_THRESHOLD_MS = 30000;  // 30 seconds
    public static final long DEFAULT_CHECK_INTERVAL_MS = 10000;  // 10 seconds
    
    private long thresholdMs = DEFAULT_THRESHOLD_MS;
    private boolean captureStackTrace = false;
    private long checkIntervalMs = DEFAULT_CHECK_INTERVAL_MS;
    private LeakLogLevel logLevel = LeakLogLevel.WARN;
    
    public LeakDetectionConfig() {}
    
    public LeakDetectionConfig threshold(long ms) {
        this.thresholdMs = ms;
        return this;
    }
    
    public LeakDetectionConfig captureStackTrace(boolean capture) {
        this.captureStackTrace = capture;
        return this;
    }
    
    public LeakDetectionConfig checkInterval(long ms) {
        this.checkIntervalMs = ms;
        return this;
    }
    
    public LeakDetectionConfig logLevel(LeakLogLevel level) {
        this.logLevel = level;
        return this;
    }
    
    public long getThresholdMs() { return thresholdMs; }
    public boolean isCaptureStackTrace() { return captureStackTrace; }
    public long getCheckIntervalMs() { return checkIntervalMs; }
    public LeakLogLevel getLogLevel() { return logLevel; }
}

enum LeakLogLevel {
    DEBUG, WARN, ERROR
}

/**
 * Information about a connection checkout
 */
class CheckoutInfo {
    final Instant checkoutTime;
    final long threadId;
    final String stackTrace;
    final Map<String, String> metadata;
    
    CheckoutInfo(boolean captureStackTrace, Map<String, String> metadata) {
        this.checkoutTime = Instant.now();
        this.threadId = Thread.currentThread().getId();
        this.metadata = metadata != null ? metadata : new ConcurrentHashMap<>();
        
        if (captureStackTrace) {
            StackTraceElement[] stack = Thread.currentThread().getStackTrace();
            StringBuilder sb = new StringBuilder();
            for (StackTraceElement element : stack) {
                sb.append("  at ").append(element.toString()).append("\n");
            }
            this.stackTrace = sb.toString();
        } else {
            this.stackTrace = null;
        }
    }
    
    Duration getHeldDuration() {
        return Duration.between(checkoutTime, Instant.now());
    }
}

/**
 * Detects potential connection leaks
 */
public class LeakDetector {
    private static final Logger LOGGER = Logger.getLogger(LeakDetector.class.getName());
    
    private final LeakDetectionConfig config;
    private final Map<String, CheckoutInfo> checkouts;
    private final ScheduledExecutorService scheduler;
    private final AtomicBoolean running;
    
    public LeakDetector(LeakDetectionConfig config) {
        this.config = config;
        this.checkouts = new ConcurrentHashMap<>();
        this.scheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "sb-leak-detector");
            t.setDaemon(true);
            return t;
        });
        this.running = new AtomicBoolean(false);
    }
    
    public LeakDetector() {
        this(new LeakDetectionConfig());
    }
    
    /**
     * Start the leak detection monitoring
     */
    public void start() {
        if (running.compareAndSet(false, true)) {
            scheduler.scheduleAtFixedRate(
                this::checkLeaks,
                config.getCheckIntervalMs(),
                config.getCheckIntervalMs(),
                TimeUnit.MILLISECONDS
            );
            LOGGER.fine("Leak detector started");
        }
    }
    
    /**
     * Stop the leak detection monitoring
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
            LOGGER.fine("Leak detector stopped");
        }
    }
    
    /**
     * Register a connection checkout
     */
    public LeakDetectionGuard checkout(String connectionId) {
        return checkout(connectionId, null);
    }
    
    /**
     * Register a connection checkout with metadata
     */
    public LeakDetectionGuard checkout(String connectionId, Map<String, String> metadata) {
        CheckoutInfo info = new CheckoutInfo(config.isCaptureStackTrace(), metadata);
        checkouts.put(connectionId, info);
        LOGGER.finest("Connection checked out: " + connectionId);
        return new LeakDetectionGuard(this, connectionId);
    }
    
    /**
     * Register a connection return
     */
    public void checkin(String connectionId) {
        CheckoutInfo info = checkouts.remove(connectionId);
        if (info != null) {
            Duration held = info.getHeldDuration();
            if (held.toMillis() > config.getThresholdMs()) {
                LOGGER.warning(String.format(
                    "Connection %s held for %d ms (threshold: %d ms) - returned",
                    connectionId, held.toMillis(), config.getThresholdMs()
                ));
            } else {
                LOGGER.finest("Connection returned: " + connectionId);
            }
        }
    }
    
    /**
     * Get active checkout count
     */
    public int getActiveCount() {
        return checkouts.size();
    }
    
    /**
     * Get leak statistics
     */
    public LeakStats getStats() {
        int potentialLeaks = 0;
        for (CheckoutInfo info : checkouts.values()) {
            if (info.getHeldDuration().toMillis() > config.getThresholdMs()) {
                potentialLeaks++;
            }
        }
        return new LeakStats(checkouts.size(), potentialLeaks);
    }
    
    private void checkLeaks() {
        for (Map.Entry<String, CheckoutInfo> entry : checkouts.entrySet()) {
            String connId = entry.getKey();
            CheckoutInfo info = entry.getValue();
            
            long heldMs = info.getHeldDuration().toMillis();
            if (heldMs > config.getThresholdMs()) {
                logLeak(connId, info, heldMs);
            }
        }
    }
    
    private void logLeak(String connId, CheckoutInfo info, long heldMs) {
        String message = String.format(
            "POSSIBLE CONNECTION LEAK: conn=%s, held=%d ms, threshold=%d ms, thread=%d, metadata=%s",
            connId, heldMs, config.getThresholdMs(), info.threadId, info.metadata
        );
        
        switch (config.getLogLevel()) {
            case DEBUG:
                LOGGER.fine(message);
                break;
            case WARN:
                LOGGER.warning(message);
                break;
            case ERROR:
                LOGGER.severe(message);
                break;
        }
        
        if (info.stackTrace != null) {
            LOGGER.warning("Stack trace for connection " + connId + ":\n" + info.stackTrace);
        }
    }
    
    /**
     * Statistics for leak detection
     */
    public static class LeakStats {
        public final int activeCheckouts;
        public final int potentialLeaks;
        
        public LeakStats(int activeCheckouts, int potentialLeaks) {
            this.activeCheckouts = activeCheckouts;
            this.potentialLeaks = potentialLeaks;
        }
    }
}

/**
 * RAII guard for leak detection
 */
class LeakDetectionGuard implements AutoCloseable {
    private final LeakDetector detector;
    private final String connectionId;
    private boolean released = false;
    
    LeakDetectionGuard(LeakDetector detector, String connectionId) {
        this.detector = detector;
        this.connectionId = connectionId;
    }
    
    public void release() {
        if (!released) {
            detector.checkin(connectionId);
            released = true;
        }
    }
    
    @Override
    public void close() {
        release();
    }
}
