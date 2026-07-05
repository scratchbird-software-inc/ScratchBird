#!/usr/bin/env python3
"""
DDL (Data Definition Language) Tests

Tests for CREATE, ALTER, DROP operations.
"""

from dataclasses import dataclass
from typing import Any, List


@dataclass
class DDLTest:
    name: str
    description: str
    category: str
    setup_sql: str
    ddl_sql: str
    verification_sql: str
    expected_result: Any


class DDLTests:
    """DDL operation tests."""

    @staticmethod
    def get_all_tests() -> List[DDLTest]:
        tests = []

        # CREATE TABLE tests
        tests.extend([
            DDLTest(
                name="create_table_basic",
                description="Create basic table",
                category="create",
                setup_sql="DROP TABLE IF EXISTS test_tbl",
                ddl_sql="CREATE TABLE test_tbl (id INT PRIMARY KEY, name VARCHAR(50))",
                verification_sql="SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'TEST_TBL'",
                expected_result=1,
            ),
            DDLTest(
                name="create_table_with_constraints",
                description="Create table with multiple constraints",
                category="create",
                setup_sql="DROP TABLE IF EXISTS orders; DROP TABLE IF EXISTS customers",
                ddl_sql="""
                    CREATE TABLE customers (
                        id INT PRIMARY KEY,
                        email VARCHAR(100) UNIQUE NOT NULL,
                        age INT CHECK (age >= 18),
                        status VARCHAR(20) DEFAULT 'active'
                    )
                """,
                verification_sql="SELECT COUNT(*) FROM information_schema.table_constraints WHERE table_name = 'CUSTOMERS'",
                expected_result=4,  # PK, UNIQUE, CHECK, NOT NULL
            ),
        ])

        # ALTER TABLE tests
        tests.extend([
            DDLTest(
                name="alter_table_add_column",
                description="Add column to existing table",
                category="alter",
                setup_sql="CREATE TABLE IF NOT EXISTS alter_test (id INT PRIMARY KEY); DELETE FROM alter_test",
                ddl_sql="ALTER TABLE alter_test ADD COLUMN new_col VARCHAR(50)",
                verification_sql="SELECT COUNT(*) FROM information_schema.columns WHERE table_name = 'ALTER_TEST' AND column_name = 'NEW_COL'",
                expected_result=1,
            ),
            DDLTest(
                name="alter_table_add_constraint",
                description="Add foreign key constraint",
                category="alter",
                setup_sql="""
                    DROP TABLE IF EXISTS child; DROP TABLE IF EXISTS parent;
                    CREATE TABLE parent (id INT PRIMARY KEY);
                    CREATE TABLE child (id INT PRIMARY KEY, parent_id INT)
                """,
                ddl_sql="ALTER TABLE child ADD CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES parent(id)",
                verification_sql="SELECT COUNT(*) FROM information_schema.table_constraints WHERE constraint_name LIKE '%FK_PARENT%'",
                expected_result=1,
            ),
        ])

        # CREATE INDEX tests
        tests.extend([
            DDLTest(
                name="create_index",
                description="Create index on table",
                category="index",
                setup_sql="""
                    DROP TABLE IF EXISTS idx_test;
                    CREATE TABLE idx_test (id INT, name VARCHAR(50));
                    INSERT INTO idx_test SELECT seq, 'name' || seq FROM generate_series(1, 10000) seq
                """,
                ddl_sql="CREATE INDEX idx_test_name ON idx_test(name)",
                verification_sql="SELECT COUNT(*) FROM pg_indexes WHERE indexname = 'idx_test_name'",
                expected_result=1,
            ),
            DDLTest(
                name="create_unique_index",
                description="Create unique index",
                category="index",
                setup_sql="DROP TABLE IF EXISTS idx_test; CREATE TABLE idx_test (id INT, code VARCHAR(20))",
                ddl_sql="CREATE UNIQUE INDEX idx_test_code ON idx_test(code)",
                verification_sql="SELECT is_unique FROM information_schema.statistics WHERE index_name = 'IDX_TEST_CODE'",
                expected_result="YES",
            ),
        ])

        # DROP tests
        tests.extend([
            DDLTest(
                name="drop_table",
                description="Drop table",
                category="drop",
                setup_sql="CREATE TABLE IF NOT EXISTS drop_test (id INT)",
                ddl_sql="DROP TABLE drop_test",
                verification_sql="SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'DROP_TEST'",
                expected_result=0,
            ),
        ])

        return tests


def get_all_tests() -> List[DDLTest]:
    return DDLTests.get_all_tests()


if __name__ == '__main__':
    tests = get_all_tests()
    print(f"Total DDL Tests: {len(tests)}")
    for t in tests:
        print(f"  - {t.name} ({t.category})")
