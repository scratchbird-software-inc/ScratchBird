# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pytest

from scratchbird import errors
from scratchbird.circuit_breaker import CircuitBreaker, CircuitBreakerConfig, CircuitBreakerError, CircuitState
from scratchbird.connection import ConnectionConfig
from scratchbird.keepalive import KeepaliveConfig, KeepaliveManager
from scratchbird.leak_detection import LeakDetectionConfig, LeakDetector
from scratchbird.pool import CachingConnection, ConnectionPool, PoolConfig, StatementCache, retry_with_backoff
from scratchbird.telemetry import TelemetryCollector, TelemetryConfig
import scratchbird.circuit_breaker as circuit_breaker_module
import scratchbird.keepalive as keepalive_module
import scratchbird.leak_detection as leak_detection_module
import scratchbird.pool as pool_module
import scratchbird.telemetry as telemetry_module


class _FakeLeakGuard:
    def __init__(self) -> None:
        self.released = False

    def release(self) -> None:
        self.released = True


class _FakeLeakDetector:
    def __init__(self, _config) -> None:
        self.start_called = False
        self.stop_called = False
        self.checkout_calls = []

    def start(self) -> None:
        self.start_called = True

    def stop(self) -> None:
        self.stop_called = True

    def checkout(self, conn_id: str, metadata):
        self.checkout_calls.append((conn_id, dict(metadata)))
        return _FakeLeakGuard()


class _FakeConnection:
    def __init__(self, label: str, fail_ping: bool = False) -> None:
        self.label = label
        self.fail_ping = fail_ping
        self.closed = False
        self.ping_calls = 0
        self.exec_calls = []

    def ping(self) -> bool:
        self.ping_calls += 1
        if self.fail_ping:
            raise RuntimeError("stale connection")
        return True

    def close(self) -> None:
        self.closed = True

    def execute(self, sql=None, params=None):
        self.exec_calls.append((sql, params))
        return {"sql": sql, "params": params}


def _connection_config() -> ConnectionConfig:
    return ConnectionConfig(database="db_main", user="driver_user", password="secret")


def test_circuit_breaker_transitions_open_half_open_closed(monkeypatch):
    now = [100.0]
    monkeypatch.setattr(circuit_breaker_module.time, "monotonic", lambda: now[0])
    breaker = CircuitBreaker(
        CircuitBreakerConfig(
            failure_threshold=2,
            recovery_timeout=5.0,
            success_threshold=2,
            half_open_max_requests=2,
        ),
        name="python-unit",
    )

    breaker.record_failure()
    assert breaker.state == CircuitState.CLOSED
    breaker.record_failure()
    assert breaker.state == CircuitState.OPEN
    assert breaker.allow_request() is False

    now[0] = 106.0
    assert breaker.allow_request() is True
    assert breaker.state == CircuitState.HALF_OPEN
    assert breaker.allow_request() is True
    assert breaker.allow_request() is False

    breaker.record_success()
    assert breaker.state == CircuitState.HALF_OPEN
    breaker.record_success()
    assert breaker.state == CircuitState.CLOSED


def test_circuit_breaker_raises_open_error(monkeypatch):
    now = [50.0]
    monkeypatch.setattr(circuit_breaker_module.time, "monotonic", lambda: now[0])
    breaker = CircuitBreaker(CircuitBreakerConfig(failure_threshold=1, recovery_timeout=60.0), name="python-open")
    breaker.record_failure()

    with pytest.raises(CircuitBreakerError, match="is OPEN"):
        breaker.call(lambda: 1)


def test_keepalive_manager_refreshes_healthy_trackers_and_leaves_stale_ones(monkeypatch):
    now = [0.0]
    monkeypatch.setattr(keepalive_module.time, "monotonic", lambda: now[0])
    manager = KeepaliveManager(KeepaliveConfig(interval=999.0, max_idle_before_check=5.0, validation_timeout=0.02))
    healthy_calls = []
    stale_calls = []

    healthy_tracker = manager.register("healthy", lambda: healthy_calls.append("ok") or True)
    stale_tracker = manager.register("stale", lambda: stale_calls.append("fail") or False)

    now[0] = 6.0
    manager._check_connections()
    assert len(healthy_calls) == 1
    assert len(stale_calls) == 1
    assert healthy_tracker.needs_validation() is False
    assert stale_tracker.needs_validation() is True

    manager.unregister("healthy")
    now[0] = 12.0
    manager._check_connections()
    assert len(healthy_calls) == 1
    assert len(stale_calls) == 2


def test_leak_detector_stats_and_guard_release(monkeypatch):
    now = [10.0]
    monkeypatch.setattr(leak_detection_module.time, "monotonic", lambda: now[0])
    detector = LeakDetector(LeakDetectionConfig(threshold=1.0, capture_stack_trace=True, check_interval=100.0))
    guard = detector.checkout("conn-1", {"lane": "python"})

    assert detector.active_count() == 1
    info = detector.active_checkouts()["conn-1"]
    assert info.stack_trace

    now[0] = 12.0
    assert detector.stats()["potential_leaks"] == 1

    guard.release()
    guard.release()
    assert detector.active_count() == 0


def test_connection_pool_reuses_connections_and_enforces_closed_state(monkeypatch):
    created = []

    def fake_connect(**_kwargs):
        conn = _FakeConnection(f"conn-{len(created) + 1}")
        created.append(conn)
        return conn

    monkeypatch.setattr(pool_module, "connect", fake_connect)
    monkeypatch.setattr(pool_module, "LeakDetector", _FakeLeakDetector)

    pool = ConnectionPool(
        _connection_config(),
        PoolConfig(
            max_connections=2,
            min_connections=1,
            max_lifetime=3600.0,
            idle_timeout=3600.0,
            maintenance_interval=3600.0,
        ),
    )
    try:
        with pool.acquire() as conn_one:
            assert conn_one is created[0]
            assert pool.stats()["in_use"] == 1
            assert pool.stats()["available"] == 0

        with pool.acquire() as conn_two:
            assert conn_two is created[0]

        assert created[0].ping_calls >= 2
        stats = pool.stats()
        assert stats["available"] == 1
        assert stats["in_use"] == 0
    finally:
        pool.close()

    assert created[0].closed is True
    with pytest.raises(errors.InterfaceError, match="Pool is closed"):
        pool.acquire()


def test_connection_pool_replaces_stale_connection_on_checkout(monkeypatch):
    first = _FakeConnection("stale", fail_ping=True)
    second = _FakeConnection("fresh")
    queue = [first, second]

    def fake_connect(**_kwargs):
        return queue.pop(0)

    monkeypatch.setattr(pool_module, "connect", fake_connect)
    monkeypatch.setattr(pool_module, "LeakDetector", _FakeLeakDetector)

    pool = ConnectionPool(
        _connection_config(),
        PoolConfig(
            max_connections=2,
            min_connections=1,
            max_lifetime=3600.0,
            idle_timeout=3600.0,
            maintenance_interval=3600.0,
        ),
    )
    try:
        with pool.acquire() as conn:
            assert conn is second
        assert first.closed is True
    finally:
        pool.close()


def test_statement_cache_and_caching_connection_reuse_entries(monkeypatch):
    cache = StatementCache(max_size=2, ttl=10.0)
    cache.put("select 1", [23])
    stmt_two = cache.put("select 2", [23])
    cache.put("select 3", [23])
    assert cache.get("select 1", [23]) is None

    stmt_two.created_at -= 20.0
    assert cache.get("select 2", [23]) is None

    backing = _FakeConnection("cache")
    wrapper = CachingConnection(backing, cache=StatementCache(max_size=4, ttl=60.0))
    first = wrapper.execute("select ?::int", [1])
    second = wrapper.execute("select ?::int", [2])

    assert first["sql"] == "select ?::int"
    assert second["params"] == [2]
    stats = wrapper.cache_stats()
    assert stats["size"] == 1
    assert stats["total_uses"] == 1


def test_retry_with_backoff_retries_operational_errors(monkeypatch):
    sleep_calls = []
    monkeypatch.setattr(pool_module.time, "sleep", lambda seconds: sleep_calls.append(seconds))
    attempts = {"count": 0}

    @retry_with_backoff(
        max_retries=3,
        base_delay=0.1,
        max_delay=0.2,
        retryable_errors={"OperationalError"},
    )
    def flaky():
        attempts["count"] += 1
        if attempts["count"] < 3:
            raise errors.OperationalError("temporary")
        return "ok"

    assert flaky() == "ok"
    assert sleep_calls == [0.1, 0.2]


def test_retry_with_backoff_does_not_retry_non_retryable_errors(monkeypatch):
    sleep_calls = []
    monkeypatch.setattr(pool_module.time, "sleep", lambda seconds: sleep_calls.append(seconds))

    @retry_with_backoff(max_retries=2, retryable_errors={"OperationalError"})
    def fail_fast():
        raise errors.ProgrammingError("bad sql")

    with pytest.raises(errors.ProgrammingError, match="bad sql"):
        fail_fast()
    assert sleep_calls == []


def test_telemetry_collector_records_metrics_and_slow_queries(monkeypatch):
    now = [1_000.0]
    monkeypatch.setattr(telemetry_module.time, "time", lambda: now[0])
    monkeypatch.setattr(telemetry_module.random, "random", lambda: 0.0)
    collector = TelemetryCollector(TelemetryConfig(slow_query_threshold_ms=50, sample_rate=1.0))
    span = collector.start_span("query")
    assert span is not None

    span.with_attribute("db.statement", TelemetryCollector.sanitize_query("select 'secret'"))
    now[0] = 1_000.08
    collector.end_span(span, success=False)

    metrics = collector.get_metrics()
    assert metrics.total_queries == 1
    assert metrics.successful_queries == 0
    assert metrics.failed_queries == 1
    assert metrics.operation_metrics["query"]["count"] == 1
    assert metrics.operation_metrics["query"]["error_count"] == 1

    slow_queries = collector.get_slow_queries()
    assert len(slow_queries) == 1
    assert slow_queries[0].attributes["db.statement"] == "select '?'"
    assert "scratchbird_queries_total 1" in collector.export_prometheus_metrics()


def test_telemetry_collector_honors_sample_rate(monkeypatch):
    monkeypatch.setattr(telemetry_module.random, "random", lambda: 0.95)
    collector = TelemetryCollector(TelemetryConfig(sample_rate=0.5))
    assert collector.start_span("query") is None
