#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the legacy Execution_Plan 10 benchmark harness against current ScratchBird."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType


FORBIDDEN_SCRATCHBIRD_SQL = (
    "::",
    "AUTO_INCREMENT",
    "CREATE GENERATOR",
    "DATE_FORMAT(",
    "ENGINE=INNODB",
    "GEN_ID(",
    "SERIAL",
    "TO_CHAR(",
    "WITHIN GROUP",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def load_module(name: str, path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, f"cannot load module spec for {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def require_no_forbidden_sql(sql: str, label: str) -> None:
    upper_sql = sql.upper()
    for token in FORBIDDEN_SCRATCHBIRD_SQL:
        require(token not in upper_sql, f"{label} still emits old-project SQL token {token!r}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        raise SystemExit("usage: legacy_execution_plan10_scratchbird_benchmark_compat_gate.py <repo-root>")

    repo_root = Path(argv[1]).resolve()
    harness_root = repo_root / "docs" / "reference" / "legacy_execution_plan_10_performance_parity" / "benchmark_harness"
    require(harness_root.exists(), f"missing benchmark harness root: {harness_root}")

    sql_dialect = load_module(
        "legacy_execution_plan10_sql_dialect",
        harness_root / "stress-tests" / "generators" / "sql_dialect.py",
    )
    benchmark_runner = load_module(
        "legacy_execution_plan10_benchmark_runner",
        harness_root / "scripts" / "benchmark_runner.py",
    )
    stress_runner = load_module(
        "legacy_execution_plan10_stress_test_runner",
        harness_root / "stress-tests" / "runners" / "stress_test_runner.py",
    )
    engine_adapters = load_module(
        "legacy_execution_plan10_engine_adapters",
        harness_root / "index-comparison-tests" / "adapters" / "engine_adapters.py",
    )
    benchmark_provenance = load_module(
        "legacy_execution_plan10_benchmark_provenance",
        harness_root / "benchmark_provenance.py",
    )

    dialect = sql_dialect.SQLDialectFactory.get_dialect("scratchbird")
    require(dialect.engine == "scratchbird", "ScratchBird dialect did not identify itself")
    require(dialect.get_placeholder() == "?", "ScratchBird dialect placeholder drifted")
    require("scratchbird" in sql_dialect.SQLDialectFactory.supported_engines(),
            "ScratchBird is missing from supported engines")

    create_sql = {
        "customers": dialect.create_table_customers(),
        "products": dialect.create_table_products(),
        "orders": dialect.create_table_orders(),
        "order_items": dialect.create_table_order_items(),
    }
    for label, sql in create_sql.items():
        require_no_forbidden_sql(sql, f"ScratchBird create_table_{label}")
        require("CREATE TABLE" in sql.upper(), f"ScratchBird create_table_{label} did not emit DDL")

    require(dialect.date_trunc("MONTH", "o.order_date") == "date_trunc('month', o.order_date)",
            "ScratchBird date_trunc no longer matches current SBsql")
    require(dialect.date_extract("YEAR", "o.order_date") == "date_part('year', o.order_date)",
            "ScratchBird date_extract no longer maps to date_part")
    require(dialect.date_diff_days("a", "b") == "date_diff('day', b, a)",
            "ScratchBird date_diff_days no longer maps to date_diff")
    require(dialect.percentile_cont(0.5, "", "cost") == "PERCENTILE_CONT(cost, 0.5)",
            "ScratchBird percentile_cont no longer maps to SBsql aggregate form")

    generator = sql_dialect.StressTestSQLGenerator(dialect)
    stress_methods = (
        "inner_join_simple",
        "inner_join_large_result",
        "left_join_all_customers",
        "four_table_join",
        "aggregation_daily_sales",
        "window_function_ranking",
        "bulk_insert_select",
        "bulk_update_with_case",
        "self_join_same_country",
        "multi_dimensional_agg",
        "nested_subquery_agg",
    )
    for method_name in stress_methods:
        sql = getattr(generator, method_name)()
        require(sql and isinstance(sql, str), f"{method_name} did not generate ScratchBird SQL")
        require_no_forbidden_sql(sql, f"ScratchBird stress SQL {method_name}")

    require("scratchbird" in benchmark_runner.ENGINE_CONFIGS,
            "benchmark runner missing ScratchBird engine config")
    connector = benchmark_runner.get_connector("scratchbird")
    require(connector.__class__.__name__ == "ScratchBirdConnector",
            "benchmark runner did not resolve ScratchBirdConnector")
    require(hasattr(connector, "_resolve_sb_isql"),
            "ScratchBird benchmark connector is not resolving an sb_isql binary")
    require(hasattr(connector, "execute_benchmark"),
            "ScratchBird benchmark connector is not using the sb_isql script benchmark path")
    # Exercise the real parser definition indirectly by checking the source-level
    # argument surface that monitoring depends on.
    runner_source = (harness_root / "scripts" / "benchmark_runner.py").read_text(encoding="utf-8")
    for option in (
        "--scratchbird-script-input-dir",
        "--scratchbird-script-output-dir",
        "--scratchbird-monitor-jsonl",
        "BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR",
        "BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR",
        "BENCHMARK_SCRATCHBIRD_MONITOR_JSONL",
    ):
        require(option in runner_source, f"ScratchBird benchmark runner missing monitoring option {option}")
    schema_sql = benchmark_runner.MICRO_BENCHMARKS["single_insert"]["sql"].get("scratchbird")
    copy_data = benchmark_runner.MICRO_BENCHMARKS["single_insert"]["copy_data"].get("scratchbird")
    require(schema_sql == "COPY users.public.sbsfc021_stream_table FROM STDIN",
            "ScratchBird single_insert no longer uses live COPY route")
    rendered_params = benchmark_runner.render_benchmark_params(copy_data, 7, 1000000)
    require(rendered_params == b"id=1000007;payload=benchmark\n",
            "ScratchBird benchmark COPY payload rendering drifted")
    require("scratchbird" in benchmark_runner.SCHEMA_SQL, "ScratchBird schema DDL is not registered")
    require_no_forbidden_sql(benchmark_runner.SCHEMA_SQL["scratchbird"], "ScratchBird micro benchmark schema")
    for test_name, test_config in benchmark_runner.MICRO_BENCHMARKS.items():
        require("scratchbird" in test_config["sql"], f"{test_name} missing ScratchBird SQL")
        require_no_forbidden_sql(test_config["sql"]["scratchbird"], f"ScratchBird micro benchmark {test_name}")

    required_legacy_tests = {
        "inner_join_simple",
        "inner_join_large_result",
        "inner_join_multiple_conditions",
        "left_join_all_customers",
        "four_table_join",
        "self_join_same_country",
        "bulk_update_with_join",
    }
    require(set(stress_runner.LEGACY_EXECUTION_PLAN10_TESTS) == required_legacy_tests,
            "ScratchBird stress runner no longer targets the selected Execution_Plan 10 legacy-seven surface")
    require(stress_runner.SCRATCHBIRD_CURRENT_SURFACE_ADAPTER == "scratchbird_current_native_v1",
            "ScratchBird stress runner no longer declares the current-native adapter")
    require(stress_runner.SCRATCHBIRD_CURRENT_TABLES["customers"] == "users.public.benchmark_customers",
            "ScratchBird stress runner no longer targets the current users.public benchmark tables")
    require(hasattr(stress_runner, "ScratchBirdIsqlConnection"),
            "stress runner is missing the ScratchBird sb_isql script-backed connection")
    stress_source = (
        harness_root / "stress-tests" / "runners" / "stress_test_runner.py"
    ).read_text(encoding="utf-8")
    for option in (
        "'scratchbird'",
        "--test-set",
        "legacy-seven",
        "current-native",
        "scratchbird_current_native_v1",
        "SCRATCHBIRD_CURRENT_TABLES",
        "physical_table_name",
        "copy_rows",
        "--scratchbird-script-input-dir",
        "--scratchbird-script-output-dir",
        "--scratchbird-monitor-jsonl",
        "sb_isql executes generated current-native SQL/COPY scripts",
    ):
        require(option in stress_source, f"ScratchBird stress runner missing comparable-run support {option}")

    comparability_gate = (
        repo_root / "project" / "tests" / "sbsql_parser_worker" / "legacy_execution_plan10_comparability_gate.py"
    )
    require(comparability_gate.exists(), "Execution_Plan 10 comparability gate is missing")
    comparability_source = comparability_gate.read_text(encoding="utf-8")
    for marker in (
        "REQUIRED_LOAD_ROWS",
        "REQUIRED_TESTS",
        "MICRO_TESTS",
        "scratchbird_current_native_v1",
        "users.public.benchmark_customers",
        "--expect-comparable",
        "--expect-incomparable",
    ):
        require(marker in comparability_source, f"comparability gate missing marker {marker}")

    driver_path = repo_root / "project" / "drivers" / "driver" / "python" / "src"
    sys.path = [entry for entry in sys.path if Path(entry).resolve() != driver_path]
    engine_adapters.ensure_scratchbird_driver()
    require(str(driver_path) in sys.path, "index adapter did not resolve current in-tree ScratchBird driver")

    candidates = benchmark_provenance.scratchbird_server_binary_candidates(harness_root)
    require(repo_root / "build" / "src" / "server" / "sb_server" in candidates,
            "benchmark provenance missing current build/src/server/sb_server candidate")
    require(repo_root in benchmark_provenance.scratchbird_repo_root_candidates(harness_root),
            "benchmark provenance missing current repo root candidate")

    print("legacy_execution_plan10_scratchbird_benchmark_compat_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
