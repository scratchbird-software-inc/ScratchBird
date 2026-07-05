#!/usr/bin/env python3
"""
ACID Compliance & Transaction Tests

Tests for:
- Atomicity (all-or-nothing commits)
- Consistency (constraint enforcement)
- Isolation (concurrent transaction behavior)
- Durability (committed data survives failures)

These are critical because ScratchBird must guarantee the same
transactional properties as the engines it emulates.
"""

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Callable


@dataclass
class TransactionTest:
    """Definition of a transaction/ACID test."""
    name: str
    description: str
    category: str  # atomicity, consistency, isolation, durability
    setup_sql: Optional[str] = None
    test_sql: str = ""
    verification_sql: str = ""
    expected_result: Any = None
    requires_concurrent: bool = False
    timeout_seconds: int = 60


class AtomicityTests:
    """Tests for Atomicity (A in ACID)."""

    @staticmethod
    def get_all_tests() -> List[TransactionTest]:
        return [
            TransactionTest(
                name="atomic_commit_success",
                description="All statements in transaction commit together",
                category="atomicity",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS atomic_test (
                        id INT PRIMARY KEY,
                        metric_value INT NOT NULL,
                        name VARCHAR(50)
                    );
                    DELETE FROM atomic_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO atomic_test VALUES (1, 100, 'first');
                    INSERT INTO atomic_test VALUES (2, 200, 'second');
                    INSERT INTO atomic_test VALUES (3, 300, 'third');
                    COMMIT;
                """,
                verification_sql="SELECT COUNT(*) FROM atomic_test",
                expected_result=3,
            ),
            TransactionTest(
                name="atomic_rollback_on_error",
                description="Transaction rolls back on error - no partial commits",
                category="atomicity",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS atomic_test (
                        id INT PRIMARY KEY,
                        metric_value INT NOT NULL,
                        name VARCHAR(50)
                    );
                    DELETE FROM atomic_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO atomic_test VALUES (1, 100, 'first');
                    INSERT INTO atomic_test VALUES (2, 200, 'second');
                    INSERT INTO atomic_test VALUES (1, 999, 'duplicate_pk'); -- Should fail
                    COMMIT;
                """,
                verification_sql="SELECT COUNT(*) FROM atomic_test",
                expected_result=0,  # Should be 0 - entire transaction rolled back
            ),
            TransactionTest(
                name="atomic_explicit_rollback",
                description="Explicit ROLLBACK undoes all changes",
                category="atomicity",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS atomic_rollback_test (
                        id INT PRIMARY KEY,
                        metric_value INT NOT NULL
                    );
                    DELETE FROM atomic_rollback_test;
                    INSERT INTO atomic_rollback_test VALUES (100, 0);
                """,
                test_sql="""
                    BEGIN;
                    UPDATE atomic_rollback_test SET metric_value = metric_value + 100 WHERE id = 100;
                    INSERT INTO atomic_rollback_test VALUES (101, 100);
                    INSERT INTO atomic_rollback_test VALUES (102, 200);
                    ROLLBACK;
                """,
                verification_sql="SELECT metric_value FROM atomic_rollback_test WHERE id = 100",
                expected_result=0,  # Should be unchanged
            ),
            TransactionTest(
                name="atomic_multi_table_commit",
                description="Multi-table transaction is atomic across all tables",
                category="atomicity",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS atomic_accounts_test (
                        account_id INT PRIMARY KEY,
                        account_balance DECIMAL(12,2) NOT NULL
                    );
                    CREATE TABLE IF NOT EXISTS atomic_account_transactions_test (
                        tx_id INT PRIMARY KEY,
                        from_account INT,
                        to_account INT,
                        transfer_amount DECIMAL(12,2)
                    );
                    DELETE FROM atomic_accounts_test;
                    DELETE FROM atomic_account_transactions_test;
                    INSERT INTO atomic_accounts_test VALUES (1, 1000.00);
                    INSERT INTO atomic_accounts_test VALUES (2, 1000.00);
                """,
                test_sql="""
                    BEGIN;
                    UPDATE atomic_accounts_test SET account_balance = account_balance - 100 WHERE account_id = 1;
                    UPDATE atomic_accounts_test SET account_balance = account_balance + 100 WHERE account_id = 2;
                    INSERT INTO atomic_account_transactions_test VALUES (1, 1, 2, 100.00);
                    COMMIT;
                """,
                verification_sql="""
                    SELECT CAST(SUM(account_balance) AS DECIMAL(12,2))
                    FROM atomic_accounts_test
                """,
                expected_result=2000.00,  # Total should remain constant
            ),
            TransactionTest(
                name="atomic_savepoint_rollback",
                description="ROLLBACK TO SAVEPOINT partially undoes transaction",
                category="atomicity",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS atomic_savepoint_test (id INT PRIMARY KEY, metric_value INT);
                    DELETE FROM atomic_savepoint_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO atomic_savepoint_test VALUES (1, 100);
                    SAVEPOINT sp1;
                    INSERT INTO atomic_savepoint_test VALUES (2, 200);
                    INSERT INTO atomic_savepoint_test VALUES (3, 300);
                    ROLLBACK TO SAVEPOINT sp1;
                    INSERT INTO atomic_savepoint_test VALUES (4, 400);
                    COMMIT;
                """,
                verification_sql="SELECT COUNT(*) FROM atomic_savepoint_test",
                expected_result=2,  # Only id 1 and 4 should exist
            ),
        ]


class ConsistencyTests:
    """Tests for Consistency (C in ACID)."""

    @staticmethod
    def get_all_tests() -> List[TransactionTest]:
        return [
            TransactionTest(
                name="consistency_pk_violation",
                description="Primary key constraint is enforced",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS consistency_test (
                        id INT PRIMARY KEY,
                        name VARCHAR(50)
                    );
                    DELETE FROM consistency_test;
                    INSERT INTO consistency_test VALUES (1, 'original');
                """,
                test_sql="INSERT INTO consistency_test VALUES (1, 'duplicate')",
                verification_sql="SELECT COUNT(*) FROM consistency_test WHERE id = 1",
                expected_result=1,
            ),
            TransactionTest(
                name="consistency_fk_violation",
                description="Foreign key constraint prevents orphaned records",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS parent (
                        parent_id INT PRIMARY KEY
                    );
                    CREATE TABLE IF NOT EXISTS child (
                        child_id INT PRIMARY KEY,
                        parent_id INT,
                        FOREIGN KEY (parent_id) REFERENCES parent(parent_id)
                    );
                    DELETE FROM child;
                    DELETE FROM parent;
                    INSERT INTO parent VALUES (1);
                """,
                test_sql="INSERT INTO child VALUES (1, 999)",  # Invalid parent_id
                verification_sql="SELECT COUNT(*) FROM child",
                expected_result=0,
            ),
            TransactionTest(
                name="consistency_check_constraint",
                description="CHECK constraints are enforced",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS check_test (
                        id INT PRIMARY KEY,
                        age INT CHECK (age >= 0 AND age <= 150),
                        email VARCHAR(100) CHECK (email LIKE '%@%')
                    );
                    DELETE FROM check_test;
                """,
                test_sql="INSERT INTO check_test VALUES (1, 200, 'invalid')",
                verification_sql="SELECT COUNT(*) FROM check_test",
                expected_result=0,
            ),
            TransactionTest(
                name="consistency_unique_constraint",
                description="UNIQUE constraints prevent duplicates",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS unique_test (
                        id INT PRIMARY KEY,
                        email VARCHAR(100) UNIQUE
                    );
                    DELETE FROM unique_test;
                    INSERT INTO unique_test VALUES (1, 'test@example.com');
                """,
                test_sql="INSERT INTO unique_test VALUES (2, 'test@example.com')",
                verification_sql="SELECT COUNT(*) FROM unique_test",
                expected_result=1,
            ),
            TransactionTest(
                name="consistency_not_null",
                description="NOT NULL constraints prevent NULL insertion",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS notnull_test (
                        id INT PRIMARY KEY,
                        required_field VARCHAR(50) NOT NULL
                    );
                    DELETE FROM notnull_test;
                """,
                test_sql="INSERT INTO notnull_test (id, required_field) VALUES (1, NULL)",
                verification_sql="SELECT COUNT(*) FROM notnull_test",
                expected_result=0,
            ),
            TransactionTest(
                name="consistency_invariant_maintenance",
                description="Database invariants are maintained across operations",
                category="consistency",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS ledger (
                        entry_id INT PRIMARY KEY,
                        account_id INT NOT NULL,
                        debit DECIMAL(12,2) DEFAULT 0,
                        credit DECIMAL(12,2) DEFAULT 0,
                        CHECK (debit >= 0 AND credit >= 0)
                    );
                    DELETE FROM ledger;
                """,
                test_sql="INSERT INTO ledger VALUES (1, 1, -100, 0)",  # Negative debit
                verification_sql="SELECT COUNT(*) FROM ledger",
                expected_result=0,
            ),
        ]


class IsolationTests:
    """Tests for Isolation (I in ACID)."""

    @staticmethod
    def get_all_tests() -> List[TransactionTest]:
        return [
            TransactionTest(
                name="isolation_dirty_read_prevention",
                description="Uncommitted changes are not visible to other transactions",
                category="isolation",
                requires_concurrent=True,
                setup_sql="""
                    DROP TABLE IF EXISTS isolation_test;
                    CREATE TABLE isolation_test (
                        id INT PRIMARY KEY,
                        metric_value INT
                    );
                    DELETE FROM isolation_test;
                    INSERT INTO isolation_test VALUES (1, 100);
                """,
                test_sql="# Concurrent test required",  # Requires 2 connections
                verification_sql="SELECT metric_value FROM isolation_test WHERE id = 1",
                expected_result=100,
            ),
            TransactionTest(
                name="isolation_non_repeatable_read",
                description="Repeatable Read prevents non-repeatable reads",
                category="isolation",
                requires_concurrent=True,
                setup_sql="""
                    DROP TABLE IF EXISTS isolation_test;
                    CREATE TABLE isolation_test (
                        id INT PRIMARY KEY,
                        metric_value INT
                    );
                    DELETE FROM isolation_test;
                    INSERT INTO isolation_test VALUES (1, 100);
                """,
                test_sql="# Concurrent test required",
                verification_sql="SELECT metric_value FROM isolation_test WHERE id = 1",
                expected_result=100,
            ),
            TransactionTest(
                name="isolation_phantom_read",
                description="Serializable isolation prevents phantom reads",
                category="isolation",
                requires_concurrent=True,
                setup_sql="""
                    DROP TABLE IF EXISTS isolation_test;
                    CREATE TABLE isolation_test (
                        id INT PRIMARY KEY,
                        category VARCHAR(20),
                        metric_value INT
                    );
                    DELETE FROM isolation_test;
                    INSERT INTO isolation_test VALUES (1, 'A', 100);
                    INSERT INTO isolation_test VALUES (2, 'A', 200);
                """,
                test_sql="# Concurrent test required",
                verification_sql="SELECT COUNT(*) FROM isolation_test WHERE category = 'A'",
                expected_result=2,
            ),
            TransactionTest(
                name="isolation_lost_update",
                description="Concurrent updates don't lose data",
                category="isolation",
                requires_concurrent=True,
                setup_sql="""
                    DROP TABLE IF EXISTS isolation_test;
                    CREATE TABLE isolation_test (
                        id INT PRIMARY KEY,
                        counter INT DEFAULT 0
                    );
                    DELETE FROM isolation_test;
                    INSERT INTO isolation_test VALUES (1, 0);
                """,
                test_sql="# Concurrent test required",
                verification_sql="SELECT counter FROM isolation_test WHERE id = 1",
                expected_result=100,  # After 100 concurrent increments
            ),
            TransactionTest(
                name="isolation_read_committed_default",
                description="Default isolation level matches the engine baseline",
                category="isolation",
                setup_sql="""
                    DROP TABLE IF EXISTS isolation_test;
                    CREATE TABLE isolation_test (id INT);
                """,
                test_sql="# Check current isolation level",
                verification_sql="# Engine-specific isolation level check",
                expected_result={
                    "firebird": "READ COMMITTED",
                    "mysql": "REPEATABLE-READ",
                    "postgresql": "read committed",
                    "scratchbird": "READ COMMITTED",
                },
            ),
        ]


class DurabilityTests:
    """Tests for Durability (D in ACID)."""

    @staticmethod
    def get_all_tests() -> List[TransactionTest]:
        return [
            TransactionTest(
                name="durability_commit_persistence",
                description="Committed data persists after transaction ends",
                category="durability",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS durability_test (
                        id INT PRIMARY KEY,
                        data VARCHAR(100)
                    );
                    DELETE FROM durability_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO durability_test VALUES (1, 'committed_data');
                    COMMIT;
                """,
                verification_sql="SELECT data FROM durability_test WHERE id = 1",
                expected_result="committed_data",
            ),
            TransactionTest(
                name="durability_rollback_not_persisted",
                description="Rolled back data does not persist",
                category="durability",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS durability_test (
                        id INT PRIMARY KEY,
                        data VARCHAR(100)
                    );
                    DELETE FROM durability_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO durability_test VALUES (1, 'rolled_back_data');
                    ROLLBACK;
                """,
                verification_sql="SELECT COUNT(*) FROM durability_test WHERE id = 1",
                expected_result=0,
            ),
            TransactionTest(
                name="durability_visibility_after_commit",
                description="New transaction sees committed data immediately",
                category="durability",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS durability_visibility_test (
                        id INT PRIMARY KEY,
                        metric_value INT
                    );
                    DELETE FROM durability_visibility_test;
                """,
                test_sql="""
                    BEGIN;
                    INSERT INTO durability_visibility_test VALUES (1, 42);
                    COMMIT;
                    -- New transaction
                    BEGIN;
                """,
                verification_sql="SELECT metric_value FROM durability_visibility_test WHERE id = 1",
                expected_result=42,
            ),
        ]


def get_all_tests() -> Dict[str, List[TransactionTest]]:
    """Get all ACID tests organized by category."""
    return {
        'atomicity': AtomicityTests.get_all_tests(),
        'consistency': ConsistencyTests.get_all_tests(),
        'isolation': IsolationTests.get_all_tests(),
        'durability': DurabilityTests.get_all_tests(),
    }


def get_test_by_name(name: str) -> Optional[TransactionTest]:
    """Get a specific test by name."""
    all_tests = get_all_tests()
    for category, tests in all_tests.items():
        for test in tests:
            if test.name == name:
                return test
    return None


if __name__ == '__main__':
    tests = get_all_tests()
    total = sum(len(t) for t in tests.values())

    print(f"Total ACID Tests: {total}")
    print()

    for category, cat_tests in tests.items():
        print(f"\n{category.upper()}: {len(cat_tests)} tests")
        for t in cat_tests:
            concurrent = " [CONCURRENT]" if t.requires_concurrent else ""
            print(f"  - {t.name}: {t.description[:50]}...{concurrent}")
