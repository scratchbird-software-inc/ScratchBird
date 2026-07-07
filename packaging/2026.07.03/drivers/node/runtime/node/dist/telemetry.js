"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.TelemetryCollector = exports.SpanContext = void 0;
const node_crypto_1 = require("node:crypto");
class SpanContext {
    constructor(name, parent) {
        this.startTime = Date.now();
        this.attributes = {};
        this.traceId = parent?.traceId ?? (0, node_crypto_1.randomBytes)(16).toString("hex");
        this.spanId = (0, node_crypto_1.randomBytes)(8).toString("hex");
        this.parentSpanId = parent?.spanId;
        this.spanName = name;
    }
    withAttribute(key, value) {
        this.attributes[key] = value;
        return this;
    }
    elapsedMs() {
        return Date.now() - this.startTime;
    }
}
exports.SpanContext = SpanContext;
class LatencyHistogram {
    constructor() {
        this.ms0_10 = 0;
        this.ms10_100 = 0;
        this.ms100_1000 = 0;
        this.ms1000_10000 = 0;
        this.msOver10000 = 0;
    }
    record(durationMs) {
        if (durationMs <= 10)
            this.ms0_10 += 1;
        else if (durationMs <= 100)
            this.ms10_100 += 1;
        else if (durationMs <= 1000)
            this.ms100_1000 += 1;
        else if (durationMs <= 10000)
            this.ms1000_10000 += 1;
        else
            this.msOver10000 += 1;
    }
}
class OperationMetrics {
    constructor() {
        this.count = 0;
        this.totalTimeMs = 0;
        this.avgTimeMs = 0;
        this.errorCount = 0;
    }
    record(durationMs, success) {
        this.count += 1;
        this.totalTimeMs += durationMs;
        this.avgTimeMs = Math.floor(this.totalTimeMs / this.count);
        if (!success)
            this.errorCount += 1;
    }
}
class TelemetryCollector {
    constructor(config = {}) {
        this.spans = [];
        this.totalQueries = 0;
        this.successfulQueries = 0;
        this.failedQueries = 0;
        this.totalQueryTimeMs = 0;
        this.histogram = new LatencyHistogram();
        this.operationMetrics = new Map();
        this.slowQueries = [];
        this.config = {
            enableTracing: config.enableTracing ?? true,
            enableMetrics: config.enableMetrics ?? true,
            enableSlowQueryLog: config.enableSlowQueryLog ?? true,
            slowQueryThresholdMs: config.slowQueryThresholdMs ?? 1000,
            sanitizeQueries: config.sanitizeQueries ?? true,
            sampleRate: config.sampleRate ?? 1.0,
        };
    }
    startSpan(name) {
        if (!this.config.enableTracing)
            return null;
        if (Math.random() > this.config.sampleRate)
            return null;
        const span = new SpanContext(name);
        this.spans.push(span);
        if (this.spans.length > 1000) {
            this.spans.shift();
        }
        return span;
    }
    endSpan(span, success = true) {
        if (!span || !this.config.enableTracing)
            return;
        const durationMs = span.elapsedMs();
        this.recordQueryMetrics(span.spanName, durationMs, success);
        if (this.config.enableSlowQueryLog && durationMs > this.config.slowQueryThresholdMs) {
            this.recordSlowQuery(span, durationMs);
        }
    }
    metrics() {
        return {
            totalQueries: this.totalQueries,
            successfulQueries: this.successfulQueries,
            failedQueries: this.failedQueries,
            totalQueryTimeMs: this.totalQueryTimeMs,
            latencyHistogram: this.histogram,
            operationMetrics: Array.from(this.operationMetrics.entries()).reduce((acc, [k, v]) => {
                acc[k] = { count: v.count, avgTimeMs: v.avgTimeMs, errorCount: v.errorCount };
                return acc;
            }, {}),
        };
    }
    slowQueryLog() {
        return [...this.slowQueries];
    }
    static sanitizeQuery(sql) {
        if (!sql)
            return "";
        return sql.replace(/'[^']*'/g, "'?'");
    }
    exportPrometheusMetrics() {
        const h = this.histogram;
        return [
            "# HELP scratchbird_queries_total Total number of queries",
            "# TYPE scratchbird_queries_total counter",
            `scratchbird_queries_total ${this.totalQueries}`,
            "# HELP scratchbird_query_duration_ms Query duration histogram",
            "# TYPE scratchbird_query_duration_ms histogram",
            `scratchbird_query_duration_ms_bucket{le=\"10\"} ${h.ms0_10}`,
            `scratchbird_query_duration_ms_bucket{le=\"100\"} ${h.ms0_10 + h.ms10_100}`,
            `scratchbird_query_duration_ms_bucket{le=\"1000\"} ${h.ms0_10 + h.ms10_100 + h.ms100_1000}`,
        ].join("\n");
    }
    recordQueryMetrics(operation, durationMs, success) {
        if (!this.config.enableMetrics)
            return;
        this.totalQueries += 1;
        if (success)
            this.successfulQueries += 1;
        else
            this.failedQueries += 1;
        this.totalQueryTimeMs += durationMs;
        this.histogram.record(durationMs);
        const metrics = this.operationMetrics.get(operation) ?? new OperationMetrics();
        metrics.record(durationMs, success);
        this.operationMetrics.set(operation, metrics);
    }
    recordSlowQuery(span, durationMs) {
        this.slowQueries.push({
            traceId: span.traceId,
            spanName: span.spanName,
            durationMs,
            timestamp: new Date().toISOString(),
            attributes: span.attributes,
        });
        if (this.slowQueries.length > 100) {
            this.slowQueries.shift();
        }
    }
}
exports.TelemetryCollector = TelemetryCollector;
