"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.CircuitBreaker = exports.CircuitBreakerOpenError = void 0;
class CircuitBreakerOpenError extends Error {
    constructor(message) {
        super(message);
        this.name = "CircuitBreakerOpenError";
    }
}
exports.CircuitBreakerOpenError = CircuitBreakerOpenError;
class CircuitBreaker {
    constructor(config = {}, name = "default") {
        this.name = name;
        this.state = "CLOSED";
        this.failureCount = 0;
        this.successCount = 0;
        this.halfOpenRequests = 0;
        this.lastFailureAt = null;
        this.config = {
            failureThreshold: config.failureThreshold ?? 5,
            recoveryTimeoutMs: config.recoveryTimeoutMs ?? 30000,
            successThreshold: config.successThreshold ?? 3,
            halfOpenMaxRequests: config.halfOpenMaxRequests ?? 10,
        };
    }
    getState() {
        return this.state;
    }
    allowRequest() {
        if (this.state === "CLOSED") {
            return true;
        }
        if (this.state === "OPEN") {
            if (this.lastFailureAt !== null && Date.now() - this.lastFailureAt >= this.config.recoveryTimeoutMs) {
                this.transitionToHalfOpen();
                return this.allowHalfOpenRequest();
            }
            return false;
        }
        return this.allowHalfOpenRequest();
    }
    recordSuccess() {
        if (this.state === "CLOSED") {
            this.failureCount = 0;
            return;
        }
        if (this.state === "HALF_OPEN") {
            this.halfOpenRequests = Math.max(0, this.halfOpenRequests - 1);
            this.successCount += 1;
            if (this.successCount >= this.config.successThreshold) {
                this.transitionToClosed();
            }
        }
    }
    recordFailure() {
        if (this.state === "CLOSED") {
            this.failureCount += 1;
            if (this.failureCount >= this.config.failureThreshold) {
                this.transitionToOpen();
            }
            return;
        }
        if (this.state === "HALF_OPEN") {
            this.halfOpenRequests = Math.max(0, this.halfOpenRequests - 1);
            this.transitionToOpen();
            return;
        }
        if (this.state === "OPEN") {
            this.lastFailureAt = Date.now();
        }
    }
    reset() {
        this.transitionToClosed();
    }
    stats() {
        return {
            name: this.name,
            state: this.state,
            failureCount: this.failureCount,
            successCount: this.successCount,
            halfOpenRequests: this.halfOpenRequests,
            lastFailureAt: this.lastFailureAt,
        };
    }
    allowHalfOpenRequest() {
        if (this.halfOpenRequests < this.config.halfOpenMaxRequests) {
            this.halfOpenRequests += 1;
            return true;
        }
        return false;
    }
    transitionToHalfOpen() {
        this.state = "HALF_OPEN";
        this.failureCount = 0;
        this.successCount = 0;
        this.halfOpenRequests = 0;
    }
    transitionToOpen() {
        this.state = "OPEN";
        this.lastFailureAt = Date.now();
    }
    transitionToClosed() {
        this.state = "CLOSED";
        this.failureCount = 0;
        this.successCount = 0;
        this.halfOpenRequests = 0;
        this.lastFailureAt = null;
    }
}
exports.CircuitBreaker = CircuitBreaker;
