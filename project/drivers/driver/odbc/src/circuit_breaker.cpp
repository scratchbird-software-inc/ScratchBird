// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver
 * Circuit Breaker Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/odbc/circuit_breaker.h"
#include <iostream>

namespace ScratchBird {
namespace ODBC {

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config)
    : CircuitBreaker(config, "default")
{
}

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config, const std::string& name)
    : config_(config)
    , name_(name)
    , state_(CircuitState::CLOSED)
    , failureCount_(0)
    , successCount_(0)
    , halfOpenRequests_(0)
    , lastFailureTime_(std::chrono::steady_clock::time_point())
{
}

CircuitState CircuitBreaker::GetState() const {
    return state_.load();
}

bool CircuitBreaker::AllowRequest() {
    CircuitState currentState = state_.load();
    
    switch (currentState) {
        case CircuitState::CLOSED:
            return true;
            
        case CircuitState::OPEN: {
            auto lastFailure = lastFailureTime_.load();
            auto now = std::chrono::steady_clock::now();
            
            if (lastFailure.time_since_epoch().count() > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFailure).count() >= 
                    static_cast<int64_t>(config_.recoveryTimeoutMs)) {
                
                CircuitState expected = CircuitState::OPEN;
                if (state_.compare_exchange_strong(expected, CircuitState::HALF_OPEN)) {
                    failureCount_.store(0);
                    successCount_.store(0);
                    halfOpenRequests_.store(0);
                    std::cout << "Circuit breaker " << name_ << ": OPEN -> HALF_OPEN\n";
                }
                return AllowHalfOpenRequest();
            }
            return false;
        }
            
        case CircuitState::HALF_OPEN:
            return AllowHalfOpenRequest();
    }
    
    return false;
}

bool CircuitBreaker::AllowHalfOpenRequest() {
    DWORD current = halfOpenRequests_.load();
    if (current >= config_.halfOpenMaxRequests) {
        return false;
    }
    
    if (halfOpenRequests_.compare_exchange_strong(current, current + 1)) {
        return true;
    }
    
    // Retry
    return AllowHalfOpenRequest();
}

void CircuitBreaker::RecordSuccess() {
    CircuitState currentState = state_.load();
    
    switch (currentState) {
        case CircuitState::CLOSED:
            failureCount_.store(0);
            break;
            
        case CircuitState::HALF_OPEN: {
            halfOpenRequests_--;
            DWORD successes = successCount_.fetch_add(1) + 1;
            
            if (successes >= config_.successThreshold) {
                CircuitState expected = CircuitState::HALF_OPEN;
                if (state_.compare_exchange_strong(expected, CircuitState::CLOSED)) {
                    failureCount_.store(0);
                    successCount_.store(0);
                    std::cout << "Circuit breaker " << name_ << ": HALF_OPEN -> CLOSED\n";
                }
            }
            break;
        }
            
        default:
            break;
    }
}

void CircuitBreaker::RecordFailure() {
    CircuitState currentState = state_.load();
    
    switch (currentState) {
        case CircuitState::CLOSED: {
            DWORD failures = failureCount_.fetch_add(1) + 1;
            
            if (failures >= config_.failureThreshold) {
                CircuitState expected = CircuitState::CLOSED;
                if (state_.compare_exchange_strong(expected, CircuitState::OPEN)) {
                    lastFailureTime_.store(std::chrono::steady_clock::now());
                    std::cout << "Circuit breaker " << name_ << ": CLOSED -> OPEN\n";
                }
            }
            break;
        }
            
        case CircuitState::HALF_OPEN:
            halfOpenRequests_--;
            if (state_.compare_exchange_strong(currentState, CircuitState::OPEN)) {
                lastFailureTime_.store(std::chrono::steady_clock::now());
                std::cout << "Circuit breaker " << name_ << ": HALF_OPEN -> OPEN\n";
            }
            break;
            
        case CircuitState::OPEN:
            lastFailureTime_.store(std::chrono::steady_clock::now());
            break;
    }
}

void CircuitBreaker::Reset() {
    state_.store(CircuitState::CLOSED);
    failureCount_.store(0);
    successCount_.store(0);
    halfOpenRequests_.store(0);
    std::cout << "Circuit breaker " << name_ << ": manually reset to CLOSED\n";
}

CircuitBreaker::Stats CircuitBreaker::GetStats() const {
    return {
        state_.load(),
        failureCount_.load(),
        successCount_.load(),
        halfOpenRequests_.load(),
        lastFailureTime_.load()
    };
}

} // namespace ODBC
} // namespace ScratchBird
