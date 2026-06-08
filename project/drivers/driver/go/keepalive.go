// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"context"
	"sync"
	"time"
)

// KeepaliveConfig configures connection keepalive behavior
type KeepaliveConfig struct {
	// How often to check idle connections (default: 2 minutes)
	Interval time.Duration
	// Maximum time a connection can be idle before validation (default: 10 minutes)
	MaxIdleBeforeCheck time.Duration
	// Timeout for validation query (default: 5 seconds)
	ValidationTimeout time.Duration
}

// DefaultKeepaliveConfig returns default keepalive configuration
func DefaultKeepaliveConfig() KeepaliveConfig {
	return KeepaliveConfig{
		Interval:           2 * time.Minute,
		MaxIdleBeforeCheck: 10 * time.Minute,
		ValidationTimeout:  5 * time.Second,
	}
}

// KeepaliveTracker tracks activity time for connection keepalive
type KeepaliveTracker struct {
	lastActivity sync.RWMutex
	lastTime     time.Time
	config       KeepaliveConfig
}

// NewKeepaliveTracker creates a new keepalive tracker
func NewKeepaliveTracker(config KeepaliveConfig) *KeepaliveTracker {
	return &KeepaliveTracker{
		lastTime: time.Now(),
		config:   config,
	}
}

// MarkActive marks the connection as active
func (k *KeepaliveTracker) MarkActive() {
	k.lastActivity.Lock()
	defer k.lastActivity.Unlock()
	k.lastTime = time.Now()
}

// NeedsValidation checks if connection needs validation
func (k *KeepaliveTracker) NeedsValidation() bool {
	k.lastActivity.RLock()
	defer k.lastActivity.RUnlock()
	return time.Since(k.lastTime) > k.config.MaxIdleBeforeCheck
}

// IdleDuration returns time since last activity
func (k *KeepaliveTracker) IdleDuration() time.Duration {
	k.lastActivity.RLock()
	defer k.lastActivity.RUnlock()
	return time.Since(k.lastTime)
}

// KeepaliveManager manages keepalive for a pool of connections
type KeepaliveManager struct {
	config   KeepaliveConfig
	ctx      context.Context
	cancel   context.CancelFunc
	checkers map[string]*KeepaliveChecker
	mu       sync.RWMutex
}

// KeepaliveChecker checks a single connection
type KeepaliveChecker struct {
	connID  string
	tracker *KeepaliveTracker
	pinger  func() error
}

// NewKeepaliveManager creates a new keepalive manager
func NewKeepaliveManager(config KeepaliveConfig) *KeepaliveManager {
	ctx, cancel := context.WithCancel(context.Background())
	
	km := &KeepaliveManager{
		config:   config,
		ctx:      ctx,
		cancel:   cancel,
		checkers: make(map[string]*KeepaliveChecker),
	}
	
	// Start background check loop
	go km.checkLoop()
	
	return km
}

// Register registers a connection for keepalive monitoring
func (km *KeepaliveManager) Register(connID string, pinger func() error) *KeepaliveTracker {
	tracker := NewKeepaliveTracker(km.config)
	
	checker := &KeepaliveChecker{
		connID:  connID,
		tracker: tracker,
		pinger:  pinger,
	}
	
	km.mu.Lock()
	km.checkers[connID] = checker
	km.mu.Unlock()
	
	return tracker
}

// Unregister removes a connection from keepalive monitoring
func (km *KeepaliveManager) Unregister(connID string) {
	km.mu.Lock()
	delete(km.checkers, connID)
	km.mu.Unlock()
}

// checkLoop runs periodic keepalive checks
func (km *KeepaliveManager) checkLoop() {
	ticker := time.NewTicker(km.config.Interval)
	defer ticker.Stop()
	
	for {
		select {
		case <-km.ctx.Done():
			return
		case <-ticker.C:
			km.checkConnections()
		}
	}
}

// checkConnections validates idle connections
func (km *KeepaliveManager) checkConnections() {
	km.mu.RLock()
	checkers := make([]*KeepaliveChecker, 0, len(km.checkers))
	for _, c := range km.checkers {
		checkers = append(checkers, c)
	}
	km.mu.RUnlock()
	
	for _, checker := range checkers {
		if checker.tracker.NeedsValidation() {
			ctx, cancel := context.WithTimeout(km.ctx, km.config.ValidationTimeout)
			
			// Run ping in goroutine to respect timeout
			done := make(chan error, 1)
			go func() {
				done <- checker.pinger()
			}()
			
			select {
			case err := <-done:
				if err != nil {
					// Connection is dead - mark as needing replacement
					// The pool will handle this on next checkout
				}
				checker.tracker.MarkActive() // Reset timer even on failure
			case <-ctx.Done():
				// Timeout - connection may be slow or dead
			}
			
			cancel()
		}
	}
}

// Stop stops the keepalive manager
func (km *KeepaliveManager) Stop() {
	km.cancel()
}
