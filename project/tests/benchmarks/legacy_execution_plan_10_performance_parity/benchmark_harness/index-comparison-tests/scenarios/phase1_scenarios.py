#!/usr/bin/env python3
"""Phase 1 index comparison scenarios."""

from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple


@dataclass(frozen=True)
class IndexComparisonScenario:
    scenario_id: str
    description: str
    index_family: str
    workload_family: str
    table_name: str
    create_table_sql: str
    create_index_sql: List[str]
    query_sql: Dict[str, str]
    expected_access_patterns: List[str]
    expected_index_names: List[str]
    require_order_from_index: bool = False
    expected_row_floor: int = 1


def build_point_rows() -> List[Tuple[int, int, str]]:
    return [(row_id, row_id * 2, f"payload-{row_id}") for row_id in range(1, 1501)]


def build_range_rows() -> List[Tuple[int, int, int, str]]:
    return [
        (row_id, row_id, row_id % 97, f"segment-{row_id % 23}")
        for row_id in range(1, 2001)
    ]


def build_composite_rows() -> List[Tuple[int, str, int, str]]:
    states = ("active", "pending", "inactive", "archived")
    return [
        (row_id, states[row_id % len(states)], row_id % 20, f"group-{row_id % 41}")
        for row_id in range(1, 2501)
    ]


def phase1_insert_rows(scenario_id: str) -> Sequence[Tuple]:
    builders = {
        "btree_point_lookup": build_point_rows,
        "btree_range_scan": build_range_rows,
        "btree_composite_order": build_composite_rows,
    }
    return builders[scenario_id]()


PHASE1_SCENARIOS = [
    IndexComparisonScenario(
        scenario_id="btree_point_lookup",
        description="Selective lookup through a secondary B-tree index.",
        index_family="btree",
        workload_family="point_lookup",
        table_name="idxcmp_point_lookup",
        create_table_sql="""
            CREATE TABLE idxcmp_point_lookup (
                id INTEGER NOT NULL PRIMARY KEY,
                lookup_key INTEGER NOT NULL,
                payload VARCHAR(128) NOT NULL
            )
        """.strip(),
        create_index_sql=[
            "CREATE INDEX idx_idxcmp_point_lookup_key ON idxcmp_point_lookup (lookup_key)"
        ],
        query_sql={
            "firebird": "SELECT id, payload FROM idxcmp_point_lookup WHERE lookup_key = 842",
            "mysql": "SELECT id, payload FROM idxcmp_point_lookup WHERE lookup_key = 842",
            "postgresql": "SELECT id, payload FROM idxcmp_point_lookup WHERE lookup_key = 842",
            "scratchbird": "SELECT id, payload FROM idxcmp_point_lookup WHERE lookup_key = 842"
        },
        expected_access_patterns=["index_scan", "index_lookup", "index_only_scan"],
        expected_index_names=["IDX_IDXCMP_POINT_LOOKUP_KEY"]
    ),
    IndexComparisonScenario(
        scenario_id="btree_range_scan",
        description="Range filter through a single-column B-tree index.",
        index_family="btree",
        workload_family="range_scan",
        table_name="idxcmp_range_scan",
        create_table_sql="""
            CREATE TABLE idxcmp_range_scan (
                id INTEGER NOT NULL PRIMARY KEY,
                range_key INTEGER NOT NULL,
                bucket INTEGER NOT NULL,
                payload VARCHAR(64) NOT NULL
            )
        """.strip(),
        create_index_sql=[
            "CREATE INDEX idx_idxcmp_range_scan_key ON idxcmp_range_scan (range_key)"
        ],
        query_sql={
            "firebird": "SELECT id, range_key FROM idxcmp_range_scan WHERE range_key BETWEEN 300 AND 600",
            "mysql": "SELECT id, range_key FROM idxcmp_range_scan WHERE range_key BETWEEN 300 AND 600",
            "postgresql": "SELECT id, range_key FROM idxcmp_range_scan WHERE range_key BETWEEN 300 AND 600",
            "scratchbird": "SELECT id, range_key FROM idxcmp_range_scan WHERE range_key BETWEEN 300 AND 600"
        },
        expected_access_patterns=["index_scan", "range_scan", "index_lookup"],
        expected_index_names=["IDX_IDXCMP_RANGE_SCAN_KEY"]
    ),
    IndexComparisonScenario(
        scenario_id="btree_composite_order",
        description="Composite predicate with ordered output satisfied by a composite B-tree index.",
        index_family="btree",
        workload_family="order_preserving_scan",
        table_name="idxcmp_composite_order",
        create_table_sql="""
            CREATE TABLE idxcmp_composite_order (
                id INTEGER NOT NULL PRIMARY KEY,
                status VARCHAR(16) NOT NULL,
                category INTEGER NOT NULL,
                payload VARCHAR(64) NOT NULL
            )
        """.strip(),
        create_index_sql=[
            "CREATE INDEX idx_idxcmp_composite_sci ON idxcmp_composite_order (status, category, id)"
        ],
        query_sql={
            "firebird": (
                "SELECT id, status, category "
                "FROM idxcmp_composite_order "
                "WHERE status = 'active' AND category BETWEEN 5 AND 9 "
                "ORDER BY category, id ROWS 25"
            ),
            "mysql": (
                "SELECT id, status, category "
                "FROM idxcmp_composite_order "
                "WHERE status = 'active' AND category BETWEEN 5 AND 9 "
                "ORDER BY category, id LIMIT 25"
            ),
            "postgresql": (
                "SELECT id, status, category "
                "FROM idxcmp_composite_order "
                "WHERE status = 'active' AND category BETWEEN 5 AND 9 "
                "ORDER BY category, id LIMIT 25"
            ),
            "scratchbird": (
                "SELECT id, status, category "
                "FROM idxcmp_composite_order "
                "WHERE status = 'active' AND category BETWEEN 5 AND 9 "
                "ORDER BY category, id LIMIT 25"
            )
        },
        expected_access_patterns=["index_scan", "range_scan", "index_lookup"],
        expected_index_names=["IDX_IDXCMP_COMPOSITE_SCI"],
        require_order_from_index=True
    )
]
