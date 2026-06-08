// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Connection Checkout Leak Detector Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include <scratchbird/client/leak_detector.h>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <utility>

#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    define SCRATCHBIRD_CLIENT_HAS_EXECINFO 1
#    include <execinfo.h>
#  endif
#endif

namespace scratchbird {
namespace client {

namespace {

constexpr const char* kDetectorKind = "connection_checkout_leak_detector";
constexpr const char* kAuthorityScope =
    "driver_checkout_leak_evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_donor_or_benchmark_authority";

struct StackTraceEvidence {
    std::string status;
    std::string trace;
};

std::string threadIdString(std::thread::id id) {
    std::ostringstream oss;
    oss << id;
    return oss.str();
}

StackTraceEvidence captureStackTraceEvidence(bool enabled) {
    if (!enabled) {
        return {"disabled", ""};
    }

#if defined(SCRATCHBIRD_CLIENT_HAS_EXECINFO)
    void* frames[32];
    const int frame_count = ::backtrace(
        frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    if (frame_count <= 0) {
        return {"capture_failed", ""};
    }

    char** symbols = ::backtrace_symbols(frames, frame_count);
    if (!symbols) {
        return {"capture_failed", ""};
    }

    std::ostringstream oss;
    for (int i = 0; i < frame_count; ++i) {
        if (i != 0) {
            oss << "\\n";
        }
        oss << symbols[i];
    }
    std::free(symbols);
    return {"captured", oss.str()};
#else
    return {"unsupported", ""};
#endif
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch);
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

void appendMetadataJson(std::ostream& out,
                        const std::map<std::string, std::string>& metadata) {
    out << "{";
    size_t index = 0;
    for (const auto& pair : metadata) {
        if (index++ != 0) {
            out << ",";
        }
        out << "\"" << jsonEscape(pair.first) << "\":\""
            << jsonEscape(pair.second) << "\"";
    }
    out << "}";
}

uint64_t heldMsSince(const std::chrono::steady_clock::time_point& checkout_time,
                     const std::chrono::steady_clock::time_point& now) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now - checkout_time).count());
}

CheckoutDiagnostic makeDiagnostic(const std::string& connection_id,
                                  const CheckoutInfo& info,
                                  const std::chrono::steady_clock::time_point& now) {
    return CheckoutDiagnostic{
        info.checkout_id,
        connection_id,
        heldMsSince(info.checkout_time, now),
        info.owner_thread,
        info.stack_trace_status,
        info.stack_trace,
        info.metadata
    };
}

void appendDiagnosticJson(std::ostream& out, const CheckoutDiagnostic& diagnostic) {
    out << "{"
        << "\"checkout_id\":" << diagnostic.checkout_id << ","
        << "\"connection_id\":\"" << jsonEscape(diagnostic.connection_id) << "\","
        << "\"age_ms\":" << diagnostic.age_ms << ","
        << "\"owner_thread\":\"" << jsonEscape(diagnostic.owner_thread) << "\","
        << "\"stack_trace_status\":\"" << jsonEscape(diagnostic.stack_trace_status) << "\",";
    if (diagnostic.stack_trace_status == "captured") {
        out << "\"stack_trace\":\"" << jsonEscape(diagnostic.stack_trace) << "\",";
    } else if (diagnostic.stack_trace_status == "unsupported") {
        out << "\"stack_trace_unsupported_reason\":\"platform stack trace capture is unavailable\",";
    }
    out << "\"metadata\":";
    appendMetadataJson(out, diagnostic.metadata);
    out << "}";
}

} // namespace

class CheckoutRegistryState {
public:
    std::map<std::string, std::unique_ptr<CheckoutInfo>> checkouts;
    mutable std::mutex mutex;
    bool alive{true};
};

// CheckoutInfo implementation
CheckoutInfo::CheckoutInfo(uint64_t checkout_id_value,
                           bool capture_stack_trace,
                           const std::map<std::string, std::string>& metadata_value)
    : checkout_id(checkout_id_value)
    , checkout_time(std::chrono::steady_clock::now())
    , thread_id(std::this_thread::get_id())
    , owner_thread(threadIdString(thread_id))
    , metadata(metadata_value)
{
    auto stack = captureStackTraceEvidence(capture_stack_trace);
    stack_trace_status = std::move(stack.status);
    stack_trace = std::move(stack.trace);
}

std::chrono::milliseconds CheckoutInfo::GetHeldDuration() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - checkout_time);
}

// LeakDetectionGuard implementation
LeakDetectionGuard::LeakDetectionGuard(LeakDetector* detector, const std::string& connection_id)
    : state_(detector ? detector->state_ : std::weak_ptr<CheckoutRegistryState>{})
    , connection_id_(connection_id)
    , checkout_id_(0)
    , released_(false)
{
}

LeakDetectionGuard::LeakDetectionGuard(std::weak_ptr<CheckoutRegistryState> state,
                                       const std::string& connection_id,
                                       uint64_t checkout_id)
    : state_(std::move(state))
    , connection_id_(connection_id)
    , checkout_id_(checkout_id)
    , released_(false)
{
}

LeakDetectionGuard::~LeakDetectionGuard() {
    Release();
}

void LeakDetectionGuard::Release() {
    if (!released_.exchange(true)) {
        LeakDetector::ReleaseCheckout(state_, connection_id_, checkout_id_);
    }
}

// LeakDetector implementation
LeakDetector::LeakDetector(const sb_leak_detection_config& config)
    : config_(config)
    , state_(std::make_shared<CheckoutRegistryState>())
    , running_(false)
    , stop_requested_(false)
    , next_checkout_id_(1)
{
}

LeakDetector::~LeakDetector() {
    Stop();
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->alive = false;
        state_->checkouts.clear();
    }
}

void LeakDetector::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        stop_requested_ = false;
        worker_thread_ = std::thread(&LeakDetector::MonitorLoop, this);
    }
}

void LeakDetector::Stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        stop_requested_ = true;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

std::unique_ptr<LeakDetectionGuard> LeakDetector::Checkout(const std::string& connection_id) {
    return Checkout(connection_id, {});
}

std::unique_ptr<LeakDetectionGuard> LeakDetector::Checkout(
    const std::string& connection_id,
    const std::map<std::string, std::string>& metadata) {

    const uint64_t checkout_id = next_checkout_id_.fetch_add(1);
    auto info = std::make_unique<CheckoutInfo>(
        checkout_id, config_.capture_stack_trace, metadata);

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) {
            return std::make_unique<LeakDetectionGuard>(
                std::weak_ptr<CheckoutRegistryState>{}, connection_id, checkout_id);
        }
        state_->checkouts[connection_id] = std::move(info);
    }

    return std::make_unique<LeakDetectionGuard>(state_, connection_id, checkout_id);
}

bool LeakDetector::Checkin(const std::string& connection_id) {
    return Checkin(connection_id, 0);
}

bool LeakDetector::Checkin(const std::string& connection_id, uint64_t checkout_id) {
    std::unique_ptr<CheckoutInfo> info;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->alive) {
            return false;
        }
        auto it = state_->checkouts.find(connection_id);
        if (it == state_->checkouts.end()) {
            return false;
        }
        if (checkout_id != 0 && it->second->checkout_id != checkout_id) {
            return false;
        }
        info = std::move(it->second);
        state_->checkouts.erase(it);
    }

    if (info) {
        auto held_ms = static_cast<uint32_t>(info->GetHeldDuration().count());
        if (held_ms > config_.threshold_ms) {
            std::cout << "{"
                      << "\"detector_kind\":\"" << DetectorKind() << "\","
                      << "\"event\":\"checkout_returned_after_threshold\","
                      << "\"checkout_id\":" << info->checkout_id << ","
                      << "\"connection_id\":\"" << jsonEscape(connection_id) << "\","
                      << "\"age_ms\":" << held_ms << ","
                      << "\"threshold_ms\":" << config_.threshold_ms << ","
                      << "\"owner_thread\":\"" << jsonEscape(info->owner_thread) << "\","
                      << "\"stack_trace_status\":\"" << jsonEscape(info->stack_trace_status) << "\","
                      << "\"authority_scope\":\"" << AuthorityScope() << "\""
                      << "}\n";
        }
    }
    return true;
}

bool LeakDetector::ReleaseCheckout(const std::weak_ptr<CheckoutRegistryState>& weak_state,
                                   const std::string& connection_id,
                                   uint64_t checkout_id) {
    auto state = weak_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->alive) {
        return false;
    }
    auto it = state->checkouts.find(connection_id);
    if (it == state->checkouts.end()) {
        return false;
    }
    if (checkout_id != 0 && it->second->checkout_id != checkout_id) {
        return false;
    }
    state->checkouts.erase(it);
    return true;
}

size_t LeakDetector::GetActiveCount() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->checkouts.size();
}

LeakDetector::LeakStats LeakDetector::GetStats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    
    size_t potential_leaks = 0;
    auto now = std::chrono::steady_clock::now();
    
    for (const auto& pair : state_->checkouts) {
        auto held_ms = heldMsSince(pair.second->checkout_time, now);
        if (held_ms > config_.threshold_ms) {
            potential_leaks++;
        }
    }
    
    return {state_->checkouts.size(), potential_leaks};
}

std::vector<CheckoutDiagnostic> LeakDetector::GetActiveDiagnostics() const {
    std::vector<CheckoutDiagnostic> diagnostics;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto now = std::chrono::steady_clock::now();
    diagnostics.reserve(state_->checkouts.size());
    for (const auto& pair : state_->checkouts) {
        diagnostics.push_back(makeDiagnostic(pair.first, *pair.second, now));
    }
    return diagnostics;
}

std::string LeakDetector::ExportDiagnosticsJson() const {
    const auto stats = GetStats();
    const auto diagnostics = GetActiveDiagnostics();
    std::ostringstream json;
    json << "{"
         << "\"detector_kind\":\"" << DetectorKind() << "\","
         << "\"authority_scope\":\"" << AuthorityScope() << "\","
         << "\"active_checkouts\":" << stats.active_checkouts << ","
         << "\"potential_checkout_leaks\":" << stats.potential_leaks << ","
         << "\"heap_leak_detection\":false,"
         << "\"checkouts\":[";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0) {
            json << ",";
        }
        appendDiagnosticJson(json, diagnostics[i]);
    }
    json << "]}";
    return json.str();
}

const char* LeakDetector::DetectorKind() {
    return kDetectorKind;
}

const char* LeakDetector::AuthorityScope() {
    return kAuthorityScope;
}

void LeakDetector::MonitorLoop() {
    while (!stop_requested_) {
        uint32_t remaining_ms = config_.check_interval_ms;
        while (!stop_requested_ && remaining_ms > 0) {
            const uint32_t slice = std::min<uint32_t>(remaining_ms, 100u);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining_ms -= slice;
        }

        if (!stop_requested_) {
            CheckLeaks();
        }
    }
}

void LeakDetector::CheckLeaks() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto now = std::chrono::steady_clock::now();
    
    for (const auto& pair : state_->checkouts) {
        auto held_ms = static_cast<uint32_t>(heldMsSince(pair.second->checkout_time, now));
        
        if (held_ms > config_.threshold_ms) {
            LogLeak(pair.first, *pair.second, held_ms);
        }
    }
}

void LeakDetector::LogLeak(const std::string& conn_id, const CheckoutInfo& info, uint32_t held_ms) {
    CheckoutDiagnostic diagnostic{
        info.checkout_id,
        conn_id,
        held_ms,
        info.owner_thread,
        info.stack_trace_status,
        info.stack_trace,
        info.metadata
    };

    std::stringstream ss;
    ss << "{"
       << "\"detector_kind\":\"" << DetectorKind() << "\","
       << "\"event\":\"checkout_threshold_exceeded\","
       << "\"threshold_ms\":" << config_.threshold_ms << ","
       << "\"authority_scope\":\"" << AuthorityScope() << "\","
       << "\"checkout\":";
    appendDiagnosticJson(ss, diagnostic);
    ss << "}";
    
    switch (config_.log_level) {
        case sb_leak_log_level::SB_LEAK_LOG_DEBUG:
            break;
        case sb_leak_log_level::SB_LEAK_LOG_WARN:
            std::cout << ss.str() << "\n";
            break;
        case sb_leak_log_level::SB_LEAK_LOG_ERROR:
            std::cerr << ss.str() << "\n";
            break;
    }
}

} // namespace client
} // namespace scratchbird

// C API Implementation
namespace scratchbird {
extern "C" {

namespace {
using DetectorType = scratchbird::client::LeakDetector;
using GuardType = scratchbird::client::LeakDetectionGuard;

std::mutex g_c_api_guard_mutex;
std::unordered_map<DetectorType*,
                   std::unordered_map<std::string, std::unique_ptr<GuardType>>> g_c_api_guards;
}

sb_leak_detector* sb_leak_detector_create(const struct sb_leak_detection_config* config) {
    auto* detector = new scratchbird::client::LeakDetector(
        config ? *config : scratchbird::sb_leak_detection_config_default()
    );
    return reinterpret_cast<sb_leak_detector*>(detector);
}

void sb_leak_detector_destroy(sb_leak_detector* detector) {
    if (detector) {
        auto* typed = reinterpret_cast<scratchbird::client::LeakDetector*>(detector);
        {
            std::lock_guard<std::mutex> lock(g_c_api_guard_mutex);
            g_c_api_guards.erase(typed);
        }
        delete typed;
    }
}

void sb_leak_detector_start(sb_leak_detector* detector) {
    if (detector) {
        reinterpret_cast<scratchbird::client::LeakDetector*>(detector)->Start();
    }
}

void sb_leak_detector_stop(sb_leak_detector* detector) {
    if (detector) {
        reinterpret_cast<scratchbird::client::LeakDetector*>(detector)->Stop();
    }
}

void sb_leak_detector_checkout(sb_leak_detector* detector, const char* conn_id) {
    if (detector && conn_id) {
        auto* typed = reinterpret_cast<scratchbird::client::LeakDetector*>(detector);
        auto guard = typed->Checkout(conn_id);
        std::lock_guard<std::mutex> lock(g_c_api_guard_mutex);
        g_c_api_guards[typed][conn_id] = std::move(guard);
    }
}

void sb_leak_detector_checkout_with_metadata(sb_leak_detector* detector, const char* conn_id,
                                              const char** keys, const char** values, size_t count) {
    if (detector && conn_id) {
        auto* typed = reinterpret_cast<scratchbird::client::LeakDetector*>(detector);
        std::map<std::string, std::string> metadata;
        for (size_t i = 0; i < count; i++) {
            if (keys[i] && values[i]) {
                metadata[keys[i]] = values[i];
            }
        }
        auto guard = typed->Checkout(conn_id, metadata);
        std::lock_guard<std::mutex> lock(g_c_api_guard_mutex);
        g_c_api_guards[typed][conn_id] = std::move(guard);
    }
}

void sb_leak_detector_checkin(sb_leak_detector* detector, const char* conn_id) {
    if (detector && conn_id) {
        auto* typed = reinterpret_cast<scratchbird::client::LeakDetector*>(detector);
        std::unique_ptr<GuardType> guard;
        {
            std::lock_guard<std::mutex> lock(g_c_api_guard_mutex);
            auto detector_it = g_c_api_guards.find(typed);
            if (detector_it != g_c_api_guards.end()) {
                auto conn_it = detector_it->second.find(conn_id);
                if (conn_it != detector_it->second.end()) {
                    guard = std::move(conn_it->second);
                    detector_it->second.erase(conn_it);
                }
            }
        }
        if (guard) {
            guard->Release();
            return;
        }
        typed->Checkin(conn_id);
    }
}

size_t sb_leak_detector_get_active_count(sb_leak_detector* detector) {
    if (detector) {
        return reinterpret_cast<scratchbird::client::LeakDetector*>(detector)->GetActiveCount();
    }
    return 0;
}

} // extern "C"
} // namespace scratchbird
