// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Connection Leak Detector
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_ODBC_LEAK_DETECTOR_H
#define SB_ODBC_LEAK_DETECTOR_H

#include "scratchbird/odbc/platform.h"
#include <map>
#include <string>
#include <chrono>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

namespace ScratchBird {
namespace ODBC {

enum class LeakLogLevel {
    Debug,
    Warn,
    Error
};

struct LeakDetectionConfig {
    DWORD thresholdMs = 30000;           // 30 seconds
    bool captureStackTrace = false;
    DWORD checkIntervalMs = 10000;       // 10 seconds
    LeakLogLevel logLevel = LeakLogLevel::Warn;
};

class CheckoutInfo {
public:
    CheckoutInfo(bool captureStackTrace, const std::map<std::string, std::string>& metadata);
    
    std::chrono::milliseconds GetHeldDuration() const;
    
    const std::chrono::steady_clock::time_point checkoutTime;
    const DWORD threadId;
    const std::string stackTrace;
    const std::map<std::string, std::string> metadata;
};

class LeakDetector;

// RAII guard for leak detection
class LeakDetectionGuard {
public:
    LeakDetectionGuard(LeakDetector* detector, const std::string& connectionId);
    ~LeakDetectionGuard();
    
    void Release();
    
private:
    LeakDetector* detector_;
    std::string connectionId_;
    std::atomic<bool> released_;
};

class LeakDetector {
public:
    explicit LeakDetector(const LeakDetectionConfig& config = LeakDetectionConfig());
    ~LeakDetector();
    
    // Non-copyable
    LeakDetector(const LeakDetector&) = delete;
    LeakDetector& operator=(const LeakDetector&) = delete;
    
    void Start();
    void Stop();
    
    // Checkout/checkin
    std::unique_ptr<LeakDetectionGuard> Checkout(const std::string& connectionId);
    std::unique_ptr<LeakDetectionGuard> Checkout(const std::string& connectionId, 
                                                  const std::map<std::string, std::string>& metadata);
    void Checkin(const std::string& connectionId);
    
    // Statistics
    size_t GetActiveCount() const;
    struct LeakStats {
        size_t activeCheckouts;
        size_t potentialLeaks;
    };
    LeakStats GetStats() const;
    
private:
    void CheckLeaks();
    void LogLeak(const std::string& connId, const CheckoutInfo& info, DWORD heldMs);
    
    LeakDetectionConfig config_;
    std::map<std::string, std::unique_ptr<CheckoutInfo>> checkouts_;
    mutable std::mutex mutex_;
    std::thread workerThread_;
    std::atomic<bool> running_;
    SB_EVENT stopEvent_;
};

} // namespace ODBC
} // namespace ScratchBird

#endif // SB_ODBC_LEAK_DETECTOR_H
