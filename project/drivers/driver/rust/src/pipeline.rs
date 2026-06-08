// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Query Pipelining Module
// Allows sending multiple queries without waiting for each response

use std::collections::VecDeque;
use std::sync::Arc;
use tokio::sync::{mpsc, oneshot, Mutex};

/// Configuration for query pipelining
#[derive(Debug, Clone, Copy)]
pub struct PipelineConfig {
    /// Maximum number of in-flight requests (default: 100)
    pub max_in_flight: usize,
    /// Whether to enable auto-flush on batch size (default: true)
    pub auto_flush: bool,
    /// Auto-flush threshold (default: 10)
    pub auto_flush_threshold: usize,
    /// Timeout for flush operations (default: 5 seconds)
    pub flush_timeout: std::time::Duration,
}

impl Default for PipelineConfig {
    fn default() -> Self {
        Self {
            max_in_flight: 100,
            auto_flush: true,
            auto_flush_threshold: 10,
            flush_timeout: std::time::Duration::from_secs(5),
        }
    }
}

/// A pipelined query request (placeholder)
pub struct PipelinedRequest<T> {
    pub data: T,
    pub response_tx: oneshot::Sender<T>,
}

/// Pipeline manager for batching and sending queries
pub struct QueryPipeline<T> {
    config: PipelineConfig,
    queue: Arc<Mutex<VecDeque<PipelinedRequest<T>>>>,
    in_flight: Arc<Mutex<usize>>,
    flush_tx: mpsc::Sender<()>,
}

impl<T: Clone> QueryPipeline<T> {
    pub fn new(config: PipelineConfig) -> (Self, mpsc::Receiver<()>) {
        let (flush_tx, flush_rx) = mpsc::channel(1);
        let queue = Arc::new(Mutex::new(VecDeque::new()));
        let in_flight = Arc::new(Mutex::new(0));

        (
            Self {
                config,
                queue,
                in_flight,
                flush_tx,
            },
            flush_rx,
        )
    }

    /// Queue a request for pipelined execution
    pub async fn queue(&self, data: T) -> oneshot::Receiver<T> {
        let (tx, rx) = oneshot::channel();

        let pipelined_req = PipelinedRequest {
            data,
            response_tx: tx,
        };

        let mut queue = self.queue.lock().await;
        queue.push_back(pipelined_req);

        // Auto-flush if threshold reached
        if self.config.auto_flush && queue.len() >= self.config.auto_flush_threshold {
            drop(queue); // Release lock before flushing
            let _ = self.flush_tx.try_send(());
        }

        rx
    }

    /// Get all pending requests for batch processing
    pub async fn drain_pending(&self) -> Vec<PipelinedRequest<T>> {
        let mut queue = self.queue.lock().await;
        let mut pending = Vec::with_capacity(queue.len());

        while let Some(req) = queue.pop_front() {
            pending.push(req);
        }

        pending
    }

    /// Get number of pending requests
    pub async fn pending_count(&self) -> usize {
        self.queue.lock().await.len()
    }

    /// Check if pipeline has capacity for more requests
    pub async fn has_capacity(&self) -> bool {
        let in_flight = *self.in_flight.lock().await;
        in_flight < self.config.max_in_flight
    }

    /// Increment in-flight counter
    pub async fn increment_in_flight(&self) {
        let mut count = self.in_flight.lock().await;
        *count += 1;
    }

    /// Decrement in-flight counter
    pub async fn decrement_in_flight(&self) {
        let mut count = self.in_flight.lock().await;
        *count = count.saturating_sub(1);
    }

    /// Get current in-flight count
    pub async fn in_flight_count(&self) -> usize {
        *self.in_flight.lock().await
    }

    /// Trigger manual flush
    pub async fn flush(&self) -> Result<(), String> {
        self.flush_tx
            .send(())
            .await
            .map_err(|_| "Flush channel closed".to_string())
    }
}

/// Builder for pipelined queries
pub struct PipelineBuilder<T> {
    requests: Vec<T>,
}

impl<T> PipelineBuilder<T> {
    pub fn new() -> Self {
        Self {
            requests: Vec::new(),
        }
    }

    pub fn add(mut self, request: T) -> Self {
        self.requests.push(request);
        self
    }

    pub fn build(self) -> Vec<T> {
        self.requests
    }
}

impl<T> Default for PipelineBuilder<T> {
    fn default() -> Self {
        Self::new()
    }
}

/// Statistics for pipeline monitoring
#[derive(Debug, Default)]
pub struct PipelineStats {
    pub pending_requests: usize,
    pub in_flight_requests: usize,
    pub total_pipelined: u64,
    pub total_batches: u64,
    pub avg_batch_size: f64,
}
