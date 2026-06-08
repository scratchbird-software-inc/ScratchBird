// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"errors"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestKeepaliveTrackerNeedsValidationAndMarkActive(t *testing.T) {
	cfg := KeepaliveConfig{
		Interval:           time.Hour,
		MaxIdleBeforeCheck: 20 * time.Millisecond,
		ValidationTimeout:  20 * time.Millisecond,
	}
	tracker := NewKeepaliveTracker(cfg)

	tracker.lastActivity.Lock()
	tracker.lastTime = time.Now().Add(-100 * time.Millisecond)
	tracker.lastActivity.Unlock()

	if !tracker.NeedsValidation() {
		t.Fatalf("expected tracker to require validation after forced idle interval")
	}

	tracker.MarkActive()
	if tracker.NeedsValidation() {
		t.Fatalf("expected tracker to be marked active after MarkActive")
	}
}

func TestKeepaliveManagerCheckConnectionsValidatesIdleConnections(t *testing.T) {
	cfg := KeepaliveConfig{
		Interval:           time.Hour,
		MaxIdleBeforeCheck: 10 * time.Millisecond,
		ValidationTimeout:  50 * time.Millisecond,
	}

	km := NewKeepaliveManager(cfg)
	defer km.Stop()

	var pingCount int32
	tracker := km.Register("conn-1", func() error {
		atomic.AddInt32(&pingCount, 1)
		return nil
	})

	tracker.lastActivity.Lock()
	tracker.lastTime = time.Now().Add(-100 * time.Millisecond)
	tracker.lastActivity.Unlock()

	km.checkConnections()

	if got := atomic.LoadInt32(&pingCount); got != 1 {
		t.Fatalf("expected one keepalive ping, got %d", got)
	}
	if tracker.NeedsValidation() {
		t.Fatalf("expected idle tracker to be reset after successful ping")
	}
}

func TestKeepaliveManagerCheckConnectionsTimeoutLeavesTrackerIdle(t *testing.T) {
	cfg := KeepaliveConfig{
		Interval:           time.Hour,
		MaxIdleBeforeCheck: 10 * time.Millisecond,
		ValidationTimeout:  10 * time.Millisecond,
	}

	km := NewKeepaliveManager(cfg)
	defer km.Stop()

	tracker := km.Register("conn-timeout", func() error {
		time.Sleep(60 * time.Millisecond)
		return nil
	})

	tracker.lastActivity.Lock()
	tracker.lastTime = time.Now().Add(-100 * time.Millisecond)
	tracker.lastActivity.Unlock()

	start := time.Now()
	km.checkConnections()
	elapsed := time.Since(start)

	if elapsed >= 45*time.Millisecond {
		t.Fatalf("expected timeout path to return quickly; elapsed=%v", elapsed)
	}
	if !tracker.NeedsValidation() {
		t.Fatalf("expected tracker to remain idle after timed-out validation")
	}
}

func TestLeakDetectorCheckoutCheckinAndStats(t *testing.T) {
	cfg := LeakDetectionConfig{
		Threshold:         20 * time.Millisecond,
		CaptureStackTrace: false,
		CheckInterval:     time.Hour,
	}

	ld := NewLeakDetector(cfg)
	defer ld.Stop()

	ld.Checkout("conn-1", map[string]string{"role": "rw"})
	if ld.ActiveCount() != 1 {
		t.Fatalf("expected one active checkout, got %d", ld.ActiveCount())
	}

	ld.mu.Lock()
	if info, ok := ld.checkouts["conn-1"]; ok {
		info.CheckoutTime = time.Now().Add(-200 * time.Millisecond)
	}
	ld.mu.Unlock()

	stats := ld.Stats()
	if stats.ActiveCheckouts != 1 {
		t.Fatalf("expected stats active checkouts=1, got %d", stats.ActiveCheckouts)
	}
	if stats.PotentialLeaks != 1 {
		t.Fatalf("expected stats potential leaks=1, got %d", stats.PotentialLeaks)
	}

	ld.Checkin("conn-1")
	if ld.ActiveCount() != 0 {
		t.Fatalf("expected no active checkouts after checkin, got %d", ld.ActiveCount())
	}
}

func TestLeakDetectionGuardReleaseIsIdempotent(t *testing.T) {
	cfg := LeakDetectionConfig{
		Threshold:         time.Second,
		CaptureStackTrace: false,
		CheckInterval:     time.Hour,
	}

	ld := NewLeakDetector(cfg)
	defer ld.Stop()

	guard := NewLeakDetectionGuard("conn-guard", ld, map[string]string{"component": "test"})
	if ld.ActiveCount() != 1 {
		t.Fatalf("expected one active checkout after guard creation, got %d", ld.ActiveCount())
	}

	guard.Release()
	guard.Release()
	if ld.ActiveCount() != 0 {
		t.Fatalf("expected no active checkouts after double release, got %d", ld.ActiveCount())
	}
}

func TestCircuitBreakerTransitionsAndExecuteOpenGuard(t *testing.T) {
	cfg := CircuitBreakerConfig{
		FailureThreshold:    2,
		RecoveryTimeout:     15 * time.Millisecond,
		SuccessThreshold:    2,
		HalfOpenMaxRequests: 1,
	}

	cb := NewCircuitBreaker(cfg)
	if cb.State() != StateClosed {
		t.Fatalf("expected initial state CLOSED, got %s", cb.State())
	}

	cb.RecordFailure()
	if cb.State() != StateClosed {
		t.Fatalf("expected CLOSED after first failure, got %s", cb.State())
	}
	cb.RecordFailure()
	if cb.State() != StateOpen {
		t.Fatalf("expected OPEN after threshold failures, got %s", cb.State())
	}
	if cb.AllowRequest() {
		t.Fatalf("expected open circuit to reject request before recovery timeout")
	}

	time.Sleep(25 * time.Millisecond)
	if !cb.AllowRequest() {
		t.Fatalf("expected request to be allowed when transitioning OPEN -> HALF_OPEN")
	}
	if cb.State() != StateHalfOpen {
		t.Fatalf("expected HALF_OPEN after recovery timeout, got %s", cb.State())
	}
	if cb.AllowRequest() {
		t.Fatalf("expected HALF_OPEN max request guard to reject extra request")
	}

	cb.RecordSuccess()
	if !cb.AllowRequest() {
		t.Fatalf("expected HALF_OPEN to allow next probe after prior request completion")
	}
	cb.RecordSuccess()
	if cb.State() != StateClosed {
		t.Fatalf("expected CLOSED after success threshold, got %s", cb.State())
	}

	cbOpen := NewCircuitBreaker(CircuitBreakerConfig{
		FailureThreshold:    1,
		RecoveryTimeout:     time.Hour,
		SuccessThreshold:    1,
		HalfOpenMaxRequests: 1,
	})
	first := cbOpen.Execute(func() error { return errors.New("boom") })
	if first == nil || first.Error() != "boom" {
		t.Fatalf("expected first execute failure to propagate, got %v", first)
	}

	second := cbOpen.Execute(func() error { return nil })
	if !errors.Is(second, ErrCircuitOpen) {
		t.Fatalf("expected second execute to fail with ErrCircuitOpen, got %v", second)
	}
}

func TestTelemetryCollectorRecordsMetricsAndSlowQueries(t *testing.T) {
	cfg := TelemetryConfig{
		EnableTracing:        true,
		EnableMetrics:        true,
		EnableSlowQueryLog:   true,
		SlowQueryThresholdMs: 5,
		SanitizeQueries:      true,
		SampleRate:           1.0,
	}

	tc := NewTelemetryCollector(cfg)
	span := tc.StartSpan("query")
	if span == nil {
		t.Fatalf("expected tracing span when tracing is enabled")
	}

	span.WithAttribute("sql", "SELECT 1")
	span.StartTime = time.Now().Add(-10 * time.Millisecond)
	tc.EndSpan(span, true)

	metrics := tc.GetMetrics()
	if metrics.TotalQueries != 1 {
		t.Fatalf("expected total queries=1, got %d", metrics.TotalQueries)
	}
	if metrics.SuccessfulQueries != 1 {
		t.Fatalf("expected successful queries=1, got %d", metrics.SuccessfulQueries)
	}
	if metrics.FailedQueries != 0 {
		t.Fatalf("expected failed queries=0, got %d", metrics.FailedQueries)
	}
	if op := metrics.OperationMetrics["query"]; op.Count != 1 {
		t.Fatalf("expected operation metric count=1, got %d", op.Count)
	}

	slow := tc.GetSlowQueries()
	if len(slow) != 1 {
		t.Fatalf("expected one slow-query log, got %d", len(slow))
	}
	if slow[0].SpanName != "query" {
		t.Fatalf("unexpected slow-query span name: %s", slow[0].SpanName)
	}

	exported := tc.ExportPrometheusMetrics()
	if !strings.Contains(exported, "scratchbird_queries_total 1") {
		t.Fatalf("expected prometheus export to include query total, got: %s", exported)
	}
}
