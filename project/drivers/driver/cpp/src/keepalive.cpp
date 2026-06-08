// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Keepalive Manager Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include <scratchbird/client/keepalive.h>
#include <algorithm>
#include <cstring>

namespace scratchbird {
namespace client {

// KeepaliveTracker implementation
KeepaliveTracker::KeepaliveTracker(const sb_keepalive_config& config)
    : config_(config)
    , last_activity_(std::chrono::steady_clock::now())
{
}

void KeepaliveTracker::MarkActive() {
    last_activity_ = std::chrono::steady_clock::now();
}

bool KeepaliveTracker::NeedsValidation() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_.load()).count();
    return elapsed > static_cast<int64_t>(config_.max_idle_before_check_ms);
}

std::chrono::milliseconds KeepaliveTracker::GetIdleDuration() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_activity_.load());
}

// KeepaliveManager implementation
KeepaliveManager::KeepaliveManager(const sb_keepalive_config& config)
    : config_(config)
    , running_(false)
    , stop_requested_(false)
{
}

KeepaliveManager::~KeepaliveManager() {
    Stop();
}

void KeepaliveManager::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        stop_requested_ = false;
        worker_thread_ = std::thread(&KeepaliveManager::CheckLoop, this);
    }
}

void KeepaliveManager::Stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        stop_requested_ = true;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

KeepaliveTracker* KeepaliveManager::Register(const std::string& connection_id, sb_connection* conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tracker = std::make_unique<KeepaliveTracker>(config_);
    auto* ptr = tracker.get();
    trackers_[connection_id] = std::move(tracker);
    connections_[connection_id] = conn;
    return ptr;
}

void KeepaliveManager::Unregister(const std::string& connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    trackers_.erase(connection_id);
    connections_.erase(connection_id);
}

size_t KeepaliveManager::GetMonitoredCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trackers_.size();
}

void KeepaliveManager::CheckLoop() {
    while (!stop_requested_) {
        // Sleep in short slices so Stop() can join promptly.
        uint32_t remaining_ms = config_.interval_ms;
        while (!stop_requested_ && remaining_ms > 0) {
            const uint32_t slice = std::min<uint32_t>(remaining_ms, 100u);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining_ms -= slice;
        }

        if (stop_requested_) break;
        
        // Check all connections
        std::map<std::string, sb_connection*> conns;
        std::map<std::string, KeepaliveTracker*> trks;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            conns = connections_;
            for (auto& pair : trackers_) {
                trks[pair.first] = pair.second.get();
            }
        }
        
        for (auto& pair : trks) {
            const auto& conn_id = pair.first;
            auto* tracker = pair.second;
            
            if (tracker->NeedsValidation()) {
                auto it = conns.find(conn_id);
                if (it != conns.end() && ValidateConnection(it->second)) {
                    tracker->MarkActive();
                }
            }
        }
    }
}

bool KeepaliveManager::ValidateConnection(sb_connection* conn) {
    sb_error err;
    // Use ping if available, otherwise a simple query
    return sb_is_healthy(conn, &err);
}

} // namespace client
} // namespace scratchbird

// C API Implementation
namespace scratchbird {
extern "C" {

sb_keepalive_manager* sb_keepalive_manager_create(const struct sb_keepalive_config* config) {
    auto* manager = new scratchbird::client::KeepaliveManager(
        config ? *config : scratchbird::sb_keepalive_config_default()
    );
    return reinterpret_cast<sb_keepalive_manager*>(manager);
}

void sb_keepalive_manager_destroy(sb_keepalive_manager* manager) {
    if (manager) {
        delete reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager);
    }
}

void sb_keepalive_manager_start(sb_keepalive_manager* manager) {
    if (manager) {
        reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager)->Start();
    }
}

void sb_keepalive_manager_stop(sb_keepalive_manager* manager) {
    if (manager) {
        reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager)->Stop();
    }
}

void sb_keepalive_register(sb_keepalive_manager* manager, const char* conn_id, sb_connection* conn) {
    if (manager && conn_id && conn) {
        reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager)->Register(conn_id, conn);
    }
}

void sb_keepalive_unregister(sb_keepalive_manager* manager, const char* conn_id) {
    if (manager && conn_id) {
        reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager)->Unregister(conn_id);
    }
}

size_t sb_keepalive_get_count(sb_keepalive_manager* manager) {
    if (manager) {
        return reinterpret_cast<scratchbird::client::KeepaliveManager*>(manager)->GetMonitoredCount();
    }
    return 0;
}

} // extern "C"
} // namespace scratchbird
