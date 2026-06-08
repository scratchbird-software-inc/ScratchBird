// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird C++ Client
 * Circuit Breaker Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include <scratchbird/client/circuit_breaker.h>
#include <iostream>

namespace scratchbird {
namespace client {

CircuitBreaker::CircuitBreaker(const sb_circuit_breaker_config& config)
    : CircuitBreaker(config, "default")
{
}

CircuitBreaker::CircuitBreaker(const sb_circuit_breaker_config& config, const std::string& name)
    : config_(config)
    , name_(name)
    , state_(sb_circuit_state::SB_CIRCUIT_CLOSED)
    , failure_count_(0)
    , success_count_(0)
    , half_open_requests_(0)
    , last_failure_time_(std::chrono::steady_clock::time_point())
{
}

sb_circuit_state CircuitBreaker::GetState() const {
    return state_.load();
}

bool CircuitBreaker::AllowRequest() {
    sb_circuit_state currentState = state_.load();
    
    switch (currentState) {
        case sb_circuit_state::SB_CIRCUIT_CLOSED:
            return true;
            
        case sb_circuit_state::SB_CIRCUIT_OPEN: {
            auto lastFailure = last_failure_time_.load();
            auto now = std::chrono::steady_clock::now();
            
            if (lastFailure.time_since_epoch().count() > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFailure).count() >= 
                    static_cast<int64_t>(config_.recovery_timeout_ms)) {
                
                sb_circuit_state expected = sb_circuit_state::SB_CIRCUIT_OPEN;
                if (state_.compare_exchange_strong(expected, sb_circuit_state::SB_CIRCUIT_HALF_OPEN)) {
                    failure_count_.store(0);
                    success_count_.store(0);
                    half_open_requests_.store(0);
                }
                return AllowHalfOpenRequest();
            }
            return false;
        }
            
        case sb_circuit_state::SB_CIRCUIT_HALF_OPEN:
            return AllowHalfOpenRequest();
    }
    
    return false;
}

bool CircuitBreaker::AllowHalfOpenRequest() {
    uint32_t current = half_open_requests_.load();
    if (current >= config_.half_open_max_requests) {
        return false;
    }
    
    if (half_open_requests_.compare_exchange_strong(current, current + 1)) {
        return true;
    }
    
    // Retry
    return AllowHalfOpenRequest();
}

void CircuitBreaker::RecordSuccess() {
    sb_circuit_state currentState = state_.load();
    
    switch (currentState) {
        case sb_circuit_state::SB_CIRCUIT_CLOSED:
            failure_count_.store(0);
            break;
            
        case sb_circuit_state::SB_CIRCUIT_HALF_OPEN: {
            half_open_requests_--;
            uint32_t successes = success_count_.fetch_add(1) + 1;
            
            if (successes >= config_.success_threshold) {
                sb_circuit_state expected = sb_circuit_state::SB_CIRCUIT_HALF_OPEN;
                if (state_.compare_exchange_strong(expected, sb_circuit_state::SB_CIRCUIT_CLOSED)) {
                    failure_count_.store(0);
                    success_count_.store(0);
                }
            }
            break;
        }
            
        default:
            break;
    }
}

void CircuitBreaker::RecordFailure() {
    sb_circuit_state currentState = state_.load();
    
    switch (currentState) {
        case sb_circuit_state::SB_CIRCUIT_CLOSED: {
            uint32_t failures = failure_count_.fetch_add(1) + 1;
            
            if (failures >= config_.failure_threshold) {
                sb_circuit_state expected = sb_circuit_state::SB_CIRCUIT_CLOSED;
                if (state_.compare_exchange_strong(expected, sb_circuit_state::SB_CIRCUIT_OPEN)) {
                    last_failure_time_.store(std::chrono::steady_clock::now());
                }
            }
            break;
        }
            
        case sb_circuit_state::SB_CIRCUIT_HALF_OPEN:
            half_open_requests_--;
            if (state_.compare_exchange_strong(currentState, sb_circuit_state::SB_CIRCUIT_OPEN)) {
                last_failure_time_.store(std::chrono::steady_clock::now());
            }
            break;
            
        case sb_circuit_state::SB_CIRCUIT_OPEN:
            last_failure_time_.store(std::chrono::steady_clock::now());
            break;
    }
}

void CircuitBreaker::Reset() {
    state_.store(sb_circuit_state::SB_CIRCUIT_CLOSED);
    failure_count_.store(0);
    success_count_.store(0);
    half_open_requests_.store(0);
}

CircuitBreaker::Stats CircuitBreaker::GetStats() const {
    return {
        state_.load(),
        failure_count_.load(),
        success_count_.load(),
        half_open_requests_.load(),
        last_failure_time_.load()
    };
}

} // namespace client
} // namespace scratchbird

// C API Implementation
namespace scratchbird {
extern "C" {

sb_circuit_breaker* sb_circuit_breaker_create(const struct sb_circuit_breaker_config* config) {
    auto* breaker = new scratchbird::client::CircuitBreaker(
        config ? *config : scratchbird::sb_circuit_breaker_config_default()
    );
    return reinterpret_cast<sb_circuit_breaker*>(breaker);
}

void sb_circuit_breaker_destroy(sb_circuit_breaker* breaker) {
    if (breaker) {
        delete reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker);
    }
}

enum sb_circuit_state sb_circuit_breaker_get_state(sb_circuit_breaker* breaker) {
    if (breaker) {
        return reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker)->GetState();
    }
    return sb_circuit_state::SB_CIRCUIT_OPEN;
}

int sb_circuit_breaker_allow_request(sb_circuit_breaker* breaker) {
    if (breaker) {
        return reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker)->AllowRequest() ? 1 : 0;
    }
    return 0;
}

void sb_circuit_breaker_record_success(sb_circuit_breaker* breaker) {
    if (breaker) {
        reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker)->RecordSuccess();
    }
}

void sb_circuit_breaker_record_failure(sb_circuit_breaker* breaker) {
    if (breaker) {
        reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker)->RecordFailure();
    }
}

void sb_circuit_breaker_reset(sb_circuit_breaker* breaker) {
    if (breaker) {
        reinterpret_cast<scratchbird::client::CircuitBreaker*>(breaker)->Reset();
    }
}

} // extern "C"
} // namespace scratchbird
