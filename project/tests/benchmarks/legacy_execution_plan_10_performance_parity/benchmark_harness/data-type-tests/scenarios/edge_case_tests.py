#!/usr/bin/env python3
"""
Data Type Edge Case Tests

Tests for numeric, string, date/time, and binary edge cases.
Critical for finding emulation gaps.
"""

from dataclasses import dataclass
from typing import Any, List, Optional


@dataclass
class EdgeCaseTest:
    """Definition of a data type edge case test."""
    name: str
    description: str
    category: str  # numeric, string, datetime, binary, null
    setup_sql: str
    test_sql: str
    expected_result: Any
    expected_error: bool = False


class NumericEdgeCaseTests:
    """Numeric type edge cases."""

    @staticmethod
    def get_all_tests() -> List[EdgeCaseTest]:
        return [
            EdgeCaseTest(
                name="numeric_max_int",
                description="Maximum INTEGER value handling",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS num_test (id INT PRIMARY KEY, val INT)",
                test_sql="INSERT INTO num_test VALUES (1, 2147483647)",
                expected_result=1,  # rows affected
            ),
            EdgeCaseTest(
                name="numeric_max_bigint",
                description="Maximum BIGINT value handling",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS num_test2 (id INT PRIMARY KEY, val BIGINT)",
                test_sql="INSERT INTO num_test2 VALUES (1, 9223372036854775807)",
                expected_result=1,
            ),
            EdgeCaseTest(
                name="numeric_overflow",
                description="Integer overflow should error",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS num_test (id INT PRIMARY KEY, val INT)",
                test_sql="INSERT INTO num_test VALUES (999, 2147483648)",  # Overflow
                expected_result=0,
                expected_error=True,
            ),
            EdgeCaseTest(
                name="numeric_division_by_zero",
                description="Division by zero handling",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS num_test (id INT)",
                test_sql="SELECT 100 / 0 FROM num_test",
                expected_result=None,
                expected_error=True,
            ),
            EdgeCaseTest(
                name="numeric_decimal_precision",
                description="Decimal precision preservation",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS dec_test (id INT PRIMARY KEY, val DECIMAL(10,4))",
                test_sql="INSERT INTO dec_test VALUES (1, 12345.6789)",
                expected_result=1,
            ),
            EdgeCaseTest(
                name="numeric_decimal_rounding",
                description="Decimal rounding behavior",
                category="numeric",
                setup_sql="CREATE TABLE IF NOT EXISTS dec_test (id INT PRIMARY KEY, val DECIMAL(10,2))",
                test_sql="INSERT INTO dec_test VALUES (1, 10.005)",  # Rounds to 10.01 or 10.00?
                expected_result=1,
            ),
        ]


class StringEdgeCaseTests:
    """String type edge cases."""

    @staticmethod
    def get_all_tests() -> List[EdgeCaseTest]:
        return [
            EdgeCaseTest(
                name="string_empty_vs_null",
                description="Empty string vs NULL distinction",
                category="string",
                setup_sql="CREATE TABLE IF NOT EXISTS str_test (id INT PRIMARY KEY, val VARCHAR(50))",
                test_sql="INSERT INTO str_test VALUES (1, ''), (2, NULL)",
                expected_result=2,
            ),
            EdgeCaseTest(
                name="string_unicode_basic",
                description="Unicode BMP characters",
                category="string",
                setup_sql="CREATE TABLE IF NOT EXISTS str_test (id INT PRIMARY KEY, val VARCHAR(100))",
                test_sql="INSERT INTO str_test VALUES (1, '日本語テキスト')",
                expected_result=1,
            ),
            EdgeCaseTest(
                name="string_unicode_emoji",
                description="Emoji (non-BMP) handling",
                category="string",
                setup_sql="CREATE TABLE IF NOT EXISTS str_test (id INT PRIMARY KEY, val VARCHAR(100))",
                test_sql="INSERT INTO str_test VALUES (1, 'Hello 👋 World 🌍')",
                expected_result=1,
            ),
        ]


class DateTimeEdgeCaseTests:
    """Date/Time edge cases."""

    @staticmethod
    def get_all_tests() -> List[EdgeCaseTest]:
        return [
            EdgeCaseTest(
                name="datetime_leap_year",
                description="February 29 on leap year",
                category="datetime",
                setup_sql="CREATE TABLE IF NOT EXISTS dt_test (id INT PRIMARY KEY, val DATE)",
                test_sql="INSERT INTO dt_test VALUES (1, '2024-02-29')",
                expected_result=1,
            ),
            EdgeCaseTest(
                name="datetime_invalid_date",
                description="Invalid date should error",
                category="datetime",
                setup_sql="CREATE TABLE IF NOT EXISTS dt_test (id INT PRIMARY KEY, val DATE)",
                test_sql="INSERT INTO dt_test VALUES (1, '2024-13-45')",
                expected_result=0,
                expected_error=True,
            ),
        ]


class NullHandlingTests:
    """NULL handling edge cases."""

    @staticmethod
    def get_all_tests() -> List[EdgeCaseTest]:
        return [
            EdgeCaseTest(
                name="null_aggregate_count",
                description="COUNT(*) vs COUNT(column) with NULLs",
                category="null",
                setup_sql="""
                    CREATE TABLE IF NOT EXISTS null_test (id INT PRIMARY KEY, val INT);
                    INSERT INTO null_test VALUES (1, 10), (2, NULL), (3, 30)
                """,
                test_sql="SELECT COUNT(*), COUNT(val) FROM null_test",
                expected_result=(3, 2),
            ),
        ]


def get_all_tests() -> dict:
    """Get all edge case tests organized by category."""
    return {
        'numeric': NumericEdgeCaseTests.get_all_tests(),
        'string': StringEdgeCaseTests.get_all_tests(),
        'datetime': DateTimeEdgeCaseTests.get_all_tests(),
        'null': NullHandlingTests.get_all_tests(),
    }


if __name__ == '__main__':
    tests = get_all_tests()
    total = sum(len(t) for t in tests.values())
    print(f"Total Edge Case Tests: {total}")
