#!/usr/bin/env python3
"""
JOIN Stress Test Scenarios

Comprehensive tests for all JOIN types with verification:
- INNER JOIN
- LEFT JOIN / LEFT OUTER JOIN
- RIGHT JOIN / RIGHT OUTER JOIN
- FULL JOIN / FULL OUTER JOIN
- CROSS JOIN
- SELF JOIN
- Multi-table JOINs (3+ tables)
- Complex JOINs with WHERE, GROUP BY, aggregates
"""

from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass
class JoinTest:
    """Definition of a JOIN stress test."""
    name: str
    description: str
    sql: str
    verification_sql: Optional[str] = None  # Query to verify correctness
    expected_min_rows: Optional[int] = None  # Minimum expected rows
    expected_max_rows: Optional[int] = None  # Maximum expected rows
    timeout_seconds: int = 300  # Default 5 minute timeout


class JoinStressTests:
    """Collection of JOIN stress test scenarios."""

    @staticmethod
    def get_all_tests() -> List[JoinTest]:
        """Get all JOIN stress tests."""
        tests = []

        # Basic JOINs
        tests.extend(JoinStressTests._inner_join_tests())
        tests.extend(JoinStressTests._outer_join_tests())
        tests.extend(JoinStressTests._cross_join_tests())
        tests.extend(JoinStressTests._self_join_tests())

        # Complex JOINs
        tests.extend(JoinStressTests._multi_table_join_tests())
        tests.extend(JoinStressTests._complex_condition_tests())
        tests.extend(JoinStressTests._aggregate_join_tests())
        tests.extend(JoinStressTests._subquery_join_tests())

        return tests

    @staticmethod
    def _inner_join_tests() -> List[JoinTest]:
        """INNER JOIN test scenarios."""
        return [
            JoinTest(
                name="inner_join_simple",
                description="Simple INNER JOIN between orders and customers",
                sql="""
                    SELECT o.order_id, o.order_date, c.first_name, c.last_name, c.email
                    FROM orders o
                    INNER JOIN customers c ON o.customer_id = c.customer_id
                """,
                verification_sql="SELECT COUNT(*) FROM orders",  # Should match orders count
                timeout_seconds=60,
            ),
            JoinTest(
                name="inner_join_multiple_conditions",
                description="INNER JOIN with additional WHERE conditions",
                sql="""
                    SELECT o.order_id, o.total_amount, c.country_code
                    FROM orders o
                    INNER JOIN customers c ON o.customer_id = c.customer_id
                    WHERE o.order_date >= '2023-01-01'
                      AND o.total_amount > 1000
                      AND c.country_code IN ('US', 'CA', 'MX')
                """,
                timeout_seconds=120,
            ),
            JoinTest(
                name="inner_join_products_orders",
                description="INNER JOIN finding products that have been ordered",
                sql="""
                    SELECT DISTINCT p.product_id, p.name, p.category
                    FROM products p
                    INNER JOIN order_items oi ON p.product_id = oi.product_id
                    WHERE p.is_active = 1
                    ORDER BY p.category, p.name
                """,
                timeout_seconds=180,
            ),
            JoinTest(
                name="inner_join_with_expression",
                description="INNER JOIN with expression in ON clause",
                sql="""
                    SELECT o.order_id, oi.item_id,
                           oi.quantity * oi.unit_price * (1 - oi.discount_pct/100) as line_total
                    FROM orders o
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                        AND oi.discount_pct > 0
                """,
                timeout_seconds=120,
            ),
            JoinTest(
                name="inner_join_large_result",
                description="INNER JOIN returning millions of rows",
                sql="""
                    SELECT oi.*, o.order_date, o.status, c.first_name, c.last_name
                    FROM order_items oi
                    INNER JOIN orders o ON oi.order_id = o.order_id
                    INNER JOIN customers c ON o.customer_id = c.customer_id
                """,
                timeout_seconds=600,
            ),
        ]

    @staticmethod
    def _outer_join_tests() -> List[JoinTest]:
        """OUTER JOIN test scenarios."""
        return [
            JoinTest(
                name="left_join_all_customers",
                description="LEFT JOIN to find all customers and their orders (including those without)",
                sql="""
                    SELECT c.customer_id, c.first_name, c.last_name,
                           COUNT(o.order_id) as order_count,
                           COALESCE(SUM(o.total_amount), 0) as total_spent
                    FROM customers c
                    LEFT JOIN orders o ON c.customer_id = o.customer_id
                    GROUP BY c.customer_id, c.first_name, c.last_name
                """,
                expected_min_rows=1,  # Should return all customers
                timeout_seconds=180,
            ),
            JoinTest(
                name="left_join_recent_orders",
                description="LEFT JOIN with date filter in ON clause vs WHERE",
                sql="""
                    SELECT c.customer_id, c.first_name, o.order_id, o.order_date
                    FROM customers c
                    LEFT JOIN orders o ON c.customer_id = o.customer_id
                        AND o.order_date >= '2024-01-01'
                    WHERE c.registration_date <= '2023-12-31'
                """,
                timeout_seconds=120,
            ),
            JoinTest(
                name="right_join_variant",
                description="RIGHT JOIN (same as reversed LEFT JOIN)",
                sql="""
                    SELECT o.order_id, o.order_date, c.customer_id, c.email
                    FROM orders o
                    RIGHT JOIN customers c ON o.customer_id = c.customer_id
                    WHERE c.account_balance > 1000
                """,
                timeout_seconds=120,
            ),
            JoinTest(
                name="full_outer_join_all_data",
                description="FULL OUTER JOIN to find all records from both tables",
                sql="""
                    SELECT c.customer_id as cust_id, c.first_name,
                           o.order_id, o.total_amount,
                           CASE
                               WHEN c.customer_id IS NULL THEN 'Orphan Order'
                               WHEN o.order_id IS NULL THEN 'No Orders'
                               ELSE 'Has Orders'
                           END as status
                    FROM customers c
                    FULL OUTER JOIN orders o ON c.customer_id = o.customer_id
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="outer_join_chain",
                description="Chain of LEFT JOINs",
                sql="""
                    SELECT c.customer_id, c.first_name,
                           o.order_id, o.order_date,
                           oi.item_id, oi.quantity, p.name as product_name
                    FROM customers c
                    LEFT JOIN orders o ON c.customer_id = o.customer_id
                    LEFT JOIN order_items oi ON o.order_id = oi.order_id
                    LEFT JOIN products p ON oi.product_id = p.product_id
                    WHERE c.country_code = 'US'
                """,
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _cross_join_tests() -> List[JoinTest]:
        """CROSS JOIN test scenarios."""
        return [
            JoinTest(
                name="cross_join_limited",
                description="CROSS JOIN with limiting WHERE clause",
                sql="""
                    SELECT c.customer_id, p.product_id, p.price
                    FROM customers c
                    CROSS JOIN products p
                    WHERE c.customer_id <= 100  -- Limit to prevent explosion
                      AND p.is_active = 1
                """,
                expected_max_rows=100000,  # Should be limited
                timeout_seconds=60,
            ),
            JoinTest(
                name="cross_join_with_agg",
                description="CROSS JOIN with aggregation (cartesian product analysis)",
                sql="""
                    SELECT c.country_code, p.category,
                           COUNT(*) as combinations,
                           AVG(p.price) as avg_price
                    FROM customers c
                    CROSS JOIN products p
                    GROUP BY c.country_code, p.category
                """,
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _self_join_tests() -> List[JoinTest]:
        """SELF JOIN test scenarios."""
        return [
            JoinTest(
                name="self_join_same_country",
                description="SELF JOIN to find customer pairs in same country",
                sql="""
                    SELECT c1.customer_id as customer1_id,
                           c1.first_name as customer1_name,
                           c2.customer_id as customer2_id,
                           c2.first_name as customer2_name,
                           c1.country_code
                    FROM customers c1
                    INNER JOIN customers c2 ON c1.country_code = c2.country_code
                        AND c1.customer_id < c2.customer_id
                    WHERE c1.registration_date >= '2024-01-01'
                    LIMIT 10000
                """,
                timeout_seconds=180,
            ),
            JoinTest(
                name="self_join_hierarchy",
                description="SELF JOIN simulating hierarchical relationship",
                sql="""
                    SELECT o1.order_id as original_order, o1.order_date,
                           o2.order_id as related_order, o2.total_amount
                    FROM orders o1
                    INNER JOIN orders o2 ON o1.customer_id = o2.customer_id
                        AND o1.order_id <> o2.order_id
                        AND ABS(DATEDIFF(day, o1.order_date, o2.order_date)) <= 7
                    WHERE o1.total_amount > 5000
                    LIMIT 5000
                """,
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _multi_table_join_tests() -> List[JoinTest]:
        """Multi-table JOIN test scenarios (3+ tables)."""
        return [
            JoinTest(
                name="four_table_join",
                description="JOIN all four tables together",
                sql="""
                    SELECT
                        c.customer_id, c.first_name, c.last_name,
                        o.order_id, o.order_date, o.total_amount,
                        oi.quantity, oi.unit_price,
                        p.product_id, p.name as product_name, p.category
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    WHERE o.order_date >= '2024-06-01'
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="four_table_join_mixed_outer",
                description="JOIN with mix of INNER and LEFT joins",
                sql="""
                    SELECT
                        c.customer_id, c.country_code,
                        o.order_id, o.status,
                        oi.item_id,
                        p.name as product_name
                    FROM customers c
                    LEFT JOIN orders o ON c.customer_id = o.customer_id
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    LEFT JOIN products p ON oi.product_id = p.product_id
                    WHERE c.account_balance > 0
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="three_table_agg_join",
                description="3-table JOIN with aggregation",
                sql="""
                    SELECT
                        c.country_code,
                        p.category,
                        COUNT(DISTINCT o.order_id) as total_orders,
                        SUM(oi.quantity) as total_quantity,
                        SUM(oi.quantity * oi.unit_price) as total_revenue
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    GROUP BY c.country_code, p.category
                    HAVING SUM(oi.quantity * oi.unit_price) > 10000
                    ORDER BY total_revenue DESC
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="five_table_join_simulation",
                description="JOIN with derived tables (simulating 5+ tables)",
                sql="""
                    SELECT
                        c.customer_id,
                        cat_summary.category,
                        cat_summary.order_count,
                        cat_summary.total_spent,
                        c.account_balance
                    FROM customers c
                    INNER JOIN (
                        SELECT
                            o.customer_id,
                            p.category,
                            COUNT(DISTINCT o.order_id) as order_count,
                            SUM(oi.quantity * oi.unit_price) as total_spent
                        FROM orders o
                        INNER JOIN order_items oi ON o.order_id = oi.order_id
                        INNER JOIN products p ON oi.product_id = p.product_id
                        GROUP BY o.customer_id, p.category
                    ) cat_summary ON c.customer_id = cat_summary.customer_id
                    WHERE cat_summary.total_spent > c.account_balance
                """,
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def _complex_condition_tests() -> List[JoinTest]:
        """JOINs with complex conditions."""
        return [
            JoinTest(
                name="join_with_or_conditions",
                description="JOIN with OR conditions in ON clause",
                sql="""
                    SELECT o.order_id, o.total_amount, c.customer_id, c.email
                    FROM orders o
                    INNER JOIN customers c ON o.customer_id = c.customer_id
                    WHERE (o.total_amount > 10000 OR o.shipping_cost > 100)
                      AND (c.account_balance < 0 OR c.country_code = 'US')
                """,
                timeout_seconds=180,
            ),
            JoinTest(
                name="join_with_between",
                description="JOIN with BETWEEN and range conditions",
                sql="""
                    SELECT o.order_id, o.order_date, o.total_amount,
                           p.name, p.price, oi.quantity
                    FROM orders o
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    WHERE o.order_date BETWEEN '2024-01-01' AND '2024-12-31'
                      AND p.price BETWEEN 100 AND 1000
                      AND oi.quantity BETWEEN 5 AND 50
                """,
                timeout_seconds=180,
            ),
            JoinTest(
                name="join_with_case",
                description="JOIN with CASE expressions",
                sql="""
                    SELECT
                        c.customer_id,
                        c.first_name,
                        o.order_id,
                        CASE
                            WHEN o.total_amount < 100 THEN 'Small'
                            WHEN o.total_amount < 1000 THEN 'Medium'
                            ELSE 'Large'
                        END as order_size,
                        CASE c.country_code
                            WHEN 'US' THEN 'North America'
                            WHEN 'CA' THEN 'North America'
                            WHEN 'MX' THEN 'North America'
                            ELSE 'International'
                        END as region
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                """,
                timeout_seconds=180,
            ),
        ]

    @staticmethod
    def _aggregate_join_tests() -> List[JoinTest]:
        """JOINs with aggregations."""
        return [
            JoinTest(
                name="join_with_rollup",
                description="JOIN with ROLLUP for subtotals",
                sql="""
                    SELECT
                        COALESCE(c.country_code, 'ALL') as country,
                        COALESCE(p.category, 'ALL') as category,
                        COUNT(*) as line_items,
                        SUM(oi.quantity) as total_qty,
                        SUM(oi.quantity * oi.unit_price) as revenue
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    GROUP BY ROLLUP(c.country_code, p.category)
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="join_with_window_functions",
                description="JOIN with window functions for rankings",
                sql="""
                    SELECT
                        c.customer_id,
                        c.first_name,
                        o.order_id,
                        o.total_amount,
                        RANK() OVER (PARTITION BY c.customer_id ORDER BY o.total_amount DESC) as amount_rank,
                        SUM(o.total_amount) OVER (PARTITION BY c.customer_id) as customer_total,
                        AVG(o.total_amount) OVER (PARTITION BY c.country_code) as country_avg
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    WHERE o.order_date >= '2024-01-01'
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="join_complex_aggregation",
                description="Complex aggregation with multiple joins",
                sql="""
                    SELECT
                        DATE_TRUNC('month', o.order_date) as month,
                        p.category,
                        COUNT(DISTINCT c.customer_id) as unique_customers,
                        COUNT(DISTINCT o.order_id) as order_count,
                        SUM(oi.quantity) as units_sold,
                        SUM(oi.quantity * oi.unit_price) as gross_revenue,
                        SUM(oi.quantity * (oi.unit_price - p.cost)) as gross_profit,
                        AVG(oi.quantity * oi.unit_price) as avg_line_value,
                        PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY oi.quantity * oi.unit_price) as median_line_value
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    INNER JOIN order_items oi ON o.order_id = oi.order_id
                    INNER JOIN products p ON oi.product_id = p.product_id
                    WHERE o.status = 'delivered'
                    GROUP BY DATE_TRUNC('month', o.order_date), p.category
                    HAVING COUNT(DISTINCT o.order_id) >= 10
                    ORDER BY month DESC, gross_revenue DESC
                """,
                timeout_seconds=600,
            ),
        ]

    @staticmethod
    def _subquery_join_tests() -> List[JoinTest]:
        """JOINs with subqueries."""
        return [
            JoinTest(
                name="join_with_correlated_subquery",
                description="JOIN with correlated subquery",
                sql="""
                    SELECT
                        c.customer_id,
                        c.first_name,
                        c.last_name,
                        (SELECT COUNT(*) FROM orders o WHERE o.customer_id = c.customer_id) as order_count,
                        (SELECT COALESCE(SUM(total_amount), 0) FROM orders o WHERE o.customer_id = c.customer_id) as lifetime_value
                    FROM customers c
                    WHERE c.registration_date >= '2023-01-01'
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="join_with_derived_table",
                description="JOIN with derived table (inline view)",
                sql="""
                    SELECT
                        c.customer_id,
                        c.email,
                        order_stats.total_orders,
                        order_stats.avg_order_value,
                        order_stats.max_order_value
                    FROM customers c
                    INNER JOIN (
                        SELECT
                            customer_id,
                            COUNT(*) as total_orders,
                            AVG(total_amount) as avg_order_value,
                            MAX(total_amount) as max_order_value
                        FROM orders
                        WHERE order_date >= '2024-01-01'
                        GROUP BY customer_id
                        HAVING COUNT(*) >= 5
                    ) order_stats ON c.customer_id = order_stats.customer_id
                    ORDER BY order_stats.total_orders DESC
                """,
                timeout_seconds=300,
            ),
            JoinTest(
                name="join_with_cte",
                description="JOIN with Common Table Expression (CTE)",
                sql="""
                    WITH top_customers AS (
                        SELECT
                            customer_id,
                            SUM(total_amount) as total_spent,
                            RANK() OVER (ORDER BY SUM(total_amount) DESC) as spend_rank
                        FROM orders
                        WHERE order_date >= '2024-01-01'
                        GROUP BY customer_id
                    ),
                    customer_categories AS (
                        SELECT
                            customer_id,
                            CASE
                                WHEN total_spent > 50000 THEN 'VIP'
                                WHEN total_spent > 10000 THEN 'Premium'
                                ELSE 'Standard'
                            END as segment
                        FROM top_customers
                        WHERE spend_rank <= 1000
                    )
                    SELECT
                        c.customer_id,
                        c.first_name,
                        c.last_name,
                        c.country_code,
                        cc.segment
                    FROM customers c
                    INNER JOIN customer_categories cc ON c.customer_id = cc.customer_id
                    ORDER BY cc.segment, c.customer_id
                """,
                timeout_seconds=300,
            ),
        ]

    @staticmethod
    def get_test_by_name(name: str) -> Optional[JoinTest]:
        """Get a specific test by name."""
        for test in JoinStressTests.get_all_tests():
            if test.name == name:
                return test
        return None


if __name__ == "__main__":
    # List all tests
    tests = JoinStressTests.get_all_tests()
    print(f"Total JOIN stress tests: {len(tests)}")
    print()

    # Group by category
    categories = {
        "INNER JOIN": [t for t in tests if "inner" in t.name],
        "OUTER JOIN": [t for t in tests if "outer" in t.name or "left" in t.name or "right" in t.name],
        "CROSS JOIN": [t for t in tests if "cross" in t.name],
        "SELF JOIN": [t for t in tests if "self" in t.name],
        "Multi-table": [t for t in tests if "multi" in t.name or "four" in t.name or "three" in t.name or "five" in t.name],
        "Complex conditions": [t for t in tests if "complex" in t.name or "or_conditions" in t.name or "between" in t.name],
        "Aggregations": [t for t in tests if "agg" in t.name or "window" in t.name or "rollup" in t.name],
        "Subqueries": [t for t in tests if "subquery" in t.name or "derived" in t.name or "cte" in t.name],
    }

    for category, cat_tests in categories.items():
        print(f"\n{category}: {len(cat_tests)} tests")
        for t in cat_tests:
            print(f"  - {t.name}: {t.description[:60]}...")
