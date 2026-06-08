<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * ScratchBird PHP Driver
 * Keepalive Manager - Prevents connection timeouts
 * Copyright (c) 2025-2026 Dalton Calford
 */

namespace ScratchBird;

class KeepaliveConfig {
    public int $intervalMs = 120000;           // 2 minutes
    public int $maxIdleBeforeCheckMs = 600000; // 10 minutes
    public int $validationTimeoutMs = 5000;    // 5 seconds
    
    public function __construct(array $options = []) {
        $this->intervalMs = $options['intervalMs'] ?? 120000;
        $this->maxIdleBeforeCheckMs = $options['maxIdleBeforeCheckMs'] ?? 600000;
        $this->validationTimeoutMs = $options['validationTimeoutMs'] ?? 5000;
    }
}

class KeepaliveTracker {
    private KeepaliveConfig $config;
    private float $lastActivity;
    
    public function __construct(KeepaliveConfig $config) {
        $this->config = $config;
        $this->lastActivity = microtime(true);
    }
    
    public function markActive(): void {
        $this->lastActivity = microtime(true);
    }
    
    public function needsValidation(): bool {
        return ((microtime(true) - $this->lastActivity) * 1000) > $this->config->maxIdleBeforeCheckMs;
    }
    
    public function getIdleDurationMs(): int {
        return (int)((microtime(true) - $this->lastActivity) * 1000);
    }
}

class KeepaliveManager {
    private KeepaliveConfig $config;
    private array $trackers = [];
    private array $connections = [];
    private $timerId = null;
    private bool $running = false;
    
    public function __construct(KeepaliveConfig $config = null) {
        $this->config = $config ?? new KeepaliveConfig();
    }
    
    public function start(): void {
        if ($this->running) return;
        $this->running = true;
        
        // In PHP, we use a periodic check approach
        $this->timerId = true; // Placeholder for timer state
    }
    
    public function stop(): void {
        if (!$this->running) return;
        $this->running = false;
        $this->timerId = null;
    }
    
    public function register(string $connectionId, $connection, callable $pinger): KeepaliveTracker {
        $tracker = new KeepaliveTracker($this->config);
        $this->trackers[$connectionId] = $tracker;
        $this->connections[$connectionId] = ['connection' => $connection, 'pinger' => $pinger];
        return $tracker;
    }
    
    public function unregister(string $connectionId): void {
        unset($this->trackers[$connectionId]);
        unset($this->connections[$connectionId]);
    }
    
    public function getMonitoredCount(): int {
        return count($this->trackers);
    }
    
    public function checkConnections(): void {
        foreach ($this->trackers as $connId => $tracker) {
            if ($tracker->needsValidation()) {
                $connInfo = $this->connections[$connId] ?? null;
                if ($connInfo) {
                    try {
                        $isHealthy = ($connInfo['pinger'])();
                        if ($isHealthy) {
                            $tracker->markActive();
                        }
                    } catch (\Exception $e) {
                        // Validation error
                    }
                }
            }
        }
    }
}
