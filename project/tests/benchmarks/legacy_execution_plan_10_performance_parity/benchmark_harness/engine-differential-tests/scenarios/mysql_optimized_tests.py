#!/usr/bin/env python3
"""
MySQL/InnoDB Optimized Tests

Tests designed to perform exceptionally well on MySQL/InnoDB due to its
architectural strengths, while performing poorly on PostgreSQL and Firebird.

MySQL/InnoDB Architectural Strengths:
1. Clustered Primary Key - Data stored in PK order, range scans are sequential I/O
2. Covering Indexes - Secondary indexes include PK values, index-only scans
3. Buffer Pool - Efficient caching of frequently accessed pages
4. Insert Buffering - Secondary index insert buffering for write performance
5. Adaptive Hash Index - In-memory hash index for hot pages
6. Change Buffering - Deferred secondary index updates

Weaknesses to exploit for cross-engine comparison:
- PostgreSQL/Firebird use heap tables (random I/O on PK range scans)
- Secondary index lookups require heap access (unless covering)
- Table locks during DDL
"""

from dataclasses import dataclass
from typing import List, Optional


@dataclass
class MySQLOptimizedTest:
    """Test optimized for MySQL/InnoDB performance."""
    name: str
    description: str
    why_mysql_faster: str
    mysql_advantage_factor: str  # e.g., "10-50x"
    setup_sql: str
    test_sql: str
    expected_mysql_pattern: str  # What MySQL should do
    expected_pg_pattern: str     # What PostgreSQL will do (slower)
    expected_fb_pattern: str     # What Firebird will do (slower)


class MySQLClusteredPKTests:
    """Tests exploiting MySQL's clustered primary key."""

    @staticmethod
    def get_all_tests() -> List[MySQLOptimizedTest]:
        return [
            MySQLOptimizedTest(
                name="clustered_pk_range_scan",
                description="Large range scan on primary key",
                why_mysql_faster="""
MySQL: Clustered PK means data IS the index. Range scan = sequential I/O.
PostgreSQL: Heap table - PK is separate B-tree, lookups require random I/O to heap.
Firebird: Heap with pointer pages - PK index + random record fetches.
                """.strip(),
                mysql_advantage_factor="5-20x",
                setup_sql="""
-- Create table with auto-increment PK (clustered in MySQL)
CREATE TABLE orders_clustered (
    order_id BIGINT PRIMARY KEY AUTO_INCREMENT,
    customer_id INT NOT NULL,
    order_date DATETIME NOT NULL,
    total_amount DECIMAL(12,2),
    status VARCHAR(20),
    INDEX idx_customer (customer_id),
    INDEX idx_date (order_date)
) ENGINE=InnoDB;

-- Insert 10M rows in PK order (optimal for MySQL)
INSERT INTO orders_clustered (customer_id, order_date, total_amount, status)
SELECT
    (seq % 100000) + 1,
    DATE_ADD('2020-01-01', INTERVAL (seq % 1825) DAY),
    RAND() * 1000,
    CASE seq % 5 WHEN 0 THEN 'pending' WHEN 1 THEN 'shipped'
         WHEN 2 THEN 'delivered' WHEN 3 THEN 'cancelled' ELSE 'refunded' END
FROM (SELECT @row := @row + 1 as seq FROM
    (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) t1,
    (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) t2,
    (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) t3,
    (SELECT 0 UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) t4,
    (SELECT @row := 0) init
    LIMIT 10000000) seqs;

ANALYZE TABLE orders_clustered;
                """,
                test_sql="""
-- Range scan on PK - MySQL reads sequentially from clustered index
-- PostgreSQL/Firebird do random I/O via heap
SELECT * FROM orders_clustered
WHERE order_id BETWEEN 1000000 AND 2000000
ORDER BY order_id;
                """,
                expected_mysql_pattern="Index range scan on PRIMARY (clustered), sequential read",
                expected_pg_pattern="Index Scan + Bitmap Heap Scan (random I/O)",
                expected_fb_pattern="Index scan + pointer page traversal (random I/O)"
            ),

            MySQLOptimizedTest(
                name="clustered_pk_prefix_scan",
                description="Prefix range scan exploiting clustered index locality",
                why_mysql_faster="""
MySQL: Recent orders stored together physically (clustered by PK/time).
PostgreSQL: Heap storage - temporal locality not guaranteed.
Firebird: Record versions scattered, no physical clustering.
                """.strip(),
                mysql_advantage_factor="3-10x",
                setup_sql="""
-- Same table as above
CREATE TABLE IF NOT EXISTS orders_clustered (
    order_id BIGINT PRIMARY KEY AUTO_INCREMENT,
    customer_id INT NOT NULL,
    order_date DATETIME NOT NULL,
    total_amount DECIMAL(12,2),
    status VARCHAR(20)
) ENGINE=InnoDB;

-- Ensure data exists
INSERT IGNORE INTO orders_clustered (order_id, customer_id, order_date, total_amount, status)
SELECT seq, (seq % 100000) + 1, NOW() - INTERVAL (seq % 365) DAY, 100, 'pending'
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 10000000) x;
                """,
                test_sql="""
-- Get most recent 100K orders - MySQL reads contiguous pages
SELECT * FROM orders_clustered
WHERE order_id > (SELECT MAX(order_id) - 100000 FROM orders_clustered)
ORDER BY order_id DESC;
                """,
                expected_mysql_pattern="Clustered index scan of last pages",
                expected_pg_pattern="Index scan + heap fetches (scattered)",
                expected_fb_pattern="Index navigation + scattered record fetches"
            ),

            MySQLOptimizedTest(
                name="covering_index_lookup",
                description="Index-only scan using covering index",
                why_mysql_faster="""
MySQL: Secondary index includes PK values. If query needs PK + indexed cols = covering.
PostgreSQL: Index-only scans need visibility map, limited by tuple visibility.
Firebird: Record versions may require table access for visibility.
                """.strip(),
                mysql_advantage_factor="2-5x",
                setup_sql="""
CREATE TABLE products_covering (
    product_id INT PRIMARY KEY AUTO_INCREMENT,
    category_id INT NOT NULL,
    sku VARCHAR(20) NOT NULL,
    price DECIMAL(10,2) NOT NULL,
    -- Covering index: includes all columns needed by query
    INDEX idx_category_covering (category_id, price, sku)
) ENGINE=InnoDB;

-- Insert 5M products
INSERT INTO products_covering (category_id, sku, price)
SELECT
    (seq % 1000) + 1,
    CONCAT('SKU', LPAD(seq, 8, '0')),
    RAND() * 999.99
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 5000000) x;

ANALYZE TABLE products_covering;
                """,
                test_sql="""
-- Covering index: all columns in query are in idx_category_covering
-- MySQL: Never touches heap, reads index only
-- PostgreSQL: Needs visibility map check, may access heap
-- Firebird: May need record version check
SELECT sku, price FROM products_covering
WHERE category_id = 500
ORDER BY price DESC
LIMIT 1000;
                """,
                expected_mysql_pattern="Using index (covering) - no table access",
                expected_pg_pattern="Index Only Scan + visibility map check",
                expected_fb_pattern="Index scan + possible record version read"
            ),

            MySQLOptimizedTest(
                name="secondary_index_covering_count",
                description="COUNT(*) using covering secondary index",
                why_mysql_faster="""
MySQL: Any secondary index can satisfy COUNT(*) without touching heap.
PostgreSQL: Requires visibility map or heap access for accurate count.
Firebird: May need to check record versions for visibility.
                """.strip(),
                mysql_advantage_factor="3-8x",
                setup_sql="""
CREATE TABLE events (
    event_id BIGINT PRIMARY KEY AUTO_INCREMENT,
    event_type INT NOT NULL,
    created_at DATETIME NOT NULL,
    user_id INT NOT NULL,
    data VARCHAR(255),
    INDEX idx_type_time (event_type, created_at),
    INDEX idx_user (user_id)
) ENGINE=InnoDB;

-- Insert 20M events
INSERT INTO events (event_type, created_at, user_id, data)
SELECT
    (seq % 100),
    NOW() - INTERVAL (seq % 525600) MINUTE,
    (seq % 1000000),
    REPEAT('x', 200)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 20000000) x;
                """,
                test_sql="""
-- MySQL counts using smallest secondary index
-- Others may need heap access for visibility
SELECT event_type, COUNT(*) as cnt
FROM events
WHERE created_at > NOW() - INTERVAL 30 DAY
GROUP BY event_type;
                """,
                expected_mysql_pattern="Index scan on idx_type_time (covering)",
                expected_pg_pattern="Index Only Scan with visibility map",
                expected_fb_pattern="Index scan + version checking overhead"
            ),
        ]


class MySQLBufferPoolTests:
    """Tests exploiting MySQL's buffer pool efficiency."""

    @staticmethod
    def get_all_tests() -> List[MySQLOptimizedTest]:
        return [
            MySQLOptimizedTest(
                name="repeated_hot_page_access",
                description="Repeated access to small working set",
                why_mysql_faster="""
MySQL: Buffer pool keeps hot pages in memory. LRU eviction optimized for repeated access.
PostgreSQL: Shared buffers, but heap pages may be evicted. Checkpoint activity.
Firebird: Page cache, but record versions spread across pages.
                """.strip(),
                mysql_advantage_factor="2-4x",
                setup_sql="""
CREATE TABLE hot_data (
    id INT PRIMARY KEY,
    category INT NOT NULL,
    value DECIMAL(12,2),
    INDEX idx_category (category)
) ENGINE=InnoDB;

-- Insert 1M rows, only 10 categories (small working set)
INSERT INTO hot_data (id, category, value)
SELECT seq, (seq % 10), RAND() * 1000
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 1000000) x;
                """,
                test_sql="""
-- Run 1000 times - MySQL keeps these pages in buffer pool
-- Others may have more cache pressure
SELECT category, AVG(value), COUNT(*)
FROM hot_data
WHERE category IN (1, 3, 5, 7, 9)
GROUP BY category;
-- Execute this query 1000 times in rapid succession
                """,
                expected_mysql_pattern="All pages in buffer pool (InnoDB buffer hit 100%)",
                expected_pg_pattern="Good cache hit, some shared buffer contention",
                expected_fb_pattern="Page cache effective but version management overhead"
            ),

            MySQLOptimizedTest(
                name="point_select_pk_buffer_efficiency",
                description="High-rate point lookups on PK",
                why_mysql_faster="""
MySQL: Adaptive hash index accelerates hot page lookups. Buffer pool pins hot pages.
PostgreSQL: Buffer mapping overhead, no adaptive hashing.
Firebird: Index traversal overhead for each lookup.
                """.strip(),
                mysql_advantage_factor="2-3x",
                setup_sql="""
CREATE TABLE lookup_table (
    id INT PRIMARY KEY,
    data VARCHAR(1000),
    INDEX idx_data (data(100))
) ENGINE=InnoDB;

-- Insert 100K rows
INSERT INTO lookup_table (id, data)
SELECT seq, REPEAT(CONCAT('data', seq % 1000), 100)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 100000) x;
                """,
                test_sql="""
-- Execute 100K point lookups
-- MySQL adaptive hash index makes this extremely fast
SELECT * FROM lookup_table WHERE id = ?;
-- Run for id = 1 to 100000
                """,
                expected_mysql_pattern="Adaptive hash index hits, buffer pool resident",
                expected_pg_pattern="Shared buffer lookups, no adaptive hashing",
                expected_fb_pattern="B-tree index traversal each time"
            ),
        ]


class MySQLWriteOptimizationTests:
    """Tests exploiting MySQL's write optimizations."""

    @staticmethod
    def get_all_tests() -> List[MySQLOptimizedTest]:
        return [
            MySQLOptimizedTest(
                name="secondary_index_insert_buffering",
                description="Bulk insert with multiple secondary indexes",
                why_mysql_faster="""
MySQL: Change buffer defers secondary index updates. Sequential PK insert is optimal.
PostgreSQL: All indexes updated immediately, WAL overhead for each.
Firebird: Index updates immediate, record version creation overhead.
                """.strip(),
                mysql_advantage_factor="3-10x",
                setup_sql="""
CREATE TABLE insert_heavy (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    col1 INT,
    col2 INT,
    col3 INT,
    col4 INT,
    data VARCHAR(100),
    -- Multiple secondary indexes
    INDEX idx1 (col1),
    INDEX idx2 (col2),
    INDEX idx3 (col3),
    INDEX idx4 (col4),
    INDEX idx_composite (col1, col2, col3)
) ENGINE=InnoDB;
                """,
                test_sql="""
-- Bulk insert 1M rows
INSERT INTO insert_heavy (col1, col2, col3, col4, data)
SELECT
    RAND() * 100000,
    RAND() * 100000,
    RAND() * 100000,
    RAND() * 100000,
    REPEAT('x', 100)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 1000000) x;
                """,
                expected_mysql_pattern="Change buffer absorbs secondary index writes",
                expected_pg_pattern="Immediate index maintenance + WAL logging",
                expected_fb_pattern="Immediate index updates + version creation"
            ),

            MySQLOptimizedTest(
                name="sequential_pk_insert_performance",
                description="Monotonically increasing PK insert",
                why_mysql_faster="""
MySQL: Appends to end of clustered index. No page splits. Minimal locking.
PostgreSQL: Heap insert + index insert. Possible page splits in index.
Firebird: Pointer page management, record version allocation.
                """.strip(),
                mysql_advantage_factor="2-5x",
                setup_sql="""
CREATE TABLE seq_insert (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    payload VARCHAR(200)
) ENGINE=InnoDB;
                """,
                test_sql="""
-- Insert 5M rows with auto-increment PK
-- MySQL appends perfectly sequentially
INSERT INTO seq_insert (payload)
SELECT REPEAT('data', 50)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 5000000) x;
                """,
                expected_mysql_pattern="Append-only, no page splits, minimal fragmentation",
                expected_pg_pattern="Heap fill factor + B-tree maintenance",
                expected_fb_pattern="Data page allocation + index maintenance"
            ),
        ]


class MySQLWeaknessTests:
    """Tests where MySQL performs poorly (for cross-engine comparison)."""

    @staticmethod
    def get_all_tests() -> List[MySQLOptimizedTest]:
        return [
            MySQLOptimizedTest(
                name="random_pk_updates",
                description="Random updates to primary key order",
                why_mysql_faster="THIS IS WHERE MYSQL IS SLOWER - included for contrast",
                mysql_advantage_factor="0.2-0.5x (MySQL is SLOWER)",
                setup_sql="""
CREATE TABLE random_updates (
    id INT PRIMARY KEY,
    data VARCHAR(1000),
    counter INT DEFAULT 0
) ENGINE=InnoDB;

-- Insert 1M rows
INSERT INTO random_updates (id, data)
SELECT seq, REPEAT('x', 500)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 1000000) x;
                """,
                test_sql="""
-- Update rows in random order
-- MySQL: Clustered index means random page access, fragmentation
-- PostgreSQL: Heap update creates new tuple, HOT possible
-- Firebird: Record versions, efficient update-in-place
UPDATE random_updates
SET counter = counter + 1
WHERE id = ?;
-- Execute for 100K random ids
                """,
                expected_mysql_pattern="Random I/O on clustered index, fragmentation",
                expected_pg_pattern="HOT updates (Heap Only Tuple) - very efficient",
                expected_fb_pattern="Delta record creation, minimal page impact"
            ),

            MySQLOptimizedTest(
                name="non_covering_secondary_scan",
                description="Secondary index scan requiring heap lookup",
                why_mysql_faster="THIS IS WHERE MYSQL IS SLOWER - included for contrast",
                mysql_advantage_factor="0.5-0.8x (MySQL is SLOWER)",
                setup_sql="""
CREATE TABLE wide_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    lookup_key INT NOT NULL,
    large_payload TEXT,  # Not in secondary index
    INDEX idx_lookup (lookup_key)
) ENGINE=InnoDB;

INSERT INTO wide_table (lookup_key, large_payload)
SELECT seq % 10000, REPEAT('data', 1000)
FROM (SELECT @r := @r + 1 as seq FROM (SELECT @r := 0) init,
    information_schema.columns LIMIT 1000000) x;
                """,
                test_sql="""
-- Query needs columns not in secondary index
-- MySQL: PK lookups from secondary index (double lookup)
-- PostgreSQL: Index scan + heap scan (similar cost)
-- Firebird: Record pointer direct access
SELECT * FROM wide_table
WHERE lookup_key BETWEEN 1000 AND 2000;
                """,
                expected_mysql_pattern="Index lookup + PK bookmark lookup",
                expected_pg_pattern="Index scan + heap fetch",
                expected_fb_pattern="Index scan + direct record fetch"
            ),
        ]


def get_all_tests() -> List[MySQLOptimizedTest]:
    """Get all MySQL-optimized tests."""
    tests = []
    tests.extend(MySQLClusteredPKTests.get_all_tests())
    tests.extend(MySQLBufferPoolTests.get_all_tests())
    tests.extend(MySQLWriteOptimizationTests.get_all_tests())
    tests.extend(MySQLWeaknessTests.get_all_tests())
    return tests


if __name__ == '__main__':
    tests = get_all_tests()
    print(f"MySQL-Optimized Tests: {len(tests)}")
    print()

    advantage_tests = [t for t in tests if not t.name.startswith("random_") and not t.name.startswith("non_covering_")]
    weakness_tests = [t for t in tests if t.name.startswith("random_") or t.name.startswith("non_covering_")]

    print(f"  MySQL Strengths: {len(advantage_tests)} tests")
    for t in advantage_tests:
        print(f"    - {t.name}: {t.mysql_advantage_factor} faster")

    print(f"\n  MySQL Weaknesses (for contrast): {len(weakness_tests)} tests")
    for t in weakness_tests:
        print(f"    - {t.name}: {t.mysql_advantage_factor} (MySQL slower)")
