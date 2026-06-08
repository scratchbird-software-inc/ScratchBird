// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Keepalive Manager - Prevents connection timeouts
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_CLIENT_KEEPALIVE_H
#define SB_CLIENT_KEEPALIVE_H

#include <scratchbird/client/scratchbird_client.h>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace scratchbird {

// Configuration for keepalive
struct sb_keepalive_config {
    uint32_t interval_ms;           // Default: 120000 (2 minutes)
    uint32_t max_idle_before_check_ms; // Default: 600000 (10 minutes)
    uint32_t validation_timeout_ms; // Default: 5000 (5 seconds)
};

// Default configuration
static inline struct sb_keepalive_config sb_keepalive_config_default() {
    return {120000, 600000, 5000};
}

// Opaque keepalive manager handle
typedef struct sb_keepalive_manager sb_keepalive_manager;

// C API
#ifdef __cplusplus
extern "C" {
#endif

// Create/destroy keepalive manager
sb_keepalive_manager* sb_keepalive_manager_create(const struct sb_keepalive_config* config);
void sb_keepalive_manager_destroy(sb_keepalive_manager* manager);

// Start/stop monitoring
void sb_keepalive_manager_start(sb_keepalive_manager* manager);
void sb_keepalive_manager_stop(sb_keepalive_manager* manager);

// Register/unregister connections
void sb_keepalive_register(sb_keepalive_manager* manager, const char* conn_id, sb_connection* conn);
void sb_keepalive_unregister(sb_keepalive_manager* manager, const char* conn_id);

// Get monitored count
size_t sb_keepalive_get_count(sb_keepalive_manager* manager);

#ifdef __cplusplus
}
#endif

// C++ API
#ifdef __cplusplus

namespace client {

class KeepaliveTracker {
public:
    explicit KeepaliveTracker(const sb_keepalive_config& config);
    
    void MarkActive();
    bool NeedsValidation() const;
    std::chrono::milliseconds GetIdleDuration() const;
    
private:
    const sb_keepalive_config& config_;
    std::atomic<std::chrono::steady_clock::time_point> last_activity_;
};

class KeepaliveManager {
public:
    explicit KeepaliveManager(const sb_keepalive_config& config = sb_keepalive_config_default());
    ~KeepaliveManager();
    
    // Non-copyable
    KeepaliveManager(const KeepaliveManager&) = delete;
    KeepaliveManager& operator=(const KeepaliveManager&) = delete;
    
    void Start();
    void Stop();
    
    // Register a connection
    KeepaliveTracker* Register(const std::string& connection_id, sb_connection* conn);
    void Unregister(const std::string& connection_id);
    
    // Statistics
    size_t GetMonitoredCount() const;
    
private:
    void CheckLoop();
    bool ValidateConnection(sb_connection* conn);
    
    sb_keepalive_config config_;
    std::map<std::string, std::unique_ptr<KeepaliveTracker>> trackers_;
    std::map<std::string, sb_connection*> connections_;
    mutable std::mutex mutex_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
};

} // namespace client
} // namespace scratchbird

#endif // __cplusplus

#endif // SB_CLIENT_KEEPALIVE_H
