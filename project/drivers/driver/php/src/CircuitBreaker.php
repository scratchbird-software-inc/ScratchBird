<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * ScratchBird PHP Driver - Circuit Breaker
 * Copyright (c) 2025-2026 Dalton Calford
 */

namespace ScratchBird;

enum CircuitState: string {
    case CLOSED = 'CLOSED';
    case OPEN = 'OPEN';
    case HALF_OPEN = 'HALF_OPEN';
}

class CircuitBreakerConfig {
    public int $failureThreshold = 5;
    public int $recoveryTimeoutMs = 30000;
    public int $successThreshold = 3;
    public int $halfOpenMaxRequests = 10;
    
    public function __construct(array $options = []) {
        $this->failureThreshold = $options['failureThreshold'] ?? 5;
        $this->recoveryTimeoutMs = $options['recoveryTimeoutMs'] ?? 30000;
        $this->successThreshold = $options['successThreshold'] ?? 3;
        $this->halfOpenMaxRequests = $options['halfOpenMaxRequests'] ?? 10;
    }
}

class CircuitBreakerOpenException extends \Exception {
    public function __construct(string $message = 'Circuit breaker is OPEN') {
        parent::__construct($message);
    }
}

class CircuitBreaker {
    private CircuitBreakerConfig $config;
    private string $name;
    private CircuitState $state;
    private int $failureCount = 0;
    private int $successCount = 0;
    private int $halfOpenRequests = 0;
    private ?float $lastFailureTime = null;
    
    public function __construct(CircuitBreakerConfig $config = null, string $name = 'default') {
        $this->config = $config ?? new CircuitBreakerConfig();
        $this->name = $name;
        $this->state = CircuitState::CLOSED;
    }
    
    public function getState(): CircuitState {
        return $this->state;
    }
    
    public function allowRequest(): bool {
        switch ($this->state) {
            case CircuitState::CLOSED:
                return true;
                
            case CircuitState::OPEN:
                if ($this->lastFailureTime !== null && 
                    ((microtime(true) - $this->lastFailureTime) * 1000) >= $this->config->recoveryTimeoutMs) {
                    $this->state = CircuitState::HALF_OPEN;
                    $this->failureCount = $this->successCount = $this->halfOpenRequests = 0;
                    return $this->allowHalfOpenRequest();
                }
                return false;
                
            case CircuitState::HALF_OPEN:
                return $this->allowHalfOpenRequest();
        }
        return false;
    }
    
    private function allowHalfOpenRequest(): bool {
        if ($this->halfOpenRequests < $this->config->halfOpenMaxRequests) {
            $this->halfOpenRequests++;
            return true;
        }
        return false;
    }
    
    public function recordSuccess(): void {
        switch ($this->state) {
            case CircuitState::CLOSED:
                $this->failureCount = 0;
                break;
                
            case CircuitState::HALF_OPEN:
                $this->halfOpenRequests--;
                if (++$this->successCount >= $this->config->successThreshold) {
                    $this->state = CircuitState::CLOSED;
                    $this->failureCount = $this->successCount = 0;
                }
                break;
        }
    }
    
    public function recordFailure(): void {
        switch ($this->state) {
            case CircuitState::CLOSED:
                if (++$this->failureCount >= $this->config->failureThreshold) {
                    $this->state = CircuitState::OPEN;
                    $this->lastFailureTime = microtime(true);
                }
                break;
                
            case CircuitState::HALF_OPEN:
                $this->halfOpenRequests--;
                $this->state = CircuitState::OPEN;
                $this->lastFailureTime = microtime(true);
                break;
                
            case CircuitState::OPEN:
                $this->lastFailureTime = microtime(true);
                break;
        }
    }
    
    public function reset(): void {
        $this->state = CircuitState::CLOSED;
        $this->failureCount = $this->successCount = $this->halfOpenRequests = 0;
        $this->lastFailureTime = null;
    }
    
    public function execute(callable $fn) {
        if (!$this->allowRequest()) {
            throw new CircuitBreakerOpenException();
        }
        
        try {
            $result = $fn();
            $this->recordSuccess();
            return $result;
        } catch (\Exception $e) {
            $this->recordFailure();
            throw $e;
        }
    }
    
    public function getStats(): array {
        return [
            'state' => $this->state->value,
            'failureCount' => $this->failureCount,
            'successCount' => $this->successCount,
            'halfOpenRequests' => $this->halfOpenRequests,
            'lastFailureTime' => $this->lastFailureTime
        ];
    }
}
