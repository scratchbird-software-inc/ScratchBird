// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"fmt"
	"runtime"
	"sync"
	"time"
)

// LeakDetectionConfig configures leak detection behavior
type LeakDetectionConfig struct {
	// Threshold for warning about potential leaks (default: 30 seconds)
	Threshold time.Duration
	// Whether to capture stack traces (for debugging)
	CaptureStackTrace bool
	// Check interval (default: 10 seconds)
	CheckInterval time.Duration
}

// DefaultLeakDetectionConfig returns default leak detection configuration
func DefaultLeakDetectionConfig() LeakDetectionConfig {
	return LeakDetectionConfig{
		Threshold:         30 * time.Second,
		CaptureStackTrace: false,
		CheckInterval:     10 * time.Second,
	}
}

// CheckoutInfo tracks information about a checked-out connection
type CheckoutInfo struct {
	CheckoutTime  time.Time
	GoroutineID   int64
	StackTrace    string
	Metadata      map[string]string
}

// HeldDuration returns how long the connection has been held
func (c *CheckoutInfo) HeldDuration() time.Duration {
	return time.Since(c.CheckoutTime)
}

// LeakDetector detects connection leaks
type LeakDetector struct {
	config    LeakDetectionConfig
	checkouts map[string]*CheckoutInfo
	mu        sync.RWMutex
	stopCh    chan struct{}
}

// NewLeakDetector creates a new leak detector
func NewLeakDetector(config LeakDetectionConfig) *LeakDetector {
	ld := &LeakDetector{
		config:    config,
		checkouts: make(map[string]*CheckoutInfo),
		stopCh:    make(chan struct{}),
	}
	
	// Start monitoring goroutine
	go ld.monitor()
	
	return ld
}

// Checkout registers a connection checkout
func (ld *LeakDetector) Checkout(connID string, metadata map[string]string) {
	info := &CheckoutInfo{
		CheckoutTime: time.Now(),
		GoroutineID:  getGoroutineID(),
		Metadata:     metadata,
	}
	
	if ld.config.CaptureStackTrace {
		info.StackTrace = captureStackTrace()
	}
	
	ld.mu.Lock()
	ld.checkouts[connID] = info
	ld.mu.Unlock()
}

// Checkin registers a connection return
func (ld *LeakDetector) Checkin(connID string) {
	ld.mu.Lock()
	info, exists := ld.checkouts[connID]
	if exists {
		delete(ld.checkouts, connID)
		
		// Check if held longer than threshold
		if info.HeldDuration() > ld.config.Threshold {
			// Log warning but connection was returned
			Logger.Printf("Connection %s held for %v (threshold: %v)",
				connID, info.HeldDuration(), ld.config.Threshold)
		}
	}
	ld.mu.Unlock()
}

// ActiveCheckouts returns information about active checkouts
func (ld *LeakDetector) ActiveCheckouts() map[string]*CheckoutInfo {
	ld.mu.RLock()
	defer ld.mu.RUnlock()
	
	result := make(map[string]*CheckoutInfo, len(ld.checkouts))
	for k, v := range ld.checkouts {
		// Create a copy to avoid race conditions
		infoCopy := *v
		result[k] = &infoCopy
	}
	return result
}

// ActiveCount returns the number of active checkouts
func (ld *LeakDetector) ActiveCount() int {
	ld.mu.RLock()
	defer ld.mu.RUnlock()
	return len(ld.checkouts)
}

// monitor periodically checks for potential leaks
func (ld *LeakDetector) monitor() {
	ticker := time.NewTicker(ld.config.CheckInterval)
	defer ticker.Stop()
	
	for {
		select {
		case <-ticker.C:
			ld.checkLeaks()
		case <-ld.stopCh:
			return
		}
	}
}

// checkLeaks checks for connections held beyond threshold
func (ld *LeakDetector) checkLeaks() {
	ld.mu.RLock()
	checkouts := make([]struct {
		id   string
		info *CheckoutInfo
	}, 0, len(ld.checkouts))
	
	for id, info := range ld.checkouts {
		if info.HeldDuration() > ld.config.Threshold {
			checkouts = append(checkouts, struct {
				id   string
				info *CheckoutInfo
			}{id, info})
		}
	}
	ld.mu.RUnlock()
	
	// Log warnings outside of lock
	for _, checkout := range checkouts {
		Logger.Printf("POSSIBLE CONNECTION LEAK: conn=%s held=%v goroutine=%d metadata=%v",
			checkout.id, checkout.info.HeldDuration(), checkout.info.GoroutineID, checkout.info.Metadata)
		
		if checkout.info.StackTrace != "" {
			Logger.Printf("Stack trace for connection %s:\n%s", checkout.id, checkout.info.StackTrace)
		}
	}
}

// Stop stops the leak detector
func (ld *LeakDetector) Stop() {
	close(ld.stopCh)
}

// LeakDetectionGuard provides RAII-style leak detection
type LeakDetectionGuard struct {
	connID   string
	detector *LeakDetector
	checked  bool
}

// NewLeakDetectionGuard creates a new leak detection guard
func NewLeakDetectionGuard(connID string, detector *LeakDetector, metadata map[string]string) *LeakDetectionGuard {
	detector.Checkout(connID, metadata)
	return &LeakDetectionGuard{
		connID:   connID,
		detector: detector,
		checked:  false,
	}
}

// Release marks the connection as returned
func (g *LeakDetectionGuard) Release() {
	if !g.checked {
		g.detector.Checkin(g.connID)
		g.checked = true
	}
}

// Helper to get current goroutine ID (implementation-specific)
func getGoroutineID() int64 {
	// This is a simplified version - in production use a proper implementation
	// or remove if not needed
	return 0
}

// captureStackTrace captures the current stack trace
func captureStackTrace() string {
	buf := make([]byte, 4096)
	n := runtime.Stack(buf, false)
	return string(buf[:n])
}

// LeakStats contains statistics from the leak detector
type LeakStats struct {
	ActiveCheckouts  int
	PotentialLeaks   int
}

// Stats returns current leak statistics
func (ld *LeakDetector) Stats() LeakStats {
	checkouts := ld.ActiveCheckouts()
	
	potentialLeaks := 0
	for _, info := range checkouts {
		if info.HeldDuration() > ld.config.Threshold {
			potentialLeaks++
		}
	}
	
	return LeakStats{
		ActiveCheckouts: len(checkouts),
		PotentialLeaks:  potentialLeaks,
	}
}

// Logger interface for leak detection logging
type LeakLogger interface {
	Printf(format string, v ...interface{})
}

// Default logger
var Logger LeakLogger = &defaultLogger{}

type defaultLogger struct{}

func (l *defaultLogger) Printf(format string, v ...interface{}) {
	fmt.Printf("[ScratchBird LeakDetector] "+format+"\n", v...)
}

// SetLogger sets the logger for leak detection
func SetLogger(l LeakLogger) {
	Logger = l
}
