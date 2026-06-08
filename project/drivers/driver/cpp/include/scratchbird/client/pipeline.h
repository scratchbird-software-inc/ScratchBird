// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Query Pipelining - Batches multiple queries
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_CLIENT_PIPELINE_H
#define SB_CLIENT_PIPELINE_H

#include <scratchbird/client/scratchbird_client.h>
#include <functional>
#include <future>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

namespace scratchbird {

struct sb_pipeline_config {
    uint32_t max_in_flight;
    int auto_flush;
    uint32_t auto_flush_threshold;
    uint32_t flush_timeout_ms;
};

static inline struct sb_pipeline_config sb_pipeline_config_default() {
    return {100, 1, 10, 5000};
}

// Opaque pipeline handle
typedef struct sb_pipeline sb_pipeline;

#ifdef __cplusplus
extern "C" {
#endif

// Create/destroy pipeline
sb_pipeline* sb_pipeline_create(const struct sb_pipeline_config* config);
void sb_pipeline_destroy(sb_pipeline* pipeline);

// Start/stop pipeline
void sb_pipeline_start(sb_pipeline* pipeline, sb_connection* conn);
void sb_pipeline_stop(sb_pipeline* pipeline);

// Queue operations
int sb_pipeline_has_capacity(sb_pipeline* pipeline);
uint32_t sb_pipeline_pending_count(sb_pipeline* pipeline);
void sb_pipeline_flush(sb_pipeline* pipeline);

#ifdef __cplusplus
}
#endif

// C++ API
#ifdef __cplusplus

namespace client {

template<typename T>
class PipelinedRequest {
public:
    std::string sql;
    std::vector<sb_value> params;
    std::promise<T> promise;
};

class Pipeline {
public:
    explicit Pipeline(const sb_pipeline_config& config = sb_pipeline_config_default());
    ~Pipeline();
    
    // Non-copyable
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    
    void Start(sb_connection* conn);
    void Stop();
    
    template<typename T>
    std::future<T> Queue(const std::string& sql, const std::vector<sb_value>& params = {}) {
        auto request = std::make_unique<PipelinedRequest<T>>();
        request->sql = sql;
        request->params = params;
        
        std::future<T> future = request->promise.get_future();
        
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(std::move(request));
        }
        
        // Auto-flush if threshold reached
        if (config_.auto_flush && queue_.size() >= config_.auto_flush_threshold) {
            Flush();
        }
        
        return future;
    }
    
    bool HasCapacity() const;
    size_t PendingCount() const;
    void Flush();
    
private:
    void RunLoop();
    
    sb_pipeline_config config_;
    sb_connection* connection_;
    std::queue<std::unique_ptr<void>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
    std::atomic<uint32_t> in_flight_;
};

} // namespace client
} // namespace scratchbird

#endif // __cplusplus

#endif // SB_CLIENT_PIPELINE_H
