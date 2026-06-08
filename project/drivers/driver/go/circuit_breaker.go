// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

// CircuitState represents the state of a circuit breaker
type CircuitState int32

const (
	// StateClosed - normal operation, requests allowed
	StateClosed CircuitState = iota
	// StateOpen - failure threshold reached, requests blocked
	StateOpen
	// StateHalfOpen - testing if service recovered
	StateHalfOpen
)

func (s CircuitState) String() string {
	switch s {
	case StateClosed:
		return "CLOSED"
	case StateOpen:
		return "OPEN"
	case StateHalfOpen:
		return "HALF_OPEN"
	default:
		return "UNKNOWN"
	}
}

// CircuitBreakerConfig configures circuit breaker behavior
type CircuitBreakerConfig struct {
	// Number of failures before opening circuit (default: 5)
	FailureThreshold uint32
	// Duration to wait before attempting recovery (default: 30 seconds)
	RecoveryTimeout time.Duration
	// Number of successes required to close circuit from half-open (default: 3)
	SuccessThreshold uint32
	// Maximum number of requests to allow in half-open state (default: 10)
	HalfOpenMaxRequests uint32
}

// DefaultCircuitBreakerConfig returns default circuit breaker configuration
func DefaultCircuitBreakerConfig() CircuitBreakerConfig {
	return CircuitBreakerConfig{
		FailureThreshold:    5,
		RecoveryTimeout:     30 * time.Second,
		SuccessThreshold:    3,
		HalfOpenMaxRequests: 10,
	}
}

// CircuitBreaker implements the circuit breaker pattern
type CircuitBreaker struct {
	config              CircuitBreakerConfig
	state               int32 // atomic CircuitState
	failureCount        uint32
	successCount        uint32
	halfOpenRequests    uint32
	lastFailureTime     int64 // UnixNano
	lastFailureTimeMu   sync.RWMutex
	onStateChange       func(from, to CircuitState)
}

// NewCircuitBreaker creates a new circuit breaker
func NewCircuitBreaker(config CircuitBreakerConfig) *CircuitBreaker {
	return &CircuitBreaker{
		config: config,
		state:  int32(StateClosed),
	}
}

// NewCircuitBreakerWithCallback creates a circuit breaker with state change callback
func NewCircuitBreakerWithCallback(config CircuitBreakerConfig, onStateChange func(from, to CircuitState)) *CircuitBreaker {
	cb := NewCircuitBreaker(config)
	cb.onStateChange = onStateChange
	return cb
}

// State returns the current state
func (cb *CircuitBreaker) State() CircuitState {
	return CircuitState(atomic.LoadInt32(&cb.state))
}

// AllowRequest checks if a request should be allowed
func (cb *CircuitBreaker) AllowRequest() bool {
	state := cb.State()
	
	switch state {
	case StateClosed:
		return true
		
	case StateOpen:
		// Check if recovery timeout has passed
		cb.lastFailureTimeMu.RLock()
		lastFailure := cb.lastFailureTime
		cb.lastFailureTimeMu.RUnlock()
		
		if time.Since(time.Unix(0, lastFailure)) >= cb.config.RecoveryTimeout {
			// Transition to half-open
			if atomic.CompareAndSwapInt32(&cb.state, int32(StateOpen), int32(StateHalfOpen)) {
				atomic.StoreUint32(&cb.failureCount, 0)
				atomic.StoreUint32(&cb.successCount, 0)
				atomic.StoreUint32(&cb.halfOpenRequests, 0)
				cb.notifyStateChange(StateOpen, StateHalfOpen)
			}
			
			// Try to allow this request in half-open state
			return cb.allowHalfOpenRequest()
		}
		
		return false
		
	case StateHalfOpen:
		return cb.allowHalfOpenRequest()
		
	default:
		return false
	}
}

func (cb *CircuitBreaker) allowHalfOpenRequest() bool {
	current := atomic.LoadUint32(&cb.halfOpenRequests)
	if current >= cb.config.HalfOpenMaxRequests {
		return false
	}
	
	if atomic.CompareAndSwapUint32(&cb.halfOpenRequests, current, current+1) {
		return true
	}
	
	// CAS failed, try again
	return cb.allowHalfOpenRequest()
}

// RecordSuccess records a successful operation
func (cb *CircuitBreaker) RecordSuccess() {
	state := cb.State()
	
	switch state {
	case StateClosed:
		atomic.StoreUint32(&cb.failureCount, 0)
		
	case StateHalfOpen:
		atomic.AddUint32(&cb.halfOpenRequests, ^uint32(0)) // Decrement
		
		successes := atomic.AddUint32(&cb.successCount, 1)
		if successes >= cb.config.SuccessThreshold {
			if atomic.CompareAndSwapInt32(&cb.state, int32(StateHalfOpen), int32(StateClosed)) {
				atomic.StoreUint32(&cb.failureCount, 0)
				atomic.StoreUint32(&cb.successCount, 0)
				cb.notifyStateChange(StateHalfOpen, StateClosed)
			}
		}
	}
}

// RecordFailure records a failed operation
func (cb *CircuitBreaker) RecordFailure() {
	state := cb.State()
	
	switch state {
	case StateClosed:
		failures := atomic.AddUint32(&cb.failureCount, 1)
		if failures >= cb.config.FailureThreshold {
			if atomic.CompareAndSwapInt32(&cb.state, int32(StateClosed), int32(StateOpen)) {
				cb.lastFailureTimeMu.Lock()
				cb.lastFailureTime = time.Now().UnixNano()
				cb.lastFailureTimeMu.Unlock()
				cb.notifyStateChange(StateClosed, StateOpen)
			}
		}
		
	case StateHalfOpen:
		atomic.AddUint32(&cb.halfOpenRequests, ^uint32(0)) // Decrement
		if atomic.CompareAndSwapInt32(&cb.state, int32(StateHalfOpen), int32(StateOpen)) {
			cb.lastFailureTimeMu.Lock()
			cb.lastFailureTime = time.Now().UnixNano()
			cb.lastFailureTimeMu.Unlock()
			cb.notifyStateChange(StateHalfOpen, StateOpen)
		}
		
	case StateOpen:
		// Update last failure time
		cb.lastFailureTimeMu.Lock()
		cb.lastFailureTime = time.Now().UnixNano()
		cb.lastFailureTimeMu.Unlock()
	}
}

// Reset resets the circuit breaker to closed state
func (cb *CircuitBreaker) Reset() {
	oldState := cb.State()
	atomic.StoreInt32(&cb.state, int32(StateClosed))
	atomic.StoreUint32(&cb.failureCount, 0)
	atomic.StoreUint32(&cb.successCount, 0)
	atomic.StoreUint32(&cb.halfOpenRequests, 0)
	
	if oldState != StateClosed {
		cb.notifyStateChange(oldState, StateClosed)
	}
}

func (cb *CircuitBreaker) notifyStateChange(from, to CircuitState) {
	if cb.onStateChange != nil {
		cb.onStateChange(from, to)
	}
}

// CircuitBreakerStats contains circuit breaker statistics
type CircuitBreakerStats struct {
	State           CircuitState
	FailureCount    uint32
	SuccessCount    uint32
	HalfOpenRequests uint32
}

// Stats returns current statistics
func (cb *CircuitBreaker) Stats() CircuitBreakerStats {
	return CircuitBreakerStats{
		State:            cb.State(),
		FailureCount:     atomic.LoadUint32(&cb.failureCount),
		SuccessCount:     atomic.LoadUint32(&cb.successCount),
		HalfOpenRequests: atomic.LoadUint32(&cb.halfOpenRequests),
	}
}

// ErrCircuitOpen is returned when the circuit is open
var ErrCircuitOpen = errors.New("circuit breaker is open")

// CircuitBreakerFunc is a function that can be executed with circuit breaker protection
type CircuitBreakerFunc func() error

// Execute executes a function with circuit breaker protection
func (cb *CircuitBreaker) Execute(fn CircuitBreakerFunc) error {
	if !cb.AllowRequest() {
		return ErrCircuitOpen
	}
	
	err := fn()
	if err != nil {
		cb.RecordFailure()
		return err
	}
	
	cb.RecordSuccess()
	return nil
}

// TypedCircuitBreaker is a generic circuit breaker for typed operations
type TypedCircuitBreaker[T any] struct {
	*CircuitBreaker
}

// NewTypedCircuitBreaker creates a new typed circuit breaker
func NewTypedCircuitBreaker[T any](config CircuitBreakerConfig) *TypedCircuitBreaker[T] {
	return &TypedCircuitBreaker[T]{
		CircuitBreaker: NewCircuitBreaker(config),
	}
}

// TypedCircuitBreakerFunc is a function that returns a value and error
type TypedCircuitBreakerFunc[T any] func() (T, error)

// Execute executes a function with circuit breaker protection
func (cb *TypedCircuitBreaker[T]) Execute(fn TypedCircuitBreakerFunc[T]) (T, error) {
	var zero T
	
	if !cb.AllowRequest() {
		return zero, ErrCircuitOpen
	}
	
	result, err := fn()
	if err != nil {
		cb.RecordFailure()
		return zero, err
	}
	
	cb.RecordSuccess()
	return result, nil
}

// String returns a string representation of the circuit breaker state
func (cb *CircuitBreaker) String() string {
	stats := cb.Stats()
	return fmt.Sprintf("CircuitBreaker{state=%s, failures=%d, successes=%d}",
		stats.State, stats.FailureCount, stats.SuccessCount)
}
