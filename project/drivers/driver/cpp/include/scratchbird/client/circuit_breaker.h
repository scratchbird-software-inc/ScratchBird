// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Circuit Breaker - Prevents cascading failures
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_CLIENT_CIRCUIT_BREAKER_H
#define SB_CLIENT_CIRCUIT_BREAKER_H

#include <scratchbird/client/scratchbird_client.h>
#include <atomic>
#include <chrono>
#include <string>
#include <functional>
#include <stdexcept>

namespace scratchbird {

enum class sb_circuit_state {
    SB_CIRCUIT_CLOSED,
    SB_CIRCUIT_OPEN,
    SB_CIRCUIT_HALF_OPEN
};

struct sb_circuit_breaker_config {
    uint32_t failure_threshold;
    uint32_t recovery_timeout_ms;
    uint32_t success_threshold;
    uint32_t half_open_max_requests;
};

static inline struct sb_circuit_breaker_config sb_circuit_breaker_config_default() {
    return {5, 30000, 3, 10};
}

// Opaque circuit breaker handle
typedef struct sb_circuit_breaker sb_circuit_breaker;

#ifdef __cplusplus
extern "C" {
#endif

// Create/destroy circuit breaker
sb_circuit_breaker* sb_circuit_breaker_create(const struct sb_circuit_breaker_config* config);
void sb_circuit_breaker_destroy(sb_circuit_breaker* breaker);

// State management
enum sb_circuit_state sb_circuit_breaker_get_state(sb_circuit_breaker* breaker);
int sb_circuit_breaker_allow_request(sb_circuit_breaker* breaker);
void sb_circuit_breaker_record_success(sb_circuit_breaker* breaker);
void sb_circuit_breaker_record_failure(sb_circuit_breaker* breaker);
void sb_circuit_breaker_reset(sb_circuit_breaker* breaker);

#ifdef __cplusplus
}
#endif

// C++ API
#ifdef __cplusplus

namespace client {

class CircuitBreaker {
public:
    explicit CircuitBreaker(const sb_circuit_breaker_config& config = sb_circuit_breaker_config_default());
    explicit CircuitBreaker(const sb_circuit_breaker_config& config, const std::string& name);
    
    // Non-copyable but movable
    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;
    CircuitBreaker(CircuitBreaker&&) = default;
    CircuitBreaker& operator=(CircuitBreaker&&) = default;
    
    sb_circuit_state GetState() const;
    bool AllowRequest();
    void RecordSuccess();
    void RecordFailure();
    void Reset();
    
    // Execute with circuit breaker protection
    template<typename Func>
    auto Execute(Func&& func) -> decltype(func()) {
        if (!AllowRequest()) {
            throw std::runtime_error("Circuit breaker is OPEN");
        }
        
        try {
            auto result = func();
            RecordSuccess();
            return result;
        } catch (...) {
            RecordFailure();
            throw;
        }
    }
    
    // Statistics
    struct Stats {
        sb_circuit_state state;
        uint32_t failure_count;
        uint32_t success_count;
        uint32_t half_open_requests;
        std::chrono::steady_clock::time_point last_failure_time;
    };
    Stats GetStats() const;
    
private:
    bool AllowHalfOpenRequest();
    
    sb_circuit_breaker_config config_;
    std::string name_;
    std::atomic<sb_circuit_state> state_;
    std::atomic<uint32_t> failure_count_;
    std::atomic<uint32_t> success_count_;
    std::atomic<uint32_t> half_open_requests_;
    std::atomic<std::chrono::steady_clock::time_point> last_failure_time_;
};

} // namespace client
} // namespace scratchbird

#endif // __cplusplus

#endif // SB_CLIENT_CIRCUIT_BREAKER_H
