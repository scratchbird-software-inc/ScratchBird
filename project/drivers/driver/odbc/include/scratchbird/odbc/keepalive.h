// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Keepalive Manager - Prevents connection timeouts
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_ODBC_KEEPALIVE_H
#define SB_ODBC_KEEPALIVE_H

#include "scratchbird/odbc/platform.h"
#include <map>
#include <string>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace ScratchBird {
namespace ODBC {

// Configuration for keepalive
struct KeepaliveConfig {
    DWORD intervalMs = 120000;           // 2 minutes
    DWORD maxIdleBeforeCheckMs = 600000; // 10 minutes
    DWORD validationTimeoutMs = 5000;    // 5 seconds
    
    KeepaliveConfig& Interval(DWORD ms) { intervalMs = ms; return *this; }
    KeepaliveConfig& MaxIdleBeforeCheck(DWORD ms) { maxIdleBeforeCheckMs = ms; return *this; }
    KeepaliveConfig& ValidationTimeout(DWORD ms) { validationTimeoutMs = ms; return *this; }
};

// Tracks activity for a single connection
class KeepaliveTracker {
public:
    explicit KeepaliveTracker(const KeepaliveConfig& config);
    
    void MarkActive();
    bool NeedsValidation() const;
    DWORD GetIdleDurationMs() const;
    
private:
    const KeepaliveConfig& config_;
    std::atomic<std::chrono::steady_clock::time_point> lastActivity_;
};

// Manages keepalive for multiple connections
class KeepaliveManager {
public:
    explicit KeepaliveManager(const KeepaliveConfig& config = KeepaliveConfig());
    ~KeepaliveManager();
    
    // Non-copyable
    KeepaliveManager(const KeepaliveManager&) = delete;
    KeepaliveManager& operator=(const KeepaliveManager&) = delete;
    
    void Start();
    void Stop();
    
    // Register a connection for monitoring
    KeepaliveTracker* Register(const std::string& connectionId, SQLHDBC hdbc);
    void Unregister(const std::string& connectionId);
    
    // Get count of monitored connections
    size_t GetMonitoredCount() const;
    
private:
    void CheckLoop();
    bool ValidateConnection(SQLHDBC hdbc);
    
    KeepaliveConfig config_;
    std::map<std::string, std::unique_ptr<KeepaliveTracker>> trackers_;
    std::map<std::string, SQLHDBC> connections_;
    mutable std::mutex mutex_;
    std::thread workerThread_;
    std::atomic<bool> running_;
    SB_EVENT stopEvent_;
};

} // namespace ODBC
} // namespace ScratchBird

#endif // SB_ODBC_KEEPALIVE_H
