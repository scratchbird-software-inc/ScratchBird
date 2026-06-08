// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Circuit Breaker - Prevents cascading failures
 * Copyright (c) 2025-2026 Dalton Calford
 */
package com.scratchbird.jdbc;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import java.util.logging.Logger;

/**
 * States of the circuit breaker
 */
enum CircuitState {
    CLOSED,     // Normal operation
    OPEN,       // Failure threshold reached
    HALF_OPEN   // Testing recovery
}

/**
 * Configuration for circuit breaker
 */
class CircuitBreakerConfig {
    public static final int DEFAULT_FAILURE_THRESHOLD = 5;
    public static final long DEFAULT_RECOVERY_TIMEOUT_MS = 30000;  // 30 seconds
    public static final int DEFAULT_SUCCESS_THRESHOLD = 3;
    public static final int DEFAULT_HALF_OPEN_MAX_REQUESTS = 10;
    
    private int failureThreshold = DEFAULT_FAILURE_THRESHOLD;
    private long recoveryTimeoutMs = DEFAULT_RECOVERY_TIMEOUT_MS;
    private int successThreshold = DEFAULT_SUCCESS_THRESHOLD;
    private int halfOpenMaxRequests = DEFAULT_HALF_OPEN_MAX_REQUESTS;
    
    public CircuitBreakerConfig() {}
    
    public CircuitBreakerConfig failureThreshold(int threshold) {
        this.failureThreshold = threshold;
        return this;
    }
    
    public CircuitBreakerConfig recoveryTimeout(long ms) {
        this.recoveryTimeoutMs = ms;
        return this;
    }
    
    public CircuitBreakerConfig successThreshold(int threshold) {
        this.successThreshold = threshold;
        return this;
    }
    
    public CircuitBreakerConfig halfOpenMaxRequests(int max) {
        this.halfOpenMaxRequests = max;
        return this;
    }
    
    public int getFailureThreshold() { return failureThreshold; }
    public long getRecoveryTimeoutMs() { return recoveryTimeoutMs; }
    public int getSuccessThreshold() { return successThreshold; }
    public int getHalfOpenMaxRequests() { return halfOpenMaxRequests; }
}

/**
 * Exception thrown when circuit is open
 */
class CircuitBreakerOpenException extends Exception {
    public CircuitBreakerOpenException(String message) {
        super(message);
    }
}

/**
 * Circuit breaker statistics
 */
class CircuitBreakerStats {
    public final CircuitState state;
    public final int failureCount;
    public final int successCount;
    public final int halfOpenRequests;
    public final long lastFailureTimeMs;
    
    public CircuitBreakerStats(CircuitState state, int failureCount, int successCount, 
                               int halfOpenRequests, long lastFailureTimeMs) {
        this.state = state;
        this.failureCount = failureCount;
        this.successCount = successCount;
        this.halfOpenRequests = halfOpenRequests;
        this.lastFailureTimeMs = lastFailureTimeMs;
    }
}

/**
 * Circuit breaker implementation
 */
public class CircuitBreaker {
    private static final Logger LOGGER = Logger.getLogger(CircuitBreaker.class.getName());
    
    private final CircuitBreakerConfig config;
    private final AtomicReference<CircuitState> state;
    private final AtomicInteger failureCount;
    private final AtomicInteger successCount;
    private final AtomicInteger halfOpenRequests;
    private final AtomicLong lastFailureTime;
    private final String name;
    
    public CircuitBreaker(CircuitBreakerConfig config) {
        this(config, "default");
    }
    
    public CircuitBreaker(CircuitBreakerConfig config, String name) {
        this.config = config;
        this.name = name;
        this.state = new AtomicReference<>(CircuitState.CLOSED);
        this.failureCount = new AtomicInteger(0);
        this.successCount = new AtomicInteger(0);
        this.halfOpenRequests = new AtomicInteger(0);
        this.lastFailureTime = new AtomicLong(0);
    }
    
    public CircuitBreaker() {
        this(new CircuitBreakerConfig());
    }
    
    /**
     * Get current state
     */
    public CircuitState getState() {
        return state.get();
    }
    
    /**
     * Check if request should be allowed
     */
    public boolean allowRequest() {
        CircuitState currentState = state.get();
        
        switch (currentState) {
            case CLOSED:
                return true;
                
            case OPEN:
                // Check if recovery timeout has passed
                long lastFailure = lastFailureTime.get();
                if (lastFailure > 0 && 
                    System.currentTimeMillis() - lastFailure >= config.getRecoveryTimeoutMs()) {
                    // Try to transition to half-open
                    if (state.compareAndSet(CircuitState.OPEN, CircuitState.HALF_OPEN)) {
                        failureCount.set(0);
                        successCount.set(0);
                        halfOpenRequests.set(0);
                        LOGGER.info("Circuit breaker " + name + ": OPEN -> HALF_OPEN");
                    }
                    return allowHalfOpenRequest();
                }
                return false;
                
            case HALF_OPEN:
                return allowHalfOpenRequest();
                
            default:
                return false;
        }
    }
    
    private boolean allowHalfOpenRequest() {
        int current = halfOpenRequests.get();
        if (current >= config.getHalfOpenMaxRequests()) {
            return false;
        }
        return halfOpenRequests.compareAndSet(current, current + 1) || allowHalfOpenRequest();
    }
    
    /**
     * Record a successful operation
     */
    public void recordSuccess() {
        CircuitState currentState = state.get();
        
        switch (currentState) {
            case CLOSED:
                failureCount.set(0);
                break;
                
            case HALF_OPEN:
                halfOpenRequests.decrementAndGet();
                int successes = successCount.incrementAndGet();
                
                if (successes >= config.getSuccessThreshold()) {
                    if (state.compareAndSet(CircuitState.HALF_OPEN, CircuitState.CLOSED)) {
                        failureCount.set(0);
                        successCount.set(0);
                        LOGGER.info("Circuit breaker " + name + ": HALF_OPEN -> CLOSED");
                    }
                }
                break;
                
            default:
                break;
        }
    }
    
    /**
     * Record a failed operation
     */
    public void recordFailure() {
        CircuitState currentState = state.get();
        
        switch (currentState) {
            case CLOSED:
                int failures = failureCount.incrementAndGet();
                
                if (failures >= config.getFailureThreshold()) {
                    if (state.compareAndSet(CircuitState.CLOSED, CircuitState.OPEN)) {
                        lastFailureTime.set(System.currentTimeMillis());
                        LOGGER.warning("Circuit breaker " + name + ": CLOSED -> OPEN");
                    }
                }
                break;
                
            case HALF_OPEN:
                halfOpenRequests.decrementAndGet();
                if (state.compareAndSet(CircuitState.HALF_OPEN, CircuitState.OPEN)) {
                    lastFailureTime.set(System.currentTimeMillis());
                    LOGGER.warning("Circuit breaker " + name + ": HALF_OPEN -> OPEN");
                }
                break;
                
            case OPEN:
                lastFailureTime.set(System.currentTimeMillis());
                break;
        }
    }
    
    /**
     * Reset circuit breaker to closed state
     */
    public void reset() {
        state.set(CircuitState.CLOSED);
        failureCount.set(0);
        successCount.set(0);
        halfOpenRequests.set(0);
        LOGGER.info("Circuit breaker " + name + ": manually reset to CLOSED");
    }
    
    /**
     * Execute a supplier with circuit breaker protection
     */
    public <T> T execute(Supplier<T> supplier) throws CircuitBreakerOpenException, Exception {
        if (!allowRequest()) {
            throw new CircuitBreakerOpenException("Circuit breaker " + name + " is OPEN");
        }
        
        try {
            T result = supplier.get();
            recordSuccess();
            return result;
        } catch (Exception e) {
            recordFailure();
            throw e;
        }
    }
    
    /**
     * Execute a runnable with circuit breaker protection
     */
    public void execute(Runnable runnable) throws CircuitBreakerOpenException, Exception {
        if (!allowRequest()) {
            throw new CircuitBreakerOpenException("Circuit breaker " + name + " is OPEN");
        }
        
        try {
            runnable.run();
            recordSuccess();
        } catch (Exception e) {
            recordFailure();
            throw e;
        }
    }
    
    /**
     * Get statistics
     */
    public CircuitBreakerStats getStats() {
        return new CircuitBreakerStats(
            state.get(),
            failureCount.get(),
            successCount.get(),
            halfOpenRequests.get(),
            lastFailureTime.get()
        );
    }
    
    @Override
    public String toString() {
        CircuitBreakerStats stats = getStats();
        return String.format("CircuitBreaker{name='%s', state=%s, failures=%d, successes=%d}",
            name, stats.state, stats.failureCount, stats.successCount);
    }
}
