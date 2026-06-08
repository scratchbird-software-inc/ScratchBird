// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Keepalive Manager Implementation (Cross-Platform)
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/odbc/keepalive.h"
#include <iostream>

namespace ScratchBird {
namespace ODBC {

// KeepaliveTracker implementation
KeepaliveTracker::KeepaliveTracker(const KeepaliveConfig& config)
    : config_(config)
    , lastActivity_(std::chrono::steady_clock::now())
{
}

void KeepaliveTracker::MarkActive() {
    lastActivity_ = std::chrono::steady_clock::now();
}

bool KeepaliveTracker::NeedsValidation() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastActivity_.load()).count();
    return elapsed > static_cast<int64_t>(config_.maxIdleBeforeCheckMs);
}

DWORD KeepaliveTracker::GetIdleDurationMs() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastActivity_.load()).count());
}

// KeepaliveManager implementation
KeepaliveManager::KeepaliveManager(const KeepaliveConfig& config)
    : config_(config)
    , running_(false)
{
    SB_EVENT_CREATE(stopEvent_, SB_EVENT_MANUAL, 0);
}

KeepaliveManager::~KeepaliveManager() {
    Stop();
    SB_EVENT_DESTROY(stopEvent_);
}

void KeepaliveManager::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        SB_EVENT_RESET(stopEvent_);
        workerThread_ = std::thread(&KeepaliveManager::CheckLoop, this);
    }
}

void KeepaliveManager::Stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        SB_EVENT_SET(stopEvent_);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }
}

KeepaliveTracker* KeepaliveManager::Register(const std::string& connectionId, SQLHDBC hdbc) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tracker = std::make_unique<KeepaliveTracker>(config_);
    auto* ptr = tracker.get();
    trackers_[connectionId] = std::move(tracker);
    connections_[connectionId] = hdbc;
    return ptr;
}

void KeepaliveManager::Unregister(const std::string& connectionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    trackers_.erase(connectionId);
    connections_.erase(connectionId);
}

size_t KeepaliveManager::GetMonitoredCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trackers_.size();
}

void KeepaliveManager::CheckLoop() {
    while (running_) {
        int waitResult = SB_EVENT_WAIT_TIMEOUT(stopEvent_, config_.intervalMs);
        
        if (waitResult == 0) {
            // Stop event signaled
            break;
        }
        
        // Check all connections
        std::map<std::string, SQLHDBC> conns;
        std::map<std::string, KeepaliveTracker*> trks;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : connections_) {
                conns[pair.first] = pair.second;
            }
            for (auto& pair : trackers_) {
                trks[pair.first] = pair.second.get();
            }
        }
        
        for (auto& pair : trks) {
            const auto& connId = pair.first;
            auto* tracker = pair.second;
            
            if (tracker->NeedsValidation()) {
                auto it = conns.find(connId);
                if (it != conns.end() && ValidateConnection(it->second)) {
                    tracker->MarkActive();
                }
            }
        }
    }
}

bool KeepaliveManager::ValidateConnection(SQLHDBC hdbc) {
    // Send a ping query to validate connection
    const SQLCHAR* pingQuery = (const SQLCHAR*)"SELECT 1";
    SQLHSTMT hstmt;
    
    SQLRETURN ret = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    if (!SQL_SUCCEEDED(ret)) {
        return false;
    }
    
    // Set query timeout
    SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, 
                   reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(config_.validationTimeoutMs / 1000)), 0);
    
    ret = SQLExecDirect(hstmt, (SQLCHAR*)pingQuery, SQL_NTS);
    
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    
    return SQL_SUCCEEDED(ret);
}

} // namespace ODBC
} // namespace ScratchBird
