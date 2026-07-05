#!/usr/bin/env python3
"""
PostgreSQL Optimized Tests

Tests designed to perform exceptionally well on PostgreSQL due to its
architectural strengths, while performing poorly on MySQL and Firebird.

PostgreSQL Architectural Strengths:
1. Parallel Query Execution - Multiple workers for scans, joins, aggregates
2. Advanced Index Types - GiST, GIN, SP-GiST, BRIN for specialized access
3. Partial Indexes - Index subset of table, smaller faster indexes
4. Index-Only Scans - Visibility map enables heap avoidance
5. Sophisticated Joins - Hash joins, merge joins (not just nested loop)
6. TOAST - Efficient out-of-line storage for large values
7. HOT Updates - Heap-only tuple updates avoid index maintenance
8. Partitioning - Native partitioning with pruning
9. Parallel Aggregation - Multi-worker aggregation
10. JIT Compilation - Just-in-time compilation for complex queries

Weaknesses to exploit for cross-engine comparison:
- MySQL: Limited parallel query, nested loop joins mostly
- Firebird: No parallel execution, simpler optimizer
"""

from dataclasses import dataclass
from typing import List


@dataclass
class PostgreSQLOptimizedTest:
    name: str
    description: str
    why_postgres_faster: str
    postgres_advantage_factor: str
    setup_sql: str
    test_sql: str
    expected_pg_pattern: str
    expected_mysql_pattern: str
    expected_fb_pattern: str


class PostgreSQLParallelQueryTests:
    """Tests exploiting PostgreSQL's parallel query execution."""

    @staticmethod
    def get_all_tests() -> List[PostgreSQLOptimizedTest]:
        return [
            PostgreSQLOptimizedTest(
                name="parallel_seq_scan_large_table",
                description="Full table scan with parallel workers",
                why_postgres_faster="""
PostgreSQL: Parallel sequential scan splits table across workers (4-8x speedup).
MySQL: No parallel scan - single thread reads entire table.
Firebird: No parallel execution - single thread scan.
                """.strip(),
                postgres_advantage_factor="4-8x",
                setup_sql="""
-- PostgreSQL parallel setup
SET max_parallel_workers_per_gather = 8;
SET parallel_tuple_cost = 0.01;
SET parallel_setup_cost = 100;

CREATE TABLE large_analytics (
    id BIGSERIAL PRIMARY KEY,
    category_id INT NOT NULL,
    region_id INT NOT NULL,
    amount DECIMAL(12,2),
    quantity INT,
    created_at TIMESTAMP DEFAULT NOW(),
    data JSONB
);

-- Insert 50M rows
INSERT INTO large_analytics (category_id, region_id, amount, quantity, data)
SELECT
    (seq % 1000),
    (seq % 100),
    RANDOM() * 10000,
    (RANDOM() * 100)::INT,
    jsonb_build_object('key', seq, 'value', RANDOM())
FROM generate_series(1, 50000000) seq;

ANALYZE large_analytics;
                """,
                test_sql="""
-- PostgreSQL: Parallel seq scan + partial aggregation
-- MySQL: Single thread full scan
-- Firebird: Single thread scan
SELECT
    category_id,
    COUNT(*) as cnt,
    SUM(amount) as total,
    AVG(quantity) as avg_qty,
    PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) as median
FROM large_analytics
WHERE created_at > '2020-01-01'
GROUP BY category_id
ORDER BY total DESC;
                """,
                expected_pg_pattern="Parallel Seq Scan + Partial Aggregate + Gather Merge",
                expected_mysql_pattern="Full table scan, single thread, no parallelism",
                expected_fb_pattern="Sequential scan, single thread"
            ),

            PostgreSQLOptimizedTest(
                name="parallel_hash_join",
                description="Large hash join with parallel build",
                why_postgres_faster="""
PostgreSQL: Parallel hash join builds hash table across workers.
MySQL: Hash joins exist but not parallel (8.0.18+ limited).
Firebird: No hash join, nested loop only.
                """.strip(),
                postgres_advantage_factor="3-6x",
                setup_sql="""
CREATE TABLE fact_sales (
    sale_id BIGSERIAL PRIMARY KEY,
    product_id INT NOT NULL,
    customer_id INT NOT NULL,
    sale_date DATE NOT NULL,
    amount DECIMAL(12,2)
);

CREATE TABLE dim_product (
    product_id INT PRIMARY KEY,
    product_name VARCHAR(100),
    category VARCHAR(50),
    brand VARCHAR(50),
    cost DECIMAL(10,2)
);

-- 20M sales
INSERT INTO fact_sales (product_id, customer_id, sale_date, amount)
SELECT
    (seq % 100000) + 1,
    (seq % 1000000) + 1,
    '2020-01-01'::DATE + (seq % 1000),
    RANDOM() * 500
FROM generate_series(1, 20000000) seq;

-- 100K products
INSERT INTO dim_product (product_id, product_name, category, brand, cost)
SELECT
    seq,
    'Product ' || seq,
    CASE seq % 10 WHEN 0 THEN 'Electronics' WHEN 1 THEN 'Clothing' ELSE 'Other' END,
    'Brand ' || (seq % 100),
    RANDOM() * 100
FROM generate_series(1, 100000) seq;

ANALYZE fact_sales;
ANALYZE dim_product;
                """,
                test_sql="""
-- Large hash join - PostgreSQL parallelizes
SELECT p.category, COUNT(*) as sales_count, SUM(s.amount) as revenue
FROM fact_sales s
JOIN dim_product p ON s.product_id = p.product_id
WHERE s.sale_date BETWEEN '2020-01-01' AND '2020-12-31'
GROUP BY p.category;
                """,
                expected_pg_pattern="Parallel Hash Join + Parallel Aggregate",
                expected_mysql_pattern="Nested loop or block nested loop (slower)",
                expected_fb_pattern="Nested loop join (very slow on large tables)"
            ),

            PostgreSQLOptimizedTest(
                name="parallel_bitmap_heap_scan",
                description="Bitmap index scan with parallel heap access",
                why_postgres_faster="""
PostgreSQL: Bitmap scan with parallel heap access for moderate selectivity.
MySQL: Index range scan, no parallel bitmap operations.
Firebird: Index navigation, no bitmap scans.
                """.strip(),
                postgres_advantage_factor="3-5x",
                setup_sql="""
CREATE TABLE parallel_bitmap_test (
    id BIGSERIAL PRIMARY KEY,
    status VARCHAR(20),
    category INT,
    data TEXT
);

CREATE INDEX idx_status ON parallel_bitmap_test(status);
CREATE INDEX idx_category ON parallel_bitmap_test(category);

-- 10M rows, mixed status
INSERT INTO parallel_bitmap_test (status, category, data)
SELECT
    CASE (seq % 10) WHEN 0 THEN 'active' WHEN 1 THEN 'pending' ELSE 'inactive' END,
    seq % 1000,
    REPEAT('data', 100)
FROM generate_series(1, 10000000) seq;

ANALYZE parallel_bitmap_test;
                """,
                test_sql="""
-- Bitmap scan with parallel heap access
SELECT * FROM parallel_bitmap_test
WHERE status IN ('active', 'pending')
  AND category BETWEEN 100 AND 200
ORDER BY id;
                """,
                expected_pg_pattern="Parallel Bitmap Heap Scan + BitmapAnd",
                expected_mysql_pattern="Index Merge or range scan, single thread",
                expected_fb_pattern="Index navigation, sequential fetch"
            ),
        ]


class PostgreSQLAdvancedIndexTests:
    """Tests exploiting PostgreSQL's advanced index types."""

    @staticmethod
    def get_all_tests() -> List[PostgreSQLOptimizedTest]:
        return [
            PostgreSQLOptimizedTest(
                name="gin_fulltext_search",
                description="Full-text search with GIN index",
                why_postgres_faster="""
PostgreSQL: GIN index for fast full-text search with tsvector.
MySQL: Full-text index (InnoDB) but less flexible, no ranking as flexible.
Firebird: No native full-text search, must use external or LIKE scans.
                """.strip(),
                postgres_advantage_factor="50-100x",
                setup_sql="""
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    search_vector TSVECTOR
);

-- Create GIN index for full-text search
CREATE INDEX idx_fts ON documents USING GIN(search_vector);

-- Insert documents and create search vectors
INSERT INTO documents (title, content, search_vector)
SELECT
    'Document ' || seq,
    REPEAT('word' || seq || ' ', 100) || ' special keyword',
    to_tsvector('english', REPEAT('word' || seq || ' ', 100) || ' special keyword')
FROM generate_series(1, 1000000) seq;

ANALYZE documents;
                """,
                test_sql="""
-- Full-text search query
SELECT id, title, ts_rank(search_vector, query) as rank
FROM documents, plainto_tsquery('english', 'special keyword') query
WHERE search_vector @@ query
ORDER BY rank DESC
LIMIT 100;
                """,
                expected_pg_pattern="Bitmap Index Scan on idx_fts (GIN)",
                expected_mysql_pattern="Fulltext index scan (less efficient)",
                expected_fb_pattern="Full table scan (no FTS support)"
            ),

            PostgreSQLOptimizedTest(
                name="gist_range_queries",
                description="Range queries with GiST index",
                why_postgres_faster="""
PostgreSQL: GiST index for range types with overlap/containment queries.
MySQL: No range type support, must use two columns + index.
Firebird: No range type support.
                """.strip(),
                postgres_advantage_factor="10-50x",
                setup_sql="""
CREATE TABLE reservations (
    id SERIAL PRIMARY KEY,
    room_id INT,
    during TSTZRANGE,  # PostgreSQL range type
    guest_name TEXT
);

-- GiST index for range overlap queries
CREATE INDEX idx_reservation_time ON reservations USING GiST(during);

-- Insert 1M reservations
INSERT INTO reservations (room_id, during, guest_name)
SELECT
    (seq % 1000) + 1,
    TSTZRANGE(
        NOW() + (seq % 365) * INTERVAL '1 day',
        NOW() + (seq % 365) * INTERVAL '1 day' + INTERVAL '1-7 days',
        '[)'
    ),
    'Guest ' || seq
FROM generate_series(1, 1000000) seq;

ANALYZE reservations;
                """,
                test_sql="""
-- Find overlapping reservations (GiST excels at this)
SELECT * FROM reservations
WHERE during && TSTZRANGE('2024-06-01', '2024-06-30', '[)')
  AND room_id = 500;
                """,
                expected_pg_pattern="Index Scan using idx_reservation_time (GiST)",
                expected_mysql_pattern="Index on start_date + end_date, less efficient",
                expected_fb_pattern="Index scan with two comparisons, no overlap operator"
            ),

            PostgreSQLOptimizedTest(
                name="brin_warehouse_scan",
                description="Block Range Index for sequential correlation",
                why_postgres_faster="""
PostgreSQL: BRIN index is tiny (KB vs MB) for correlated data like time series.
MySQL: Must use B-tree (larger) or partition (more complex).
Firebird: No BRIN equivalent, regular index much larger.
                """.strip(),
                postgres_advantage_factor="5-20x index size, 2-3x query",
                setup_sql="""
CREATE TABLE sensor_readings (
    reading_id BIGSERIAL PRIMARY KEY,
    sensor_id INT,
    reading_time TIMESTAMP,
    temperature DECIMAL(5,2),
    humidity DECIMAL(5,2)
);

-- BRIN index for time-series data (tiny size)
CREATE INDEX idx_readings_time_brin ON sensor_readings
USING BRIN(reading_time) WITH (pages_per_range = 128);

-- Compare to B-tree (for size comparison)
CREATE INDEX idx_readings_time_btree ON sensor_readings (reading_time);

-- 100M time-series readings (naturally ordered by time)
INSERT INTO sensor_readings (sensor_id, reading_time, temperature, humidity)
SELECT
    (seq % 1000),
    '2020-01-01'::TIMESTAMP + (seq * INTERVAL '1 minute'),
    20 + RANDOM() * 15,
    40 + RANDOM() * 40
FROM generate_series(1, 100000000) seq;

ANALYZE sensor_readings;
                """,
                test_sql="""
-- BRIN index scan for time range
SELECT sensor_id, AVG(temperature), MAX(temperature)
FROM sensor_readings
WHERE reading_time BETWEEN '2023-01-01' AND '2023-01-31'
GROUP BY sensor_id;
                """,
                expected_pg_pattern="Bitmap Index Scan on idx_readings_time_brin (tiny index)",
                expected_mysql_pattern="Index range scan on large B-tree",
                expected_fb_pattern="Index range scan on B-tree"
            ),

            PostgreSQLOptimizedTest(
                name="partial_index_selective",
                description="Partial index for selective queries",
                why_postgres_faster="""
PostgreSQL: Partial index is tiny - only indexes active orders (e.g., 5% of table).
MySQL: Full index required, no partial index support.
Firebird: Expression/conditional indexes limited.
                """.strip(),
                postgres_advantage_factor="10-20x smaller index, 5-10x faster",
                setup_sql="""
CREATE TABLE orders_with_status (
    order_id BIGSERIAL PRIMARY KEY,
    customer_id INT,
    order_date TIMESTAMP,
    status VARCHAR(20),  # 'active', 'completed', 'cancelled'
    total_amount DECIMAL(12,2)
);

-- Partial index: only active orders (small!)
CREATE INDEX idx_active_orders ON orders_with_status(customer_id, order_date)
WHERE status = 'active';

-- 10M orders, mostly completed
INSERT INTO orders_with_status (customer_id, order_date, status, total_amount)
SELECT
    (seq % 100000) + 1,
    NOW() - INTERVAL '1 day' * (seq % 365),
    CASE WHEN seq % 20 = 0 THEN 'active' ELSE 'completed' END,
    RANDOM() * 1000
FROM generate_series(1, 10000000) seq;

ANALYZE orders_with_status;
                """,
                test_sql="""
-- Query matches partial index predicate
SELECT * FROM orders_with_status
WHERE status = 'active'
  AND customer_id = 12345
ORDER BY order_date DESC;
                """,
                expected_pg_pattern="Index Scan using idx_active_orders (tiny partial index)",
                expected_mysql_pattern="Index on status+customer_id (full size, larger)",
                expected_fb_pattern="Full index scan or table scan"
            ),
        ]


class PostgreSQLJoinOptimizationTests:
    """Tests exploiting PostgreSQL's sophisticated join algorithms."""

    @staticmethod
    def get_all_tests() -> List[PostgreSQLOptimizedTest]:
        return [
            PostgreSQLOptimizedTest(
                name="hash_join_large_tables",
                description="Hash join for large unsorted tables",
                why_postgres_faster="""
PostgreSQL: Hash join O(n+m) complexity for large tables.
MySQL: Block nested loop O(n*m) or hash join (8.0.18+, limited).
Firebird: Nested loop only O(n*m) - disastrous for large tables.
                """.strip(),
                postgres_advantage_factor="10-100x",
                setup_sql="""
CREATE TABLE large_table_a (
    id SERIAL PRIMARY KEY,
    join_key INT,
    data TEXT
);

CREATE TABLE large_table_b (
    id SERIAL PRIMARY KEY,
    join_key INT,
    data TEXT
);

-- No indexes on join_key - force hash join
INSERT INTO large_table_a (join_key, data)
SELECT seq, REPEAT('a', 100)
FROM generate_series(1, 5000000) seq;

INSERT INTO large_table_b (join_key, data)
SELECT seq, REPEAT('b', 100)
FROM generate_series(1, 5000000) seq;

ANALYZE large_table_a;
ANALYZE large_table_b;
                """,
                test_sql="""
-- Hash join (PostgreSQL) vs Nested Loop (Firebird)
SELECT COUNT(*)
FROM large_table_a a
JOIN large_table_b b ON a.join_key = b.join_key
WHERE a.join_key BETWEEN 1 AND 1000000;
                """,
                expected_pg_pattern="Hash Join (fast, single pass)",
                expected_mysql_pattern="Hash Join (8.0+) or Block Nested Loop",
                expected_fb_pattern="Nested Loop (extremely slow)"
            ),

            PostgreSQLOptimizedTest(
                name="merge_join_sorted",
                description="Merge join for pre-sorted data",
                why_postgres_faster="""
PostgreSQL: Merge join O(n+m) when both inputs sorted.
MySQL: Nested loop even with indexes.
Firebird: Nested loop even with indexes.
                """.strip(),
                postgres_advantage_factor="5-10x",
                setup_sql="""
CREATE TABLE sorted_a (
    id SERIAL PRIMARY KEY,
    sort_key INT,
    data TEXT
);

CREATE TABLE sorted_b (
    id SERIAL PRIMARY KEY,
    sort_key INT,
    data TEXT
);

CREATE INDEX idx_a ON sorted_a(sort_key);
CREATE INDEX idx_b ON sorted_b(sort_key);

INSERT INTO sorted_a (sort_key, data)
SELECT seq, REPEAT('a', 100)
FROM generate_series(1, 2000000) seq;

INSERT INTO sorted_b (sort_key, data)
SELECT seq, REPEAT('b', 100)
FROM generate_series(1, 2000000) seq;

ANALYZE sorted_a;
ANALYZE sorted_b;
                """,
                test_sql="""
-- Merge join on sorted data
SELECT a.sort_key, a.data, b.data
FROM sorted_a a
JOIN sorted_b b ON a.sort_key = b.sort_key
WHERE a.sort_key BETWEEN 1 AND 1000000;
                """,
                expected_pg_pattern="Merge Join (optimal for sorted inputs)",
                expected_mysql_pattern="Nested Loop with index lookups",
                expected_fb_pattern="Nested Loop with index lookups"
            ),
        ]


class PostgreSQLTOASTTests:
    """Tests exploiting PostgreSQL's TOAST for large values."""

    @staticmethod
    def get_all_tests() -> List[PostgreSQLOptimizedTest]:
        return [
            PostgreSQLOptimizedTest(
                name="toast_large_column_scan",
                description="Table scan with TOASTed large columns",
                why_postgres_faster="""
PostgreSQL: TOAST stores large values out-of-line. Scanning narrow columns is fast.
MySQL: External pages for large blobs, but less efficient compression.
Firebird: Blobs stored separately, but page management overhead.
                """.strip(),
                postgres_advantage_factor="2-5x",
                setup_sql="""
CREATE TABLE large_objects (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100),
    metadata JSONB,
    large_content TEXT  -- Will be TOASTed
);

-- Insert 1M rows with large content (TOASTed)
INSERT INTO large_objects (name, metadata, large_content)
SELECT
    'object_' || seq,
    jsonb_build_object('id', seq, 'type', 'document'),
    REPEAT('Large content data ', 10000)  -- ~200KB each, TOASTed
FROM generate_series(1, 1000000) seq;

ANALYZE large_objects;
                """,
                test_sql="""
-- Query doesn't need large_content, TOAST not accessed
SELECT id, name, metadata->>'type'
FROM large_objects
WHERE id BETWEEN 1 AND 100000;
                """,
                expected_pg_pattern="Seq Scan (narrow, TOAST not accessed)",
                expected_mysql_pattern="Index scan, may access overflow pages",
                expected_fb_pattern="Record fetch, blob ID dereference"
            ),
        ]


class PostgreSQLHOTUpdateTests:
    """Tests exploiting PostgreSQL's HOT (Heap-Only Tuple) updates."""

    @staticmethod
    def get_all_tests() -> List[PostgreSQLOptimizedTest]:
        return [
            PostgreSQLOptimizedTest(
                name="hot_update_frequent",
                description="Frequent updates to same page (HOT)",
                why_postgres_faster="""
PostgreSQL: HOT updates don't touch indexes, minimal WAL, no index bloat.
MySQL: Undo log records, index updates (even covering indexes affected).
Firebird: Record versions created, index pointers updated.
                """.strip(),
                postgres_advantage_factor="3-10x",
                setup_sql="""
CREATE TABLE counter_table (
    id SERIAL PRIMARY KEY,
    counter INT DEFAULT 0,
    last_updated TIMESTAMP,
    -- Fill factor to keep tuples together
    filler CHAR(100) DEFAULT 'x'
);

-- Insert 100K counters
INSERT INTO counter_table (counter, last_updated)
SELECT 0, NOW()
FROM generate_series(1, 100000) seq;

ANALYZE counter_table;
                """,
                test_sql="""
-- Update same row 1000 times (HOT chain)
UPDATE counter_table
SET counter = counter + 1, last_updated = NOW()
WHERE id = 1;
-- Execute 1000 times
                """,
                expected_pg_pattern="HOT update (no index touch, minimal WAL)",
                expected_mysql_pattern="Undo log growth, buffer pool pressure",
                expected_fb_pattern="Record version chain growth"
            ),
        ]


def get_all_tests() -> List[PostgreSQLOptimizedTest]:
    """Get all PostgreSQL-optimized tests."""
    tests = []
    tests.extend(PostgreSQLParallelQueryTests.get_all_tests())
    tests.extend(PostgreSQLAdvancedIndexTests.get_all_tests())
    tests.extend(PostgreSQLJoinOptimizationTests.get_all_tests())
    tests.extend(PostgreSQLTOASTTests.get_all_tests())
    tests.extend(PostgreSQLHOTUpdateTests.get_all_tests())
    return tests


if __name__ == '__main__':
    tests = get_all_tests()
    print(f"PostgreSQL-Optimized Tests: {len(tests)}")
    print()

    for t in tests:
        print(f"  - {t.name}: {t.postgres_advantage_factor} faster")
        print(f"    {t.description}")
