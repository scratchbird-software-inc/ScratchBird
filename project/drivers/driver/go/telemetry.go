// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// TelemetryConfig configures telemetry collection
type TelemetryConfig struct {
	EnableTracing        bool
	EnableMetrics        bool
	EnableSlowQueryLog   bool
	SlowQueryThresholdMs int64
	SanitizeQueries      bool
	SampleRate           float64
}

// DefaultTelemetryConfig returns default telemetry configuration
func DefaultTelemetryConfig() TelemetryConfig {
	return TelemetryConfig{
		EnableTracing:        true,
		EnableMetrics:        true,
		EnableSlowQueryLog:   true,
		SlowQueryThresholdMs: 1000,
		SanitizeQueries:      true,
		SampleRate:           1.0,
	}
}

// SpanContext represents a distributed tracing span
type SpanContext struct {
	TraceID       string
	SpanID        string
	ParentSpanID  string
	SpanName      string
	StartTime     time.Time
	Attributes    map[string]string
}

// NewSpanContext creates a new span context
func NewSpanContext(name string) *SpanContext {
	return &SpanContext{
		TraceID:    generateTraceID(),
		SpanID:     generateSpanID(),
		SpanName:   name,
		StartTime:  time.Now(),
		Attributes: make(map[string]string),
	}
}

// NewChildSpan creates a child span
func NewChildSpan(name string, parent *SpanContext) *SpanContext {
	span := NewSpanContext(name)
	span.TraceID = parent.TraceID
	span.ParentSpanID = parent.SpanID
	return span
}

// WithAttribute adds an attribute
func (s *SpanContext) WithAttribute(key, value string) *SpanContext {
	s.Attributes[key] = value
	return s
}

// Elapsed returns elapsed time in milliseconds
func (s *SpanContext) Elapsed() int64 {
	return time.Since(s.StartTime).Milliseconds()
}

func generateTraceID() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func generateSpanID() string {
	b := make([]byte, 8)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// LatencyHistogram tracks query latencies
type LatencyHistogram struct {
	Ms0_10       atomic.Int64
	Ms10_100     atomic.Int64
	Ms100_1000   atomic.Int64
	Ms1000_10000 atomic.Int64
	MsOver10000  atomic.Int64
}

// Record records a duration
func (h *LatencyHistogram) Record(durationMs int64) {
	switch {
	case durationMs <= 10:
		h.Ms0_10.Add(1)
	case durationMs <= 100:
		h.Ms10_100.Add(1)
	case durationMs <= 1000:
		h.Ms100_1000.Add(1)
	case durationMs <= 10000:
		h.Ms1000_10000.Add(1)
	default:
		h.MsOver10000.Add(1)
	}
}

// OperationMetrics tracks per-operation metrics
type OperationMetrics struct {
	Count       int64
	TotalTimeMs int64
	AvgTimeMs   int64
	ErrorCount  int64
	mu          sync.RWMutex
}

// Record records a metric
func (m *OperationMetrics) Record(durationMs int64, success bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	
	m.Count++
	m.TotalTimeMs += durationMs
	m.AvgTimeMs = m.TotalTimeMs / m.Count
	if !success {
		m.ErrorCount++
	}
}

// SlowQueryLog represents a slow query entry
type SlowQueryLog struct {
	TraceID    string
	SpanName   string
	DurationMs int64
	Timestamp  time.Time
	Attributes map[string]string
}

// TelemetryCollector collects spans and metrics
type TelemetryCollector struct {
	config           TelemetryConfig
	spans            []*SpanContext
	spansMu          sync.RWMutex
	totalQueries     atomic.Int64
	successfulQueries atomic.Int64
	failedQueries    atomic.Int64
	totalQueryTimeMs atomic.Int64
	histogram        LatencyHistogram
	operationMetrics map[string]*OperationMetrics
	opMetricsMu      sync.RWMutex
	slowQueries      []*SlowQueryLog
	slowQueriesMu    sync.RWMutex
}

// NewTelemetryCollector creates a new collector
func NewTelemetryCollector(config TelemetryConfig) *TelemetryCollector {
	return &TelemetryCollector{
		config:           config,
		spans:            make([]*SpanContext, 0, 1000),
		operationMetrics: make(map[string]*OperationMetrics),
		slowQueries:      make([]*SlowQueryLog, 0, 100),
	}
}

// StartSpan begins a new span
func (t *TelemetryCollector) StartSpan(name string) *SpanContext {
	if !t.config.EnableTracing {
		return nil
	}
	
	// Check sample rate
	if t.config.SampleRate < 1.0 {
		// Simplified sampling
	}
	
	span := NewSpanContext(name)
	
	t.spansMu.Lock()
	t.spans = append(t.spans, span)
	if len(t.spans) > 1000 {
		t.spans = t.spans[1:]
	}
	t.spansMu.Unlock()
	
	return span
}

// EndSpan ends a span and records metrics
func (t *TelemetryCollector) EndSpan(span *SpanContext, success bool) {
	if span == nil || !t.config.EnableTracing {
		return
	}
	
	durationMs := span.Elapsed()
	t.recordQueryMetrics(span.SpanName, durationMs, success)
	
	if t.config.EnableSlowQueryLog && durationMs > t.config.SlowQueryThresholdMs {
		t.recordSlowQuery(span, durationMs)
	}
}

func (t *TelemetryCollector) recordQueryMetrics(operation string, durationMs int64, success bool) {
	if !t.config.EnableMetrics {
		return
	}
	
	t.totalQueries.Add(1)
	if success {
		t.successfulQueries.Add(1)
	} else {
		t.failedQueries.Add(1)
	}
	t.totalQueryTimeMs.Add(durationMs)
	t.histogram.Record(durationMs)
	
	// Per-operation metrics
	t.opMetricsMu.Lock()
	metrics, exists := t.operationMetrics[operation]
	if !exists {
		metrics = &OperationMetrics{}
		t.operationMetrics[operation] = metrics
	}
	t.opMetricsMu.Unlock()
	
	metrics.Record(durationMs, success)
}

func (t *TelemetryCollector) recordSlowQuery(span *SpanContext, durationMs int64) {
	log := &SlowQueryLog{
		TraceID:    span.TraceID,
		SpanName:   span.SpanName,
		DurationMs: durationMs,
		Timestamp:  time.Now(),
		Attributes: make(map[string]string),
	}
	
	for k, v := range span.Attributes {
		log.Attributes[k] = v
	}
	
	t.slowQueriesMu.Lock()
	t.slowQueries = append(t.slowQueries, log)
	if len(t.slowQueries) > 100 {
		t.slowQueries = t.slowQueries[1:]
	}
	t.slowQueriesMu.Unlock()
}

// MetricsSnapshot contains metrics data
type MetricsSnapshot struct {
	TotalQueries      int64
	SuccessfulQueries int64
	FailedQueries     int64
	TotalQueryTimeMs  int64
	LatencyHistogram  LatencyHistogram
	OperationMetrics  map[string]OperationMetrics
}

// GetMetrics returns current metrics
func (t *TelemetryCollector) GetMetrics() MetricsSnapshot {
	t.opMetricsMu.RLock()
	opMetrics := make(map[string]OperationMetrics)
	for k, v := range t.operationMetrics {
		v.mu.RLock()
		opMetrics[k] = *v
		v.mu.RUnlock()
	}
	t.opMetricsMu.RUnlock()
	
	return MetricsSnapshot{
		TotalQueries:      t.totalQueries.Load(),
		SuccessfulQueries: t.successfulQueries.Load(),
		FailedQueries:     t.failedQueries.Load(),
		TotalQueryTimeMs:  t.totalQueryTimeMs.Load(),
		LatencyHistogram:  t.histogram,
		OperationMetrics:  opMetrics,
	}
}

// GetSlowQueries returns slow query logs
func (t *TelemetryCollector) GetSlowQueries() []*SlowQueryLog {
	t.slowQueriesMu.RLock()
	defer t.slowQueriesMu.RUnlock()
	
	result := make([]*SlowQueryLog, len(t.slowQueries))
	copy(result, t.slowQueries)
	return result
}

// SanitizeQuery removes sensitive data from SQL
func SanitizeQuery(sql string) string {
	if sql == "" {
		return sql
	}
	// Simple sanitization - replace quoted strings
	// In production, use proper SQL parsing
	return strings.ReplaceAll(sql, "'", "'?' ")
}

// ExportPrometheusMetrics exports metrics in Prometheus format
func (t *TelemetryCollector) ExportPrometheusMetrics() string {
	m := t.GetMetrics()
	var b strings.Builder
	
	b.WriteString("# HELP scratchbird_queries_total Total number of queries\n")
	b.WriteString("# TYPE scratchbird_queries_total counter\n")
	b.WriteString(fmt.Sprintf("scratchbird_queries_total %d\n", m.TotalQueries))
	
	b.WriteString("# HELP scratchbird_query_duration_ms Query duration histogram\n")
	b.WriteString("# TYPE scratchbird_query_duration_ms histogram\n")
	b.WriteString(fmt.Sprintf("scratchbird_query_duration_ms_bucket{le=\"10\"} %d\n", m.LatencyHistogram.Ms0_10.Load()))
	b.WriteString(fmt.Sprintf("scratchbird_query_duration_ms_bucket{le=\"100\"} %d\n", 
		m.LatencyHistogram.Ms0_10.Load()+m.LatencyHistogram.Ms10_100.Load()))
	b.WriteString(fmt.Sprintf("scratchbird_query_duration_ms_bucket{le=\"1000\"} %d\n",
		m.LatencyHistogram.Ms0_10.Load()+m.LatencyHistogram.Ms10_100.Load()+m.LatencyHistogram.Ms100_1000.Load()))
	
	return b.String()
}

// TelemetrySpanGuard provides RAII-style span management
type TelemetrySpanGuard struct {
	collector *TelemetryCollector
	span      *SpanContext
	success   bool
}

// NewTelemetrySpanGuard creates a new guard
func NewTelemetrySpanGuard(collector *TelemetryCollector, span *SpanContext) *TelemetrySpanGuard {
	return &TelemetrySpanGuard{
		collector: collector,
		span:      span,
		success:   true,
	}
}

// MarkFailed marks the span as failed
func (g *TelemetrySpanGuard) MarkFailed() {
	g.success = false
}

// Finish ends the span
func (g *TelemetrySpanGuard) Finish() {
	if g.collector != nil && g.span != nil {
		g.collector.EndSpan(g.span, g.success)
	}
}
