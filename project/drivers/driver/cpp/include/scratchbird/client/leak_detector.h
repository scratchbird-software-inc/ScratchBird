// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Connection Checkout Leak Detector
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_CLIENT_LEAK_DETECTOR_H
#define SB_CLIENT_LEAK_DETECTOR_H

#include <scratchbird/client/scratchbird_client.h>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scratchbird {

enum class sb_leak_log_level {
    SB_LEAK_LOG_DEBUG,
    SB_LEAK_LOG_WARN,
    SB_LEAK_LOG_ERROR
};

struct sb_leak_detection_config {
    uint32_t threshold_ms;          // Default: 30000 (30 seconds)
    bool capture_stack_trace;       // Default: false
    uint32_t check_interval_ms;     // Default: 10000 (10 seconds)
    sb_leak_log_level log_level;    // Default: SB_LEAK_LOG_WARN
};

static inline struct sb_leak_detection_config sb_leak_detection_config_default() {
    return {30000, false, 10000, sb_leak_log_level::SB_LEAK_LOG_WARN};
}

/*
 * MMCH_DRIVER_CHECKOUT_LEAK_DETECTOR
 *
 * Compatibility note: the exported sb_leak_detector_* names are retained for
 * existing driver ABI/source users. The detector is scoped only to connection
 * checkout/checkin lifecycle evidence. It is not a generic heap or memory leak
 * detector and does not claim allocator, transaction finality, visibility,
 * authorization, recovery, parser, donor, or benchmark authority.
 */

// Opaque connection-checkout leak detector handle
typedef struct sb_leak_detector sb_leak_detector;

#ifdef __cplusplus
extern "C" {
#endif

// Create/destroy leak detector
sb_leak_detector* sb_leak_detector_create(const struct sb_leak_detection_config* config);
void sb_leak_detector_destroy(sb_leak_detector* detector);

// Start/stop monitoring
void sb_leak_detector_start(sb_leak_detector* detector);
void sb_leak_detector_stop(sb_leak_detector* detector);

// Checkout/checkin
void sb_leak_detector_checkout(sb_leak_detector* detector, const char* conn_id);
void sb_leak_detector_checkout_with_metadata(sb_leak_detector* detector, const char* conn_id, 
                                              const char** keys, const char** values, size_t count);
void sb_leak_detector_checkin(sb_leak_detector* detector, const char* conn_id);

// Statistics
size_t sb_leak_detector_get_active_count(sb_leak_detector* detector);

#ifdef __cplusplus
}
#endif

// C++ API
#ifdef __cplusplus

namespace client {

class CheckoutRegistryState;
class LeakDetector;

class LeakDetectionGuard {
public:
    // Source-compatible constructor retained for legacy callers. Prefer
    // LeakDetector::Checkout so the guard carries a checkout id.
    LeakDetectionGuard(LeakDetector* detector, const std::string& connection_id);
    LeakDetectionGuard(std::weak_ptr<CheckoutRegistryState> state,
                       const std::string& connection_id,
                       uint64_t checkout_id);
    ~LeakDetectionGuard();
    
    void Release();
    
private:
    std::weak_ptr<CheckoutRegistryState> state_;
    std::string connection_id_;
    uint64_t checkout_id_;
    std::atomic<bool> released_;
};

class CheckoutInfo {
public:
    CheckoutInfo(uint64_t checkout_id,
                 bool capture_stack_trace,
                 const std::map<std::string, std::string>& metadata);
    
    std::chrono::milliseconds GetHeldDuration() const;
    
    const uint64_t checkout_id;
    const std::chrono::steady_clock::time_point checkout_time;
    const std::thread::id thread_id;
    const std::string owner_thread;
    std::string stack_trace_status;
    std::string stack_trace;
    const std::map<std::string, std::string> metadata;
};

struct CheckoutDiagnostic {
    uint64_t checkout_id;
    std::string connection_id;
    uint64_t age_ms;
    std::string owner_thread;
    std::string stack_trace_status;
    std::string stack_trace;
    std::map<std::string, std::string> metadata;
};

// Compatibility class name; semantically this is a connection checkout detector.
class LeakDetector {
public:
    explicit LeakDetector(const sb_leak_detection_config& config = sb_leak_detection_config_default());
    ~LeakDetector();
    
    // Non-copyable
    LeakDetector(const LeakDetector&) = delete;
    LeakDetector& operator=(const LeakDetector&) = delete;
    
    void Start();
    void Stop();
    
    // Checkout with RAII guard
    std::unique_ptr<LeakDetectionGuard> Checkout(const std::string& connection_id);
    std::unique_ptr<LeakDetectionGuard> Checkout(const std::string& connection_id,
                                                  const std::map<std::string, std::string>& metadata);
    bool Checkin(const std::string& connection_id);
    
    // Statistics
    size_t GetActiveCount() const;
    struct LeakStats {
        size_t active_checkouts;
        size_t potential_leaks;
    };
    LeakStats GetStats() const;
    std::vector<CheckoutDiagnostic> GetActiveDiagnostics() const;
    std::string ExportDiagnosticsJson() const;

    static const char* DetectorKind();
    static const char* AuthorityScope();
    
private:
    friend class LeakDetectionGuard;

    static bool ReleaseCheckout(const std::weak_ptr<CheckoutRegistryState>& state,
                                const std::string& connection_id,
                                uint64_t checkout_id);
    bool Checkin(const std::string& connection_id, uint64_t checkout_id);
    void MonitorLoop();
    void CheckLeaks();
    void LogLeak(const std::string& conn_id, const CheckoutInfo& info, uint32_t held_ms);
    
    sb_leak_detection_config config_;
    std::shared_ptr<CheckoutRegistryState> state_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
    std::atomic<uint64_t> next_checkout_id_;
};

} // namespace client
} // namespace scratchbird

#endif // __cplusplus

#endif // SB_CLIENT_LEAK_DETECTOR_H
