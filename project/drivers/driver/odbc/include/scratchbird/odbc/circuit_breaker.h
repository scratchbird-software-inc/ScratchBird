// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Circuit Breaker - Prevents cascading failures
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_ODBC_CIRCUIT_BREAKER_H
#define SB_ODBC_CIRCUIT_BREAKER_H

#include "scratchbird/odbc/platform.h"
#include <atomic>
#include <string>
#include <chrono>
#include <functional>
#include <stdexcept>

namespace ScratchBird {
namespace ODBC {

enum class CircuitState {
    CLOSED,     // Normal operation
    OPEN,       // Failure threshold reached
    HALF_OPEN   // Testing recovery
};

struct CircuitBreakerConfig {
    DWORD failureThreshold = 5;
    DWORD recoveryTimeoutMs = 30000;     // 30 seconds
    DWORD successThreshold = 3;
    DWORD halfOpenMaxRequests = 10;
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(const CircuitBreakerConfig& config = CircuitBreakerConfig());
    explicit CircuitBreaker(const CircuitBreakerConfig& config, const std::string& name);
    
    // Non-copyable but movable
    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;
    CircuitBreaker(CircuitBreaker&&) = default;
    CircuitBreaker& operator=(CircuitBreaker&&) = default;
    
    CircuitState GetState() const;
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
        CircuitState state;
        DWORD failureCount;
        DWORD successCount;
        DWORD halfOpenRequests;
        std::chrono::steady_clock::time_point lastFailureTime;
    };
    Stats GetStats() const;
    
private:
    bool AllowHalfOpenRequest();
    
    CircuitBreakerConfig config_;
    std::string name_;
    std::atomic<CircuitState> state_;
    std::atomic<DWORD> failureCount_;
    std::atomic<DWORD> successCount_;
    std::atomic<DWORD> halfOpenRequests_;
    std::atomic<std::chrono::steady_clock::time_point> lastFailureTime_;
};

} // namespace ODBC
} // namespace ScratchBird

#endif // SB_ODBC_CIRCUIT_BREAKER_H
