// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Telemetry Module
// Distributed tracing and metrics for observability

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

/// Configuration for telemetry
#[derive(Debug, Clone)]
pub struct TelemetryConfig {
    /// Enable distributed tracing (default: true)
    pub enable_tracing: bool,
    /// Enable metrics collection (default: true)
    pub enable_metrics: bool,
    /// Enable slow query logging (default: true)
    pub enable_slow_query_log: bool,
    /// Threshold for slow queries (default: 1 second)
    pub slow_query_threshold: Duration,
    /// Sanitize query text to remove sensitive data (default: true)
    pub sanitize_queries: bool,
    /// Sample rate for tracing (0.0-1.0, default: 1.0)
    pub sample_rate: f64,
}

impl Default for TelemetryConfig {
    fn default() -> Self {
        Self {
            enable_tracing: true,
            enable_metrics: true,
            enable_slow_query_log: true,
            slow_query_threshold: Duration::from_secs(1),
            sanitize_queries: true,
            sample_rate: 1.0,
        }
    }
}

/// Span context for distributed tracing
#[derive(Debug, Clone)]
pub struct SpanContext {
    pub trace_id: String,
    pub span_id: String,
    pub parent_span_id: Option<String>,
    pub span_name: String,
    pub start_time: Instant,
    pub attributes: HashMap<String, String>,
}

impl SpanContext {
    pub fn new(name: &str) -> Self {
        Self {
            trace_id: generate_trace_id(),
            span_id: generate_span_id(),
            parent_span_id: None,
            span_name: name.to_string(),
            start_time: Instant::now(),
            attributes: HashMap::new(),
        }
    }

    pub fn with_parent(mut self, parent: &SpanContext) -> Self {
        self.trace_id = parent.trace_id.clone();
        self.parent_span_id = Some(parent.span_id.clone());
        self
    }

    pub fn with_attribute(mut self, key: &str, value: &str) -> Self {
        self.attributes.insert(key.to_string(), value.to_string());
        self
    }

    pub fn elapsed(&self) -> Duration {
        self.start_time.elapsed()
    }
}

fn generate_trace_id() -> String {
    format!("{:032x}", rand::random::<u128>())
}

fn generate_span_id() -> String {
    format!("{:016x}", rand::random::<u64>())
}

/// Telemetry collector for spans and metrics
pub struct TelemetryCollector {
    config: TelemetryConfig,
    spans: Arc<RwLock<Vec<SpanContext>>>,
    metrics: Arc<RwLock<Metrics>>,
    slow_queries: Arc<RwLock<Vec<SlowQueryLog>>>,
}

impl TelemetryCollector {
    pub fn new(config: TelemetryConfig) -> Self {
        Self {
            config,
            spans: Arc::new(RwLock::new(Vec::new())),
            metrics: Arc::new(RwLock::new(Metrics::default())),
            slow_queries: Arc::new(RwLock::new(Vec::new())),
        }
    }

    /// Start a new span
    pub async fn start_span(&self, name: &str) -> Option<SpanContext> {
        if !self.config.enable_tracing {
            return None;
        }

        // Check sample rate
        if rand::random::<f64>() > self.config.sample_rate {
            return None;
        }

        let span = SpanContext::new(name);

        let mut spans = self.spans.write().await;
        spans.push(span.clone());

        // Keep only last 1000 spans
        if spans.len() > 1000 {
            spans.remove(0);
        }

        Some(span)
    }

    /// End a span and record metrics
    pub async fn end_span(&self, span: SpanContext, success: bool) {
        if !self.config.enable_tracing {
            return;
        }

        let duration = span.elapsed();

        // Record metrics
        self.record_query_metrics(&span.span_name, duration, success)
            .await;

        // Log slow queries
        if self.config.enable_slow_query_log && duration > self.config.slow_query_threshold {
            self.record_slow_query(&span, duration).await;
        }
    }

    /// Record query metrics
    async fn record_query_metrics(&self, operation: &str, duration: Duration, success: bool) {
        if !self.config.enable_metrics {
            return;
        }

        let mut metrics = self.metrics.write().await;

        metrics.total_queries += 1;

        if success {
            metrics.successful_queries += 1;
        } else {
            metrics.failed_queries += 1;
        }

        let duration_ms = duration.as_millis() as u64;
        metrics.total_query_time_ms += duration_ms;

        // Update histogram bucket
        let bucket = match duration_ms {
            0..=10 => &mut metrics.latency_histogram.ms_0_10,
            11..=100 => &mut metrics.latency_histogram.ms_10_100,
            101..=1000 => &mut metrics.latency_histogram.ms_100_1000,
            1001..=10000 => &mut metrics.latency_histogram.ms_1000_10000,
            _ => &mut metrics.latency_histogram.ms_over_10000,
        };
        *bucket += 1;

        // Per-operation metrics
        let op_metrics = metrics
            .operation_metrics
            .entry(operation.to_string())
            .or_insert_with(OperationMetrics::default);

        op_metrics.count += 1;
        op_metrics.total_time_ms += duration_ms;
        op_metrics.avg_time_ms = op_metrics.total_time_ms / op_metrics.count;

        if !success {
            op_metrics.error_count += 1;
        }
    }

    /// Record slow query
    async fn record_slow_query(&self, span: &SpanContext, duration: Duration) {
        let slow_query = SlowQueryLog {
            trace_id: span.trace_id.clone(),
            span_name: span.span_name.clone(),
            duration,
            timestamp: chrono::Utc::now(),
            attributes: span.attributes.clone(),
        };

        let mut slow_queries = self.slow_queries.write().await;
        slow_queries.push(slow_query);

        // Keep only last 100 slow queries
        if slow_queries.len() > 100 {
            slow_queries.remove(0);
        }
    }

    /// Get current metrics
    pub async fn get_metrics(&self) -> Metrics {
        self.metrics.read().await.clone()
    }

    /// Get slow queries
    pub async fn get_slow_queries(&self) -> Vec<SlowQueryLog> {
        self.slow_queries.read().await.clone()
    }

    /// Sanitize query text (remove sensitive data)
    pub fn sanitize_query(sql: &str) -> String {
        // Simple sanitization: replace quoted strings
        let mut result = String::new();
        let mut in_string = false;
        let mut prev_char = ' ';

        for ch in sql.chars() {
            if ch == '\'' && prev_char != '\\' {
                if in_string {
                    result.push('?');
                    result.push('\'');
                } else {
                    result.push('\'');
                }
                in_string = !in_string;
            } else if !in_string {
                result.push(ch);
            }
            prev_char = ch;
        }

        result
    }
}

/// Metrics snapshot
#[derive(Debug, Clone, Default)]
pub struct Metrics {
    pub total_queries: u64,
    pub successful_queries: u64,
    pub failed_queries: u64,
    pub total_query_time_ms: u64,
    pub latency_histogram: LatencyHistogram,
    pub operation_metrics: HashMap<String, OperationMetrics>,
    pub connection_pool_metrics: ConnectionPoolMetrics,
}

#[derive(Debug, Clone, Default)]
pub struct LatencyHistogram {
    pub ms_0_10: u64,
    pub ms_10_100: u64,
    pub ms_100_1000: u64,
    pub ms_1000_10000: u64,
    pub ms_over_10000: u64,
}

#[derive(Debug, Clone, Default)]
pub struct OperationMetrics {
    pub count: u64,
    pub total_time_ms: u64,
    pub avg_time_ms: u64,
    pub error_count: u64,
}

#[derive(Debug, Clone, Default)]
pub struct ConnectionPoolMetrics {
    pub total_connections: u32,
    pub active_connections: u32,
    pub idle_connections: u32,
    pub pending_requests: u32,
}

/// Slow query log entry
#[derive(Debug, Clone)]
pub struct SlowQueryLog {
    pub trace_id: String,
    pub span_name: String,
    pub duration: Duration,
    pub timestamp: chrono::DateTime<chrono::Utc>,
    pub attributes: HashMap<String, String>,
}

/// Export metrics in Prometheus format
pub fn export_prometheus_metrics(metrics: &Metrics) -> String {
    let mut output = String::new();

    // Total queries
    output.push_str("# HELP scratchbird_queries_total Total number of queries\n");
    output.push_str("# TYPE scratchbird_queries_total counter\n");
    output.push_str(&format!(
        "scratchbird_queries_total {}\n",
        metrics.total_queries
    ));

    // Query duration histogram
    output.push_str("# HELP scratchbird_query_duration_ms Query duration histogram\n");
    output.push_str("# TYPE scratchbird_query_duration_ms histogram\n");
    output.push_str(&format!(
        "scratchbird_query_duration_ms_bucket{{le=\"10\"}} {}\n",
        metrics.latency_histogram.ms_0_10
    ));
    output.push_str(&format!(
        "scratchbird_query_duration_ms_bucket{{le=\"100\"}} {}\n",
        metrics.latency_histogram.ms_0_10 + metrics.latency_histogram.ms_10_100
    ));
    output.push_str(&format!(
        "scratchbird_query_duration_ms_bucket{{le=\"1000\"}} {}\n",
        metrics.latency_histogram.ms_0_10
            + metrics.latency_histogram.ms_10_100
            + metrics.latency_histogram.ms_100_1000
    ));

    // Connection pool metrics
    output.push_str("# HELP scratchbird_pool_connections_total Total connections in pool\n");
    output.push_str("# TYPE scratchbird_pool_connections_total gauge\n");
    output.push_str(&format!(
        "scratchbird_pool_connections_total {}\n",
        metrics.connection_pool_metrics.total_connections
    ));

    output.push_str("# HELP scratchbird_pool_connections_active Active connections\n");
    output.push_str("# TYPE scratchbird_pool_connections_active gauge\n");
    output.push_str(&format!(
        "scratchbird_pool_connections_active {}\n",
        metrics.connection_pool_metrics.active_connections
    ));

    output
}
