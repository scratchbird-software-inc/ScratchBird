// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Keepalive Module for Rust Driver
// Prevents connection timeouts by periodically validating idle connections

use std::time::{Duration, Instant};
use tokio::sync::RwLock;
use tokio::time::interval;

/// Configuration for connection keepalive
#[derive(Debug, Clone, Copy)]
pub struct KeepaliveConfig {
    /// How often to check idle connections (default: 2 minutes)
    pub interval: Duration,
    /// Maximum time a connection can be idle before validation (default: 10 minutes)
    pub max_idle_before_check: Duration,
    /// Timeout for validation query (default: 5 seconds)
    pub validation_timeout: Duration,
}

impl Default for KeepaliveConfig {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(120),              // 2 minutes
            max_idle_before_check: Duration::from_secs(600), // 10 minutes
            validation_timeout: Duration::from_secs(5),
        }
    }
}

/// Tracks last activity time for connection keepalive
#[derive(Debug)]
pub struct KeepaliveTracker {
    last_activity: RwLock<Instant>,
    config: KeepaliveConfig,
}

impl KeepaliveTracker {
    pub fn new(config: KeepaliveConfig) -> Self {
        Self {
            last_activity: RwLock::new(Instant::now()),
            config,
        }
    }

    /// Mark connection as active
    pub async fn mark_active(&self) {
        let mut last = self.last_activity.write().await;
        *last = Instant::now();
    }

    /// Check if connection needs validation
    pub async fn needs_validation(&self) -> bool {
        let last = self.last_activity.read().await;
        last.elapsed() > self.config.max_idle_before_check
    }

    /// Get time since last activity
    pub async fn idle_duration(&self) -> Duration {
        let last = self.last_activity.read().await;
        last.elapsed()
    }
}

/// Background keepalive task for connection pools
pub struct KeepaliveTask {
    config: KeepaliveConfig,
    shutdown_tx: tokio::sync::watch::Sender<bool>,
}

impl KeepaliveTask {
    pub fn new(config: KeepaliveConfig) -> (Self, tokio::sync::watch::Receiver<bool>) {
        let (shutdown_tx, shutdown_rx) = tokio::sync::watch::channel(false);
        (
            Self {
                config,
                shutdown_tx,
            },
            shutdown_rx,
        )
    }

    /// Start the keepalive background task
    pub fn spawn<F, Fut>(&self, mut shutdown_rx: tokio::sync::watch::Receiver<bool>, check_fn: F)
    where
        F: Fn() -> Fut + Send + Sync + 'static,
        Fut: std::future::Future<Output = ()> + Send + 'static,
    {
        let interval_duration = self.config.interval;

        tokio::spawn(async move {
            let mut ticker = interval(interval_duration);

            loop {
                tokio::select! {
                    _ = ticker.tick() => {
                        check_fn().await;
                    }
                    _ = shutdown_rx.changed() => {
                        if *shutdown_rx.borrow() {
                            break;
                        }
                    }
                }
            }
        });
    }

    /// Shutdown the keepalive task
    pub fn shutdown(&self) {
        let _ = self.shutdown_tx.send(true);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_keepalive_tracker() {
        let config = KeepaliveConfig {
            interval: Duration::from_secs(1),
            max_idle_before_check: Duration::from_millis(100),
            validation_timeout: Duration::from_secs(1),
        };

        let tracker = KeepaliveTracker::new(config);

        // Initially should not need validation
        assert!(!tracker.needs_validation().await);

        // Wait for idle threshold
        tokio::time::sleep(Duration::from_millis(150)).await;

        // Now should need validation
        assert!(tracker.needs_validation().await);

        // Mark active
        tracker.mark_active().await;

        // Should not need validation anymore
        assert!(!tracker.needs_validation().await);
    }
}
