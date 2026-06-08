// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export type CircuitState = "CLOSED" | "OPEN" | "HALF_OPEN";

export interface CircuitBreakerConfig {
  failureThreshold?: number;
  recoveryTimeoutMs?: number;
  successThreshold?: number;
  halfOpenMaxRequests?: number;
}

export class CircuitBreakerOpenError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CircuitBreakerOpenError";
  }
}

export class CircuitBreaker {
  private readonly config: Required<CircuitBreakerConfig>;
  private state: CircuitState = "CLOSED";
  private failureCount = 0;
  private successCount = 0;
  private halfOpenRequests = 0;
  private lastFailureAt: number | null = null;

  constructor(config: CircuitBreakerConfig = {}, private readonly name = "default") {
    this.config = {
      failureThreshold: config.failureThreshold ?? 5,
      recoveryTimeoutMs: config.recoveryTimeoutMs ?? 30_000,
      successThreshold: config.successThreshold ?? 3,
      halfOpenMaxRequests: config.halfOpenMaxRequests ?? 10,
    };
  }

  getState(): CircuitState {
    return this.state;
  }

  allowRequest(): boolean {
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

  recordSuccess(): void {
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

  recordFailure(): void {
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

  reset(): void {
    this.transitionToClosed();
  }

  stats(): Record<string, unknown> {
    return {
      name: this.name,
      state: this.state,
      failureCount: this.failureCount,
      successCount: this.successCount,
      halfOpenRequests: this.halfOpenRequests,
      lastFailureAt: this.lastFailureAt,
    };
  }

  private allowHalfOpenRequest(): boolean {
    if (this.halfOpenRequests < this.config.halfOpenMaxRequests) {
      this.halfOpenRequests += 1;
      return true;
    }
    return false;
  }

  private transitionToHalfOpen(): void {
    this.state = "HALF_OPEN";
    this.failureCount = 0;
    this.successCount = 0;
    this.halfOpenRequests = 0;
  }

  private transitionToOpen(): void {
    this.state = "OPEN";
    this.lastFailureAt = Date.now();
  }

  private transitionToClosed(): void {
    this.state = "CLOSED";
    this.failureCount = 0;
    this.successCount = 0;
    this.halfOpenRequests = 0;
    this.lastFailureAt = null;
  }
}
