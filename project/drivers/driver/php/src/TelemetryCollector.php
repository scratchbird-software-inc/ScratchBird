<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * ScratchBird PHP Driver - OpenTelemetry Telemetry
 * Copyright (c) 2025-2026 Dalton Calford
 */

namespace ScratchBird;

class TelemetryConfig {
    public bool $enableTracing = true;
    public bool $enableMetrics = true;
    public bool $enableSlowQueryLog = true;
    public int $slowQueryThresholdMs = 1000;
    public bool $sanitizeQueries = true;
    public float $sampleRate = 1.0;
    
    public function __construct(array $options = []) {
        $this->enableTracing = $options['enableTracing'] ?? true;
        $this->enableMetrics = $options['enableMetrics'] ?? true;
        $this->enableSlowQueryLog = $options['enableSlowQueryLog'] ?? true;
        $this->slowQueryThresholdMs = $options['slowQueryThresholdMs'] ?? 1000;
        $this->sanitizeQueries = $options['sanitizeQueries'] ?? true;
        $this->sampleRate = $options['sampleRate'] ?? 1.0;
    }
}

class SpanContext {
    public string $traceId;
    public string $spanId;
    public ?string $parentSpanId;
    public string $spanName;
    public float $startTime;
    public array $attributes = [];
    
    public function __construct(string $name, ?SpanContext $parent = null) {
        $this->traceId = $parent ? $parent->traceId : bin2hex(random_bytes(16));
        $this->spanId = bin2hex(random_bytes(8));
        $this->parentSpanId = $parent ? $parent->spanId : null;
        $this->spanName = $name;
        $this->startTime = microtime(true);
    }
    
    public function withAttribute(string $key, string $value): self {
        $this->attributes[$key] = $value;
        return $this;
    }
    
    public function elapsed(): int {
        return (int)((microtime(true) - $this->startTime) * 1000);
    }
}

class LatencyHistogram {
    public int $ms0_10 = 0;
    public int $ms10_100 = 0;
    public int $ms100_1000 = 0;
    public int $ms1000_10000 = 0;
    public int $msOver10000 = 0;
    
    public function record(int $durationMs): void {
        if ($durationMs <= 10) $this->ms0_10++;
        else if ($durationMs <= 100) $this->ms10_100++;
        else if ($durationMs <= 1000) $this->ms100_1000++;
        else if ($durationMs <= 10000) $this->ms1000_10000++;
        else $this->msOver10000++;
    }
}

class OperationMetrics {
    public int $count = 0;
    public int $totalTimeMs = 0;
    public int $avgTimeMs = 0;
    public int $errorCount = 0;
    
    public function record(int $durationMs, bool $success): void {
        $this->count++;
        $this->totalTimeMs += $durationMs;
        $this->avgTimeMs = (int)($this->totalTimeMs / $this->count);
        if (!$success) $this->errorCount++;
    }
}

class TelemetryCollector {
    private TelemetryConfig $config;
    private array $spans = [];
    private int $totalQueries = 0;
    private int $successfulQueries = 0;
    private int $failedQueries = 0;
    private int $totalQueryTimeMs = 0;
    private LatencyHistogram $histogram;
    private array $operationMetrics = [];
    private array $slowQueries = [];
    
    public function __construct(TelemetryConfig $config = null) {
        $this->config = $config ?? new TelemetryConfig();
        $this->histogram = new LatencyHistogram();
    }
    
    public function startSpan(string $name): ?SpanContext {
        if (!$this->config->enableTracing) return null;
        if (mt_rand() / mt_getrandmax() > $this->config->sampleRate) return null;
        
        $span = new SpanContext($name);
        $this->spans[] = $span;
        if (count($this->spans) > 1000) array_shift($this->spans);
        return $span;
    }
    
    public function endSpan(?SpanContext $span, bool $success = true): void {
        if (!$span || !$this->config->enableTracing) return;
        $durationMs = $span->elapsed();
        $this->recordQueryMetrics($span->spanName, $durationMs, $success);
        if ($this->config->enableSlowQueryLog && $durationMs > $this->config->slowQueryThresholdMs) {
            $this->recordSlowQuery($span, $durationMs);
        }
    }
    
    private function recordQueryMetrics(string $operation, int $durationMs, bool $success): void {
        if (!$this->config->enableMetrics) return;
        $this->totalQueries++;
        $success ? $this->successfulQueries++ : $this->failedQueries++;
        $this->totalQueryTimeMs += $durationMs;
        $this->histogram->record($durationMs);
        if (!isset($this->operationMetrics[$operation])) {
            $this->operationMetrics[$operation] = new OperationMetrics();
        }
        $this->operationMetrics[$operation]->record($durationMs, $success);
    }
    
    private function recordSlowQuery(SpanContext $span, int $durationMs): void {
        $this->slowQueries[] = [
            'traceId' => $span->traceId,
            'spanName' => $span->spanName,
            'durationMs' => $durationMs,
            'timestamp' => time(),
            'attributes' => $span->attributes
        ];
        if (count($this->slowQueries) > 100) array_shift($this->slowQueries);
    }
    
    public function getMetrics(): array {
        return [
            'totalQueries' => $this->totalQueries,
            'successfulQueries' => $this->successfulQueries,
            'failedQueries' => $this->failedQueries,
            'totalQueryTimeMs' => $this->totalQueryTimeMs,
            'latencyHistogram' => $this->histogram,
            'operationMetrics' => $this->operationMetrics
        ];
    }
    
    public function getSlowQueries(): array {
        return $this->slowQueries;
    }
    
    public static function sanitizeQuery(?string $sql): ?string {
        if (!$sql) return $sql;
        return preg_replace("/'[^']*'/", "'?'", $sql);
    }
    
    public function exportPrometheusMetrics(): string {
        $m = $this->getMetrics();
        $h = $m['latencyHistogram'];
        $bucket10 = $h->ms0_10;
        $bucket100 = $h->ms0_10 + $h->ms10_100;
        $bucket1000 = $h->ms0_10 + $h->ms10_100 + $h->ms100_1000;
        return "# HELP scratchbird_queries_total Total number of queries\n"
            . "# TYPE scratchbird_queries_total counter\n"
            . "scratchbird_queries_total {$m['totalQueries']}\n"
            . "# HELP scratchbird_query_duration_ms Query duration histogram\n"
            . "# TYPE scratchbird_query_duration_ms histogram\n"
            . "scratchbird_query_duration_ms_bucket{le=\"10\"} {$bucket10}\n"
            . "scratchbird_query_duration_ms_bucket{le=\"100\"} {$bucket100}\n"
            . "scratchbird_query_duration_ms_bucket{le=\"1000\"} {$bucket1000}\n";
    }
}
