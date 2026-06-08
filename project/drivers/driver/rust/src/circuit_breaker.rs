// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Circuit Breaker Module
// Prevents cascading failures by stopping requests after consecutive failures

use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

/// States of the circuit breaker
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CircuitState {
    /// Normal operation - requests allowed
    #[default]
    Closed,
    /// Failure threshold reached - requests blocked
    Open,
    /// Testing if service recovered
    HalfOpen,
}

/// Configuration for circuit breaker
#[derive(Debug, Clone, Copy)]
pub struct CircuitBreakerConfig {
    /// Number of failures before opening circuit (default: 5)
    pub failure_threshold: u32,
    /// Duration to wait before attempting recovery (default: 30 seconds)
    pub recovery_timeout: Duration,
    /// Number of successes required to close circuit from half-open (default: 3)
    pub success_threshold: u32,
    /// Maximum number of requests to allow in half-open state (default: 10)
    pub half_open_max_requests: u32,
}

impl Default for CircuitBreakerConfig {
    fn default() -> Self {
        Self {
            failure_threshold: 5,
            recovery_timeout: Duration::from_secs(30),
            success_threshold: 3,
            half_open_max_requests: 10,
        }
    }
}

/// Statistics for circuit breaker
#[derive(Debug, Default)]
pub struct CircuitBreakerStats {
    pub state: CircuitState,
    pub failure_count: u32,
    pub success_count: u32,
    pub last_failure_time: Option<Instant>,
    pub total_requests: u64,
    pub total_failures: u64,
    pub total_successes: u64,
    pub total_rejections: u64,
}

/// Circuit breaker for database operations
pub struct CircuitBreaker {
    config: CircuitBreakerConfig,
    state: AtomicI32, // 0=Closed, 1=Open, 2=HalfOpen
    failure_count: AtomicU32,
    success_count: AtomicU32,
    half_open_requests: AtomicU32,
    last_failure_time: RwLock<Option<Instant>>,
}

impl CircuitBreaker {
    pub fn new(config: CircuitBreakerConfig) -> Self {
        Self {
            config,
            state: AtomicI32::new(0), // Closed
            failure_count: AtomicU32::new(0),
            success_count: AtomicU32::new(0),
            half_open_requests: AtomicU32::new(0),
            last_failure_time: RwLock::new(None),
        }
    }

    /// Check if request should be allowed
    pub async fn allow_request(&self) -> bool {
        let state = self.state.load(Ordering::SeqCst);

        match state {
            0 => {
                // Closed
                true
            }
            1 => {
                // Open
                // Check if recovery timeout has passed
                let last_failure = *self.last_failure_time.read().await;

                if let Some(time) = last_failure {
                    if time.elapsed() >= self.config.recovery_timeout {
                        // Try to transition to half-open
                        if self
                            .state
                            .compare_exchange(1, 2, Ordering::SeqCst, Ordering::SeqCst)
                            .is_ok()
                        {
                            self.failure_count.store(0, Ordering::SeqCst);
                            self.success_count.store(0, Ordering::SeqCst);
                            self.half_open_requests.store(0, Ordering::SeqCst);
                        }
                        return self.allow_half_open_request().await;
                    }
                }

                false
            }
            2 => {
                // HalfOpen
                self.allow_half_open_request().await
            }
            _ => false,
        }
    }

    async fn allow_half_open_request(&self) -> bool {
        let current = self.half_open_requests.load(Ordering::SeqCst);

        if current >= self.config.half_open_max_requests {
            return false;
        }

        self.half_open_requests.fetch_add(1, Ordering::SeqCst);
        true
    }

    /// Record a successful operation
    pub async fn record_success(&self) {
        let state = self.state.load(Ordering::SeqCst);

        match state {
            0 => {
                // Closed
                self.failure_count.store(0, Ordering::SeqCst);
            }
            2 => {
                // HalfOpen
                self.half_open_requests.fetch_sub(1, Ordering::SeqCst);

                let successes = self.success_count.fetch_add(1, Ordering::SeqCst) + 1;

                if successes >= self.config.success_threshold {
                    // Transition to closed
                    if self
                        .state
                        .compare_exchange(2, 0, Ordering::SeqCst, Ordering::SeqCst)
                        .is_ok()
                    {
                        self.failure_count.store(0, Ordering::SeqCst);
                        self.success_count.store(0, Ordering::SeqCst);
                    }
                }
            }
            _ => {}
        }
    }

    /// Record a failed operation
    pub async fn record_failure(&self) {
        let state = self.state.load(Ordering::SeqCst);

        match state {
            0 => {
                // Closed
                let failures = self.failure_count.fetch_add(1, Ordering::SeqCst) + 1;

                if failures >= self.config.failure_threshold {
                    // Transition to open
                    if self
                        .state
                        .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
                        .is_ok()
                    {
                        *self.last_failure_time.write().await = Some(Instant::now());
                    }
                }
            }
            2 => {
                // HalfOpen
                self.half_open_requests.fetch_sub(1, Ordering::SeqCst);
                // Any failure in half-open immediately opens circuit again
                if self
                    .state
                    .compare_exchange(2, 1, Ordering::SeqCst, Ordering::SeqCst)
                    .is_ok()
                {
                    *self.last_failure_time.write().await = Some(Instant::now());
                }
            }
            1 => {
                // Open
                *self.last_failure_time.write().await = Some(Instant::now());
            }
            _ => {}
        }
    }

    /// Get current state
    pub fn state(&self) -> CircuitState {
        match self.state.load(Ordering::SeqCst) {
            0 => CircuitState::Closed,
            1 => CircuitState::Open,
            2 => CircuitState::HalfOpen,
            _ => CircuitState::Open,
        }
    }

    /// Get statistics
    pub async fn stats(&self) -> CircuitBreakerStats {
        CircuitBreakerStats {
            state: self.state(),
            failure_count: self.failure_count.load(Ordering::SeqCst),
            success_count: self.success_count.load(Ordering::SeqCst),
            last_failure_time: *self.last_failure_time.read().await,
            ..Default::default()
        }
    }

    /// Reset circuit breaker to closed state
    pub async fn reset(&self) {
        self.state.store(0, Ordering::SeqCst);
        self.failure_count.store(0, Ordering::SeqCst);
        self.success_count.store(0, Ordering::SeqCst);
        self.half_open_requests.store(0, Ordering::SeqCst);
    }
}

/// Result wrapper for circuit breaker operations
#[derive(Debug)]
pub enum CircuitBreakerError<E> {
    /// Circuit is open - request not attempted
    CircuitOpen,
    /// Operation was attempted but failed
    OperationFailed(E),
}

/// Execute operation with circuit breaker protection
pub async fn with_circuit_breaker<T, E, F, Fut>(
    breaker: &CircuitBreaker,
    operation: F,
) -> Result<T, CircuitBreakerError<E>>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Result<T, E>>,
{
    if !breaker.allow_request().await {
        return Err(CircuitBreakerError::CircuitOpen);
    }

    match operation().await {
        Ok(result) => {
            breaker.record_success().await;
            Ok(result)
        }
        Err(e) => {
            breaker.record_failure().await;
            Err(CircuitBreakerError::OperationFailed(e))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_circuit_breaker_states() {
        let config = CircuitBreakerConfig {
            failure_threshold: 2,
            recovery_timeout: Duration::from_millis(100),
            success_threshold: 1,
            half_open_max_requests: 10,
        };

        let breaker = CircuitBreaker::new(config);

        // Initially closed
        assert_eq!(breaker.state(), CircuitState::Closed);
        assert!(breaker.allow_request().await);

        // Record failures to open circuit
        breaker.record_failure().await;
        assert_eq!(breaker.state(), CircuitState::Closed);

        breaker.record_failure().await;
        assert_eq!(breaker.state(), CircuitState::Open);

        // Requests should be rejected
        assert!(!breaker.allow_request().await);

        // Wait for recovery timeout
        tokio::time::sleep(Duration::from_millis(150)).await;

        // Should transition to half-open
        assert!(breaker.allow_request().await);
        assert_eq!(breaker.state(), CircuitState::HalfOpen);

        // Record success to close circuit
        breaker.record_success().await;
        assert_eq!(breaker.state(), CircuitState::Closed);
    }
}
