"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.LeakDetector = exports.LeakDetectionGuard = exports.CheckoutInfo = void 0;
class CheckoutInfo {
    constructor(metadata, captureStackTrace) {
        this.metadata = metadata;
        this.checkoutTime = Date.now();
        if (captureStackTrace) {
            this.stackTrace = new Error().stack;
        }
    }
    heldDurationMs() {
        return Date.now() - this.checkoutTime;
    }
}
exports.CheckoutInfo = CheckoutInfo;
class LeakDetectionGuard {
    constructor(detector, connectionId) {
        this.detector = detector;
        this.connectionId = connectionId;
        this.released = false;
    }
    release() {
        if (this.released) {
            return;
        }
        this.released = true;
        this.detector.checkin(this.connectionId);
    }
}
exports.LeakDetectionGuard = LeakDetectionGuard;
class LeakDetector {
    constructor(config = {}) {
        this.checkouts = new Map();
        this.config = {
            thresholdMs: config.thresholdMs ?? 30000,
            captureStackTrace: config.captureStackTrace ?? false,
            checkIntervalMs: config.checkIntervalMs ?? 10000,
        };
    }
    start() {
        if (this.timer) {
            return;
        }
        this.timer = setInterval(() => this.checkLeaks(), this.config.checkIntervalMs);
        this.timer.unref?.();
    }
    stop() {
        if (this.timer) {
            clearInterval(this.timer);
            this.timer = undefined;
        }
    }
    checkout(connectionId, metadata = {}) {
        const info = new CheckoutInfo(metadata, this.config.captureStackTrace);
        this.checkouts.set(connectionId, info);
        return new LeakDetectionGuard(this, connectionId);
    }
    checkin(connectionId) {
        this.checkouts.delete(connectionId);
    }
    activeCount() {
        return this.checkouts.size;
    }
    stats() {
        let potentialLeaks = 0;
        for (const info of this.checkouts.values()) {
            if (info.heldDurationMs() > this.config.thresholdMs) {
                potentialLeaks += 1;
            }
        }
        return { activeCheckouts: this.checkouts.size, potentialLeaks };
    }
    checkLeaks() {
        for (const [connId, info] of this.checkouts.entries()) {
            if (info.heldDurationMs() > this.config.thresholdMs) {
                // Use console warning to avoid pulling in logging deps
                // eslint-disable-next-line no-console
                console.warn(`POSSIBLE CONNECTION LEAK: conn=${connId} held=${info.heldDurationMs()}ms`);
            }
        }
    }
}
exports.LeakDetector = LeakDetector;
