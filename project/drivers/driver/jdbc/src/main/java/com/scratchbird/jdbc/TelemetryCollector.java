// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * OpenTelemetry-compatible Telemetry and Metrics
 * Copyright (c) 2025-2026 Dalton Calford
 */
package com.scratchbird.jdbc;

import java.time.Duration;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Logger;

/**
 * Configuration for telemetry
 */
class TelemetryConfig {
    public static final long DEFAULT_SLOW_QUERY_THRESHOLD_MS = 1000;
    public static final double DEFAULT_SAMPLE_RATE = 1.0;
    
    private boolean enableTracing = true;
    private boolean enableMetrics = true;
    private boolean enableSlowQueryLog = true;
    private long slowQueryThresholdMs = DEFAULT_SLOW_QUERY_THRESHOLD_MS;
    private boolean sanitizeQueries = true;
    private double sampleRate = DEFAULT_SAMPLE_RATE;
    
    public TelemetryConfig() {}
    
    public TelemetryConfig enableTracing(boolean enable) {
        this.enableTracing = enable;
        return this;
    }
    
    public TelemetryConfig enableMetrics(boolean enable) {
        this.enableMetrics = enable;
        return this;
    }
    
    public TelemetryConfig enableSlowQueryLog(boolean enable) {
        this.enableSlowQueryLog = enable;
        return this;
    }
    
    public TelemetryConfig slowQueryThreshold(long ms) {
        this.slowQueryThresholdMs = ms;
        return this;
    }
    
    public TelemetryConfig sanitizeQueries(boolean sanitize) {
        this.sanitizeQueries = sanitize;
        return this;
    }
    
    public TelemetryConfig sampleRate(double rate) {
        this.sampleRate = Math.max(0.0, Math.min(1.0, rate));
        return this;
    }
    
    public boolean isEnableTracing() { return enableTracing; }
    public boolean isEnableMetrics() { return enableMetrics; }
    public boolean isEnableSlowQueryLog() { return enableSlowQueryLog; }
    public long getSlowQueryThresholdMs() { return slowQueryThresholdMs; }
    public boolean isSanitizeQueries() { return sanitizeQueries; }
    public double getSampleRate() { return sampleRate; }
}

/**
 * Span context for distributed tracing
 */
class SpanContext {
    public final String traceId;
    public final String spanId;
    public final String parentSpanId;
    public final String spanName;
    public final Instant startTime;
    public final Map<String, String> attributes;
    
    public SpanContext(String name) {
        this.traceId = generateTraceId();
        this.spanId = generateSpanId();
        this.parentSpanId = null;
        this.spanName = name;
        this.startTime = Instant.now();
        this.attributes = new ConcurrentHashMap<>();
    }
    
    public SpanContext(String name, SpanContext parent) {
        this.traceId = parent.traceId;
        this.spanId = generateSpanId();
        this.parentSpanId = parent.spanId;
        this.spanName = name;
        this.startTime = Instant.now();
        this.attributes = new ConcurrentHashMap<>();
    }
    
    public SpanContext withAttribute(String key, String value) {
        attributes.put(key, value);
        return this;
    }
    
    public Duration elapsed() {
        return Duration.between(startTime, Instant.now());
    }
    
    private static String generateTraceId() {
        return UUID.randomUUID().toString().replace("-", "");
    }
    
    private static String generateSpanId() {
        return UUID.randomUUID().toString().replace("-", "").substring(0, 16);
    }
}

/**
 * Latency histogram
 */
class LatencyHistogram {
    final AtomicLong ms0_10 = new AtomicLong(0);
    final AtomicLong ms10_100 = new AtomicLong(0);
    final AtomicLong ms100_1000 = new AtomicLong(0);
    final AtomicLong ms1000_10000 = new AtomicLong(0);
    final AtomicLong msOver10000 = new AtomicLong(0);
    
    void record(long durationMs) {
        if (durationMs <= 10) ms0_10.incrementAndGet();
        else if (durationMs <= 100) ms10_100.incrementAndGet();
        else if (durationMs <= 1000) ms100_1000.incrementAndGet();
        else if (durationMs <= 10000) ms1000_10000.incrementAndGet();
        else msOver10000.incrementAndGet();
    }
}

/**
 * Operation metrics
 */
class OperationMetrics {
    final AtomicLong count = new AtomicLong(0);
    final AtomicLong totalTimeMs = new AtomicLong(0);
    volatile long avgTimeMs = 0;
    final AtomicLong errorCount = new AtomicLong(0);
    
    void record(long durationMs, boolean success) {
        long newCount = count.incrementAndGet();
        long newTotal = totalTimeMs.addAndGet(durationMs);
        avgTimeMs = newTotal / newCount;
        if (!success) errorCount.incrementAndGet();
    }
}

/**
 * Metrics snapshot
 */
class Metrics {
    public final long totalQueries;
    public final long successfulQueries;
    public final long failedQueries;
    public final long totalQueryTimeMs;
    public final LatencyHistogramSnapshot latencyHistogram;
    public final Map<String, OperationMetricsSnapshot> operationMetrics;
    
    public Metrics(long totalQueries, long successfulQueries, long failedQueries,
                   long totalQueryTimeMs, LatencyHistogramSnapshot histogram,
                   Map<String, OperationMetricsSnapshot> opMetrics) {
        this.totalQueries = totalQueries;
        this.successfulQueries = successfulQueries;
        this.failedQueries = failedQueries;
        this.totalQueryTimeMs = totalQueryTimeMs;
        this.latencyHistogram = histogram;
        this.operationMetrics = opMetrics;
    }
}

/**
 * Histogram snapshot
 */
class LatencyHistogramSnapshot {
    public final long ms0_10;
    public final long ms10_100;
    public final long ms100_1000;
    public final long ms1000_10000;
    public final long msOver10000;
    
    public LatencyHistogramSnapshot(long ms0_10, long ms10_100, long ms100_1000,
                                     long ms1000_10000, long msOver10000) {
        this.ms0_10 = ms0_10;
        this.ms10_100 = ms10_100;
        this.ms100_1000 = ms100_1000;
        this.ms1000_10000 = ms1000_10000;
        this.msOver10000 = msOver10000;
    }
}

/**
 * Operation metrics snapshot
 */
class OperationMetricsSnapshot {
    public final long count;
    public final long totalTimeMs;
    public final long avgTimeMs;
    public final long errorCount;
    
    public OperationMetricsSnapshot(long count, long totalTimeMs, long avgTimeMs, long errorCount) {
        this.count = count;
        this.totalTimeMs = totalTimeMs;
        this.avgTimeMs = avgTimeMs;
        this.errorCount = errorCount;
    }
}

/**
 * Slow query log entry
 */
class SlowQueryLog {
    public final String traceId;
    public final String spanName;
    public final long durationMs;
    public final Instant timestamp;
    public final Map<String, String> attributes;
    
    public SlowQueryLog(String traceId, String spanName, long durationMs, 
                        Map<String, String> attributes) {
        this.traceId = traceId;
        this.spanName = spanName;
        this.durationMs = durationMs;
        this.timestamp = Instant.now();
        this.attributes = new HashMap<>(attributes);
    }
}

/**
 * Telemetry collector for spans and metrics
 */
public class TelemetryCollector {
    private static final Logger LOGGER = Logger.getLogger(TelemetryCollector.class.getName());
    
    private final TelemetryConfig config;
    private final List<SpanContext> spans;
    private final AtomicLong totalQueries;
    private final AtomicLong successfulQueries;
    private final AtomicLong failedQueries;
    private final AtomicLong totalQueryTimeMs;
    private final LatencyHistogram histogram;
    private final Map<String, OperationMetrics> operationMetrics;
    private final List<SlowQueryLog> slowQueries;
    
    public TelemetryCollector(TelemetryConfig config) {
        this.config = config;
        this.spans = Collections.synchronizedList(new ArrayList<>());
        this.totalQueries = new AtomicLong(0);
        this.successfulQueries = new AtomicLong(0);
        this.failedQueries = new AtomicLong(0);
        this.totalQueryTimeMs = new AtomicLong(0);
        this.histogram = new LatencyHistogram();
        this.operationMetrics = new ConcurrentHashMap<>();
        this.slowQueries = Collections.synchronizedList(new ArrayList<>());
    }
    
    public TelemetryCollector() {
        this(new TelemetryConfig());
    }
    
    /**
     * Start a new span
     */
    public SpanContext startSpan(String name) {
        if (!config.isEnableTracing()) {
            return null;
        }
        
        // Check sample rate
        if (Math.random() > config.getSampleRate()) {
            return null;
        }
        
        SpanContext span = new SpanContext(name);
        spans.add(span);
        
        // Keep only last 1000 spans
        if (spans.size() > 1000) {
            spans.remove(0);
        }
        
        return span;
    }
    
    /**
     * End a span and record metrics
     */
    public void endSpan(SpanContext span, boolean success) {
        if (!config.isEnableTracing() || span == null) {
            return;
        }
        
        long durationMs = span.elapsed().toMillis();
        
        // Record metrics
        recordQueryMetrics(span.spanName, durationMs, success);
        
        // Log slow queries
        if (config.isEnableSlowQueryLog() && durationMs > config.getSlowQueryThresholdMs()) {
            recordSlowQuery(span, durationMs);
        }
    }
    
    /**
     * Record query metrics
     */
    private void recordQueryMetrics(String operation, long durationMs, boolean success) {
        if (!config.isEnableMetrics()) {
            return;
        }
        
        totalQueries.incrementAndGet();
        
        if (success) {
            successfulQueries.incrementAndGet();
        } else {
            failedQueries.incrementAndGet();
        }
        
        totalQueryTimeMs.addAndGet(durationMs);
        histogram.record(durationMs);
        
        // Per-operation metrics
        OperationMetrics metrics = operationMetrics.computeIfAbsent(operation, k -> new OperationMetrics());
        metrics.record(durationMs, success);
    }
    
    /**
     * Record slow query
     */
    private void recordSlowQuery(SpanContext span, long durationMs) {
        SlowQueryLog log = new SlowQueryLog(span.traceId, span.spanName, durationMs, span.attributes);
        slowQueries.add(log);
        
        // Keep only last 100 slow queries
        if (slowQueries.size() > 100) {
            slowQueries.remove(0);
        }
        
        LOGGER.warning(String.format("Slow query detected: %s (%d ms)", span.spanName, durationMs));
    }
    
    /**
     * Get current metrics
     */
    public Metrics getMetrics() {
        Map<String, OperationMetricsSnapshot> opMetrics = new HashMap<>();
        operationMetrics.forEach((k, v) -> {
            opMetrics.put(k, new OperationMetricsSnapshot(
                v.count.get(), v.totalTimeMs.get(), v.avgTimeMs, v.errorCount.get()
            ));
        });
        
        return new Metrics(
            totalQueries.get(),
            successfulQueries.get(),
            failedQueries.get(),
            totalQueryTimeMs.get(),
            new LatencyHistogramSnapshot(
                histogram.ms0_10.get(),
                histogram.ms10_100.get(),
                histogram.ms100_1000.get(),
                histogram.ms1000_10000.get(),
                histogram.msOver10000.get()
            ),
            opMetrics
        );
    }
    
    /**
     * Get slow queries
     */
    public List<SlowQueryLog> getSlowQueries() {
        return new ArrayList<>(slowQueries);
    }
    
    /**
     * Sanitize query text
     */
    public static String sanitizeQuery(String sql) {
        if (sql == null) return null;
        
        // Replace quoted strings with '?'
        return sql.replaceAll("'[^']*'", "'?'");
    }
    
    /**
     * Export metrics in Prometheus format
     */
    public String exportPrometheusMetrics() {
        Metrics m = getMetrics();
        StringBuilder sb = new StringBuilder();
        
        // Total queries
        sb.append("# HELP scratchbird_queries_total Total number of queries\n");
        sb.append("# TYPE scratchbird_queries_total counter\n");
        sb.append(String.format("scratchbird_queries_total %d\n", m.totalQueries));
        
        // Latency histogram
        sb.append("# HELP scratchbird_query_duration_ms Query duration histogram\n");
        sb.append("# TYPE scratchbird_query_duration_ms histogram\n");
        sb.append(String.format("scratchbird_query_duration_ms_bucket{le=\"10\"} %d\n", m.latencyHistogram.ms0_10));
        sb.append(String.format("scratchbird_query_duration_ms_bucket{le=\"100\"} %d\n", 
            m.latencyHistogram.ms0_10 + m.latencyHistogram.ms10_100));
        sb.append(String.format("scratchbird_query_duration_ms_bucket{le=\"1000\"} %d\n",
            m.latencyHistogram.ms0_10 + m.latencyHistogram.ms10_100 + m.latencyHistogram.ms100_1000));
        
        return sb.toString();
    }
}

/**
 * Span guard for automatic completion
 */
class TelemetrySpanGuard implements AutoCloseable {
    private final TelemetryCollector collector;
    private final SpanContext span;
    private boolean success = true;
    
    TelemetrySpanGuard(TelemetryCollector collector, SpanContext span) {
        this.collector = collector;
        this.span = span;
    }
    
    public void markFailed() {
        this.success = false;
    }
    
    @Override
    public void close() {
        collector.endSpan(span, success);
    }
}
