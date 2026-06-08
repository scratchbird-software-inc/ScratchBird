// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

mod client;
mod config;
mod errors;
pub mod metadata;
pub mod pool;
pub mod protocol;
mod scram;
pub mod sql;
pub mod types;

// Resilience and monitoring modules
pub mod circuit_breaker;
pub mod keepalive;
pub mod leak_detection;
pub mod pipeline;
pub mod telemetry;

pub use client::{
    probe_auth_surface, AuthMethodSurface, AuthProbeResult, BatchItemSummary, BatchSummary, Client,
    CopyOptions, CopyResult, CopyState, FieldSummary, QueryResult, QueryStream,
    ResolvedAuthContext, ResultSetSummary,
};
pub use config::Config;
pub use errors::{
    is_retryable_sqlstate, retry_scope_for_sqlstate, Error, ErrorKind, Result, RetryScope,
};
pub use pool::{with_retry, ConnectionPool, PoolConfig, PoolStats, PooledConnection, RetryConfig};
pub use protocol::{
    canonical_read_committed_mode_label, READ_COMMITTED_MODE_DEFAULT,
    READ_COMMITTED_MODE_NO_RECORD_VERSION, READ_COMMITTED_MODE_READ_CONSISTENCY,
    READ_COMMITTED_MODE_RECORD_VERSION,
};
pub use sql::{normalize, normalize_callable, normalize_callable_sql, NormalizedQuery, Params};
pub use types::{
    Column, Date, Decimal, Geometry, Interval, Json, Jsonb, Money, Param, Range, RangeValue,
    RawValue, Time, Timestamp, TimestampTz, Value,
};

// Re-export key resilience types
pub use circuit_breaker::{
    with_circuit_breaker, CircuitBreaker, CircuitBreakerConfig, CircuitBreakerStats, CircuitState,
};
pub use keepalive::{KeepaliveConfig, KeepaliveTask, KeepaliveTracker};
pub use leak_detection::{
    CheckoutInfo, LeakDetectionConfig, LeakDetectionGuard, LeakDetector, LeakStatistics,
};
pub use pipeline::{PipelineBuilder, PipelineConfig, PipelineStats, QueryPipeline};
pub use telemetry::{
    export_prometheus_metrics, Metrics, SpanContext, TelemetryCollector, TelemetryConfig,
};
