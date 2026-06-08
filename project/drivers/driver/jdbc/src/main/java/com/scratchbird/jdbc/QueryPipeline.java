// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Query Pipelining - Batches multiple queries for efficiency
 * Copyright (c) 2025-2026 Dalton Calford
 */
package com.scratchbird.jdbc;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Logger;

/**
 * Configuration for query pipelining
 */
class PipelineConfig {
    public static final int DEFAULT_MAX_IN_FLIGHT = 100;
    public static final int DEFAULT_AUTO_FLUSH_THRESHOLD = 10;
    public static final long DEFAULT_FLUSH_TIMEOUT_MS = 5000;
    
    private int maxInFlight = DEFAULT_MAX_IN_FLIGHT;
    private boolean autoFlush = true;
    private int autoFlushThreshold = DEFAULT_AUTO_FLUSH_THRESHOLD;
    private long flushTimeoutMs = DEFAULT_FLUSH_TIMEOUT_MS;
    
    public PipelineConfig() {}
    
    public PipelineConfig maxInFlight(int max) {
        this.maxInFlight = max;
        return this;
    }
    
    public PipelineConfig autoFlush(boolean auto) {
        this.autoFlush = auto;
        return this;
    }
    
    public PipelineConfig autoFlushThreshold(int threshold) {
        this.autoFlushThreshold = threshold;
        return this;
    }
    
    public PipelineConfig flushTimeout(long ms) {
        this.flushTimeoutMs = ms;
        return this;
    }
    
    public int getMaxInFlight() { return maxInFlight; }
    public boolean isAutoFlush() { return autoFlush; }
    public int getAutoFlushThreshold() { return autoFlushThreshold; }
    public long getFlushTimeoutMs() { return flushTimeoutMs; }
}

/**
 * A pipelined request with future result
 */
class PipelinedRequest<T> {
    final String sql;
    final List<Object> params;
    final CompletableFuture<T> future;
    
    PipelinedRequest(String sql, List<Object> params, CompletableFuture<T> future) {
        this.sql = sql;
        this.params = params;
        this.future = future;
    }
}

/**
 * Query pipeline for batching requests
 */
public class QueryPipeline<T> {
    private static final Logger LOGGER = Logger.getLogger(QueryPipeline.class.getName());
    
    private final PipelineConfig config;
    private final BlockingQueue<PipelinedRequest<T>> queue;
    private final AtomicInteger inFlight;
    private final ExecutorService executor;
    private volatile boolean running;
    
    public QueryPipeline(PipelineConfig config) {
        this.config = config;
        this.queue = new LinkedBlockingQueue<>();
        this.inFlight = new AtomicInteger(0);
        this.executor = Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "sb-pipeline");
            t.setDaemon(true);
            return t;
        });
        this.running = false;
    }
    
    public QueryPipeline() {
        this(new PipelineConfig());
    }
    
    /**
     * Start the pipeline executor
     */
    public void start(Connection connection) {
        if (!running) {
            running = true;
            executor.submit(() -> runLoop(connection));
        }
    }
    
    /**
     * Stop the pipeline
     */
    public void stop() {
        running = false;
        executor.shutdown();
        try {
            if (!executor.awaitTermination(5, TimeUnit.SECONDS)) {
                executor.shutdownNow();
            }
        } catch (InterruptedException e) {
            executor.shutdownNow();
            Thread.currentThread().interrupt();
        }
    }
    
    /**
     * Queue a query for pipelined execution
     */
    public CompletableFuture<T> queue(String sql) {
        return queue(sql, null);
    }
    
    /**
     * Queue a query with parameters
     */
    public CompletableFuture<T> queue(String sql, List<Object> params) {
        CompletableFuture<T> future = new CompletableFuture<>();
        
        if (inFlight.get() >= config.getMaxInFlight()) {
            future.completeExceptionally(new IllegalStateException("Pipeline at capacity"));
            return future;
        }
        
        PipelinedRequest<T> request = new PipelinedRequest<>(sql, params, future);
        queue.offer(request);
        
        // Auto-flush if threshold reached
        if (config.isAutoFlush() && queue.size() >= config.getAutoFlushThreshold()) {
            synchronized (this) {
                this.notify();
            }
        }
        
        return future;
    }
    
    /**
     * Get pending count
     */
    public int getPendingCount() {
        return queue.size();
    }
    
    /**
     * Get in-flight count
     */
    public int getInFlightCount() {
        return inFlight.get();
    }
    
    /**
     * Check if pipeline has capacity
     */
    public boolean hasCapacity() {
        return inFlight.get() < config.getMaxInFlight();
    }
    
    /**
     * Trigger manual flush
     */
    public synchronized void flush() {
        this.notify();
    }
    
    private void runLoop(Connection connection) {
        while (running) {
            try {
                // Wait for requests or timeout
                synchronized (this) {
                    this.wait(100);
                }
                
                // Process pending requests
                List<PipelinedRequest<T>> batch = new ArrayList<>();
                queue.drainTo(batch);
                
                if (!batch.isEmpty()) {
                    processBatch(connection, batch);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            } catch (Exception e) {
                LOGGER.warning("Pipeline execution error: " + e.getMessage());
            }
        }
    }
    
    @SuppressWarnings("unchecked")
    private void processBatch(Connection connection, List<PipelinedRequest<T>> batch) {
        inFlight.addAndGet(batch.size());
        
        try {
            // Execute each request in the batch
            for (PipelinedRequest<T> request : batch) {
                try {
                    T result = executeRequest(connection, request);
                    request.future.complete(result);
                } catch (Exception e) {
                    request.future.completeExceptionally(e);
                }
            }
        } finally {
            inFlight.addAndGet(-batch.size());
        }
    }
    
    @SuppressWarnings("unchecked")
    private T executeRequest(Connection connection, PipelinedRequest<T> request) throws SQLException {
        try (PreparedStatement stmt = connection.prepareStatement(request.sql)) {
            // Bind parameters
            if (request.params != null) {
                for (int i = 0; i < request.params.size(); i++) {
                    stmt.setObject(i + 1, request.params.get(i));
                }
            }
            
            // Determine if query or update
            boolean hasResultSet = stmt.execute();
            
            if (hasResultSet) {
                try (ResultSet rs = stmt.getResultSet()) {
                    List<Object[]> rows = new ArrayList<>();
                    int columnCount = rs.getMetaData().getColumnCount();
                    
                    while (rs.next()) {
                        Object[] row = new Object[columnCount];
                        for (int i = 0; i < columnCount; i++) {
                            row[i] = rs.getObject(i + 1);
                        }
                        rows.add(row);
                    }
                    return (T) rows;
                }
            } else {
                int updateCount = stmt.getUpdateCount();
                return (T) Integer.valueOf(updateCount);
            }
        }
    }
}

/**
 * Builder for pipeline batch operations
 */
class PipelineBuilder {
    private final List<String> queries;
    
    public PipelineBuilder() {
        this.queries = new ArrayList<>();
    }
    
    public PipelineBuilder add(String sql) {
        queries.add(sql);
        return this;
    }
    
    public List<String> build() {
        return new ArrayList<>(queries);
    }
}
