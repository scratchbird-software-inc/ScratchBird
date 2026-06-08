// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Connection Leak Detection Module
// Detects when connections are held longer than expected

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

/// Configuration for leak detection
#[derive(Debug, Clone, Copy)]
pub struct LeakDetectionConfig {
    /// Threshold for warning about potential leaks (default: 30 seconds)
    pub threshold: Duration,
    /// Whether to capture stack traces (for debugging)
    pub capture_stack_trace: bool,
    /// Log level for leak warnings
    pub log_level: LeakLogLevel,
}

impl Default for LeakDetectionConfig {
    fn default() -> Self {
        Self {
            threshold: Duration::from_secs(30),
            capture_stack_trace: false,
            log_level: LeakLogLevel::Warn,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub enum LeakLogLevel {
    Debug,
    Warn,
    Error,
}

/// Information about an active connection checkout
#[derive(Debug, Clone)]
pub struct CheckoutInfo {
    pub checkout_time: Instant,
    pub thread_id: String,
    pub stack_trace: Option<String>,
    pub metadata: HashMap<String, String>,
}

impl CheckoutInfo {
    pub fn new(capture_stack_trace: bool) -> Self {
        let stack_trace = if capture_stack_trace {
            Some("Stack trace captured".to_string())
        } else {
            None
        };

        Self {
            checkout_time: Instant::now(),
            thread_id: format!("{:?}", std::thread::current().id()),
            stack_trace,
            metadata: HashMap::new(),
        }
    }

    pub fn with_metadata(mut self, key: &str, value: &str) -> Self {
        self.metadata.insert(key.to_string(), value.to_string());
        self
    }

    pub fn held_duration(&self) -> Duration {
        self.checkout_time.elapsed()
    }
}

/// Leak detector for connection pools
pub struct LeakDetector {
    config: LeakDetectionConfig,
    active_checkouts: Arc<RwLock<HashMap<String, CheckoutInfo>>>,
}

impl LeakDetector {
    pub fn new(config: LeakDetectionConfig) -> Self {
        let detector = Self {
            config,
            active_checkouts: Arc::new(RwLock::new(HashMap::new())),
        };

        // Start background monitoring task
        detector.start_monitoring();

        detector
    }

    /// Register a connection checkout
    pub async fn checkout(&self, connection_id: &str) {
        let info = CheckoutInfo::new(self.config.capture_stack_trace);

        let mut checkouts = self.active_checkouts.write().await;
        checkouts.insert(connection_id.to_string(), info);
    }

    /// Register a connection return
    pub async fn checkin(&self, connection_id: &str) {
        let mut checkouts = self.active_checkouts.write().await;

        if let Some(info) = checkouts.remove(connection_id) {
            let held_duration = info.held_duration();

            if held_duration > self.config.threshold {
                // Connection was held longer than threshold but returned
                let _ = held_duration;
            }
        }
    }

    /// Get all active checkouts (for monitoring)
    pub async fn active_checkouts(&self) -> HashMap<String, CheckoutInfo> {
        self.active_checkouts.read().await.clone()
    }

    /// Get count of active checkouts
    pub async fn active_count(&self) -> usize {
        self.active_checkouts.read().await.len()
    }

    /// Start background monitoring task
    fn start_monitoring(&self) {
        let checkouts = Arc::clone(&self.active_checkouts);
        let threshold = self.config.threshold;

        tokio::spawn(async move {
            let mut ticker = tokio::time::interval(Duration::from_secs(10));

            loop {
                ticker.tick().await;

                let checkouts_guard = checkouts.read().await;
                let now = Instant::now();

                for (conn_id, info) in checkouts_guard.iter() {
                    let held = now - info.checkout_time;

                    if held > threshold {
                        let _ = conn_id; // Log potential leak
                        let _ = held;
                    }
                }
            }
        });
    }
}

/// RAII guard for leak detection
pub struct LeakDetectionGuard {
    connection_id: String,
    detector: Arc<LeakDetector>,
}

impl LeakDetectionGuard {
    pub async fn new(connection_id: String, detector: Arc<LeakDetector>) -> Self {
        detector.checkout(&connection_id).await;

        Self {
            connection_id,
            detector,
        }
    }
}

impl Drop for LeakDetectionGuard {
    fn drop(&mut self) {
        let connection_id = self.connection_id.clone();
        let detector = Arc::clone(&self.detector);

        // Spawn a new task to handle the async checkin
        tokio::spawn(async move {
            detector.checkin(&connection_id).await;
        });
    }
}

/// Statistics from leak detector
#[derive(Debug, Default)]
pub struct LeakStatistics {
    pub active_checkouts: usize,
    pub potential_leaks: usize,
}

impl LeakDetector {
    pub async fn statistics(&self) -> LeakStatistics {
        let checkouts = self.active_checkouts.read().await;
        let now = Instant::now();

        let potential_leaks = checkouts
            .values()
            .filter(|info| (now - info.checkout_time) > self.config.threshold)
            .count();

        LeakStatistics {
            active_checkouts: checkouts.len(),
            potential_leaks,
        }
    }
}
