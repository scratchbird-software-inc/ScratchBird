// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::collections::VecDeque;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use tokio::time;

use crate::leak_detection::{LeakDetectionConfig, LeakDetectionGuard, LeakDetector};
use crate::{Client, Config, Error, ErrorKind, Result};

/// Configuration for connection pool
#[derive(Debug, Clone)]
pub struct PoolConfig {
    /// Maximum number of connections in the pool
    pub max_connections: usize,
    /// Minimum number of connections to maintain
    pub min_connections: usize,
    /// Maximum lifetime of a connection
    pub max_lifetime: Duration,
    /// Maximum idle time before connection is closed
    pub idle_timeout: Duration,
    /// Timeout for acquiring a connection from the pool
    pub acquire_timeout: Duration,
}

impl Default for PoolConfig {
    fn default() -> Self {
        Self {
            max_connections: 10,
            min_connections: 1,
            max_lifetime: Duration::from_secs(3600),
            idle_timeout: Duration::from_secs(600),
            acquire_timeout: Duration::from_secs(30),
        }
    }
}

/// A pooled connection wrapper
pub struct PooledConnection {
    inner: Option<Client>,
    pool: Arc<ConnectionPoolInner>,
    created_at: Instant,
    last_used_at: Instant,
    connection_id: String,
    leak_guard: Option<LeakDetectionGuard>,
}

impl PooledConnection {
    /// Get a reference to the underlying client
    pub fn client(&self) -> &Client {
        self.inner.as_ref().unwrap()
    }

    /// Get a mutable reference to the underlying client
    pub fn client_mut(&mut self) -> &mut Client {
        self.inner.as_mut().unwrap()
    }

    /// Check if the connection is still healthy
    pub async fn is_healthy(&mut self) -> bool {
        if let Some(client) = self.inner.as_mut() {
            client.ping().await.is_ok()
        } else {
            false
        }
    }
}

impl Drop for PooledConnection {
    fn drop(&mut self) {
        // Return connection to pool if it's still valid
        if let Some(client) = self.inner.take() {
            let pool = Arc::clone(&self.pool);
            let created_at = self.created_at;
            let connection_id = self.connection_id.clone();

            tokio::spawn(async move {
                // Return the connection to available pool
                let mut connections = pool.connections.lock().unwrap();
                connections.push_back(PooledConnectionState {
                    client,
                    created_at,
                    last_used_at: Instant::now(),
                    connection_id,
                });
                drop(connections);
                // Increment available count
                pool.available.fetch_add(1, Ordering::SeqCst);
            });
        }
    }
}

/// Internal state for a pooled connection
struct PooledConnectionState {
    client: Client,
    created_at: Instant,
    last_used_at: Instant,
    connection_id: String,
}

/// Inner pool state
struct ConnectionPoolInner {
    #[allow(dead_code)]
    config: Config,
    pool_config: PoolConfig,
    connections: Mutex<VecDeque<PooledConnectionState>>,
    available: AtomicUsize,
    leak_detector: Arc<LeakDetector>,
    next_id: AtomicUsize,
}

/// A connection pool for ScratchBird connections
#[derive(Clone)]
pub struct ConnectionPool {
    inner: Arc<ConnectionPoolInner>,
}

impl ConnectionPool {
    /// Create a new connection pool
    pub async fn new(config: Config, pool_config: PoolConfig) -> Result<Self> {
        let mut connections = VecDeque::new();
        let leak_detector = Arc::new(LeakDetector::new(LeakDetectionConfig::default()));
        let next_id = AtomicUsize::new(0);

        // Create minimum connections
        for _ in 0..pool_config.min_connections {
            let mut client = Client::new(config.clone());
            match client.connect().await {
                Ok(()) => {
                    let connection_id =
                        format!("conn-{}", next_id.fetch_add(1, Ordering::SeqCst) + 1);
                    connections.push_back(PooledConnectionState {
                        client,
                        created_at: Instant::now(),
                        last_used_at: Instant::now(),
                        connection_id,
                    });
                }
                Err(e) => {
                    return Err(Error::new(
                        ErrorKind::Connection,
                        format!("Failed to create initial pool connection: {}", e),
                    ));
                }
            }
        }

        Ok(Self {
            inner: Arc::new(ConnectionPoolInner {
                config: config.clone(),
                pool_config,
                connections: Mutex::new(connections),
                available: AtomicUsize::new(0),
                leak_detector,
                next_id,
            }),
        })
    }

    /// Get a connection from the pool
    pub async fn acquire(&self) -> Result<PooledConnection> {
        let timeout = self.inner.pool_config.acquire_timeout;

        match time::timeout(timeout, self.try_acquire()).await {
            Ok(result) => result,
            Err(_) => Err(Error::new(
                ErrorKind::Connection,
                "Timeout acquiring connection from pool",
            )),
        }
    }

    async fn try_acquire(&self) -> Result<PooledConnection> {
        loop {
            let available = self.inner.available.load(Ordering::SeqCst);
            let in_use = {
                let connections = self.inner.connections.lock().unwrap();
                connections.len()
            };
            let total = available + in_use;

            // If we have available connections, try to get one
            if available > 0 {
                if self.inner.available.fetch_sub(1, Ordering::SeqCst) > 0 {
                    let mut connections = self.inner.connections.lock().unwrap();
                    if let Some(conn) = connections.pop_front() {
                        // Check if connection is still valid
                        let age = Instant::now().duration_since(conn.created_at);
                        let idle_time = Instant::now().duration_since(conn.last_used_at);

                        if age > self.inner.pool_config.max_lifetime
                            || idle_time > self.inner.pool_config.idle_timeout
                        {
                            // Connection expired, close it
                            let mut client = conn.client;
                            drop(connections);
                            client.close().await;
                            continue;
                        }
                        let leak_guard = LeakDetectionGuard::new(
                            conn.connection_id.clone(),
                            Arc::clone(&self.inner.leak_detector),
                        )
                        .await;
                        return Ok(PooledConnection {
                            inner: Some(conn.client),
                            pool: Arc::clone(&self.inner),
                            created_at: conn.created_at,
                            last_used_at: Instant::now(),
                            connection_id: conn.connection_id,
                            leak_guard: Some(leak_guard),
                        });
                    }
                }
                // Failed to get connection, increment back
                self.inner.available.fetch_add(1, Ordering::SeqCst);
            }

            // If under limit, create new connection
            if total < self.inner.pool_config.max_connections {
                let mut client = Client::new(self.inner.config.clone());
                match client.connect().await {
                    Ok(()) => {
                        let connection_id = format!(
                            "conn-{}",
                            self.inner.next_id.fetch_add(1, Ordering::SeqCst) + 1
                        );
                        let leak_guard = LeakDetectionGuard::new(
                            connection_id.clone(),
                            Arc::clone(&self.inner.leak_detector),
                        )
                        .await;
                        return Ok(PooledConnection {
                            inner: Some(client),
                            pool: Arc::clone(&self.inner),
                            created_at: Instant::now(),
                            last_used_at: Instant::now(),
                            connection_id,
                            leak_guard: Some(leak_guard),
                        });
                    }
                    Err(e) => return Err(e),
                }
            }

            // Wait a bit and retry
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    }

    /// Get connection with retry logic
    pub async fn acquire_with_retry(
        &self,
        max_retries: u32,
        base_delay_ms: u64,
    ) -> Result<PooledConnection> {
        let mut retries = 0;
        let mut delay = base_delay_ms;

        loop {
            match self.acquire().await {
                Ok(conn) => return Ok(conn),
                Err(e) => {
                    if retries >= max_retries {
                        return Err(e);
                    }
                    retries += 1;
                    tokio::time::sleep(Duration::from_millis(delay)).await;
                    delay *= 2; // Exponential backoff
                }
            }
        }
    }

    /// Get current pool statistics
    pub fn stats(&self) -> PoolStats {
        let connections = self.inner.connections.lock().unwrap();
        let available = self.inner.available.load(Ordering::SeqCst);
        let in_pool = connections.len();
        let total_capacity = self.inner.pool_config.max_connections;

        PoolStats {
            available_connections: available + in_pool,
            total_capacity,
            in_use: total_capacity.saturating_sub(available + in_pool),
        }
    }

    /// Close all connections in the pool
    pub async fn close(&self) -> Result<()> {
        let mut connections = self.inner.connections.lock().unwrap();
        for conn in connections.drain(..) {
            let mut client = conn.client;
            client.close().await;
        }
        Ok(())
    }
}

/// Pool statistics
#[derive(Debug, Clone)]
pub struct PoolStats {
    pub available_connections: usize,
    pub total_capacity: usize,
    pub in_use: usize,
}

/// Retry configuration for operations
#[derive(Debug, Clone)]
pub struct RetryConfig {
    pub max_retries: u32,
    pub base_delay_ms: u64,
    pub max_delay_ms: u64,
    pub retryable_errors: Vec<ErrorKind>,
}

impl Default for RetryConfig {
    fn default() -> Self {
        Self {
            max_retries: 3,
            base_delay_ms: 100,
            max_delay_ms: 5000,
            retryable_errors: vec![ErrorKind::Connection, ErrorKind::Transaction],
        }
    }
}

/// Execute an operation with retry logic
pub async fn with_retry<T, F, Fut>(config: &RetryConfig, operation: F) -> Result<T>
where
    F: Fn() -> Fut,
    Fut: std::future::Future<Output = Result<T>>,
{
    let mut retries = 0;
    let mut delay = config.base_delay_ms;

    loop {
        match operation().await {
            Ok(result) => return Ok(result),
            Err(e) => {
                let should_retry =
                    retries < config.max_retries && config.retryable_errors.contains(&e.kind);

                if !should_retry {
                    return Err(e);
                }

                retries += 1;
                tokio::time::sleep(Duration::from_millis(delay)).await;
                delay = (delay * 2).min(config.max_delay_ms);
            }
        }
    }
}
