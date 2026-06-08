# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""
OpenTelemetry-compatible Telemetry Module
Distributed tracing and metrics collection
"""

import time
import random
import re
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional, Callable
from threading import Lock
import uuid


@dataclass
class TelemetryConfig:
    """Configuration for telemetry"""
    enable_tracing: bool = True
    enable_metrics: bool = True
    enable_slow_query_log: bool = True
    slow_query_threshold_ms: int = 1000
    sanitize_queries: bool = True
    sample_rate: float = 1.0


@dataclass
class SpanContext:
    """Distributed tracing span context"""
    trace_id: str
    span_id: str
    parent_span_id: Optional[str]
    span_name: str
    start_time: float
    attributes: Dict[str, str] = field(default_factory=dict)
    
    @classmethod
    def new(cls, name: str) -> 'SpanContext':
        """Create a new root span"""
        return cls(
            trace_id=uuid.uuid4().hex,
            span_id=uuid.uuid4().hex[:16],
            parent_span_id=None,
            span_name=name,
            start_time=time.time()
        )
    
    @classmethod
    def child(cls, name: str, parent: 'SpanContext') -> 'SpanContext':
        """Create a child span"""
        return cls(
            trace_id=parent.trace_id,
            span_id=uuid.uuid4().hex[:16],
            parent_span_id=parent.span_id,
            span_name=name,
            start_time=time.time()
        )
    
    def with_attribute(self, key: str, value: str) -> 'SpanContext':
        """Add an attribute"""
        self.attributes[key] = value
        return self
    
    def elapsed_ms(self) -> int:
        """Get elapsed time in milliseconds"""
        return int((time.time() - self.start_time) * 1000)


@dataclass
class LatencyHistogram:
    """Latency distribution histogram"""
    ms_0_10: int = 0
    ms_10_100: int = 0
    ms_100_1000: int = 0
    ms_1000_10000: int = 0
    ms_over_10000: int = 0
    _lock: Lock = field(default_factory=Lock)
    
    def record(self, duration_ms: int) -> None:
        """Record a duration"""
        with self._lock:
            if duration_ms <= 10:
                self.ms_0_10 += 1
            elif duration_ms <= 100:
                self.ms_10_100 += 1
            elif duration_ms <= 1000:
                self.ms_100_1000 += 1
            elif duration_ms <= 10000:
                self.ms_1000_10000 += 1
            else:
                self.ms_over_10000 += 1
    
    def to_dict(self) -> Dict[str, int]:
        """Convert to dictionary"""
        with self._lock:
            return {
                'ms_0_10': self.ms_0_10,
                'ms_10_100': self.ms_10_100,
                'ms_100_1000': self.ms_100_1000,
                'ms_1000_10000': self.ms_1000_10000,
                'ms_over_10000': self.ms_over_10000
            }


@dataclass
class OperationMetrics:
    """Metrics for a specific operation"""
    count: int = 0
    total_time_ms: int = 0
    avg_time_ms: int = 0
    error_count: int = 0
    _lock: Lock = field(default_factory=Lock)
    
    def record(self, duration_ms: int, success: bool) -> None:
        """Record a metric"""
        with self._lock:
            self.count += 1
            self.total_time_ms += duration_ms
            self.avg_time_ms = self.total_time_ms // self.count
            if not success:
                self.error_count += 1
    
    def to_dict(self) -> Dict[str, int]:
        """Convert to dictionary"""
        with self._lock:
            return {
                'count': self.count,
                'total_time_ms': self.total_time_ms,
                'avg_time_ms': self.avg_time_ms,
                'error_count': self.error_count
            }


@dataclass
class SlowQueryLog:
    """Slow query log entry"""
    trace_id: str
    span_name: str
    duration_ms: int
    timestamp: datetime
    attributes: Dict[str, str]


@dataclass
class MetricsSnapshot:
    """Snapshot of current metrics"""
    total_queries: int
    successful_queries: int
    failed_queries: int
    total_query_time_ms: int
    latency_histogram: Dict[str, int]
    operation_metrics: Dict[str, Dict[str, int]]


class TelemetryCollector:
    """Collects spans and metrics"""
    
    def __init__(self, config: TelemetryConfig = None):
        self.config = config or TelemetryConfig()
        self._spans: List[SpanContext] = []
        self._spans_lock = Lock()
        self._total_queries = 0
        self._successful_queries = 0
        self._failed_queries = 0
        self._total_query_time_ms = 0
        self._metrics_lock = Lock()
        self._histogram = LatencyHistogram()
        self._operation_metrics: Dict[str, OperationMetrics] = {}
        self._op_metrics_lock = Lock()
        self._slow_queries: List[SlowQueryLog] = []
        self._slow_queries_lock = Lock()
    
    def start_span(self, name: str) -> Optional[SpanContext]:
        """Start a new span"""
        if not self.config.enable_tracing:
            return None
        
        # Sample rate check
        if random.random() > self.config.sample_rate:
            return None
        
        span = SpanContext.new(name)
        
        with self._spans_lock:
            self._spans.append(span)
            if len(self._spans) > 1000:
                self._spans.pop(0)
        
        return span
    
    def end_span(self, span: SpanContext, success: bool = True) -> None:
        """End a span and record metrics"""
        if not span or not self.config.enable_tracing:
            return
        
        duration_ms = span.elapsed_ms()
        self._record_query_metrics(span.span_name, duration_ms, success)
        
        if self.config.enable_slow_query_log and duration_ms > self.config.slow_query_threshold_ms:
            self._record_slow_query(span, duration_ms)
    
    def _record_query_metrics(self, operation: str, duration_ms: int, success: bool) -> None:
        """Record query metrics"""
        if not self.config.enable_metrics:
            return
        
        with self._metrics_lock:
            self._total_queries += 1
            if success:
                self._successful_queries += 1
            else:
                self._failed_queries += 1
            self._total_query_time_ms += duration_ms
        
        self._histogram.record(duration_ms)
        
        # Per-operation metrics
        with self._op_metrics_lock:
            if operation not in self._operation_metrics:
                self._operation_metrics[operation] = OperationMetrics()
            self._operation_metrics[operation].record(duration_ms, success)
    
    def _record_slow_query(self, span: SpanContext, duration_ms: int) -> None:
        """Record a slow query"""
        log = SlowQueryLog(
            trace_id=span.trace_id,
            span_name=span.span_name,
            duration_ms=duration_ms,
            timestamp=datetime.utcnow(),
            attributes=span.attributes.copy()
        )
        
        with self._slow_queries_lock:
            self._slow_queries.append(log)
            if len(self._slow_queries) > 100:
                self._slow_queries.pop(0)
    
    def get_metrics(self) -> MetricsSnapshot:
        """Get current metrics snapshot"""
        with self._metrics_lock:
            total = self._total_queries
            successful = self._successful_queries
            failed = self._failed_queries
            total_time = self._total_query_time_ms
        
        op_metrics = {}
        with self._op_metrics_lock:
            for op, metrics in self._operation_metrics.items():
                op_metrics[op] = metrics.to_dict()
        
        return MetricsSnapshot(
            total_queries=total,
            successful_queries=successful,
            failed_queries=failed,
            total_query_time_ms=total_time,
            latency_histogram=self._histogram.to_dict(),
            operation_metrics=op_metrics
        )
    
    def get_slow_queries(self) -> List[SlowQueryLog]:
        """Get slow query logs"""
        with self._slow_queries_lock:
            return self._slow_queries.copy()
    
    @staticmethod
    def sanitize_query(sql: str) -> str:
        """Sanitize query by removing sensitive data"""
        if not sql:
            return sql
        # Replace quoted strings
        return re.sub(r"'[^']*'", "'?'", sql)
    
    def export_prometheus_metrics(self) -> str:
        """Export metrics in Prometheus format"""
        m = self.get_metrics()
        h = m.latency_histogram
        
        lines = [
            "# HELP scratchbird_queries_total Total number of queries",
            "# TYPE scratchbird_queries_total counter",
            f"scratchbird_queries_total {m.total_queries}",
            "# HELP scratchbird_query_duration_ms Query duration histogram",
            "# TYPE scratchbird_query_duration_ms histogram",
            f"scratchbird_query_duration_ms_bucket{{le=\"10\"}} {h['ms_0_10']}",
            f"scratchbird_query_duration_ms_bucket{{le=\"100\"}} {h['ms_0_10'] + h['ms_10_100']}",
            f"scratchbird_query_duration_ms_bucket{{le=\"1000\"}} {h['ms_0_10'] + h['ms_10_100'] + h['ms_100_1000']}",
        ]
        
        return "\n".join(lines)


class TelemetrySpanGuard:
    """RAII guard for span lifecycle management"""
    
    def __init__(self, collector: TelemetryCollector, span: SpanContext):
        self.collector = collector
        self.span = span
        self.success = True
    
    def mark_failed(self) -> None:
        """Mark the span as failed"""
        self.success = False
    
    def __enter__(self) -> 'TelemetrySpanGuard':
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is not None:
            self.success = False
        self.collector.end_span(self.span, self.success)
