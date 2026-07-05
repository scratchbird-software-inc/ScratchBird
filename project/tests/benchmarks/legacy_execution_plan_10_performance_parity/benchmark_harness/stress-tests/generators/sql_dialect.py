#!/usr/bin/env python3
"""
SQL Dialect Adapter for Multi-Engine Support

Generates engine-specific SQL for:
- FirebirdSQL (dialect 3)
- MySQL (8.0+)
- PostgreSQL (15+)
- ScratchBird native SBsql

This ensures stress tests use native dialect for each engine.
"""

from abc import ABC, abstractmethod
from typing import Dict, List, Optional, Any


class SQLDialect(ABC):
    """Abstract base class for SQL dialects."""

    def __init__(self, engine: str):
        self.engine = engine

    @abstractmethod
    def create_table_customers(self) -> str:
        pass

    @abstractmethod
    def create_table_products(self) -> str:
        pass

    @abstractmethod
    def create_table_orders(self) -> str:
        pass

    @abstractmethod
    def create_table_order_items(self) -> str:
        pass

    @abstractmethod
    def date_trunc(self, field: str, expression: str) -> str:
        """Date truncation function."""
        pass

    @abstractmethod
    def date_extract(self, field: str, expression: str) -> str:
        """Date part extraction."""
        pass

    @abstractmethod
    def string_concat(self, *args) -> str:
        """String concatenation."""
        pass

    @abstractmethod
    def limit_clause(self, count: int, offset: Optional[int] = None) -> str:
        """LIMIT/OFFSET clause."""
        pass

    @abstractmethod
    def coalesce(self, *args) -> str:
        """COALESCE function."""
        pass

    @abstractmethod
    def stddev(self, expression: str) -> str:
        """Standard deviation function."""
        pass

    @abstractmethod
    def percentile_cont(self, fraction: float, ordering: str, within_group: str) -> str:
        """Continuous percentile (may not be supported in all engines)."""
        pass

    @abstractmethod
    def cast_as_date(self, expression: str) -> str:
        """Cast expression to DATE."""
        pass

    @abstractmethod
    def date_diff_days(self, date1: str, date2: str) -> str:
        """Difference between two dates in days."""
        pass

    @abstractmethod
    def generate_series(self, start: int, end: int) -> str:
        """Generate series of numbers (for test data)."""
        pass

    @abstractmethod
    def row_number(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        """ROW_NUMBER() window function."""
        pass

    @abstractmethod
    def rank(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        """RANK() window function."""
        pass

    @abstractmethod
    def get_placeholder(self) -> str:
        """Parameter placeholder (? or %s)."""
        pass


class FirebirdDialect(SQLDialect):
    """FirebirdSQL dialect (version 3.0+)."""

    def __init__(self):
        super().__init__("firebird")

    def create_table_customers(self) -> str:
        return """
            CREATE TABLE customers (
                customer_id BIGINT PRIMARY KEY,
                first_name VARCHAR(50),
                last_name VARCHAR(50),
                email VARCHAR(100) UNIQUE,
                phone VARCHAR(20),
                registration_date DATE,
                country_code VARCHAR(2),
                account_balance DECIMAL(12, 2)
            )
        """

    def create_table_products(self) -> str:
        return """
            CREATE TABLE products (
                product_id BIGINT PRIMARY KEY,
                product_code VARCHAR(20) UNIQUE,
                name VARCHAR(200),
                category VARCHAR(50),
                price DECIMAL(10, 2),
                cost DECIMAL(10, 2),
                stock_quantity INTEGER,
                is_active INTEGER
            )
        """

    def create_table_orders(self) -> str:
        return """
            CREATE TABLE orders (
                order_id BIGINT PRIMARY KEY,
                customer_id BIGINT,
                order_date TIMESTAMP,
                status VARCHAR(20),
                total_amount DECIMAL(12, 2),
                shipping_cost DECIMAL(8, 2),
                discount_amount DECIMAL(10, 2)
            )
        """

    def create_table_order_items(self) -> str:
        return """
            CREATE TABLE order_items (
                item_id BIGINT PRIMARY KEY,
                order_id BIGINT,
                product_id BIGINT,
                quantity INTEGER,
                unit_price DECIMAL(10, 2),
                discount_pct DECIMAL(5, 2)
            )
        """

    def date_trunc(self, field: str, expression: str) -> str:
        # Firebird doesn't have DATE_TRUNC, use EXTRACT and reconstruct
        if field.upper() == 'MONTH':
            return (f"CAST(EXTRACT(YEAR FROM {expression}) || '-' || "
                    f"LPAD(CAST(EXTRACT(MONTH FROM {expression}) AS VARCHAR(2)), 2, '0') || '-01' AS DATE)")
        elif field.upper() == 'YEAR':
            return f"CAST(EXTRACT(YEAR FROM {expression}) || '-01-01' AS DATE)"
        elif field.upper() == 'DAY':
            return f"CAST({expression} AS DATE)"
        return expression

    def date_extract(self, field: str, expression: str) -> str:
        return f"EXTRACT({field.upper()} FROM {expression})"

    def string_concat(self, *args) -> str:
        return " || ".join(args)

    def limit_clause(self, count: int, offset: Optional[int] = None) -> str:
        if offset:
            return f"ROWS {offset + 1} TO {offset + count}"
        return f"ROWS 1 TO {count}"

    def coalesce(self, *args) -> str:
        return f"COALESCE({', '.join(args)})"

    def stddev(self, expression: str) -> str:
        # Firebird uses STDDEV_SAMP or STDDEV_POP
        return f"STDDEV_SAMP({expression})"

    def percentile_cont(self, fraction: float, ordering: str, within_group: str) -> str:
        # Firebird 4.0+ supports PERCENTILE_CONT
        # For older versions, this will need approximation
        return f"PERCENTILE_CONT({fraction}) WITHIN GROUP (ORDER BY {within_group})"

    def cast_as_date(self, expression: str) -> str:
        return f"CAST({expression} AS DATE)"

    def date_diff_days(self, date1: str, date2: str) -> str:
        return f"DATEDIFF(day, {date2}, {date1})"

    def generate_series(self, start: int, end: int) -> str:
        # Firebird recursive CTE for series
        return f"""
            WITH RECURSIVE series AS (
                SELECT {start} AS n
                UNION ALL
                SELECT n + 1 FROM series WHERE n < {end}
            )
            SELECT n FROM series
        """

    def row_number(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"ROW_NUMBER() {over_clause}"

    def rank(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"RANK() {over_clause}"

    def get_placeholder(self) -> str:
        return "?"


class MySQLDialect(SQLDialect):
    """MySQL dialect (version 8.0+)."""

    def __init__(self):
        super().__init__("mysql")

    def create_table_customers(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS customers (
                customer_id BIGINT PRIMARY KEY,
                first_name VARCHAR(50),
                last_name VARCHAR(50),
                email VARCHAR(100) UNIQUE,
                phone VARCHAR(20),
                registration_date DATE,
                country_code VARCHAR(2),
                account_balance DECIMAL(12, 2)
            ) ENGINE=InnoDB
        """

    def create_table_products(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS products (
                product_id BIGINT PRIMARY KEY,
                product_code VARCHAR(20) UNIQUE,
                name VARCHAR(200),
                category VARCHAR(50),
                price DECIMAL(10, 2),
                cost DECIMAL(10, 2),
                stock_quantity INTEGER,
                is_active INTEGER
            ) ENGINE=InnoDB
        """

    def create_table_orders(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS orders (
                order_id BIGINT PRIMARY KEY,
                customer_id BIGINT,
                order_date TIMESTAMP,
                status VARCHAR(20),
                total_amount DECIMAL(12, 2),
                shipping_cost DECIMAL(8, 2),
                discount_amount DECIMAL(10, 2)
            ) ENGINE=InnoDB
        """

    def create_table_order_items(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS order_items (
                item_id BIGINT PRIMARY KEY,
                order_id BIGINT,
                product_id BIGINT,
                quantity INTEGER,
                unit_price DECIMAL(10, 2),
                discount_pct DECIMAL(5, 2)
            ) ENGINE=InnoDB
        """

    def date_trunc(self, field: str, expression: str) -> str:
        # MySQL uses DATE_FORMAT for truncation-like behavior
        if field.upper() == 'MONTH':
            return f"DATE_FORMAT({expression}, '%Y-%m-01')"
        elif field.upper() == 'YEAR':
            return f"DATE_FORMAT({expression}, '%Y-01-01')"
        elif field.upper() == 'DAY':
            return f"DATE({expression})"
        return expression

    def date_extract(self, field: str, expression: str) -> str:
        return f"EXTRACT({field.upper()} FROM {expression})"

    def string_concat(self, *args) -> str:
        return "CONCAT(" + ", ".join(args) + ")"

    def limit_clause(self, count: int, offset: Optional[int] = None) -> str:
        if offset:
            return f"LIMIT {offset}, {count}"
        return f"LIMIT {count}"

    def coalesce(self, *args) -> str:
        return f"COALESCE({', '.join(args)})"

    def stddev(self, expression: str) -> str:
        return f"STDDEV_SAMP({expression})"

    def percentile_cont(self, fraction: float, ordering: str, within_group: str) -> str:
        # MySQL 8.0+ supports window functions but not PERCENTILE_CONT
        # Use PERCENT_RANK approximation or alternative
        return f"(SELECT PERCENTILE_CONT({fraction}) WITHIN GROUP (ORDER BY {within_group}))"

    def cast_as_date(self, expression: str) -> str:
        return f"CAST({expression} AS DATE)"

    def date_diff_days(self, date1: str, date2: str) -> str:
        return f"DATEDIFF({date1}, {date2})"

    def generate_series(self, start: int, end: int) -> str:
        # MySQL 8.0 recursive CTE
        return f"""
            WITH RECURSIVE series AS (
                SELECT {start} AS n
                UNION ALL
                SELECT n + 1 FROM series WHERE n < {end}
            )
            SELECT n FROM series
        """

    def row_number(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"ROW_NUMBER() {over_clause}"

    def rank(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"RANK() {over_clause}"

    def get_placeholder(self) -> str:
        return "%s"


class PostgreSQLDialect(SQLDialect):
    """PostgreSQL dialect (version 15+)."""

    def __init__(self):
        super().__init__("postgresql")

    def create_table_customers(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS customers (
                customer_id BIGINT PRIMARY KEY,
                first_name VARCHAR(50),
                last_name VARCHAR(50),
                email VARCHAR(100) UNIQUE,
                phone VARCHAR(20),
                registration_date DATE,
                country_code VARCHAR(2),
                account_balance NUMERIC(12, 2)
            )
        """

    def create_table_products(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS products (
                product_id BIGINT PRIMARY KEY,
                product_code VARCHAR(20) UNIQUE,
                name VARCHAR(200),
                category VARCHAR(50),
                price NUMERIC(10, 2),
                cost NUMERIC(10, 2),
                stock_quantity INTEGER,
                is_active INTEGER
            )
        """

    def create_table_orders(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS orders (
                order_id BIGINT PRIMARY KEY,
                customer_id BIGINT,
                order_date TIMESTAMP,
                status VARCHAR(20),
                total_amount NUMERIC(12, 2),
                shipping_cost NUMERIC(8, 2),
                discount_amount NUMERIC(10, 2)
            )
        """

    def create_table_order_items(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS order_items (
                item_id BIGINT PRIMARY KEY,
                order_id BIGINT,
                product_id BIGINT,
                quantity INTEGER,
                unit_price NUMERIC(10, 2),
                discount_pct NUMERIC(5, 2)
            )
        """

    def date_trunc(self, field: str, expression: str) -> str:
        return f"DATE_TRUNC('{field.lower()}', {expression})"

    def date_extract(self, field: str, expression: str) -> str:
        return f"EXTRACT({field.upper()} FROM {expression})"

    def string_concat(self, *args) -> str:
        return " || ".join(args)

    def limit_clause(self, count: int, offset: Optional[int] = None) -> str:
        if offset:
            return f"LIMIT {count} OFFSET {offset}"
        return f"LIMIT {count}"

    def coalesce(self, *args) -> str:
        return f"COALESCE({', '.join(args)})"

    def stddev(self, expression: str) -> str:
        return f"STDDEV({expression})"

    def percentile_cont(self, fraction: float, ordering: str, within_group: str) -> str:
        return f"PERCENTILE_CONT({fraction}) WITHIN GROUP (ORDER BY {within_group})"

    def cast_as_date(self, expression: str) -> str:
        return f"CAST({expression} AS DATE)"

    def date_diff_days(self, date1: str, date2: str) -> str:
        return f"({date1}::date - {date2}::date)"

    def generate_series(self, start: int, end: int) -> str:
        return f"generate_series({start}, {end})"

    def row_number(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"ROW_NUMBER() {over_clause}"

    def rank(self, partition_by: Optional[str] = None, order_by: str = "") -> str:
        over_clause = "OVER ("
        if partition_by:
            over_clause += f"PARTITION BY {partition_by} "
        if order_by:
            over_clause += f"ORDER BY {order_by}"
        over_clause += ")"
        return f"RANK() {over_clause}"

    def get_placeholder(self) -> str:
        return "%s"


class ScratchBirdDialect(PostgreSQLDialect):
    """ScratchBird native dialect for the external benchmark harness."""

    def __init__(self):
        super().__init__()
        self.engine = "scratchbird"

    def create_table_customers(self) -> str:
        return """
            CREATE TABLE customers (
                customer_id bigint PRIMARY KEY,
                first_name varchar(50),
                last_name varchar(50),
                email varchar(100),
                phone varchar(20),
                registration_date date,
                country_code varchar(2),
                account_balance decimal
            )
        """

    def create_table_products(self) -> str:
        return """
            CREATE TABLE products (
                product_id bigint PRIMARY KEY,
                product_code varchar(20),
                name varchar(200),
                category varchar(50),
                price decimal,
                cost decimal,
                stock_quantity int,
                is_active int
            )
        """

    def create_table_orders(self) -> str:
        return """
            CREATE TABLE orders (
                order_id bigint PRIMARY KEY,
                customer_id bigint REFERENCES customers(customer_id),
                order_date timestamp,
                status varchar(20),
                total_amount decimal,
                shipping_cost decimal,
                discount_amount decimal
            )
        """

    def create_table_order_items(self) -> str:
        return """
            CREATE TABLE order_items (
                item_id bigint PRIMARY KEY,
                order_id bigint REFERENCES orders(order_id),
                product_id bigint REFERENCES products(product_id),
                quantity int,
                unit_price decimal,
                discount_pct decimal
            )
        """

    def date_trunc(self, field: str, expression: str) -> str:
        return f"date_trunc('{field.lower()}', {expression})"

    def date_extract(self, field: str, expression: str) -> str:
        return f"date_part('{field.lower()}', {expression})"

    def string_concat(self, *args) -> str:
        return "CONCAT(" + ", ".join(args) + ")"

    def stddev(self, expression: str) -> str:
        return f"STDDEV({expression})"

    def percentile_cont(self, fraction: float, ordering: str, within_group: str) -> str:
        return f"PERCENTILE_CONT({within_group}, {fraction})"

    def date_diff_days(self, date1: str, date2: str) -> str:
        return f"date_diff('day', {date2}, {date1})"

    def generate_series(self, start: int, end: int) -> str:
        return f"generate_series({start}, {end})"

    def get_placeholder(self) -> str:
        return "?"


class SQLDialectFactory:
    """Factory for creating dialect instances."""

    _dialects = {
        'firebird': FirebirdDialect,
        'mysql': MySQLDialect,
        'postgresql': PostgreSQLDialect,
        'scratchbird': ScratchBirdDialect,
    }

    @classmethod
    def get_dialect(cls, engine: str) -> SQLDialect:
        """Get dialect instance for engine."""
        engine = engine.lower()
        if engine not in cls._dialects:
            raise ValueError(f"Unsupported engine: {engine}")
        return cls._dialects[engine]()

    @classmethod
    def supported_engines(cls) -> List[str]:
        """Get list of supported engines."""
        return list(cls._dialects.keys())


# Template SQL generators for stress tests
class StressTestSQLGenerator:
    """Generates dialect-specific stress test SQL."""

    def __init__(self, dialect: SQLDialect):
        self.d = dialect

    def inner_join_simple(self) -> str:
        return """
            SELECT o.order_id, o.order_date, c.first_name, c.last_name, c.email
            FROM orders o
            INNER JOIN customers c ON o.customer_id = c.customer_id
        """

    def inner_join_large_result(self) -> str:
        return """
            SELECT oi.*, o.order_date, o.status, c.first_name, c.last_name
            FROM order_items oi
            INNER JOIN orders o ON oi.order_id = o.order_id
            INNER JOIN customers c ON o.customer_id = c.customer_id
        """

    def left_join_all_customers(self) -> str:
        concat = self.d.string_concat("c.first_name", "' '", "c.last_name")
        total_spent = self.d.coalesce("SUM(o.total_amount)", "0")
        if self.d.engine == 'firebird':
            total_spent = (
                "CAST(SUM(COALESCE(o.total_amount, CAST(0 AS DECIMAL(12,2)))) "
                "AS DOUBLE PRECISION)"
            )
        return f"""
            SELECT c.customer_id, {concat} as full_name,
                   COUNT(o.order_id) as order_count,
                   {total_spent} as total_spent
            FROM customers c
            LEFT JOIN orders o ON c.customer_id = o.customer_id
            GROUP BY c.customer_id, c.first_name, c.last_name
        """

    def four_table_join(self) -> str:
        return """
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
        """

    def aggregation_daily_sales(self) -> str:
        if self.d.engine == 'firebird':
            year_expr = self.d.date_extract('YEAR', 'o.order_date')
            month_expr = self.d.date_extract('MONTH', 'o.order_date')
            return f"""
                SELECT
                    {year_expr} as order_year,
                    {month_expr} as order_month,
                    p.category,
                    COUNT(DISTINCT o.order_id) as total_orders,
                    SUM(oi.quantity) as total_qty,
                    CAST(SUM(oi.quantity * oi.unit_price) AS DOUBLE PRECISION) as total_revenue,
                    CAST(AVG(oi.quantity * oi.unit_price) AS DOUBLE PRECISION) as avg_line_value
                FROM orders o
                INNER JOIN order_items oi ON o.order_id = oi.order_id
                INNER JOIN products p ON oi.product_id = p.product_id
                GROUP BY {year_expr}, {month_expr}, p.category
                HAVING COUNT(DISTINCT o.order_id) >= 10
                ORDER BY {year_expr} DESC, {month_expr} DESC, total_revenue DESC
            """

        date_trunc = self.d.date_trunc('MONTH', 'o.order_date')
        return f"""
            SELECT
                {date_trunc} as month,
                p.category,
                COUNT(DISTINCT o.order_id) as total_orders,
                SUM(oi.quantity) as total_qty,
                SUM(oi.quantity * oi.unit_price) as total_revenue,
                AVG(oi.quantity * oi.unit_price) as avg_line_value
            FROM orders o
            INNER JOIN order_items oi ON o.order_id = oi.order_id
            INNER JOIN products p ON oi.product_id = p.product_id
            GROUP BY {date_trunc}, p.category
            HAVING COUNT(DISTINCT o.order_id) >= 10
            ORDER BY month DESC, total_revenue DESC
        """

    def window_function_ranking(self) -> str:
        rank = self.d.rank("c.customer_id", "o.total_amount DESC")
        sum_over = self.d.coalesce("SUM(o.total_amount) OVER (PARTITION BY c.customer_id)", "0")
        if self.d.engine == 'firebird':
            sum_over = (
                "CAST(SUM(COALESCE(o.total_amount, CAST(0 AS DECIMAL(12,2)))) "
                "OVER (PARTITION BY c.customer_id) AS DOUBLE PRECISION)"
            )
        return f"""
            SELECT
                c.customer_id,
                c.first_name,
                o.order_id,
                o.total_amount,
                {rank} as amount_rank,
                {sum_over} as customer_total
            FROM customers c
            INNER JOIN orders o ON c.customer_id = o.customer_id
            WHERE o.order_date >= '2024-01-01'
        """

    def bulk_insert_select(self) -> str:
        if self.d.engine == 'mysql':
            seq_as_text = "CAST(seq AS CHAR(20))"
        else:
            seq_as_text = "CAST(seq AS VARCHAR(20))"
        concat = self.d.string_concat("'Data_'", seq_as_text)
        if self.d.engine == 'firebird':
            return f"""
                INSERT INTO bulk_insert_test (id, data, metric_value)
                SELECT
                    seq as id,
                    {concat} as data,
                    CAST(seq * 1.5 AS DECIMAL(10,2)) as metric_value
                FROM (
                    SELECT ROW_NUMBER() OVER (ORDER BY oi.item_id) as seq
                    FROM order_items oi
                    ROWS 1 TO 100000
                ) sub
            """
        else:  # mysql, postgresql, scratchbird
            limit = self.d.limit_clause(100000)
            window_order = "ORDER BY oi.item_id" if self.d.engine == 'scratchbird' else ""
            return f"""
                INSERT INTO bulk_insert_test (id, data, metric_value)
                SELECT
                    seq as id,
                    {concat} as data,
                    (seq * 1.5) as metric_value
                FROM (
                    SELECT ROW_NUMBER() OVER ({window_order}) as seq
                    FROM orders o
                    CROSS JOIN order_items oi
                    {limit}
                ) sub
            """

    def bulk_update_with_case(self) -> str:
        return """
            UPDATE order_items
            SET discount_pct = CASE
                WHEN quantity >= 50 THEN 20.0
                WHEN quantity >= 20 THEN 15.0
                WHEN quantity >= 10 THEN 10.0
                ELSE 5.0
            END
            WHERE discount_pct < 5.0 OR discount_pct IS NULL
        """

    def self_join_same_country(self) -> str:
        if self.d.engine == 'firebird':
            return """
                SELECT FIRST 10000
                    c1.customer_id as customer1_id,
                    c1.first_name as customer1_name,
                    c2.customer_id as customer2_id,
                    c2.first_name as customer2_name,
                    c1.country_code
                FROM customers c1
                INNER JOIN customers c2 ON c1.country_code = c2.country_code
                    AND c1.customer_id < c2.customer_id
                WHERE c1.registration_date >= '2024-01-01'
            """
        else:
            limit = self.d.limit_clause(10000)
            return f"""
                SELECT
                    c1.customer_id as customer1_id,
                    c1.first_name as customer1_name,
                    c2.customer_id as customer2_id,
                    c2.first_name as customer2_name,
                    c1.country_code
                FROM customers c1
                INNER JOIN customers c2 ON c1.country_code = c2.country_code
                    AND c1.customer_id < c2.customer_id
                WHERE c1.registration_date >= '2024-01-01'
                {limit}
            """

    def multi_dimensional_agg(self) -> str:
        year = self.d.date_extract('YEAR', 'o.order_date')
        month = self.d.date_extract('MONTH', 'o.order_date')
        revenue_expr = "SUM(oi.quantity * oi.unit_price)"
        avg_line_expr = "AVG(oi.quantity * oi.unit_price)"
        if self.d.engine == 'firebird':
            revenue_expr = "CAST(SUM(oi.quantity * oi.unit_price) AS DOUBLE PRECISION)"
            avg_line_expr = "CAST(AVG(oi.quantity * oi.unit_price) AS DOUBLE PRECISION)"
        return f"""
            SELECT
                {year} as order_year,
                {month} as order_month,
                c.country_code,
                p.category,
                COUNT(DISTINCT o.order_id) as orders,
                SUM(oi.quantity) as units,
                {revenue_expr} as revenue,
                {avg_line_expr} as avg_line
            FROM orders o
            INNER JOIN customers c ON o.customer_id = c.customer_id
            INNER JOIN order_items oi ON o.order_id = oi.order_id
            INNER JOIN products p ON oi.product_id = p.product_id
            GROUP BY {year}, {month}, c.country_code, p.category
            ORDER BY {year}, {month}, revenue DESC
        """

    def nested_subquery_agg(self) -> str:
        if self.d.engine == 'scratchbird':
            return """
                SELECT
                    country_stats.country_code,
                    country_stats.customer_count,
                    country_stats.total_revenue,
                    avg_stats.global_avg,
                    country_stats.total_revenue / NULLIF(avg_stats.global_avg, 0) as revenue_ratio
                FROM (
                    SELECT
                        c.country_code,
                        COUNT(DISTINCT c.customer_id) as customer_count,
                        SUM(o.total_amount) as total_revenue
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    GROUP BY c.country_code
                ) country_stats
                CROSS JOIN (
                    SELECT AVG(country_revenue) as global_avg
                    FROM (
                        SELECT SUM(o2.total_amount) as country_revenue
                        FROM orders o2
                        INNER JOIN customers c2 ON o2.customer_id = c2.customer_id
                        GROUP BY c2.country_code
                    ) revenue_stats
                ) avg_stats
                ORDER BY country_stats.total_revenue DESC
            """

        if self.d.engine == 'firebird':
            return """
                SELECT
                    country_stats.country_code,
                    country_stats.customer_count,
                    country_stats.total_revenue,
                    avg_stats.global_avg,
                    country_stats.total_revenue / NULLIF(avg_stats.global_avg, 0) as revenue_ratio
                FROM (
                    SELECT
                        c.country_code,
                        COUNT(DISTINCT c.customer_id) as customer_count,
                        CAST(SUM(o.total_amount) AS DOUBLE PRECISION) as total_revenue
                    FROM customers c
                    INNER JOIN orders o ON c.customer_id = o.customer_id
                    GROUP BY c.country_code
                ) country_stats
                CROSS JOIN (
                    SELECT CAST(AVG(country_revenue) AS DOUBLE PRECISION) as global_avg
                    FROM (
                        SELECT CAST(SUM(o2.total_amount) AS DOUBLE PRECISION) as country_revenue
                        FROM orders o2
                        INNER JOIN customers c2 ON o2.customer_id = c2.customer_id
                        GROUP BY c2.country_code
                    ) revenue_stats
                ) avg_stats
                ORDER BY country_stats.total_revenue DESC
            """

        return """
            SELECT
                country_stats.country_code,
                country_stats.customer_count,
                country_stats.total_revenue,
                (SELECT AVG(country_revenue) FROM (
                    SELECT SUM(total_amount) as country_revenue
                    FROM orders o2
                    INNER JOIN customers c2 ON o2.customer_id = c2.customer_id
                    GROUP BY c2.country_code
                ) sub2) as global_avg,
                country_stats.total_revenue /
                    (SELECT AVG(country_revenue) FROM (
                        SELECT SUM(total_amount) as country_revenue
                        FROM orders o3
                        INNER JOIN customers c3 ON o3.customer_id = c3.customer_id
                        GROUP BY c3.country_code
                    ) sub3) as revenue_ratio
            FROM (
                SELECT
                    c.country_code,
                    COUNT(DISTINCT c.customer_id) as customer_count,
                    SUM(o.total_amount) as total_revenue
                FROM customers c
                INNER JOIN orders o ON c.customer_id = o.customer_id
                GROUP BY c.country_code
            ) country_stats
            ORDER BY country_stats.total_revenue DESC
        """


def get_dialect_specific_sql(engine: str, test_name: str) -> Optional[str]:
    """Get dialect-specific SQL for a test."""
    try:
        dialect = SQLDialectFactory.get_dialect(engine)
        gen = StressTestSQLGenerator(dialect)

        test_map = {
            'inner_join_simple': gen.inner_join_simple,
            'inner_join_large_result': gen.inner_join_large_result,
            'left_join_all_customers': gen.left_join_all_customers,
            'four_table_join': gen.four_table_join,
            'aggregation_daily_sales': gen.aggregation_daily_sales,
            'window_function_ranking': gen.window_function_ranking,
            'bulk_insert_select': gen.bulk_insert_select,
            'bulk_update_with_case': gen.bulk_update_with_case,
            'self_join_same_country': gen.self_join_same_country,
            'multi_dimensional_agg': gen.multi_dimensional_agg,
            'nested_subquery_agg': gen.nested_subquery_agg,
        }

        if test_name in test_map:
            return test_map[test_name]()
        return None
    except Exception as e:
        print(f"Error generating SQL for {test_name}: {e}")
        return None


if __name__ == '__main__':
    # Test dialect generation
    for engine in ['firebird', 'mysql', 'postgresql']:
        print(f"\n{'='*60}")
        print(f"Engine: {engine}")
        print(f"{'='*60}")

        dialect = SQLDialectFactory.get_dialect(engine)
        gen = StressTestSQLGenerator(dialect)

        print("\n-- LEFT JOIN with aggregation:")
        print(gen.left_join_all_customers())

        print("\n-- Daily sales aggregation:")
        print(gen.aggregation_daily_sales())
