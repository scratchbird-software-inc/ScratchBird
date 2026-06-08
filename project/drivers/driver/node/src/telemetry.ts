// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import { randomBytes } from "node:crypto";

export interface TelemetryConfig {
  enableTracing?: boolean;
  enableMetrics?: boolean;
  enableSlowQueryLog?: boolean;
  slowQueryThresholdMs?: number;
  sanitizeQueries?: boolean;
  sampleRate?: number;
}

export class SpanContext {
  readonly traceId: string;
  readonly spanId: string;
  readonly parentSpanId?: string;
  readonly spanName: string;
  readonly startTime = Date.now();
  readonly attributes: Record<string, string> = {};

  constructor(name: string, parent?: SpanContext) {
    this.traceId = parent?.traceId ?? randomBytes(16).toString("hex");
    this.spanId = randomBytes(8).toString("hex");
    this.parentSpanId = parent?.spanId;
    this.spanName = name;
  }

  withAttribute(key: string, value: string): SpanContext {
    this.attributes[key] = value;
    return this;
  }

  elapsedMs(): number {
    return Date.now() - this.startTime;
  }
}

class LatencyHistogram {
  ms0_10 = 0;
  ms10_100 = 0;
  ms100_1000 = 0;
  ms1000_10000 = 0;
  msOver10000 = 0;

  record(durationMs: number): void {
    if (durationMs <= 10) this.ms0_10 += 1;
    else if (durationMs <= 100) this.ms10_100 += 1;
    else if (durationMs <= 1000) this.ms100_1000 += 1;
    else if (durationMs <= 10000) this.ms1000_10000 += 1;
    else this.msOver10000 += 1;
  }
}

class OperationMetrics {
  count = 0;
  totalTimeMs = 0;
  avgTimeMs = 0;
  errorCount = 0;

  record(durationMs: number, success: boolean): void {
    this.count += 1;
    this.totalTimeMs += durationMs;
    this.avgTimeMs = Math.floor(this.totalTimeMs / this.count);
    if (!success) this.errorCount += 1;
  }
}

export class TelemetryCollector {
  private readonly config: Required<TelemetryConfig>;
  private spans: SpanContext[] = [];
  private totalQueries = 0;
  private successfulQueries = 0;
  private failedQueries = 0;
  private totalQueryTimeMs = 0;
  private histogram = new LatencyHistogram();
  private operationMetrics = new Map<string, OperationMetrics>();
  private slowQueries: Array<Record<string, unknown>> = [];

  constructor(config: TelemetryConfig = {}) {
    this.config = {
      enableTracing: config.enableTracing ?? true,
      enableMetrics: config.enableMetrics ?? true,
      enableSlowQueryLog: config.enableSlowQueryLog ?? true,
      slowQueryThresholdMs: config.slowQueryThresholdMs ?? 1000,
      sanitizeQueries: config.sanitizeQueries ?? true,
      sampleRate: config.sampleRate ?? 1.0,
    };
  }

  startSpan(name: string): SpanContext | null {
    if (!this.config.enableTracing) return null;
    if (Math.random() > this.config.sampleRate) return null;
    const span = new SpanContext(name);
    this.spans.push(span);
    if (this.spans.length > 1000) {
      this.spans.shift();
    }
    return span;
  }

  endSpan(span: SpanContext | null, success = true): void {
    if (!span || !this.config.enableTracing) return;
    const durationMs = span.elapsedMs();
    this.recordQueryMetrics(span.spanName, durationMs, success);
    if (this.config.enableSlowQueryLog && durationMs > this.config.slowQueryThresholdMs) {
      this.recordSlowQuery(span, durationMs);
    }
  }

  metrics(): Record<string, unknown> {
    return {
      totalQueries: this.totalQueries,
      successfulQueries: this.successfulQueries,
      failedQueries: this.failedQueries,
      totalQueryTimeMs: this.totalQueryTimeMs,
      latencyHistogram: this.histogram,
      operationMetrics: Array.from(this.operationMetrics.entries()).reduce<Record<string, unknown>>((acc, [k, v]) => {
        acc[k] = { count: v.count, avgTimeMs: v.avgTimeMs, errorCount: v.errorCount };
        return acc;
      }, {}),
    };
  }

  slowQueryLog(): Array<Record<string, unknown>> {
    return [...this.slowQueries];
  }

  static sanitizeQuery(sql?: string | null): string {
    if (!sql) return "";
    return sql.replace(/'[^']*'/g, "'?'");
  }

  exportPrometheusMetrics(): string {
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

  private recordQueryMetrics(operation: string, durationMs: number, success: boolean): void {
    if (!this.config.enableMetrics) return;
    this.totalQueries += 1;
    if (success) this.successfulQueries += 1;
    else this.failedQueries += 1;
    this.totalQueryTimeMs += durationMs;
    this.histogram.record(durationMs);
    const metrics = this.operationMetrics.get(operation) ?? new OperationMetrics();
    metrics.record(durationMs, success);
    this.operationMetrics.set(operation, metrics);
  }

  private recordSlowQuery(span: SpanContext, durationMs: number): void {
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
