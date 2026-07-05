#!/usr/bin/env python3
"""
Bulk Operation Stress Tests

Tests for:
- Bulk INSERT operations (millions of rows)
- Bulk UPDATE with complex WHERE
- Bulk DELETE with verification
- Mixed read/write workloads
- Concurrent operations simulation
"""

from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass
class BulkTest:
    """Definition of a bulk operation test."""
    name: str
    description: str
    setup_sql: Optional[str] = None
    operation_sql: str = ""
    verification_sql: Optional[str] = None
    expected_rows_affected: Optional[int] = None
    timeout_seconds: int = 600
    iterations: int = 1  # Number of times to repeat


class BulkOperationTests:
    """Collection of bulk operation stress tests."""

    @staticmethod
    def get_all_tests() -> List[BulkTest]:
        """Get all bulk operation tests."""
        tests = []

        # Bulk INSERT tests
        tests.extend(BulkOperationTests._bulk_insert_tests())

        # Bulk UPDATE tests
        tests.extend(BulkOperationTests._bulk_update_tests())

        # Bulk DELETE tests
        tests.extend(BulkOperationTests._bulk_delete_tests())

        # Mixed workload tests
        tests.extend(BulkOperationTests._mixed_workload_tests())

        # Aggregation stress tests
        tests.extend(BulkOperationTests._aggregation_tests())

        return tests

    @staticmethod
    def _bulk_insert_tests() -> List[BulkTest]:
        """Bulk INSERT test scenarios."""
        return [
            BulkTest(
                name="bulk_insert_single_transaction",
                description="INSERT 100K rows in single transaction",
                setup_sql="CREATE TABLE IF NOT EXISTS bulk_insert_test (id INT PRIMARY KEY, data VARCHAR(100), value DECIMAL(10,2))",
                operation_sql="""
                    INSERT INTO bulk_insert_test (id, data, value)
                    SELECT
                        seq as id,
                        'Data_' || seq as data,
                        (seq * 1.5) as value
                    FROM (
                        SELECT ROW_NUMBER() OVER () as seq
                        FROM orders o CROSS JOIN order_items oi
                        LIMIT 100000
                    ) sub
                """,
                verification_sql="SELECT COUNT(*) FROM bulk_insert_test",
                expected_rows_affected=100000,
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_insert_select_from_large_table",
                description="INSERT by selecting from large table with JOIN",
                setup_sql="CREATE TABLE IF NOT EXISTS order_summary AS SELECT * FROM orders WHERE 1=0",
                operation_sql="""
                    INSERT INTO order_summary
                    SELECT o.*
                    FROM orders o
                    INNER JOIN customers c ON o.customer_id = c.customer_id
                    WHERE o.total_amount > 100
                """,
                verification_sql="SELECT COUNT(*) FROM order_summary",
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_insert_with_agg",
                description="INSERT aggregated data into summary table",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS daily_sales_summary (
                        sale_date DATE,
                        category VARCHAR(50),
                        total_orders BIGINT,
                        total_revenue DECIMAL(15,2),
                        avg_order_value DECIMAL(10,2)
                    )
                """,
                operation_sql="""
                    INSERT INTO daily_sales_summary
                    SELECT
                        CAST(o.order_date AS DATE) as sale_date,
                        p.category,
                        COUNT(DISTINCT o.order_id) as total_orders,
                        SUM(oi.quantity * oi.unit_price) as total_revenue,
                        AVG(oi.quantity * oi.unit_price) as avg_order_value
                    FROM orders o
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    GROUP BY CAST(o.order_date AS DATE), p.category
                """,
                verification_sql="SELECT COUNT(*) FROM daily_sales_summary",
                timeout_seconds=600,
            ),
        ]

    @staticmethod
    def _bulk_update_tests() -> List[BulkTest]:
        """Bulk UPDATE test scenarios."""
        return [
            BulkTest(
                name="bulk_update_simple",
                description="UPDATE millions of rows with simple condition",
                operation_sql="""
                    UPDATE orders
                    SET shipping_cost = shipping_cost * 1.1
                    WHERE order_date >= '2024-01-01'
                      AND status = 'pending'
                """,
                verification_sql="SELECT COUNT(*) FROM orders WHERE order_date >= '2024-01-01' AND status = 'pending'",
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_update_with_join",
                description="UPDATE with JOIN subquery",
                operation_sql="""
                    UPDATE orders
                    SET total_amount = total_amount * 0.95
                    WHERE customer_id IN (
                        SELECT customer_id
                        FROM customers
                        WHERE account_balance > 10000
                    )
                """,
                verification_sql="SELECT COUNT(*) FROM orders o JOIN customers c ON o.customer_id = c.customer_id WHERE c.account_balance > 10000",
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_update_complex_calculation",
                description="UPDATE with complex calculations",
                operation_sql="""
                    UPDATE order_items
                    SET discount_pct = CASE
                        WHEN quantity >= 50 THEN 20.0
                        WHEN quantity >= 20 THEN 15.0
                        WHEN quantity >= 10 THEN 10.0
                        ELSE 5.0
                    END
                    WHERE discount_pct < 5.0 OR discount_pct IS NULL
                """,
                verification_sql="SELECT COUNT(*) FROM order_items WHERE discount_pct >= 5.0",
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_update_with_correlated_subquery",
                description="UPDATE using correlated subquery for derived values",
                operation_sql="""
                    UPDATE orders o
                    SET total_amount = (
                        SELECT COALESCE(SUM(quantity * unit_price * (1 - discount_pct/100)), 0)
                        FROM order_items oi
                        WHERE oi.order_id = o.order_id
                    )
                    WHERE status = 'pending'
                """,
                verification_sql="SELECT COUNT(*) FROM orders WHERE status = 'pending'",
                timeout_seconds=600,
            ),
        ]

    @staticmethod
    def _bulk_delete_tests() -> List[BulkTest]:
        """Bulk DELETE test scenarios."""
        return [
            BulkTest(
                name="bulk_delete_old_records",
                description="DELETE old records with date filter",
                setup_sql="CREATE TABLE IF NOT EXISTS orders_archive AS SELECT * FROM orders WHERE 1=0",
                operation_sql="""
                    DELETE FROM orders
                    WHERE order_date < '2022-01-01'
                      AND status = 'delivered'
                """,
                verification_sql="SELECT COUNT(*) FROM orders WHERE order_date < '2022-01-01' AND status = 'delivered'",
                expected_rows_affected=0,
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_delete_with_join",
                description="DELETE using JOIN with another table",
                operation_sql="""
                    DELETE FROM order_items
                    WHERE order_id IN (
                        SELECT o.order_id
                        FROM orders o
                        JOIN customers c ON o.customer_id = c.customer_id
                        WHERE c.country_code = 'XX'
                    )
                """,
                verification_sql="SELECT COUNT(*) FROM order_items oi JOIN orders o ON oi.order_id = o.order_id JOIN customers c ON o.customer_id = c.customer_id WHERE c.country_code = 'XX'",
                expected_rows_affected=0,
                timeout_seconds=300,
            ),
            BulkTest(
                name="bulk_delete_cascade_simulation",
                description="DELETE orders and their items in sequence",
                operation_sql="""
                    DELETE FROM order_items
                    WHERE order_id IN (
                        SELECT order_id FROM orders
                        WHERE status = 'cancelled'
                    )
                """,
                verification_sql="SELECT COUNT(*) FROM order_items oi JOIN orders o ON oi.order_id = o.order_id WHERE o.status = 'cancelled'",
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _mixed_workload_tests() -> List[BulkTest]:
        """Mixed read/write workload tests."""
        return [
            BulkTest(
                name="mixed_workload_read_heavy",
                description="Read-heavy workload: 90% SELECT, 10% INSERT/UPDATE",
                setup_sql="CREATE TABLE IF NOT EXISTS workload_log (id INT PRIMARY KEY, operation VARCHAR(20), ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
                operation_sql="""
                    -- Simulate read-heavy workload with single query
                    SELECT 'read_heavy_simulation' as workload_type,
                           (SELECT COUNT(*) FROM orders) as order_count,
                           (SELECT AVG(total_amount) FROM orders) as avg_order,
                           (SELECT COUNT(*) FROM customers) as customer_count,
                           (SELECT SUM(stock_quantity) FROM products) as total_inventory
                """,
                verification_sql="SELECT 1",  # Just verify it ran
                timeout_seconds=60,
            ),
            BulkTest(
                name="mixed_workload_write_heavy",
                description="Write-heavy: Bulk INSERT followed by UPDATE",
                setup_sql="CREATE TABLE IF NOT EXISTS temp_transactions (id INT PRIMARY KEY, amt DECIMAL(10,2), processed INT DEFAULT 0)",
                operation_sql="""
                    INSERT INTO temp_transactions
                    SELECT order_id, total_amount, 0
                    FROM orders
                    WHERE order_date >= '2024-01-01'
                """,
                verification_sql="SELECT COUNT(*) FROM temp_transactions",
                timeout_seconds=300,
            ),
            BulkTest(
                name="mixed_workload_reporting_while_loading",
                description="Run complex report while data is being loaded",
                operation_sql="""
                    SELECT
                        DATE_TRUNC('month', o.order_date) as month,
                        c.country_code,
                        COUNT(*) as transaction_count,
                        SUM(o.total_amount) as revenue,
                        AVG(o.total_amount) as avg_transaction,
                        COUNT(DISTINCT c.customer_id) as unique_customers
                    FROM orders o
                    JOIN customers c ON o.customer_id = c.customer_id
                    GROUP BY DATE_TRUNC('month', o.order_date), c.country_code
                    ORDER BY month DESC, revenue DESC
                """,
                verification_sql="SELECT COUNT(*) FROM orders",
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _aggregation_tests() -> List[BulkTest]:
        """Aggregation and GROUP BY stress tests."""
        return [
            BulkTest(
                name="agg_full_table_scan",
                description="Full table aggregation without index",
                operation_sql="""
                    SELECT
                        p.category,
                        COUNT(*) as item_count,
                        SUM(oi.quantity) as total_qty,
                        AVG(oi.unit_price) as avg_price,
                        MIN(oi.unit_price) as min_price,
                        MAX(oi.unit_price) as max_price,
                        STDDEV(oi.unit_price) as price_stddev
                    FROM order_items oi
                    JOIN products p ON oi.product_id = p.product_id
                    GROUP BY p.category
                    HAVING COUNT(*) > 1000
                    ORDER BY item_count DESC
                """,
                verification_sql="SELECT COUNT(DISTINCT category) FROM products",
                timeout_seconds=300,
            ),
            BulkTest(
                name="agg_multiple_dimensions",
                description="Multi-dimensional aggregation (OLAP-style)",
                operation_sql="""
                    SELECT
                        EXTRACT(YEAR FROM o.order_date) as year,
                        EXTRACT(MONTH FROM o.order_date) as month,
                        c.country_code,
                        p.category,
                        COUNT(DISTINCT o.order_id) as orders,
                        SUM(oi.quantity) as units,
                        SUM(oi.quantity * oi.unit_price) as revenue,
                        AVG(oi.quantity * oi.unit_price) as avg_line
                    FROM orders o
                    JOIN customers c ON o.customer_id = c.customer_id
                    JOIN order_items oi ON o.order_id = oi.order_id
                    JOIN products p ON oi.product_id = p.product_id
                    GROUP BY
                        EXTRACT(YEAR FROM o.order_date),
                        EXTRACT(MONTH FROM o.order_date),
                        c.country_code,
                        p.category
                    ORDER BY year, month, revenue DESC
                """,
                verification_sql="SELECT COUNT(*) FROM orders",
                timeout_seconds=600,
            ),
            BulkTest(
                name="agg_distinct_count",
                description="Count distinct values across large dataset",
                operation_sql="""
                    SELECT
                        COUNT(DISTINCT c.customer_id) as unique_customers,
                        COUNT(DISTINCT o.order_id) as unique_orders,
                        COUNT(DISTINCT p.product_id) as unique_products,
                        COUNT(DISTINCT c.country_code) as countries,
                        COUNT(DISTINCT p.category) as categories,
                        COUNT(DISTINCT DATE_TRUNC('day', o.order_date)) as unique_dates
                    FROM orders o
                    JOIN customers c ON o.customer_id = c.customer_id
                    JOIN order_items oi ON o.order_id = oi.order_id
                    JOIN products p ON oi.product_id = p.product_id
                """,
                verification_sql="SELECT 1",
                timeout_seconds=300,
            ),
            BulkTest(
                name="agg_nested_subqueries",
                description="Nested aggregation with subqueries",
                operation_sql="""
                    SELECT
                        country_stats.country_code,
                        country_stats.customer_count,
                        country_stats.total_revenue,
                        global_stats.avg_revenue_per_country,
                        country_stats.total_revenue / global_stats.avg_revenue_per_country as revenue_ratio
                    FROM (
                        SELECT
                            c.country_code,
                            COUNT(DISTINCT c.customer_id) as customer_count,
                            SUM(o.total_amount) as total_revenue
                        FROM customers c
                        JOIN orders o ON c.customer_id = o.customer_id
                        GROUP BY c.country_code
                    ) country_stats
                    CROSS JOIN (
                        SELECT AVG(country_revenue) as avg_revenue_per_country
                        FROM (
                            SELECT SUM(total_amount) as country_revenue
                            FROM orders o
                            JOIN customers c ON o.customer_id = c.customer_id
                            GROUP BY c.country_code
                        ) sub
                    ) global_stats
                    ORDER BY country_stats.total_revenue DESC
                """,
                verification_sql="SELECT COUNT(DISTINCT country_code) FROM customers",
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def get_test_by_name(name: str) -> Optional[BulkTest]:
        """Get a specific test by name."""
        for test in BulkOperationTests.get_all_tests():
            if test.name == name:
                return test
        return None


if __name__ == "__main__":
    tests = BulkOperationTests.get_all_tests()
    print(f"Total bulk operation tests: {len(tests)}")
    print()

    categories = {
        "Bulk INSERT": [t for t in tests if "insert" in t.name],
        "Bulk UPDATE": [t for t in tests if "update" in t.name],
        "Bulk DELETE": [t for t in tests if "delete" in t.name],
        "Mixed Workload": [t for t in tests if "workload" in t.name or "mixed" in t.name],
        "Aggregation": [t for t in tests if "agg" in t.name],
    }

    for category, cat_tests in categories.items():
        print(f"\n{category}: {len(cat_tests)} tests")
        for t in cat_tests:
            print(f"  - {t.name}: {t.description[:60]}...")
