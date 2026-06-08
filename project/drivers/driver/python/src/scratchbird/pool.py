# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Connection pooling and prepared statement caching for ScratchBird Python driver."""

from __future__ import annotations

import hashlib
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

from . import errors
from .leak_detection import LeakDetector, LeakDetectionConfig
from .connection import Connection, ConnectionConfig, connect


@dataclass
class PoolConfig:
    """Configuration for connection pool."""
    max_connections: int = 10
    min_connections: int = 1
    max_lifetime: float = 3600.0  # seconds
    idle_timeout: float = 600.0  # seconds
    acquire_timeout: float = 30.0  # seconds
    test_on_checkout: bool = True
    maintenance_interval: float = 30.0  # seconds


@dataclass
class PooledConnection:
    """A pooled connection with metadata."""
    connection: Connection
    created_at: float = field(default_factory=time.time)
    last_used_at: float = field(default_factory=time.time)
    use_count: int = 0
    leak_guard: Optional[object] = None


class ConnectionPool:
    """A thread-safe connection pool for ScratchBird connections."""

    def __init__(self, config: ConnectionConfig, pool_config: Optional[PoolConfig] = None):
        self._config = config
        self._pool_config = pool_config or PoolConfig()
        self._available: List[PooledConnection] = []
        self._in_use: Set[int] = set()
        self._lock = threading.RLock()
        self._semaphore = threading.Semaphore(self._pool_config.max_connections)
        self._closed = False
        self._maintenance_thread: Optional[threading.Thread] = None
        self._stop_maintenance = threading.Event()
        self._leak_detector = LeakDetector(LeakDetectionConfig())
        self._leak_detector.start()

        # Initialize minimum connections
        self._initialize_pool()
        
        # Start maintenance thread
        self._start_maintenance()

    def _initialize_pool(self) -> None:
        """Create initial connections."""
        for _ in range(self._pool_config.min_connections):
            try:
                conn = connect(
                    host=self._config.host,
                    port=self._config.port,
                    database=self._config.database,
                    user=self._config.user,
                    password=self._config.password,
                )
                self._available.append(PooledConnection(connection=conn))
            except Exception as e:
                raise errors.OperationalError(f"Failed to initialize pool connection: {e}")

    def _start_maintenance(self) -> None:
        """Start the maintenance thread."""
        def maintenance_loop():
            while not self._stop_maintenance.wait(self._pool_config.maintenance_interval):
                self._do_maintenance()
        
        self._maintenance_thread = threading.Thread(target=maintenance_loop, daemon=True)
        self._maintenance_thread.start()

    def _do_maintenance(self) -> None:
        """Remove expired connections and ensure minimum pool size."""
        with self._lock:
            now = time.time()
            valid_connections = []
            
            for pc in self._available:
                age = now - pc.created_at
                idle_time = now - pc.last_used_at
                
                if age > self._pool_config.max_lifetime or idle_time > self._pool_config.idle_timeout:
                    try:
                        pc.connection.close()
                    except Exception:
                        pass
                else:
                    valid_connections.append(pc)
            
            self._available = valid_connections
            
            # Ensure minimum connections
            current_count = len(self._available) + len(self._in_use)
            needed = self._pool_config.min_connections - current_count
            
            for _ in range(needed):
                try:
                    conn = connect(
                        host=self._config.host,
                        port=self._config.port,
                        database=self._config.database,
                        user=self._config.user,
                        password=self._config.password,
                    )
                    self._available.append(PooledConnection(connection=conn))
                except Exception:
                    break

    def acquire(self, timeout: Optional[float] = None) -> "PooledConnectionContext":
        """Acquire a connection from the pool."""
        if self._closed:
            raise errors.InterfaceError("Pool is closed")
        
        timeout = timeout or self._pool_config.acquire_timeout
        
        if not self._semaphore.acquire(timeout=timeout):
            raise errors.OperationalError("Timeout acquiring connection from pool")
        
        with self._lock:
            # Try to find a valid connection
            while self._available:
                pc = self._available.pop(0)
                conn_id = id(pc.connection)
                
                # Check if connection is still valid
                if self._pool_config.test_on_checkout:
                    try:
                        pc.connection.ping()
                    except Exception:
                        try:
                            pc.connection.close()
                        except Exception:
                            pass
                        continue
                
                pc.last_used_at = time.time()
                pc.use_count += 1
                self._in_use.add(conn_id)
                pc.leak_guard = self._leak_detector.checkout(
                    str(conn_id),
                    {"driver": "python", "pool": "default"},
                )
                return PooledConnectionContext(self, pc)
            
            # No available connections, create new one if under limit
            current_total = len(self._available) + len(self._in_use)
            if current_total < self._pool_config.max_connections:
                try:
                    conn = connect(
                        host=self._config.host,
                        port=self._config.port,
                        database=self._config.database,
                        user=self._config.user,
                        password=self._config.password,
                    )
                    pc = PooledConnection(connection=conn)
                    pc.last_used_at = time.time()
                    pc.use_count = 1
                    self._in_use.add(id(conn))
                    pc.leak_guard = self._leak_detector.checkout(
                        str(id(conn)),
                        {"driver": "python", "pool": "default"},
                    )
                    return PooledConnectionContext(self, pc)
                except Exception as e:
                    self._semaphore.release()
                    raise errors.OperationalError(f"Failed to create new connection: {e}")
            
            self._semaphore.release()
            raise errors.OperationalError("Pool exhausted")

    def _return_connection(self, pc: PooledConnection) -> None:
        """Return a connection to the pool."""
        conn_id = id(pc.connection)
        
        with self._lock:
            if conn_id in self._in_use:
                self._in_use.remove(conn_id)
            
            if not self._closed:
                pc.last_used_at = time.time()
                self._available.append(pc)
        
        if pc.leak_guard is not None:
            try:
                pc.leak_guard.release()
            except Exception:
                pass
            pc.leak_guard = None
        
        self._semaphore.release()

    def stats(self) -> Dict[str, int]:
        """Get pool statistics."""
        with self._lock:
            return {
                "available": len(self._available),
                "in_use": len(self._in_use),
                "total_capacity": self._pool_config.max_connections,
            }

    def close(self) -> None:
        """Close all connections in the pool."""
        self._closed = True
        self._stop_maintenance.set()
        
        if self._maintenance_thread:
            self._maintenance_thread.join(timeout=5.0)
        
        with self._lock:
            for pc in self._available:
                try:
                    pc.connection.close()
                except Exception:
                    pass
            self._available.clear()
            self._in_use.clear()
        self._leak_detector.stop()

    def __enter__(self) -> ConnectionPool:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()


class PooledConnectionContext:
    """Context manager for pooled connections."""
    
    def __init__(self, pool: ConnectionPool, pooled_conn: PooledConnection):
        self._pool = pool
        self._pooled_conn = pooled_conn
        self._connection = pooled_conn.connection

    @property
    def connection(self) -> Connection:
        return self._connection

    def __enter__(self) -> Connection:
        return self._connection

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self._pool._return_connection(self._pooled_conn)


@dataclass
class CachedStatement:
    """A cached prepared statement."""
    sql: str
    statement_name: str
    param_types: List[int]
    created_at: float = field(default_factory=time.time)
    use_count: int = 0
    last_used_at: float = field(default_factory=time.time)


class StatementCache:
    """A prepared statement cache for ScratchBird connections."""

    def __init__(self, max_size: int = 100, ttl: float = 3600.0):
        self._max_size = max_size
        self._ttl = ttl
        self._cache: OrderedDict[str, CachedStatement] = OrderedDict()
        self._lock = threading.RLock()
        self._counter = 0

    def _make_key(self, sql: str, param_types: Tuple[int, ...]) -> str:
        """Create a cache key from SQL and parameter types."""
        key_data = f"{sql}:{','.join(map(str, param_types))}"
        return hashlib.sha256(key_data.encode()).hexdigest()[:32]

    def _make_name(self) -> str:
        """Generate a unique statement name."""
        self._counter += 1
        return f"__cached_{self._counter}"

    def get(self, sql: str, param_types: List[int]) -> Optional[CachedStatement]:
        """Get a cached statement if available."""
        key = self._make_key(sql, tuple(param_types))
        
        with self._lock:
            if key in self._cache:
                stmt = self._cache[key]
                now = time.time()
                
                # Check if expired
                if now - stmt.created_at > self._ttl:
                    del self._cache[key]
                    return None
                
                # Move to end (most recently used)
                self._cache.move_to_end(key)
                stmt.last_used_at = now
                stmt.use_count += 1
                return stmt
            
            return None

    def put(self, sql: str, param_types: List[int]) -> CachedStatement:
        """Add a statement to the cache."""
        key = self._make_key(sql, tuple(param_types))
        
        with self._lock:
            # Evict oldest if at capacity
            while len(self._cache) >= self._max_size:
                self._cache.popitem(last=False)
            
            stmt = CachedStatement(
                sql=sql,
                statement_name=self._make_name(),
                param_types=param_types,
            )
            self._cache[key] = stmt
            return stmt

    def invalidate(self, sql: str, param_types: List[int]) -> bool:
        """Remove a statement from the cache."""
        key = self._make_key(sql, tuple(param_types))
        
        with self._lock:
            if key in self._cache:
                del self._cache[key]
                return True
            return False

    def clear(self) -> None:
        """Clear all cached statements."""
        with self._lock:
            self._cache.clear()

    def stats(self) -> Dict[str, Any]:
        """Get cache statistics."""
        with self._lock:
            now = time.time()
            total_age = sum(now - stmt.created_at for stmt in self._cache.values())
            total_uses = sum(stmt.use_count for stmt in self._cache.values())
            
            return {
                "size": len(self._cache),
                "max_size": self._max_size,
                "hit_rate": "N/A",  # Would need hit/miss tracking
                "avg_age_seconds": total_age / len(self._cache) if self._cache else 0,
                "total_uses": total_uses,
            }


class CachingConnection:
    """A connection wrapper with statement caching."""
    
    def __init__(self, connection: Connection, cache: Optional[StatementCache] = None):
        self._connection = connection
        self._cache = cache or StatementCache()
        self._prepared: Dict[str, Any] = {}  # statement_name -> metadata

    @property
    def connection(self) -> Connection:
        return self._connection

    def execute(self, sql: str, params: Optional[List] = None) -> Any:
        """Execute with statement caching."""
        from .types import encode_param
        
        # Get parameter types
        param_types = []
        if params:
            for param in params:
                _, oid = encode_param(param)
                param_types.append(oid)
        
        # Try to get cached statement
        stmt = self._cache.get(sql, param_types)
        
        if stmt:
            # Use cached statement
            return self._execute_cached(stmt, params)
        else:
            # Create new cached statement
            stmt = self._cache.put(sql, param_types)
            return self._execute_cached(stmt, params)

    def _execute_cached(self, stmt: CachedStatement, params: Optional[List]) -> Any:
        """Execute using a cached statement."""
        # This would integrate with the connection's execute logic
        # For now, delegate to the underlying connection
        return self._connection.execute(sql=stmt.sql, params=params)

    def clear_cache(self) -> None:
        """Clear the statement cache."""
        self._cache.clear()

    def cache_stats(self) -> Dict[str, Any]:
        """Get cache statistics."""
        return self._cache.stats()


def retry_with_backoff(
    max_retries: int = 3,
    base_delay: float = 0.1,
    max_delay: float = 5.0,
    retryable_errors: Optional[Set[str]] = None,
) -> Callable:
    """Decorator for retry logic with exponential backoff.

    This helper only governs caller-approved retries from a fresh boundary. It
    does not make in-flight transactions resumable and must not be treated as a
    WAL-style replay mechanism.
    """
    
    if retryable_errors is None:
        retryable_errors = {"OperationalError", "Connection Error"}
    
    def decorator(func: Callable) -> Callable:
        def wrapper(*args, **kwargs):
            delay = base_delay
            last_exception = None
            
            for attempt in range(max_retries + 1):
                try:
                    return func(*args, **kwargs)
                except Exception as e:
                    last_exception = e
                    error_type = type(e).__name__
                    
                    if attempt < max_retries and error_type in retryable_errors:
                        time.sleep(delay)
                        delay = min(delay * 2, max_delay)
                    else:
                        raise
            
            raise last_exception
        
        return wrapper
    
    return decorator
