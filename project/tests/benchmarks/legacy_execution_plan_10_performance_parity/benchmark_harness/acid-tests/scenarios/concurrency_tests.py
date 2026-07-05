#!/usr/bin/env python3
"""
Concurrency & Locking Tests

Tests for:
- Row-level locking (FOR UPDATE, FOR SHARE)
- Deadlock detection and handling
- Lock timeouts
- Concurrent read/write patterns
- Connection pooling behavior

Critical for ScratchBird to match native engine locking behavior.
"""

from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass
class ConcurrencyTest:
    """Definition of a concurrency/locking test."""
    name: str
    description: str
    category: str  # locking, deadlock, timeout, contention
    threads: int  # Number of concurrent connections needed
    duration_seconds: int
    setup_sql: str = ""
    workload_sql: str = ""
    verification_sql: str = ""
    expected_metric: str = ""  # What to measure
    expected_min_value: float = 0
    expected_max_value: float = float('inf')


class LockingTests:
    """Row-level and table-level locking tests."""

    @staticmethod
    def get_all_tests() -> List[ConcurrencyTest]:
        return [
            ConcurrencyTest(
                name="select_for_update_locking",
                description="SELECT FOR UPDATE acquires exclusive row locks",
                category="locking",
                threads=2,
                duration_seconds=10,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (
                        id INT PRIMARY KEY,
                        value INT,
                        version INT DEFAULT 0
                    );
                    DELETE FROM lock_test;
                    INSERT INTO lock_test (id, value) VALUES (1, 100);
                    INSERT INTO lock_test (id, value) VALUES (2, 200);
                    INSERT INTO lock_test (id, value) VALUES (3, 300);
                """,
                workload_sql="""
                    -- Thread 1: Lock row 1
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE;
                    UPDATE lock_test SET value = value + 1, version = version + 1 WHERE id = 1;
                    SELECT SLEEP(0.5);
                    COMMIT;

                    -- Thread 2: Try to lock same row (should wait)
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE;
                    UPDATE lock_test SET value = value + 10 WHERE id = 1;
                    COMMIT;
                """,
                verification_sql="SELECT value, version FROM lock_test WHERE id = 1",
                expected_metric="version",
                expected_min_value=2,  # Both updates should complete
            ),
            ConcurrencyTest(
                name="select_for_share_locking",
                description="SELECT FOR SHARE allows concurrent reads but blocks writes",
                category="locking",
                threads=3,
                duration_seconds=10,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (id INT PRIMARY KEY, value INT);
                    DELETE FROM lock_test;
                    INSERT INTO lock_test VALUES (1, 100);
                """,
                workload_sql="""
                    -- Thread 1: Shared lock
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR SHARE;
                    SELECT SLEEP(1);
                    COMMIT;

                    -- Thread 2: Also shared lock (should succeed)
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR SHARE;
                    COMMIT;

                    -- Thread 3: Try exclusive lock (should wait)
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE;
                    UPDATE lock_test SET value = value + 1 WHERE id = 1;
                    COMMIT;
                """,
                verification_sql="SELECT value FROM lock_test WHERE id = 1",
                expected_metric="value",
                expected_min_value=101,
            ),
            ConcurrencyTest(
                name="nowait_locking",
                description="NOWAIT option fails immediately if lock unavailable",
                category="locking",
                threads=2,
                duration_seconds=5,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (id INT PRIMARY KEY, value INT);
                    DELETE FROM lock_test;
                    INSERT INTO lock_test VALUES (1, 100);
                """,
                workload_sql="""
                    -- Thread 1: Hold lock
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE;
                    SELECT SLEEP(2);
                    COMMIT;

                    -- Thread 2: Try NOWAIT (should fail immediately)
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE NOWAIT;
                    -- Should not reach here
                    COMMIT;
                """,
                verification_sql="SELECT 1",  # Error should occur
                expected_metric="error_occurred",
                expected_min_value=1,
            ),
            ConcurrencyTest(
                name="skip_locked",
                description="SKIP LOCKED skips locked rows",
                category="locking",
                threads=2,
                duration_seconds=10,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (id INT PRIMARY KEY, value INT, processed INT DEFAULT 0);
                    DELETE FROM lock_test;
                    INSERT INTO lock_test VALUES (1, 100, 0);
                    INSERT INTO lock_test VALUES (2, 200, 0);
                    INSERT INTO lock_test VALUES (3, 300, 0);
                """,
                workload_sql="""
                    -- Thread 1: Lock first row
                    BEGIN;
                    SELECT * FROM lock_test WHERE processed = 0 ORDER BY id FOR UPDATE LIMIT 1;
                    UPDATE lock_test SET processed = 1 WHERE id = 1;
                    SELECT SLEEP(1);
                    COMMIT;

                    -- Thread 2: Skip locked, process others
                    BEGIN;
                    SELECT * FROM lock_test WHERE processed = 0 ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 2;
                    UPDATE lock_test SET processed = 2 WHERE id IN (2, 3);
                    COMMIT;
                """,
                verification_sql="SELECT COUNT(*) FROM lock_test WHERE processed > 0",
                expected_metric="count",
                expected_min_value=3,  # All rows should be processed
            ),
            ConcurrencyTest(
                name="concurrent_reads_no_blocking",
                description="Concurrent SELECTs don't block each other",
                category="locking",
                threads=10,
                duration_seconds=10,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (id INT PRIMARY KEY, data TEXT);
                    DELETE FROM lock_test;
                    INSERT INTO lock_test VALUES (1, REPEAT('x', 1000));
                """,
                workload_sql="""
                    -- All threads: Concurrent reads
                    SELECT * FROM lock_test WHERE id = 1;
                """,
                verification_sql="SELECT 1",
                expected_metric="throughput",
                expected_min_value=1000,  # Should handle 1000 reads/sec
            ),
        ]


class DeadlockTests:
    """Deadlock detection and resolution tests."""

    @staticmethod
    def get_all_tests() -> List[ConcurrencyTest]:
        return [
            ConcurrencyTest(
                name="deadlock_two_resources",
                description="Classic deadlock on two resources is detected",
                category="deadlock",
                threads=2,
                duration_seconds=10,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS account (
                        account_id INT PRIMARY KEY,
                        balance DECIMAL(12,2)
                    );
                    DELETE FROM account;
                    INSERT INTO account VALUES (1, 1000.00);
                    INSERT INTO account VALUES (2, 1000.00);
                """,
                workload_sql="""
                    -- Thread 1: Lock A, then try B
                    BEGIN;
                    UPDATE account SET balance = balance - 100 WHERE account_id = 1;
                    SELECT SLEEP(0.1);
                    UPDATE account SET balance = balance + 100 WHERE account_id = 2;
                    COMMIT;

                    -- Thread 2: Lock B, then try A (deadlock)
                    BEGIN;
                    UPDATE account SET balance = balance - 50 WHERE account_id = 2;
                    SELECT SLEEP(0.1);
                    UPDATE account SET balance = balance + 50 WHERE account_id = 1;
                    COMMIT;
                """,
                verification_sql="SELECT SUM(balance) FROM account",
                expected_metric="sum",
                expected_min_value=2000.00,  # Total should be preserved
                expected_max_value=2000.00,
            ),
            ConcurrencyTest(
                name="deadlock_timeout_resolution",
                description="Lock timeout prevents indefinite waits",
                category="deadlock",
                threads=2,
                duration_seconds=15,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS lock_test (id INT PRIMARY KEY, value INT);
                    DELETE FROM lock_test;
                    INSERT INTO lock_test VALUES (1, 100);
                    INSERT INTO lock_test VALUES (2, 200);
                """,
                workload_sql="""
                    -- Set lock timeout (engine-specific)
                    SET LOCK_TIMEOUT = 2000; -- 2 seconds

                    -- Thread 1: Lock row 1, wait, lock row 2
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE;
                    SELECT SLEEP(3);
                    SELECT * FROM lock_test WHERE id = 2 FOR UPDATE;
                    COMMIT;

                    -- Thread 2: Lock row 2 immediately, then try row 1
                    BEGIN;
                    SELECT * FROM lock_test WHERE id = 2 FOR UPDATE;
                    SELECT * FROM lock_test WHERE id = 1 FOR UPDATE; -- Should timeout
                    COMMIT;
                """,
                verification_sql="SELECT 1",
                expected_metric="timeout_occurred",
                expected_min_value=1,
            ),
        ]


class ContentionTests:
    """High-contention workload tests."""

    @staticmethod
    def get_all_tests() -> List[ConcurrencyTest]:
        return [
            ConcurrencyTest(
                name="contention_hot_row",
                description="Many threads updating same row (hot spot)",
                category="contention",
                threads=20,
                duration_seconds=30,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS counter (
                        name VARCHAR(20) PRIMARY KEY,
                        value BIGINT DEFAULT 0
                    );
                    DELETE FROM counter;
                    INSERT INTO counter VALUES ('hot_counter', 0);
                """,
                workload_sql="""
                    BEGIN;
                    UPDATE counter SET value = value + 1 WHERE name = 'hot_counter';
                    COMMIT;
                """,
                verification_sql="SELECT value FROM counter WHERE name = 'hot_counter'",
                expected_metric="value",
                expected_min_value=100,  # Should complete many updates
            ),
            ConcurrencyTest(
                name="contention_insert_burst",
                description="High-rate concurrent inserts",
                category="contention",
                threads=10,
                duration_seconds=30,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS insert_test (
                        id INT PRIMARY KEY,
                        thread_id INT,
                        sequence_num INT,
                        ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                    );
                    DELETE FROM insert_test;
                """,
                workload_sql="""
                    -- Each thread inserts 1000 rows
                    DECLARE @i INT = 0;
                    WHILE @i < 1000
                    BEGIN
                        INSERT INTO insert_test (id, thread_id, sequence_num)
                        VALUES (thread_id * 10000 + @i, thread_id, @i);
                        SET @i = @i + 1;
                    END;
                """,
                verification_sql="SELECT COUNT(*) FROM insert_test",
                expected_metric="count",
                expected_min_value=10000,  # 10 threads * 1000 rows
            ),
            ConcurrencyTest(
                name="contention_mixed_workload",
                description="Mixed read/write workload simulation",
                category="contention",
                threads=10,
                duration_seconds=60,
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS orders (
                        order_id INT PRIMARY KEY,
                        customer_id INT,
                        status VARCHAR(20),
                        amount DECIMAL(10,2),
                        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                    );
                    DELETE FROM orders;
                    -- Seed with initial data
                    INSERT INTO orders (order_id, customer_id, status, amount)
                    SELECT seq, (seq % 100), 'pending', (seq * 10.50)
                    FROM generate_series(1, 10000) seq;
                """,
                workload_sql="""
                    -- 70% reads, 30% writes
                    IF RAND() < 0.7
                        -- Read
                        SELECT * FROM orders WHERE customer_id = (RAND() * 100);
                    ELSE IF RAND() < 0.5
                        -- Update status
                        UPDATE orders SET status = 'processing'
                        WHERE order_id = (RAND() * 10000) AND status = 'pending';
                    ELSE
                        -- Insert new
                        INSERT INTO orders (order_id, customer_id, status, amount)
                        VALUES (NEXTVAL, (RAND() * 100), 'pending', (RAND() * 1000));
                """,
                verification_sql="SELECT COUNT(*) FROM orders",
                expected_metric="throughput",
                expected_min_value=100,  # Should sustain 100 ops/sec
            ),
        ]


class ConnectionTests:
    """Connection pooling and management tests."""

    @staticmethod
    def get_all_tests() -> List[ConcurrencyTest]:
        return [
            ConcurrencyTest(
                name="connection_pool_exhaustion",
                description="Graceful handling of max connections",
                category="connection",
                threads=100,  # Try to open more connections than allowed
                duration_seconds=30,
                setup_sql="CREATE TABLE IF NOT EXISTS conn_test (id INT);",
                workload_sql="SELECT 1;",
                verification_sql="SELECT 1",
                expected_metric="successful_connections",
                expected_min_value=50,  # At least 50 should connect
            ),
            ConcurrencyTest(
                name="connection_leak_detection",
                description="Unclosed connections are cleaned up",
                category="connection",
                threads=10,
                duration_seconds=60,
                setup_sql="CREATE TABLE IF NOT EXISTS conn_test (id INT);",
                workload_sql="""
                    -- Open connection, query, don't close (simulating leak)
                    -- Engine should clean up idle connections
                """,
                verification_sql="SELECT COUNT(*) FROM pg_stat_activity WHERE state = 'idle'",
                expected_metric="idle_connections",
                expected_max_value=20,  # Should not accumulate idle connections
            ),
        ]


def get_all_tests() -> Dict[str, List[ConcurrencyTest]]:
    """Get all concurrency tests organized by category."""
    return {
        'locking': LockingTests.get_all_tests(),
        'deadlock': DeadlockTests.get_all_tests(),
        'contention': ContentionTests.get_all_tests(),
        'connection': ConnectionTests.get_all_tests(),
    }


if __name__ == '__main__':
    tests = get_all_tests()
    total = sum(len(t) for t in tests.values())

    print(f"Total Concurrency Tests: {total}")
    print()

    for category, cat_tests in tests.items():
        print(f"\n{category.upper()}: {len(cat_tests)} tests")
        for t in cat_tests:
            print(f"  - {t.name}: {t.threads} threads, {t.duration_seconds}s")
