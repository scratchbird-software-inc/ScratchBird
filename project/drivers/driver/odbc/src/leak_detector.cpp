// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Leak Detector Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/odbc/leak_detector.h"
#include <sstream>
#include <iostream>
#include <functional>
#include <thread>

namespace ScratchBird {
namespace ODBC {

static DWORD CurrentThreadIdPortable() {
#if SB_PLATFORM_WINDOWS
    return GetCurrentThreadId();
#else
    return static_cast<DWORD>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

// CheckoutInfo implementation
CheckoutInfo::CheckoutInfo(bool captureStackTrace, const std::map<std::string, std::string>& metadata)
    : checkoutTime(std::chrono::steady_clock::now())
    , threadId(CurrentThreadIdPortable())
    , stackTrace(captureStackTrace ? "Stack trace captured" : "")
    , metadata(metadata)
{
}

std::chrono::milliseconds CheckoutInfo::GetHeldDuration() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - checkoutTime);
}

// LeakDetectionGuard implementation
LeakDetectionGuard::LeakDetectionGuard(LeakDetector* detector, const std::string& connectionId)
    : detector_(detector)
    , connectionId_(connectionId)
    , released_(false)
{
}

LeakDetectionGuard::~LeakDetectionGuard() {
    Release();
}

void LeakDetectionGuard::Release() {
    if (!released_.exchange(true)) {
        detector_->Checkin(connectionId_);
    }
}

// LeakDetector implementation
LeakDetector::LeakDetector(const LeakDetectionConfig& config)
    : config_(config)
    , running_(false)
{
    SB_EVENT_CREATE(stopEvent_, SB_EVENT_MANUAL, 0);
}

LeakDetector::~LeakDetector() {
    Stop();
    SB_EVENT_DESTROY(stopEvent_);
}

void LeakDetector::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        SB_EVENT_RESET(stopEvent_);
        workerThread_ = std::thread(&LeakDetector::CheckLeaks, this);
    }
}

void LeakDetector::Stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        SB_EVENT_SET(stopEvent_);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }
}

std::unique_ptr<LeakDetectionGuard> LeakDetector::Checkout(const std::string& connectionId) {
    return Checkout(connectionId, {});
}

std::unique_ptr<LeakDetectionGuard> LeakDetector::Checkout(
    const std::string& connectionId, 
    const std::map<std::string, std::string>& metadata) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    checkouts_[connectionId] = std::make_unique<CheckoutInfo>(config_.captureStackTrace, metadata);
    
    return std::make_unique<LeakDetectionGuard>(this, connectionId);
}

void LeakDetector::Checkin(const std::string& connectionId) {
    std::unique_ptr<CheckoutInfo> info;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = checkouts_.find(connectionId);
        if (it != checkouts_.end()) {
            info = std::move(it->second);
            checkouts_.erase(it);
        }
    }
    
    if (info) {
        auto heldMs = static_cast<DWORD>(info->GetHeldDuration().count());
        if (heldMs > config_.thresholdMs) {
            std::cerr << "Connection " << connectionId << " held for " 
                      << heldMs << " ms (threshold: " << config_.thresholdMs 
                      << " ms) - returned\n";
        }
    }
}

size_t LeakDetector::GetActiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkouts_.size();
}

LeakDetector::LeakStats LeakDetector::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t potentialLeaks = 0;
    auto now = std::chrono::steady_clock::now();
    
    for (const auto& pair : checkouts_) {
        auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pair.second->checkoutTime).count();
        if (heldMs > config_.thresholdMs) {
            potentialLeaks++;
        }
    }
    
    return {checkouts_.size(), potentialLeaks};
}

void LeakDetector::CheckLeaks() {
    while (running_) {
        int waitResult = SB_EVENT_WAIT_TIMEOUT(stopEvent_, config_.checkIntervalMs);

        if (waitResult == 0) {
            break;
        }
        
        std::map<std::string, std::unique_ptr<CheckoutInfo>> checkoutsCopy;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : checkouts_) {
                auto heldMs = static_cast<DWORD>(pair.second->GetHeldDuration().count());
                if (heldMs > config_.thresholdMs) {
                    LogLeak(pair.first, *pair.second, heldMs);
                }
            }
        }
    }
}

void LeakDetector::LogLeak(const std::string& connId, const CheckoutInfo& info, DWORD heldMs) {
    std::stringstream ss;
    ss << "POSSIBLE CONNECTION LEAK: conn=" << connId 
       << ", held=" << heldMs << " ms"
       << ", threshold=" << config_.thresholdMs << " ms"
       << ", thread=" << info.threadId;
    
    switch (config_.logLevel) {
        case LeakLogLevel::Debug:
            // Debug logging
            break;
        case LeakLogLevel::Warn:
            std::cerr << "WARNING: " << ss.str() << "\n";
            break;
        case LeakLogLevel::Error:
            std::cerr << "ERROR: " << ss.str() << "\n";
            break;
    }
}

} // namespace ODBC
} // namespace ScratchBird
