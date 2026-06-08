// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * OpenTelemetry-compatible Telemetry
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_CLIENT_TELEMETRY_H
#define SB_CLIENT_TELEMETRY_H

#include <scratchbird/client/scratchbird_client.h>
#include <chrono>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>

namespace scratchbird {

enum class sb_telemetry_level {
    SB_TELEMETRY_DEBUG,
    SB_TELEMETRY_INFO,
    SB_TELEMETRY_WARN,
    SB_TELEMETRY_ERROR
};

struct sb_telemetry_config {
    int enable_tracing;
    int enable_metrics;
    int enable_slow_query_log;
    uint32_t slow_query_threshold_ms;
    int sanitize_queries;
    double sample_rate;
};

static inline struct sb_telemetry_config sb_telemetry_config_default() {
    return {1, 1, 1, 1000, 1, 1.0};
}

// Opaque telemetry handle
typedef struct sb_telemetry sb_telemetry;

#ifdef __cplusplus
extern "C" {
#endif

// Create/destroy telemetry
sb_telemetry* sb_telemetry_create(const struct sb_telemetry_config* config);
void sb_telemetry_destroy(sb_telemetry* telemetry);

// Span operations
void sb_telemetry_start_span(sb_telemetry* telemetry, const char* name, char* out_trace_id, char* out_span_id);
void sb_telemetry_end_span(sb_telemetry* telemetry, const char* trace_id, int success);
void sb_telemetry_add_attribute(sb_telemetry* telemetry, const char* trace_id, const char* key, const char* value);

// Utility
void sb_telemetry_sanitize_query(const char* sql, char* out_sanitized, size_t out_size);

#ifdef __cplusplus
}
#endif

// C++ API
#ifdef __cplusplus

namespace client {

class SpanContext {
public:
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::string span_name;
    std::chrono::steady_clock::time_point start_time;
    std::map<std::string, std::string> attributes;
    
    explicit SpanContext(const std::string& name);
    SpanContext(const std::string& name, const SpanContext& parent);
    
    SpanContext& WithAttribute(const std::string& key, const std::string& value);
    std::chrono::milliseconds Elapsed() const;
    
private:
    static std::string GenerateTraceId();
    static std::string GenerateSpanId();
};

struct LatencyHistogram {
    std::atomic<uint64_t> ms_0_10{0};
    std::atomic<uint64_t> ms_10_100{0};
    std::atomic<uint64_t> ms_100_1000{0};
    std::atomic<uint64_t> ms_1000_10000{0};
    std::atomic<uint64_t> ms_over_10000{0};
    
    void Record(uint64_t duration_ms);
};

struct OperationMetrics {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> total_time_ms{0};
    std::atomic<uint64_t> avg_time_ms{0};
    std::atomic<uint64_t> max_time_ms{0};
    std::atomic<uint64_t> error_count{0};
    
    void Record(uint64_t duration_ms, bool success);
};

struct LatencyHistogramSnapshot {
    uint64_t ms_0_10;
    uint64_t ms_10_100;
    uint64_t ms_100_1000;
    uint64_t ms_1000_10000;
    uint64_t ms_over_10000;
};

struct OperationMetricsSnapshot {
    uint64_t count;
    uint64_t total_time_ms;
    uint64_t avg_time_ms;
    uint64_t max_time_ms;
    uint64_t error_count;
};

struct SlowQueryLog {
    std::string trace_id;
    std::string span_name;
    uint64_t duration_ms;
    std::chrono::system_clock::time_point timestamp;
    std::map<std::string, std::string> attributes;
};

struct Metrics {
    uint64_t total_queries;
    uint64_t successful_queries;
    uint64_t failed_queries;
    uint64_t total_query_time_ms;
    LatencyHistogramSnapshot latency_histogram;
    std::map<std::string, OperationMetricsSnapshot> operation_metrics;
};

class TelemetryCollector {
public:
    explicit TelemetryCollector(const sb_telemetry_config& config = sb_telemetry_config_default());
    
    // Span operations
    std::unique_ptr<SpanContext> StartSpan(const std::string& name);
    void EndSpan(const SpanContext& span, bool success);
    
    // Metrics
    Metrics GetMetrics() const;
    std::vector<SlowQueryLog> GetSlowQueries() const;
    std::string ExportSlowQueriesJson() const;
    void Reset();
    
    // Utility
    static std::string SanitizeQuery(const std::string& sql);
    std::string ExportPrometheusMetrics() const;
    
private:
    void RecordQueryMetrics(const std::string& operation, uint64_t duration_ms, bool success);
    void RecordSlowQuery(const SpanContext& span, uint64_t duration_ms);
    
    sb_telemetry_config config_;
    std::vector<std::unique_ptr<SpanContext>> spans_;
    mutable std::mutex spans_mutex_;
    
    std::atomic<uint64_t> total_queries_{0};
    std::atomic<uint64_t> successful_queries_{0};
    std::atomic<uint64_t> failed_queries_{0};
    std::atomic<uint64_t> total_query_time_ms_{0};
    
    LatencyHistogram histogram_;
    std::map<std::string, std::unique_ptr<OperationMetrics>> operation_metrics_;
    mutable std::mutex metrics_mutex_;
    
    std::vector<SlowQueryLog> slow_queries_;
    mutable std::mutex slow_queries_mutex_;
};

class TelemetrySpanGuard {
public:
    TelemetrySpanGuard(TelemetryCollector& collector, std::unique_ptr<SpanContext> span);
    ~TelemetrySpanGuard();
    
    void MarkFailed();
    SpanContext* GetSpan() const { return span_.get(); }
    
private:
    TelemetryCollector& collector_;
    std::unique_ptr<SpanContext> span_;
    bool success_;
};

} // namespace client
} // namespace scratchbird

#endif // __cplusplus

#endif // SB_CLIENT_TELEMETRY_H
