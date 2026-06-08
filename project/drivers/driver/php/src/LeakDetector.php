<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * ScratchBird PHP Driver - Connection Leak Detector
 * Copyright (c) 2025-2026 Dalton Calford
 */

namespace ScratchBird;

enum LeakLogLevel: string {
    case DEBUG = 'debug';
    case WARN = 'warn';
    case ERROR = 'error';
}

class LeakDetectionConfig {
    public int $thresholdMs = 30000;
    public bool $captureStackTrace = false;
    public int $checkIntervalMs = 10000;
    public LeakLogLevel $logLevel = LeakLogLevel::WARN;
    
    public function __construct(array $options = []) {
        $this->thresholdMs = $options['thresholdMs'] ?? 30000;
        $this->captureStackTrace = $options['captureStackTrace'] ?? false;
        $this->checkIntervalMs = $options['checkIntervalMs'] ?? 10000;
        $this->logLevel = $options['logLevel'] ?? LeakLogLevel::WARN;
    }
}

class CheckoutInfo {
    public float $checkoutTime;
    public int $threadId;
    public ?string $stackTrace;
    public array $metadata;
    
    public function __construct(bool $captureStackTrace, array $metadata = []) {
        $this->checkoutTime = microtime(true);
        $this->threadId = getmypid();
        $this->metadata = $metadata;
        $this->stackTrace = $captureStackTrace ? debug_backtrace() : null;
    }
    
    public function getHeldDurationMs(): int {
        return (int)((microtime(true) - $this->checkoutTime) * 1000);
    }
}

class LeakDetectionGuard {
    private LeakDetector $detector;
    private string $connectionId;
    private bool $released = false;
    
    public function __construct(LeakDetector $detector, string $connectionId) {
        $this->detector = $detector;
        $this->connectionId = $connectionId;
    }
    
    public function release(): void {
        if (!$this->released) {
            $this->detector->checkin($this->connectionId);
            $this->released = true;
        }
    }
    
    public function __destruct() {
        $this->release();
    }
}

class LeakDetector {
    private LeakDetectionConfig $config;
    private array $checkouts = [];
    private bool $running = false;
    
    public function __construct(LeakDetectionConfig $config = null) {
        $this->config = $config ?? new LeakDetectionConfig();
    }
    
    public function start(): void {
        $this->running = true;
    }
    
    public function stop(): void {
        $this->running = false;
    }
    
    public function checkout(string $connectionId, array $metadata = []): LeakDetectionGuard {
        $this->checkouts[$connectionId] = new CheckoutInfo($this->config->captureStackTrace, $metadata);
        return new LeakDetectionGuard($this, $connectionId);
    }
    
    public function checkin(string $connectionId): void {
        $info = $this->checkouts[$connectionId] ?? null;
        if ($info) {
            unset($this->checkouts[$connectionId]);
            if ($info->getHeldDurationMs() > $this->config->thresholdMs) {
                // Log held too long
            }
        }
    }
    
    public function getActiveCount(): int {
        return count($this->checkouts);
    }
    
    public function getStats(): array {
        $potentialLeaks = 0;
        foreach ($this->checkouts as $info) {
            if ($info->getHeldDurationMs() > $this->config->thresholdMs) {
                $potentialLeaks++;
            }
        }
        return ['activeCheckouts' => count($this->checkouts), 'potentialLeaks' => $potentialLeaks];
    }
    
    public function checkLeaks(): void {
        foreach ($this->checkouts as $connId => $info) {
            if ($info->getHeldDurationMs() > $this->config->thresholdMs) {
                $this->logLeak($connId, $info);
            }
        }
    }
    
    private function logLeak(string $connId, CheckoutInfo $info): void {
        $msg = "POSSIBLE CONNECTION LEAK: conn={$connId}, held={$info->getHeldDurationMs()}ms";
        error_log($msg);
    }
}
