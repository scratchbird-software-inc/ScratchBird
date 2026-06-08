// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export interface LeakDetectionConfig {
  thresholdMs?: number;
  captureStackTrace?: boolean;
  checkIntervalMs?: number;
}

export class CheckoutInfo {
  readonly checkoutTime = Date.now();
  readonly stackTrace?: string;

  constructor(public readonly metadata: Record<string, unknown>, captureStackTrace: boolean) {
    if (captureStackTrace) {
      this.stackTrace = new Error().stack;
    }
  }

  heldDurationMs(): number {
    return Date.now() - this.checkoutTime;
  }
}

export class LeakDetectionGuard {
  private released = false;

  constructor(private readonly detector: LeakDetector, private readonly connectionId: string) {}

  release(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.detector.checkin(this.connectionId);
  }
}

export class LeakDetector {
  private readonly config: Required<LeakDetectionConfig>;
  private readonly checkouts = new Map<string, CheckoutInfo>();
  private timer?: NodeJS.Timeout;

  constructor(config: LeakDetectionConfig = {}) {
    this.config = {
      thresholdMs: config.thresholdMs ?? 30_000,
      captureStackTrace: config.captureStackTrace ?? false,
      checkIntervalMs: config.checkIntervalMs ?? 10_000,
    };
  }

  start(): void {
    if (this.timer) {
      return;
    }
    this.timer = setInterval(() => this.checkLeaks(), this.config.checkIntervalMs);
    this.timer.unref?.();
  }

  stop(): void {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = undefined;
    }
  }

  checkout(connectionId: string, metadata: Record<string, unknown> = {}): LeakDetectionGuard {
    const info = new CheckoutInfo(metadata, this.config.captureStackTrace);
    this.checkouts.set(connectionId, info);
    return new LeakDetectionGuard(this, connectionId);
  }

  checkin(connectionId: string): void {
    this.checkouts.delete(connectionId);
  }

  activeCount(): number {
    return this.checkouts.size;
  }

  stats(): Record<string, unknown> {
    let potentialLeaks = 0;
    for (const info of this.checkouts.values()) {
      if (info.heldDurationMs() > this.config.thresholdMs) {
        potentialLeaks += 1;
      }
    }
    return { activeCheckouts: this.checkouts.size, potentialLeaks };
  }

  private checkLeaks(): void {
    for (const [connId, info] of this.checkouts.entries()) {
      if (info.heldDurationMs() > this.config.thresholdMs) {
        // Use console warning to avoid pulling in logging deps
        // eslint-disable-next-line no-console
        console.warn(`POSSIBLE CONNECTION LEAK: conn=${connId} held=${info.heldDurationMs()}ms`);
      }
    }
  }
}
