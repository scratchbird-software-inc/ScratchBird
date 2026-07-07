"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.KeepaliveManager = exports.KeepaliveTracker = void 0;
class KeepaliveTracker {
    constructor(config) {
        this.config = config;
        this.lastActivity = Date.now();
    }
    markActive() {
        this.lastActivity = Date.now();
    }
    needsValidation() {
        return Date.now() - this.lastActivity > this.config.maxIdleBeforeCheckMs;
    }
    idleDurationMs() {
        return Date.now() - this.lastActivity;
    }
}
exports.KeepaliveTracker = KeepaliveTracker;
class KeepaliveManager {
    constructor(config = {}) {
        this.trackers = new Map();
        this.pingers = new Map();
        this.config = {
            intervalMs: config.intervalMs ?? 120000,
            maxIdleBeforeCheckMs: config.maxIdleBeforeCheckMs ?? 600000,
            validationTimeoutMs: config.validationTimeoutMs ?? 5000,
        };
    }
    start() {
        if (this.timer) {
            return;
        }
        this.timer = setInterval(() => void this.checkConnections(), this.config.intervalMs);
        this.timer.unref?.();
    }
    stop() {
        if (this.timer) {
            clearInterval(this.timer);
            this.timer = undefined;
        }
    }
    register(connId, pinger) {
        const tracker = new KeepaliveTracker(this.config);
        this.trackers.set(connId, tracker);
        this.pingers.set(connId, pinger);
        return tracker;
    }
    unregister(connId) {
        this.trackers.delete(connId);
        this.pingers.delete(connId);
    }
    async checkConnections() {
        for (const [connId, tracker] of this.trackers.entries()) {
            if (!tracker.needsValidation()) {
                continue;
            }
            const pinger = this.pingers.get(connId);
            if (!pinger) {
                continue;
            }
            try {
                const healthy = await this.pingWithTimeout(pinger, this.config.validationTimeoutMs);
                if (healthy) {
                    tracker.markActive();
                }
            }
            catch {
                // ping failed; connection will be handled on next request
            }
        }
    }
    async pingWithTimeout(pinger, timeoutMs) {
        const result = Promise.resolve().then(() => pinger());
        const timeout = new Promise((resolve) => {
            const handle = setTimeout(() => resolve(false), timeoutMs);
            handle.unref?.();
        });
        return Promise.race([result, timeout]);
    }
}
exports.KeepaliveManager = KeepaliveManager;
