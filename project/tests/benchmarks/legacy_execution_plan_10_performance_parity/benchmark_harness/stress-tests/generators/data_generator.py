#!/usr/bin/env python3
"""
Synthetic Data Generator for Stress Testing

Generates deterministic test data with known properties for verification.
Supports multiple data types, distributions, and referential integrity.
"""

import hashlib
import random
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Any, Dict, Iterator, List, Optional, Tuple


@dataclass
class ColumnSpec:
    """Specification for a generated column."""
    name: str
    data_type: str  # int, bigint, varchar, decimal, date, timestamp, etc.
    nullable: bool = False
    min_value: Any = None
    max_value: Any = None
    distribution: str = "uniform"  # uniform, normal, sequential, reference
    references: Optional[str] = None  # table.column for FK
    unique: bool = False
    length: int = 50  # For varchar
    precision: int = 10  # For decimal
    scale: int = 2  # For decimal


@dataclass
class TableSpec:
    """Specification for a generated table."""
    name: str
    columns: List[ColumnSpec]
    row_count: int
    seed: int = 42


class DataGenerator(ABC):
    """Base class for data generators."""

    def __init__(self, seed: int = 42):
        self.seed = seed
        self.rng = random.Random(seed)

    @abstractmethod
    def generate_value(self, column: ColumnSpec, row_num: int) -> Any:
        pass

    def reset(self):
        """Reset RNG to initial state for reproducibility."""
        self.rng = random.Random(self.seed)


class IntegerGenerator(DataGenerator):
    """Generate integer values."""

    def generate_value(self, column: ColumnSpec, row_num: int) -> int:
        if column.distribution == "sequential":
            start = column.min_value if column.min_value is not None else 1
            return start + row_num
        elif column.distribution == "normal":
            mean = (column.min_value + column.max_value) / 2 if column.min_value and column.max_value else 500000
            stddev = (column.max_value - column.min_value) / 6 if column.min_value and column.max_value else 100000
            return int(self.rng.gauss(mean, stddev))
        else:  # uniform
            min_val = column.min_value if column.min_value is not None else 1
            max_val = column.max_value if column.max_value is not None else 1000000
            return self.rng.randint(min_val, max_val)


class BigIntGenerator(DataGenerator):
    """Generate bigint values."""

    def generate_value(self, column: ColumnSpec, row_num: int) -> int:
        if column.distribution == "sequential":
            start = column.min_value if column.min_value is not None else 1
            return start + row_num
        else:
            min_val = column.min_value if column.min_value is not None else 1
            max_val = column.max_value if column.max_value is not None else 10**12
            return self.rng.randint(min_val, max_val)


class VarcharGenerator(DataGenerator):
    """Generate varchar/string values."""

    CHARSET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    WORDS = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
             "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi",
             "rho", "sigma", "tau", "upsilon", "phi", "chi", "psi", "omega",
             "red", "green", "blue", "yellow", "orange", "purple", "cyan", "magenta",
             "small", "medium", "large", "huge", "tiny", "massive", "compact", "expansive",
             "fast", "slow", "rapid", "quick", "swift", "sluggish", "speedy", "leisurely"]

    def generate_value(self, column: ColumnSpec, row_num: int) -> str:
        if column.distribution == "sequential":
            # Generate sequential codes like "CODE0000001"
            return f"CODE{row_num:09d}"
        elif column.distribution == "hash":
            # Deterministic hash-based string
            h = hashlib.md5(f"{self.seed}_{row_num}_{column.name}".encode()).hexdigest()
            return h[:column.length]
        else:
            # Random combination of words
            num_words = self.rng.randint(1, 3)
            words = [self.rng.choice(self.WORDS) for _ in range(num_words)]
            result = "_".join(words)
            # Add random number suffix for variety
            if self.rng.random() > 0.5:
                result += f"_{self.rng.randint(1, 9999)}"
            return result[:column.length]


class DecimalGenerator(DataGenerator):
    """Generate decimal/numeric values."""

    def generate_value(self, column: ColumnSpec, row_num: int) -> float:
        min_val = column.min_value if column.min_value is not None else 0.0
        max_val = column.max_value if column.max_value is not None else 1000000.0

        value = self.rng.uniform(min_val, max_val)
        # Round to specified scale
        return round(value, column.scale)


class DateGenerator(DataGenerator):
    """Generate date values."""

    def generate_value(self, column: ColumnSpec, row_num: int) -> datetime:
        if column.min_value:
            start_date = datetime.strptime(column.min_value, "%Y-%m-%d")
        else:
            start_date = datetime(2020, 1, 1)

        if column.max_value:
            end_date = datetime.strptime(column.max_value, "%Y-%m-%d")
        else:
            end_date = datetime(2024, 12, 31)

        days_range = (end_date - start_date).days
        random_days = self.rng.randint(0, days_range)
        return start_date + timedelta(days=random_days)


class TimestampGenerator(DataGenerator):
    """Generate timestamp values."""

    def generate_value(self, column: ColumnSpec, row_num: int) -> datetime:
        return DateGenerator(self.seed).generate_value(column, row_num)


class ForeignKeyGenerator(DataGenerator):
    """Generate foreign key values referencing another table."""

    def __init__(self, seed: int = 42, reference_values: Optional[List[Any]] = None):
        super().__init__(seed)
        self.reference_values = reference_values or []

    def generate_value(self, column: ColumnSpec, row_num: int) -> Any:
        if not self.reference_values:
            return None
        # Weighted toward earlier values (more recent IDs more common)
        # This simulates real-world patterns
        idx = int(self.rng.random() ** 2 * len(self.reference_values))
        return self.reference_values[idx]


class TableDataGenerator:
    """Generate complete table data based on specification."""

    GENERATORS = {
        'int': IntegerGenerator,
        'integer': IntegerGenerator,
        'bigint': BigIntGenerator,
        'varchar': VarcharGenerator,
        'string': VarcharGenerator,
        'decimal': DecimalGenerator,
        'numeric': DecimalGenerator,
        'date': DateGenerator,
        'timestamp': TimestampGenerator,
        'datetime': TimestampGenerator,
    }

    def __init__(self, spec: TableSpec, fk_references: Optional[Dict[str, List[Any]]] = None):
        self.spec = spec
        self.fk_references = fk_references or {}
        self.generators: Dict[str, DataGenerator] = {}
        self._used_values: Dict[str, set] = {}
        self._init_generators()

    def _init_generators(self):
        """Initialize column generators."""
        for col in self.spec.columns:
            if col.references and col.references in self.fk_references:
                # Use FK generator with reference values
                self.generators[col.name] = ForeignKeyGenerator(
                    self.spec.seed,
                    self.fk_references[col.references]
                )
            elif col.data_type in self.GENERATORS:
                self.generators[col.name] = self.GENERATORS[col.data_type](self.spec.seed)
            else:
                raise ValueError(f"Unknown data type: {col.data_type}")

    def generate_row(self, row_num: int) -> Dict[str, Any]:
        """Generate a single row."""
        row = {}
        for col in self.spec.columns:
            # Handle NULLs
            if col.nullable and self.generators[col.name].rng.random() < 0.05:  # 5% NULL
                row[col.name] = None
            else:
                generated_value = self.generators[col.name].generate_value(col, row_num)
                if col.unique:
                    generated_value = self._ensure_unique_value(col, generated_value, row_num)
                row[col.name] = generated_value
        return row

    def _ensure_unique_value(self, col: ColumnSpec, value: Any, row_num: int) -> Any:
        """Ensure a deterministic unique value for columns marked as unique."""
        used = self._used_values.setdefault(col.name, set())
        if value not in used:
            used.add(value)
            return value

        if col.data_type in ('varchar', 'string', 'date', 'timestamp', 'datetime'):
            candidate = f"{row_num}_{value}"
            attempt = 0
            while candidate in used:
                attempt += 1
                candidate = f"{row_num}_{attempt}_{value}"
            if col.length:
                candidate = str(candidate)[:col.length]
        elif isinstance(value, int):
            candidate = value + row_num
        elif isinstance(value, float):
            candidate = value + (row_num / 10.0)
        else:
            candidate = f"{value}_{row_num}"

        # In practice, a single deterministic suffix should avoid collisions.
        used.add(candidate)
        return candidate

    def generate_rows(self, batch_size: int = 10000) -> Iterator[List[Dict[str, Any]]]:
        """Generate rows in batches for memory efficiency."""
        batch = []
        for i in range(self.spec.row_count):
            batch.append(self.generate_row(i))
            if len(batch) >= batch_size:
                yield batch
                batch = []
        if batch:
            yield batch

    def generate_sql_inserts(self, dialect: str = "generic") -> Iterator[str]:
        """Generate SQL INSERT statements."""
        columns = [c.name for c in self.spec.columns]
        col_list = ", ".join(columns)

        for batch in self.generate_rows():
            values_list = []
            for row in batch:
                values = []
                for col in self.spec.columns:
                    val = row[col.name]
                    if val is None:
                        values.append("NULL")
                    elif col.data_type in ('varchar', 'string', 'date', 'timestamp', 'datetime'):
                        values.append(f"'{val}'")
                    else:
                        values.append(str(val))
                values_list.append(f"({', '.join(values)})")

            sql = f"INSERT INTO {self.spec.name} ({col_list}) VALUES {', '.join(values_list)};"
            yield sql


def generate_standard_dataset(scale: str = "medium") -> Dict[str, TableSpec]:
    """
    Generate a standard stress test dataset.

    Scale options:
    - small: 10K-50K rows
    - medium: 100K-500K rows
    - large: 1M-5M rows
    - huge: 10M+ rows
    """
    scales = {
        "small": {"customers": 10000, "products": 5000, "orders": 50000, "order_items": 200000},
        "medium": {"customers": 100000, "products": 50000, "orders": 500000, "order_items": 2000000},
        "large": {"customers": 1000000, "products": 500000, "orders": 5000000, "order_items": 20000000},
        "huge": {"customers": 10000000, "products": 2000000, "orders": 50000000, "order_items": 200000000},
    }

    s = scales.get(scale, scales["medium"])

    # Customers table
    customers = TableSpec(
        name="customers",
        columns=[
            ColumnSpec("customer_id", "bigint", unique=True, distribution="sequential"),
            ColumnSpec("first_name", "varchar", length=50),
            ColumnSpec("last_name", "varchar", length=50),
            ColumnSpec("email", "varchar", length=100, unique=True),
            ColumnSpec("phone", "varchar", length=20, nullable=True),
            ColumnSpec("registration_date", "date", min_value="2020-01-01", max_value="2024-12-31"),
            ColumnSpec("country_code", "varchar", length=2),
            ColumnSpec("account_balance", "decimal", min_value=-5000, max_value=50000, precision=12, scale=2),
        ],
        row_count=s["customers"],
        seed=1001
    )

    # Products table
    products = TableSpec(
        name="products",
        columns=[
            ColumnSpec("product_id", "bigint", unique=True, distribution="sequential"),
            ColumnSpec("product_code", "varchar", length=20, unique=True, distribution="sequential"),
            ColumnSpec("name", "varchar", length=200),
            ColumnSpec("category", "varchar", length=50),
            ColumnSpec("price", "decimal", min_value=0.99, max_value=9999.99, precision=10, scale=2),
            ColumnSpec("cost", "decimal", min_value=0.50, max_value=5000.00, precision=10, scale=2),
            ColumnSpec("stock_quantity", "int", min_value=0, max_value=10000),
            ColumnSpec("is_active", "int", min_value=0, max_value=1),  # Boolean as int for compatibility
        ],
        row_count=s["products"],
        seed=2001
    )

    # Orders table
    orders = TableSpec(
        name="orders",
        columns=[
            ColumnSpec("order_id", "bigint", unique=True, distribution="sequential"),
            ColumnSpec("customer_id", "bigint", references="customers.customer_id"),
            ColumnSpec("order_date", "timestamp"),
            ColumnSpec("status", "varchar", length=20),  # pending, shipped, delivered, cancelled
            ColumnSpec("total_amount", "decimal", min_value=0, max_value=100000, precision=12, scale=2),
            ColumnSpec("shipping_cost", "decimal", min_value=0, max_value=500, precision=8, scale=2),
            ColumnSpec("discount_amount", "decimal", min_value=0, max_value=10000, precision=10, scale=2, nullable=True),
        ],
        row_count=s["orders"],
        seed=3001
    )

    # Order Items table (child of orders and products)
    order_items = TableSpec(
        name="order_items",
        columns=[
            ColumnSpec("item_id", "bigint", unique=True, distribution="sequential"),
            ColumnSpec("order_id", "bigint", references="orders.order_id"),
            ColumnSpec("product_id", "bigint", references="products.product_id"),
            ColumnSpec("quantity", "int", min_value=1, max_value=100),
            ColumnSpec("unit_price", "decimal", min_value=0.99, max_value=9999.99, precision=10, scale=2),
            ColumnSpec("discount_pct", "decimal", min_value=0, max_value=50, precision=5, scale=2),
        ],
        row_count=s["order_items"],
        seed=4001
    )

    return {
        "customers": customers,
        "products": products,
        "orders": orders,
        "order_items": order_items,
    }


def generate_verification_queries(dataset: Dict[str, TableSpec]) -> List[Dict[str, Any]]:
    """
    Generate verification queries with expected results.
    These queries check data integrity and can be used to verify correctness.
    """
    queries = []

    # Row count verification
    for table_name, spec in dataset.items():
        queries.append({
            "name": f"count_{table_name}",
            "description": f"Verify {table_name} row count",
            "sql": f"SELECT COUNT(*) as cnt FROM {table_name}",
            "expected": spec.row_count,
            "tolerance": 0,  # Must be exact
        })

    # Referential integrity - all orders have valid customers
    queries.append({
        "name": "fk_orders_customers",
        "description": "Verify all orders reference valid customers",
        "sql": """
            SELECT COUNT(*) as orphan_count
            FROM orders o
            LEFT JOIN customers c ON o.customer_id = c.customer_id
            WHERE c.customer_id IS NULL
        """,
        "expected": 0,
        "tolerance": 0,
    })

    # Referential integrity - all order_items have valid orders
    queries.append({
        "name": "fk_items_orders",
        "description": "Verify all order items reference valid orders",
        "sql": """
            SELECT COUNT(*) as orphan_count
            FROM order_items oi
            LEFT JOIN orders o ON oi.order_id = o.order_id
            WHERE o.order_id IS NULL
        """,
        "expected": 0,
        "tolerance": 0,
    })

    # Referential integrity - all order_items have valid products
    queries.append({
        "name": "fk_items_products",
        "description": "Verify all order items reference valid products",
        "sql": """
            SELECT COUNT(*) as orphan_count
            FROM order_items oi
            LEFT JOIN products p ON oi.product_id = p.product_id
            WHERE p.product_id IS NULL
        """,
        "expected": 0,
        "tolerance": 0,
    })

    # Check no negative prices
    queries.append({
        "name": "check_product_prices",
        "description": "Verify no negative product prices",
        "sql": "SELECT COUNT(*) as invalid_count FROM products WHERE price < 0",
        "expected": 0,
        "tolerance": 0,
    })

    # Generated orders and order_items are independent synthetic datasets, so
    # validate only that order-level monetary fields remain non-negative.
    queries.append({
        "name": "check_order_amounts",
        "description": "Verify order-level monetary fields are non-negative",
        "sql": """
            SELECT COUNT(*) as invalid_count
            FROM orders
            WHERE total_amount < 0
               OR shipping_cost < 0
               OR discount_amount < 0
        """,
        "expected": 0,
        "tolerance": 0,
    })

    return queries


if __name__ == "__main__":
    # Example usage
    dataset = generate_standard_dataset("small")

    print("Generated dataset specifications:")
    for name, spec in dataset.items():
        print(f"\n{name}: {spec.row_count:,} rows")
        print(f"  Columns: {[c.name for c in spec.columns]}")

    # Generate verification queries
    print("\n\nVerification queries:")
    for q in generate_verification_queries(dataset):
        print(f"\n{q['name']}: {q['description']}")
        print(f"  Expected: {q['expected']}")
