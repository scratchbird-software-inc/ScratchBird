#!/usr/bin/env python3
"""
Dialect-Aware Stress Test Scenarios

Test definitions that generate engine-specific SQL.
"""

from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional

from generators.sql_dialect import (
    SQLDialectFactory,
    StressTestSQLGenerator,
    get_dialect_specific_sql
)


@dataclass
class DialectAwareTest:
    """Test definition with dialect-specific SQL generation."""
    name: str
    description: str
    sql_generator: Callable[[StressTestSQLGenerator], str]
    verification_sql: Optional[str] = None
    expected_min_rows: Optional[int] = None
    expected_max_rows: Optional[int] = None
    timeout_seconds: int = 300


class DialectAwareJoinTests:
    """JOIN tests with dialect-specific SQL generation."""

    @staticmethod
    def get_all_tests() -> List[DialectAwareTest]:
        """Get all dialect-aware JOIN tests."""
        tests = []

        # INNER JOIN tests
        tests.append(DialectAwareTest(
            name="inner_join_simple",
            description="Simple INNER JOIN between orders and customers",
            sql_generator=lambda g: g.inner_join_simple(),
            timeout_seconds=60,
        ))

        tests.append(DialectAwareTest(
            name="inner_join_large_result",
            description="INNER JOIN returning millions of rows",
            sql_generator=lambda g: g.inner_join_large_result(),
            timeout_seconds=600,
        ))

        tests.append(DialectAwareTest(
            name="inner_join_multiple_conditions",
            description="INNER JOIN with additional WHERE conditions",
            sql_generator=lambda g: f"""
                SELECT o.order_id, o.total_amount, c.country_code
                FROM orders o
                INNER JOIN customers c ON o.customer_id = c.customer_id
                WHERE o.order_date >= '2023-01-01'
                  AND o.total_amount > 1000
                  AND c.country_code IN ('US', 'CA', 'MX')
            """,
            timeout_seconds=120,
        ))

        # LEFT JOIN tests
        tests.append(DialectAwareTest(
            name="left_join_all_customers",
            description="LEFT JOIN to find all customers and their orders",
            sql_generator=lambda g: g.left_join_all_customers(),
            timeout_seconds=180,
        ))

        # Multi-table JOIN
        tests.append(DialectAwareTest(
            name="four_table_join",
            description="JOIN all four tables together",
            sql_generator=lambda g: g.four_table_join(),
            timeout_seconds=300,
        ))

        # Self JOIN
        tests.append(DialectAwareTest(
            name="self_join_same_country",
            description="SELF JOIN to find customer pairs in same country",
            sql_generator=lambda g: g.self_join_same_country(),
            timeout_seconds=180,
        ))

        # Aggregation JOIN
        tests.append(DialectAwareTest(
            name="aggregation_daily_sales",
            description="Aggregation with date truncation",
            sql_generator=lambda g: g.aggregation_daily_sales(),
            timeout_seconds=300,
        ))

        tests.append(DialectAwareTest(
            name="window_function_ranking",
            description="JOIN with window functions for rankings",
            sql_generator=lambda g: g.window_function_ranking(),
            timeout_seconds=300,
        ))

        tests.append(DialectAwareTest(
            name="multi_dimensional_agg",
            description="Multi-dimensional aggregation (OLAP-style)",
            sql_generator=lambda g: g.multi_dimensional_agg(),
            timeout_seconds=300,
        ))

        return tests


class DialectAwareBulkTests:
    """Bulk operation tests with dialect-specific SQL."""

    @staticmethod
    def get_all_tests() -> List[DialectAwareTest]:
        """Get all dialect-aware bulk tests."""
        tests = []

        # Bulk INSERT
        tests.append(DialectAwareTest(
            name="bulk_insert_select",
            description="INSERT by selecting from large table",
            sql_generator=lambda g: g.bulk_insert_select(),
            timeout_seconds=300,
        ))

        # Bulk UPDATE
        tests.append(DialectAwareTest(
            name="bulk_update_with_case",
            description="Bulk UPDATE with CASE expressions",
            sql_generator=lambda g: g.bulk_update_with_case(),
            timeout_seconds=300,
        ))

        tests.append(DialectAwareTest(
            name="bulk_update_with_join",
            description="UPDATE using subquery (JOIN alternative)",
            sql_generator=lambda g: """
                UPDATE orders
                SET total_amount = total_amount * 0.95
                WHERE customer_id IN (
                    SELECT customer_id
                    FROM customers
                    WHERE account_balance > 10000
                )
            """,
            timeout_seconds=300,
        ))

        # Aggregation stress tests
        def agg_full_table_scan(g: StressTestSQLGenerator) -> str:
            d = g.d
            return f"""
                SELECT
                    p.category,
                    COUNT(*) as item_count,
                    SUM(oi.quantity) as total_qty,
                    AVG(oi.unit_price) as avg_price,
                    MIN(oi.unit_price) as min_price,
                    MAX(oi.unit_price) as max_price,
                    {d.stddev("oi.unit_price")} as price_stddev
                FROM order_items oi
                INNER JOIN products p ON oi.product_id = p.product_id
                GROUP BY p.category
                HAVING COUNT(*) > 1000
                ORDER BY item_count DESC
            """

        tests.append(DialectAwareTest(
            name="agg_full_table_scan",
            description="Full table aggregation",
            sql_generator=agg_full_table_scan,
            timeout_seconds=300,
        ))

        # Distinct counts
        tests.append(DialectAwareTest(
            name="agg_distinct_counts",
            description="Count distinct values across large dataset",
            sql_generator=lambda g: """
                SELECT
                    COUNT(DISTINCT c.customer_id) as unique_customers,
                    COUNT(DISTINCT o.order_id) as unique_orders,
                    COUNT(DISTINCT p.product_id) as unique_products,
                    COUNT(DISTINCT c.country_code) as countries,
                    COUNT(DISTINCT p.category) as categories
                FROM orders o
                INNER JOIN customers c ON o.customer_id = c.customer_id
                INNER JOIN order_items oi ON o.order_id = oi.order_id
                INNER JOIN products p ON oi.product_id = p.product_id
            """,
            timeout_seconds=300,
        ))

        # Nested subqueries
        tests.append(DialectAwareTest(
            name="nested_subquery_agg",
            description="Nested aggregation with subqueries",
            sql_generator=lambda g: g.nested_subquery_agg(),
            timeout_seconds=300,
        ))

        return tests


def get_tests_for_engine(engine: str) -> Dict[str, Dict[str, Any]]:
    """
    Get all tests with engine-specific SQL.

    Returns a dict mapping test_name -> test_definition with 'sql' key.
    """
    dialect = SQLDialectFactory.get_dialect(engine)
    gen = StressTestSQLGenerator(dialect)

    all_tests = []
    all_tests.extend(DialectAwareJoinTests.get_all_tests())
    all_tests.extend(DialectAwareBulkTests.get_all_tests())

    result = {}
    for test in all_tests:
        try:
            sql = test.sql_generator(gen)
            result[test.name] = {
                'name': test.name,
                'description': test.description,
                'sql': sql,
                'timeout_seconds': test.timeout_seconds,
                'expected_min_rows': test.expected_min_rows,
                'expected_max_rows': test.expected_max_rows,
            }
        except Exception as e:
            print(f"Warning: Could not generate SQL for {test.name}: {e}")

    return result


if __name__ == '__main__':
    # Test dialect-aware generation
    for engine in ['firebird', 'mysql', 'postgresql']:
        print(f"\n{'='*60}")
        print(f"Engine: {engine.upper()}")
        print(f"{'='*60}")

        tests = get_tests_for_engine(engine)
        print(f"\nGenerated {len(tests)} tests")

        # Show sample SQL
        if 'left_join_all_customers' in tests:
            print("\n-- left_join_all_customers:")
            print(tests['left_join_all_customers']['sql'][:500] + "...")

        if 'aggregation_daily_sales' in tests:
            print("\n-- aggregation_daily_sales:")
            print(tests['aggregation_daily_sales']['sql'][:500] + "...")
