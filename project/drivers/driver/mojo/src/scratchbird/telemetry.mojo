# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Driver - Telemetry scaffolding in current Mojo syntax.
# Copyright (c) 2025-2026 Dalton Calford



struct TelemetryConfig:
    var enable_tracing: Bool
    var enable_metrics: Bool
    var enable_slow_query_log: Bool
    var slow_query_threshold_ms: Int
    var sanitize_queries: Bool
    var sample_rate: Float64

    def __init__(out self):
        self.enable_tracing = True
        self.enable_metrics = True
        self.enable_slow_query_log = True
        self.slow_query_threshold_ms = 1000
        self.sanitize_queries = True
        self.sample_rate = 1.0


struct SpanContext:
    var trace_id: String
    var span_id: String
    var parent_span_id: String
    var span_name: String
    var start_time_ms: Int
    var sampled: Bool
    var attributes: List[String]

    def __init__(out self, name: String):
        self.trace_id = ""
        self.span_id = ""
        self.parent_span_id = ""
        self.span_name = name
        self.start_time_ms = 0
        self.sampled = False
        self.attributes = List[String]()

    def __init__(
        out self,
        name: String,
        trace_id: String,
        span_id: String,
        parent_span_id: String,
        start_time_ms: Int,
        sampled: Bool,
    ):
        self.trace_id = trace_id
        self.span_id = span_id
        self.parent_span_id = parent_span_id
        self.span_name = name
        self.start_time_ms = start_time_ms
        self.sampled = sampled
        self.attributes = List[String]()

    def with_attribute(mut self, key: String, value: String) -> Self:
        self.attributes.append(key + "=" + value)
        return self

    def elapsed_ms(self, end_time_ms: Int) -> Int:
        if end_time_ms <= self.start_time_ms:
            return 0
        return end_time_ms - self.start_time_ms


def _find_operation_index(operation_names: List[String], operation: String) -> Int:
    for i in range(len(operation_names)):
        if operation_names[i] == operation:
            return i
    return -1


struct TelemetryCollector:
    var enable_tracing: Bool
    var enable_metrics: Bool
    var enable_slow_query_log: Bool
    var slow_query_threshold_ms: Int
    var sanitize_queries_enabled: Bool
    var sample_rate: Float64
    var next_trace_id: Int
    var next_span_id: Int
    var total_queries: Int
    var successful_queries: Int
    var failed_queries: Int
    var total_query_time_ms: Int
    var bucket_0_10: Int
    var bucket_10_100: Int
    var bucket_100_1000: Int
    var bucket_1000_10000: Int
    var bucket_over_10000: Int
    var operation_names: List[String]
    var operation_counts: List[Int]
    var operation_total_times: List[Int]
    var operation_error_counts: List[Int]
    var slow_query_logs: List[String]

    def __init__(out self):
        var config = TelemetryConfig()
        self.enable_tracing = config.enable_tracing
        self.enable_metrics = config.enable_metrics
        self.enable_slow_query_log = config.enable_slow_query_log
        self.slow_query_threshold_ms = config.slow_query_threshold_ms
        self.sanitize_queries_enabled = config.sanitize_queries
        self.sample_rate = config.sample_rate
        self.next_trace_id = 1
        self.next_span_id = 1
        self.total_queries = 0
        self.successful_queries = 0
        self.failed_queries = 0
        self.total_query_time_ms = 0
        self.bucket_0_10 = 0
        self.bucket_10_100 = 0
        self.bucket_100_1000 = 0
        self.bucket_1000_10000 = 0
        self.bucket_over_10000 = 0
        self.operation_names = List[String]()
        self.operation_counts = List[Int]()
        self.operation_total_times = List[Int]()
        self.operation_error_counts = List[Int]()
        self.slow_query_logs = List[String]()

    def __init__(out self, config: TelemetryConfig):
        self.enable_tracing = config.enable_tracing
        self.enable_metrics = config.enable_metrics
        self.enable_slow_query_log = config.enable_slow_query_log
        self.slow_query_threshold_ms = config.slow_query_threshold_ms
        self.sanitize_queries_enabled = config.sanitize_queries
        self.sample_rate = config.sample_rate
        self.next_trace_id = 1
        self.next_span_id = 1
        self.total_queries = 0
        self.successful_queries = 0
        self.failed_queries = 0
        self.total_query_time_ms = 0
        self.bucket_0_10 = 0
        self.bucket_10_100 = 0
        self.bucket_100_1000 = 0
        self.bucket_1000_10000 = 0
        self.bucket_over_10000 = 0
        self.operation_names = List[String]()
        self.operation_counts = List[Int]()
        self.operation_total_times = List[Int]()
        self.operation_error_counts = List[Int]()
        self.slow_query_logs = List[String]()

    def start_span(mut self, name: String, start_time_ms: Int = 0) -> SpanContext:
        if not self.enable_tracing:
            return SpanContext(name)

        var trace_id = "trace_" + String(self.next_trace_id)
        self.next_trace_id += 1
        var span_id = "span_" + String(self.next_span_id)
        self.next_span_id += 1

        return SpanContext(name, trace_id, span_id, "", start_time_ms, True)

    def start_child_span(mut self, name: String, parent: SpanContext, start_time_ms: Int = 0) -> SpanContext:
        if not self.enable_tracing:
            return SpanContext(name)

        var span_id = "span_" + String(self.next_span_id)
        self.next_span_id += 1
        return SpanContext(name, parent.trace_id, span_id, parent.span_id, start_time_ms, True)

    def end_span(mut self, span: SpanContext, end_time_ms: Int, success: Bool = True):
        if not span.sampled:
            return

        var duration_ms = span.elapsed_ms(end_time_ms)
        self._record_query_metrics(span.span_name, duration_ms, success)

        if self.enable_slow_query_log and duration_ms > self.slow_query_threshold_ms:
            self._record_slow_query(span, duration_ms, end_time_ms)

    def _record_query_metrics(mut self, operation: String, duration_ms: Int, success: Bool):
        if not self.enable_metrics:
            return

        self.total_queries += 1
        self.total_query_time_ms += duration_ms
        if success:
            self.successful_queries += 1
        else:
            self.failed_queries += 1

        if duration_ms <= 10:
            self.bucket_0_10 += 1
        elif duration_ms <= 100:
            self.bucket_10_100 += 1
        elif duration_ms <= 1000:
            self.bucket_100_1000 += 1
        elif duration_ms <= 10000:
            self.bucket_1000_10000 += 1
        else:
            self.bucket_over_10000 += 1

        var idx = _find_operation_index(self.operation_names, operation)
        if idx < 0:
            self.operation_names.append(operation)
            self.operation_counts.append(0)
            self.operation_total_times.append(0)
            self.operation_error_counts.append(0)
            idx = len(self.operation_names) - 1

        self.operation_counts[idx] += 1
        self.operation_total_times[idx] += duration_ms
        if not success:
            self.operation_error_counts[idx] += 1

    def _record_slow_query(mut self, span: SpanContext, duration_ms: Int, timestamp_ms: Int):
        var summary = (
            "trace_id="
            + span.trace_id
            + ",span_name="
            + span.span_name
            + ",duration_ms="
            + String(duration_ms)
            + ",timestamp_ms="
            + String(timestamp_ms)
        )
        self.slow_query_logs.append(summary)
        if len(self.slow_query_logs) > 100:
            var retained = List[String]()
            for i in range(1, len(self.slow_query_logs)):
                retained.append(self.slow_query_logs[i])
            self.slow_query_logs = retained^

    def get_metrics(self) -> List[String]:
        var result = List[String]()
        result.append("total_queries=" + String(self.total_queries))
        result.append("successful_queries=" + String(self.successful_queries))
        result.append("failed_queries=" + String(self.failed_queries))
        result.append("total_query_time_ms=" + String(self.total_query_time_ms))
        result.append(
            "latency_histogram="
            + "0_10:"
            + String(self.bucket_0_10)
            + ",10_100:"
            + String(self.bucket_10_100)
            + ",100_1000:"
            + String(self.bucket_100_1000)
            + ",1000_10000:"
            + String(self.bucket_1000_10000)
            + ",over_10000:"
            + String(self.bucket_over_10000)
        )
        return result^

    def get_slow_queries(self) -> List[String]:
        return self.slow_query_logs.copy()

    def operation_metrics(self, operation: String) -> String:
        var idx = _find_operation_index(self.operation_names, operation)
        if idx < 0:
            return "count=0,total_time_ms=0,error_count=0"
        return (
            "count="
            + String(self.operation_counts[idx])
            + ",total_time_ms="
            + String(self.operation_total_times[idx])
            + ",error_count="
            + String(self.operation_error_counts[idx])
        )

    def sanitize_query(self, sql: String) -> String:
        if not self.sanitize_queries_enabled:
            return sql
        return sql

    def export_prometheus_metrics(self) -> String:
        var result = String()
        result += "# HELP scratchbird_queries_total Total number of queries\n"
        result += "# TYPE scratchbird_queries_total counter\n"
        result += "scratchbird_queries_total " + String(self.total_queries) + "\n"
        result += "# HELP scratchbird_query_duration_ms Query duration histogram\n"
        result += "# TYPE scratchbird_query_duration_ms histogram\n"
        result += "scratchbird_query_duration_ms_bucket{le=\"10\"} " + String(self.bucket_0_10) + "\n"
        result += "scratchbird_query_duration_ms_bucket{le=\"100\"} " + String(self.bucket_0_10 + self.bucket_10_100) + "\n"
        result += (
            "scratchbird_query_duration_ms_bucket{le=\"1000\"} "
            + String(self.bucket_0_10 + self.bucket_10_100 + self.bucket_100_1000)
            + "\n"
        )
        return result


struct TelemetrySpanGuard:
    var span: SpanContext
    var success: Bool

    def __init__(out self, span: SpanContext):
        self.span = span
        self.success = True

    def mark_failed(mut self):
        self.success = False

    def finish(self, mut collector: TelemetryCollector, end_time_ms: Int = 0):
        collector.end_span(self.span, end_time_ms, self.success)
