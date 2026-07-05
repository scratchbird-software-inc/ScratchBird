#!/usr/bin/env python3
"""
Firebird Optimized Tests

Tests designed to perform exceptionally well on Firebird due to its
architectural strengths, while performing poorly on MySQL and PostgreSQL.

Firebird Architectural Strengths:
1. Multi-Generational Architecture (MGA) - Readers never block writers, no read locks
2. Record Versions - Delta storage for updates, fast rollback
3. Compact Storage - Efficient page packing, data compression
4. Fast Transactions - Minimal overhead for start/commit
5. No Lock Escalation - Row-level versioning instead of locking
6. Read Committed by Default - No locking for reads
7. Efficient Index Structure - Index compression, selective indexing
8. Garbage Collection - Background cleanup of old versions
9. Shadowing/Replication - Built-in shadow databases
10. Small Memory Footprint - Efficient buffer cache management

Weaknesses to exploit for cross-engine comparison:
- MySQL: Lock-based reads can block, more locking overhead
- PostgreSQL: MVCC requires vacuum, index bloat, snapshot overhead
"""

from dataclasses import dataclass
from typing import List


@dataclass
class FirebirdOptimizedTest:
    name: str
    description: str
    why_firebird_faster: str
    firebird_advantage_factor: str
    setup_sql: str
    test_sql: str
    expected_fb_pattern: str
    expected_mysql_pattern: str
    expected_pg_pattern: str


class FirebirdMGATests:
    """Tests exploiting Firebird's Multi-Generational Architecture."""

    @staticmethod
    def get_all_tests() -> List[FirebirdOptimizedTest]:
        return [
            FirebirdOptimizedTest(
                name="mga_readers_dont_block",
                description="Concurrent readers with heavy writes - no blocking",
                why_firebird_faster="""
Firebird: MGA means readers see consistent snapshot, never wait for writers.
MySQL: MVCC with gap locks, readers may block on write locks.
PostgreSQL: MVCC good but snapshot acquisition has overhead.
                """.strip(),
                firebird_advantage_factor="10-100x (under high contention)",
                setup_sql="""
CREATE TABLE hot_access (
    id INTEGER PRIMARY KEY,
    data VARCHAR(1000),
    access_count INTEGER DEFAULT 0
);

-- Insert 100K rows
INSERT INTO hot_access (id, data)
SELECT :id, 'Data ' || :id
FROM (SELECT gen_id(gen_rows, 1) as id FROM rdb$generator WHERE gen_id(gen_rows, 0) < 100000);
                """,
                test_sql="""
-- Concurrent scenario:
-- Thread 1-10: Continuous reads
-- Thread 11-20: Continuous updates

-- Firebird: Readers never block, consistent snapshot
-- MySQL: Readers may wait for row locks
-- PostgreSQL: Good but with some snapshot overhead

SELECT * FROM hot_access
WHERE id BETWEEN 1 AND 10000
ORDER BY id;
-- While other connections update
                """,
                expected_fb_pattern="Consistent read snapshot, zero lock waits",
                expected_mysql_pattern="MVCC but potential lock waits on concurrent writes",
                expected_pg_pattern="MVCC with snapshot, some visibility check overhead"
            ),

            FirebirdOptimizedTest(
                name="mga_long_running_read",
                description="Long-running query doesn't block vacuum/writes",
                why_firebird_faster="""
Firebird: Old record versions kept until transaction ends, no blocking.
MySQL: Undo log may be purged, causing issues for long queries.
PostgreSQL: Long query blocks vacuum, causes table bloat.
                """.strip(),
                firebird_advantage_factor="No blocking vs vacuum blocking",
                setup_sql="""
CREATE TABLE versioned_data (
    id INTEGER PRIMARY KEY,
    version INTEGER DEFAULT 1,
    payload VARCHAR(2000)
);

-- Insert initial data
INSERT INTO versioned_data (id, payload)
SELECT gen_id(gen_rows, 1), REPEAT('x', 1000)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
                """,
                test_sql="""
-- Start long-running read transaction
-- Meanwhile, other connections update every row multiple times

SELECT COUNT(*), AVG(LENGTH(payload))
FROM versioned_data
WHERE version > 0;
-- Firebird: Reads consistent snapshot, writes continue
-- PostgreSQL: Autovacuum blocked, table grows
                """,
                expected_fb_pattern="Read snapshot isolated, writes proceed freely",
                expected_mysql_pattern="Undo log management for long transactions",
                expected_pg_pattern="Vacuum blocked, table bloat accumulates"
            ),

            FirebirdOptimizedTest(
                name="mga_rollback_performance",
                description="Transaction rollback is instantaneous",
                why_firebird_faster="""
Firebird: Rollback just discards record versions - no undo needed.
MySQL: Must undo changes using rollback segments.
PostgreSQL: Must mark tuples as dead, cleanup work.
                """.strip(),
                firebird_advantage_factor="100-1000x for large transactions",
                setup_sql="""
CREATE TABLE rollback_test (
    id INTEGER PRIMARY KEY,
    data VARCHAR(1000),
    updated_at TIMESTAMP
);

INSERT INTO rollback_test (id, data, updated_at)
SELECT gen_id(gen_rows, 1), REPEAT('data', 100), CURRENT_TIMESTAMP
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
                """,
                test_sql="""
-- Start transaction, update many rows, then rollback
UPDATE rollback_test
SET data = REPEAT('modified', 100), updated_at = CURRENT_TIMESTAMP
WHERE id BETWEEN 1 AND 500000;

-- Firebird: Instant rollback (discard versions)
ROLLBACK;

-- MySQL: Must undo all changes from rollback segment
-- PostgreSQL: Must abort, mark work as dead
                """,
                expected_fb_pattern="Instant rollback (version discard)",
                expected_mysql_pattern="Undo log replay (slow for large tx)",
                expected_pg_pattern="Transaction abort with cleanup"
            ),
        ]


class FirebirdConcurrencyTests:
    """Tests exploiting Firebird's concurrency model."""

    @staticmethod
    def get_all_tests() -> List[FirebirdOptimizedTest]:
        return [
            FirebirdOptimizedTest(
                name="concurrent_read_heavy",
                description="Many concurrent readers (100+)",
                why_firebird_faster="""
Firebird: No locks for reads, MGA scales to 100s of readers.
MySQL: Read locks (even shared) have overhead at high concurrency.
PostgreSQL: Good but snapshot synchronization overhead increases.
                """.strip(),
                firebird_advantage_factor="5-10x at 100+ concurrent readers",
                setup_sql="""
CREATE TABLE read_heavy (
    id INTEGER PRIMARY KEY,
    category VARCHAR(50),
    content BLOB SUB_TYPE TEXT
);

INSERT INTO read_heavy (id, category, content)
SELECT gen_id(gen_rows, 1),
       CASE MOD(gen_id(gen_rows, 1), 10)
         WHEN 0 THEN 'news' WHEN 1 THEN 'sports' ELSE 'other' END,
       REPEAT('Content ', 200)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 500000;
                """,
                test_sql="""
-- 100 concurrent connections executing:
SELECT * FROM read_heavy
WHERE category = 'news'
ORDER BY id ROWS 100;

-- Firebird: Each gets snapshot, no locking, scales linearly
-- MySQL: Read view maintenance, some lock overhead
-- PostgreSQL: Snapshot acquisition, shared buffer contention
                """,
                expected_fb_pattern="Linear scaling, no lock contention",
                expected_mysql_pattern="Good scaling but lock overhead",
                expected_pg_pattern="Good but snapshot synchronization cost"
            ),

            FirebirdOptimizedTest(
                name="concurrent_write_heavy",
                description="Concurrent writes to different rows",
                why_firebird_faster="""
Firebird: Row-level versioning, writers don't block each other (usually).
MySQL: Row locks may cause waits on index gaps.
PostgreSQL: Row locks + predicate locks, some blocking.
                """.strip(),
                firebird_advantage_factor="2-5x",
                setup_sql="""
CREATE TABLE write_heavy (
    id INTEGER PRIMARY KEY,
    counter INTEGER DEFAULT 0,
    data VARCHAR(100)
);

INSERT INTO write_heavy (id, data)
SELECT gen_id(gen_rows, 1), 'row' || gen_id(gen_rows, 1)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
                """,
                test_sql="""
-- 50 concurrent connections, each updating different rows:
UPDATE write_heavy
SET counter = counter + 1
WHERE id = :random_id;

-- Firebird: Versioning handles concurrent updates well
-- MySQL: Row locks, potential gap lock issues
-- PostgreSQL: Row locks, some serialization failures
                """,
                expected_fb_pattern="High concurrency via versioning",
                expected_mysql_pattern="Lock waits on hot rows",
                expected_pg_pattern="Some serialization failures"
            ),
        ]


class FirebirdStorageEfficiencyTests:
    """Tests exploiting Firebird's compact storage."""

    @staticmethod
    def get_all_tests() -> List[FirebirdOptimizedTest]:
        return [
            FirebirdOptimizedTest(
                name="storage_compact_nulls",
                description="Tables with many NULL values",
                why_firebird_faster="""
Firebird: Very compact NULL storage (bitmap), RLE compression.
MySQL: NULL bitmap per row, less efficient packing.
PostgreSQL: NULL bitmap, but tuple overhead larger.
                """.strip(),
                firebird_advantage_factor="30-50% smaller",
                setup_sql="""
CREATE TABLE sparse_data (
    id INTEGER PRIMARY KEY,
    col1 INTEGER,
    col2 INTEGER,
    col3 INTEGER,
    col4 INTEGER,
    col5 INTEGER,
    col6 VARCHAR(100),
    col7 VARCHAR(100),
    col8 VARCHAR(100)
);

-- Insert with mostly NULLs (sparse data)
INSERT INTO sparse_data (id, col1, col6)
SELECT gen_id(gen_rows, 1),
       CASE WHEN MOD(gen_id(gen_rows, 1), 10) = 0 THEN gen_id(gen_rows, 1) END,
       CASE WHEN MOD(gen_id(gen_rows, 1), 10) = 0 THEN 'data' END
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 10000000;
                """,
                test_sql="""
SELECT COUNT(*), COUNT(col1), COUNT(col6) FROM sparse_data;

-- Firebird: Very compact storage, fast scan
-- MySQL: More storage overhead per row
-- PostgreSQL: Larger tuple overhead
                """,
                expected_fb_pattern="Compact storage (bitmap NULLs, RLE)",
                expected_mysql_pattern="Less efficient NULL bitmap",
                expected_pg_pattern="Tuple header overhead (27+ bytes)"
            ),

            FirebirdOptimizedTest(
                name="storage_index_compression",
                description="Index compression for repetitive data",
                why_firebird_faster="""
Firebird: Prefix compression in indexes, very compact.
MySQL: Some prefix compression but less aggressive.
PostgreSQL: Index tuples have overhead, less compression.
                """.strip(),
                firebird_advantage_factor="50-70% smaller indexes",
                setup_sql="""
CREATE TABLE repetitive_data (
    id INTEGER PRIMARY KEY,
    prefix VARCHAR(50),  # Values like 'USA-NY-001', 'USA-NY-002'
    suffix VARCHAR(20)
);

CREATE INDEX idx_prefix ON repetitive_data(prefix);

-- Insert with highly repetitive prefixes
INSERT INTO repetitive_data (id, prefix, suffix)
SELECT gen_id(gen_rows, 1),
       'USA-NY-' || LPAD(MOD(gen_id(gen_rows, 1), 1000), 4, '0'),
       'suffix' || gen_id(gen_rows, 1)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 10000000;
                """,
                test_sql="""
SELECT COUNT(*) FROM repetitive_data
WHERE prefix STARTING WITH 'USA-NY-001';

-- Firebird: Compressed index scans very fast
-- MySQL: Good but less compressed
-- PostgreSQL: Larger index to scan
                """,
                expected_fb_pattern="Highly compressed index (prefix compression)",
                expected_mysql_pattern="Moderate compression",
                expected_pg_pattern="Less compressed, more I/O"
            ),
        ]


class FirebirdIndexTests:
    """Tests exploiting Firebird's index implementation."""

    @staticmethod
    def get_all_tests() -> List[FirebirdOptimizedTest]:
        return [
            FirebirdOptimizedTest(
                name="index_selective_bitmap",
                description="Highly selective index with bitmap",
                why_firebird_faster="""
Firebird: Bitmap index scans with AND/OR combining.
MySQL: Index merge, less efficient bitmap operations.
PostgreSQL: Bitmap scan good but index larger.
                """.strip(),
                firebird_advantage_factor="2-3x",
                setup_sql="""
CREATE TABLE selective_query (
    id INTEGER PRIMARY KEY,
    status VARCHAR(20),
    priority INTEGER,
    category VARCHAR(20)
);

CREATE INDEX idx_status ON selective_query(status);
CREATE INDEX idx_priority ON selective_query(priority);
CREATE INDEX idx_category ON selective_query(category);

INSERT INTO selective_query (id, status, priority, category)
SELECT gen_id(gen_rows, 1),
       CASE MOD(gen_id(gen_rows, 1), 10)
         WHEN 0 THEN 'active' WHEN 1 THEN 'pending' ELSE 'inactive' END,
       MOD(gen_id(gen_rows, 1), 100),
       CASE MOD(gen_id(gen_rows, 1), 5)
         WHEN 0 THEN 'A' WHEN 1 THEN 'B' ELSE 'C' END
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 10000000;
                """,
                test_sql="""
SELECT * FROM selective_query
WHERE status = 'active'
  AND priority BETWEEN 10 AND 20
  AND category = 'A';

-- Firebird: Bitmap AND of three index scans
-- MySQL: Index merge or range scan
-- PostgreSQL: Bitmap scan (good but larger indexes)
                """,
                expected_fb_pattern="Bitmap AND of three compressed indexes",
                expected_mysql_pattern="Index merge (less efficient)",
                expected_pg_pattern="BitmapAnd (good but more I/O)"
            ),

            FirebirdOptimizedTest(
                name="index_navigational",
                description="Index for navigational access (NEXT/PRIOR)",
                why_firebird_faster="""
Firebird: Bidirectional index cursors, efficient for pagination.
MySQL: Forward only mostly, pagination uses OFFSET.
PostgreSQL: Good but cursor management overhead.
                """.strip(),
                firebird_advantage_factor="5-10x for pagination",
                setup_sql="""
CREATE TABLE navigational (
    id INTEGER PRIMARY KEY,
    sort_order INTEGER,
    data VARCHAR(100)
);

CREATE INDEX idx_sort ON navigational(sort_order);

INSERT INTO navigational (id, sort_order, data)
SELECT gen_id(gen_rows, 1), gen_id(gen_rows, 1), 'data'
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 5000000;
                """,
                test_sql="""
-- Paginated access pattern
SELECT FIRST 20 SKIP :offset * 20 *
FROM navigational
ORDER BY sort_order;

-- Firebird: Index navigation very efficient
-- MySQL: OFFSET gets slower as offset grows
-- PostgreSQL: OFFSET also degrades
                """,
                expected_fb_pattern="Index cursor navigation (O(log n) per page)",
                expected_mysql_pattern="Index scan with OFFSET (O(offset + limit))",
                expected_pg_pattern="Index scan with OFFSET (O(offset + limit))"
            ),
        ]


class FirebirdWeaknessTests:
    """Tests where Firebird performs poorly (for cross-engine comparison)."""

    @staticmethod
    def get_all_tests() -> List[FirebirdOptimizedTest]:
        return [
            FirebirdOptimizedTest(
                name="version_cleanup_needed",
                description="Many updates create version backlog",
                why_firebird_faster="THIS IS WHERE FIREBIRD IS SLOWER - included for contrast",
                firebird_advantage_factor="0.1-0.3x (Firebird is SLOWER)",
                setup_sql="""
CREATE TABLE version_heavy (
    id INTEGER PRIMARY KEY,
    counter INTEGER DEFAULT 0
);

INSERT INTO version_heavy (id)
SELECT gen_id(gen_rows, 1)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 100000;
                """,
                test_sql="""
-- Update same rows 1000 times each
UPDATE version_heavy SET counter = counter + 1 WHERE id = :id;
-- Run for all 100K rows, 10 times each

-- Firebird: 1M versions created, slow to clean up
-- MySQL: Undo log managed, no version accumulation
-- PostgreSQL: Tuples dead, vacuum cleans up
                """,
                expected_fb_pattern="Version accumulation, slower reads",
                expected_mysql_pattern="Undo log, no version buildup",
                expected_pg_pattern="Dead tuples, vacuum handles"
            ),

            FirebirdOptimizedTest(
                name="no_parallel_query",
                description="Large aggregation without parallel execution",
                why_firebird_faster="THIS IS WHERE FIREBIRD IS SLOWER - included for contrast",
                firebird_advantage_factor="0.1-0.2x (Firebird is SLOWER)",
                setup_sql="""
CREATE TABLE no_parallel (
    id BIGINT PRIMARY KEY,
    amount DECIMAL(12,2),
    category INTEGER
);

INSERT INTO no_parallel (id, amount, category)
SELECT gen_id(gen_rows, 1), RAND() * 1000, MOD(gen_id(gen_rows, 1), 100)
FROM rdb$generator WHERE gen_id(gen_rows, 0) < 50000000;
                """,
                test_sql="""
-- Large aggregation - no parallel execution
SELECT category, COUNT(*), SUM(amount), AVG(amount)
FROM no_parallel
GROUP BY category;

-- Firebird: Single thread, slow on large data
-- PostgreSQL: Parallel aggregate (4-8x faster)
-- MySQL: Single thread (similar to Firebird)
                """,
                expected_fb_pattern="Sequential scan, single thread",
                expected_mysql_pattern="Single thread (similar limitation)",
                expected_pg_pattern="Parallel aggregate (much faster)"
            ),

            FirebirdOptimizedTest(
                name="limited_join_optimization",
                description="Complex joins with limited optimizer",
                why_firebird_faster="THIS IS WHERE FIREBIRD IS SLOWER - included for contrast",
                firebird_advantage_factor="0.05-0.2x (Firebird is SLOWER)",
                setup_sql="""
CREATE TABLE join_a (id INTEGER PRIMARY KEY, data VARCHAR(100));
CREATE TABLE join_b (id INTEGER PRIMARY KEY, a_id INTEGER, data VARCHAR(100));
CREATE TABLE join_c (id INTEGER PRIMARY KEY, b_id INTEGER, data VARCHAR(100));

INSERT INTO join_a SELECT gen_id(gen_rows, 1), 'a' FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
INSERT INTO join_b SELECT gen_id(gen_rows, 1), gen_id(gen_rows, 1), 'b' FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
INSERT INTO join_c SELECT gen_id(gen_rows, 1), gen_id(gen_rows, 1), 'c' FROM rdb$generator WHERE gen_id(gen_rows, 0) < 1000000;
                """,
                test_sql="""
-- Multi-way join
SELECT a.*, b.*, c.*
FROM join_a a
JOIN join_b b ON a.id = b.a_id
JOIN join_c c ON b.id = c.b_id
WHERE a.id BETWEEN 1 AND 100000;

-- Firebird: Nested loops only, slow
-- PostgreSQL: Hash join (much faster)
-- MySQL: Nested loops (similar limitation)
                """,
                expected_fb_pattern="Nested loop joins (slow)",
                expected_mysql_pattern="Nested loop or block nested loop",
                expected_pg_pattern="Hash join (optimal)"
            ),
        ]


def get_all_tests() -> List[FirebirdOptimizedTest]:
    """Get all Firebird-optimized tests."""
    tests = []
    tests.extend(FirebirdMGATests.get_all_tests())
    tests.extend(FirebirdConcurrencyTests.get_all_tests())
    tests.extend(FirebirdStorageEfficiencyTests.get_all_tests())
    tests.extend(FirebirdIndexTests.get_all_tests())
    tests.extend(FirebirdWeaknessTests.get_all_tests())
    return tests


if __name__ == '__main__':
    tests = get_all_tests()
    print(f"Firebird-Optimized Tests: {len(tests)}")
    print()

    advantage_tests = [t for t in tests if not t.name.startswith("version_") and not t.name.startswith("no_") and not t.name.startswith("limited_")]
    weakness_tests = [t for t in tests if t.name.startswith("version_") or t.name.startswith("no_") or t.name.startswith("limited_")]

    print(f"  Firebird Strengths: {len(advantage_tests)} tests")
    for t in advantage_tests:
        print(f"    - {t.name}: {t.firebird_advantage_factor} faster")

    print(f"\n  Firebird Weaknesses (for contrast): {len(weakness_tests)} tests")
    for t in weakness_tests:
        print(f"    - {t.name}: {t.firebird_advantage_factor} (Firebird slower)")
