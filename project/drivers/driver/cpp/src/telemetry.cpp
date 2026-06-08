// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * OpenTelemetry-compatible Telemetry Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/client/telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <unordered_map>

namespace scratchbird {
namespace client {

static std::string random_hex(size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream oss;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t v = static_cast<uint8_t>(rng() & 0xFF);
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(v);
    }
    return oss.str();
}

SpanContext::SpanContext(const std::string& name)
    : trace_id(GenerateTraceId())
    , span_id(GenerateSpanId())
    , parent_span_id("")
    , span_name(name)
    , start_time(std::chrono::steady_clock::now()) {}

SpanContext::SpanContext(const std::string& name, const SpanContext& parent)
    : trace_id(parent.trace_id)
    , span_id(GenerateSpanId())
    , parent_span_id(parent.span_id)
    , span_name(name)
    , start_time(std::chrono::steady_clock::now()) {}

SpanContext& SpanContext::WithAttribute(const std::string& key, const std::string& value) {
    attributes[key] = value;
    return *this;
}

std::chrono::milliseconds SpanContext::Elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
}

std::string SpanContext::GenerateTraceId() {
    return random_hex(16);
}

std::string SpanContext::GenerateSpanId() {
    return random_hex(8);
}

void LatencyHistogram::Record(uint64_t duration_ms) {
    if (duration_ms <= 10) {
        ms_0_10.fetch_add(1);
    } else if (duration_ms <= 100) {
        ms_10_100.fetch_add(1);
    } else if (duration_ms <= 1000) {
        ms_100_1000.fetch_add(1);
    } else if (duration_ms <= 10000) {
        ms_1000_10000.fetch_add(1);
    } else {
        ms_over_10000.fetch_add(1);
    }
}

void OperationMetrics::Record(uint64_t duration_ms, bool success) {
    auto count_val = count.fetch_add(1) + 1;
    auto total_val = total_time_ms.fetch_add(duration_ms) + duration_ms;
    avg_time_ms.store(total_val / count_val);
    uint64_t current_max = max_time_ms.load();
    while (duration_ms > current_max &&
           !max_time_ms.compare_exchange_weak(current_max, duration_ms)) {
    }
    if (!success) {
        error_count.fetch_add(1);
    }
}

TelemetryCollector::TelemetryCollector(const sb_telemetry_config& config)
    : config_(config) {}

std::unique_ptr<SpanContext> TelemetryCollector::StartSpan(const std::string& name) {
    if (!config_.enable_tracing) {
        return nullptr;
    }
    double roll = static_cast<double>(rand()) / static_cast<double>(RAND_MAX);
    if (roll > config_.sample_rate) {
        return nullptr;
    }
    return std::make_unique<SpanContext>(name);
}

void TelemetryCollector::EndSpan(const SpanContext& span, bool success) {
    if (!config_.enable_tracing) {
        return;
    }
    auto duration_ms = static_cast<uint64_t>(span.Elapsed().count());
    RecordQueryMetrics(span.span_name, duration_ms, success);
    if (config_.enable_slow_query_log && duration_ms > config_.slow_query_threshold_ms) {
        RecordSlowQuery(span, duration_ms);
    }
}

Metrics TelemetryCollector::GetMetrics() const {
    LatencyHistogramSnapshot histogram_snapshot{
        histogram_.ms_0_10.load(),
        histogram_.ms_10_100.load(),
        histogram_.ms_100_1000.load(),
        histogram_.ms_1000_10000.load(),
        histogram_.ms_over_10000.load()
    };
    std::map<std::string, OperationMetricsSnapshot> op_metrics;
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        for (const auto& pair : operation_metrics_) {
            op_metrics.emplace(pair.first, OperationMetricsSnapshot{
                pair.second->count.load(),
                pair.second->total_time_ms.load(),
                pair.second->avg_time_ms.load(),
                pair.second->max_time_ms.load(),
                pair.second->error_count.load()
            });
        }
    }
    return Metrics{
        total_queries_.load(),
        successful_queries_.load(),
        failed_queries_.load(),
        total_query_time_ms_.load(),
        histogram_snapshot,
        op_metrics
    };
}

std::vector<SlowQueryLog> TelemetryCollector::GetSlowQueries() const {
    std::lock_guard<std::mutex> lock(slow_queries_mutex_);
    return slow_queries_;
}

std::string TelemetryCollector::ExportSlowQueriesJson() const {
    std::ostringstream oss;
    oss << "[";
    const auto logs = GetSlowQueries();
    for (size_t i = 0; i < logs.size(); ++i) {
        const auto& log = logs[i];
        if (i != 0) {
            oss << ",";
        }
        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            log.timestamp.time_since_epoch()).count();
        oss << "{"
            << "\"trace_id\":\"" << log.trace_id << "\","
            << "\"operation\":\"" << log.span_name << "\","
            << "\"duration_ms\":" << log.duration_ms << ","
            << "\"captured_unix_ms\":" << epoch_ms
            << "}";
    }
    oss << "]";
    return oss.str();
}

std::string TelemetryCollector::SanitizeQuery(const std::string& sql) {
    std::string out;
    out.reserve(sql.size());
    bool in_quote = false;
    for (char c : sql) {
        if (c == '\'') {
            in_quote = !in_quote;
            out.push_back(c);
            if (!in_quote) {
                out.push_back('?');
                out.push_back('\'');
            }
            continue;
        }
        if (!in_quote) {
            out.push_back(c);
        }
    }
    if (out.empty()) {
        return sql;
    }
    return out;
}

std::string TelemetryCollector::ExportPrometheusMetrics() const {
    Metrics m = GetMetrics();
    std::ostringstream oss;
    oss << "# HELP scratchbird_queries_total Total number of queries\n";
    oss << "# TYPE scratchbird_queries_total counter\n";
    oss << "scratchbird_queries_total " << m.total_queries << "\n";
    oss << "# HELP scratchbird_query_duration_ms Query duration histogram\n";
    oss << "# TYPE scratchbird_query_duration_ms histogram\n";
    oss << "scratchbird_query_duration_ms_bucket{le=\"10\"} " << m.latency_histogram.ms_0_10 << "\n";
    oss << "scratchbird_query_duration_ms_bucket{le=\"100\"} "
        << (m.latency_histogram.ms_0_10 + m.latency_histogram.ms_10_100) << "\n";
    oss << "scratchbird_query_duration_ms_bucket{le=\"1000\"} "
        << (m.latency_histogram.ms_0_10 + m.latency_histogram.ms_10_100 + m.latency_histogram.ms_100_1000) << "\n";
    return oss.str();
}

void TelemetryCollector::Reset() {
    total_queries_.store(0);
    successful_queries_.store(0);
    failed_queries_.store(0);
    total_query_time_ms_.store(0);
    histogram_.ms_0_10.store(0);
    histogram_.ms_10_100.store(0);
    histogram_.ms_100_1000.store(0);
    histogram_.ms_1000_10000.store(0);
    histogram_.ms_over_10000.store(0);
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        operation_metrics_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(slow_queries_mutex_);
        slow_queries_.clear();
    }
}

void TelemetryCollector::RecordQueryMetrics(const std::string& operation, uint64_t duration_ms, bool success) {
    if (!config_.enable_metrics) {
        return;
    }
    total_queries_.fetch_add(1);
    if (success) {
        successful_queries_.fetch_add(1);
    } else {
        failed_queries_.fetch_add(1);
    }
    total_query_time_ms_.fetch_add(duration_ms);
    histogram_.Record(duration_ms);

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto& metrics = operation_metrics_[operation];
    if (!metrics) {
        metrics = std::make_unique<OperationMetrics>();
    }
    metrics->Record(duration_ms, success);
}

void TelemetryCollector::RecordSlowQuery(const SpanContext& span, uint64_t duration_ms) {
    SlowQueryLog entry{
        span.trace_id,
        span.span_name,
        duration_ms,
        std::chrono::system_clock::now(),
        span.attributes
    };
    std::lock_guard<std::mutex> lock(slow_queries_mutex_);
    slow_queries_.push_back(entry);
    if (slow_queries_.size() > 100) {
        slow_queries_.erase(slow_queries_.begin());
    }
}

TelemetrySpanGuard::TelemetrySpanGuard(TelemetryCollector& collector, std::unique_ptr<SpanContext> span)
    : collector_(collector)
    , span_(std::move(span))
    , success_(true) {}

TelemetrySpanGuard::~TelemetrySpanGuard() {
    if (span_) {
        collector_.EndSpan(*span_, success_);
    }
}

void TelemetrySpanGuard::MarkFailed() {
    success_ = false;
}

} // namespace client
} // namespace scratchbird

namespace scratchbird {

struct sb_telemetry {
    scratchbird::client::TelemetryCollector collector;
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<scratchbird::client::SpanContext>> active;
};

extern "C" {

sb_telemetry* sb_telemetry_create(const struct sb_telemetry_config* config) {
    auto cfg = config ? *config : scratchbird::sb_telemetry_config_default();
    auto* telemetry = new sb_telemetry{scratchbird::client::TelemetryCollector(cfg)};
    return telemetry;
}

void sb_telemetry_destroy(sb_telemetry* telemetry) {
    delete telemetry;
}

void sb_telemetry_start_span(sb_telemetry* telemetry, const char* name,
                            char* out_trace_id, char* out_span_id) {
    if (!telemetry || !name || !out_trace_id || !out_span_id) {
        return;
    }
    auto span = telemetry->collector.StartSpan(name);
    if (!span) {
        out_trace_id[0] = '\0';
        out_span_id[0] = '\0';
        return;
    }
    std::lock_guard<std::mutex> lock(telemetry->mutex);
    std::snprintf(out_trace_id, 33, "%s", span->trace_id.c_str());
    std::snprintf(out_span_id, 17, "%s", span->span_id.c_str());
    telemetry->active.emplace(span->trace_id, std::move(span));
}

void sb_telemetry_end_span(sb_telemetry* telemetry, const char* trace_id, int success) {
    if (!telemetry || !trace_id) {
        return;
    }
    std::unique_ptr<scratchbird::client::SpanContext> span;
    {
        std::lock_guard<std::mutex> lock(telemetry->mutex);
        auto it = telemetry->active.find(trace_id);
        if (it == telemetry->active.end()) {
            return;
        }
        span = std::move(it->second);
        telemetry->active.erase(it);
    }
    if (span) {
        telemetry->collector.EndSpan(*span, success != 0);
    }
}

void sb_telemetry_add_attribute(sb_telemetry* telemetry, const char* trace_id,
                               const char* key, const char* value) {
    if (!telemetry || !trace_id || !key || !value) {
        return;
    }
    std::lock_guard<std::mutex> lock(telemetry->mutex);
    auto it = telemetry->active.find(trace_id);
    if (it != telemetry->active.end()) {
        it->second->WithAttribute(key, value);
    }
}

void sb_telemetry_sanitize_query(const char* sql, char* out_sanitized, size_t out_size) {
    if (!out_sanitized || out_size == 0) {
        return;
    }
    if (!sql) {
        out_sanitized[0] = '\0';
        return;
    }
    std::string sanitized = scratchbird::client::TelemetryCollector::SanitizeQuery(sql);
    std::snprintf(out_sanitized, out_size, "%s", sanitized.c_str());
}

} // extern "C"
} // namespace scratchbird
