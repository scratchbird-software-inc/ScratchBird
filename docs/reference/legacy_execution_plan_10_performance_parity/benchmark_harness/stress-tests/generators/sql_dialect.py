#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ScratchBird SQL dialect snippets for public benchmark comparability gates."""

from __future__ import annotations


class ScratchBirdDialect:
    engine = "scratchbird"

    def get_placeholder(self) -> str:
        return "?"

    def create_table_customers(self) -> str:
        return "CREATE TABLE users.public.benchmark_customers (id BIGINT, country TEXT)"

    def create_table_products(self) -> str:
        return "CREATE TABLE users.public.benchmark_products (id BIGINT, cost NUMERIC)"

    def create_table_orders(self) -> str:
        return "CREATE TABLE users.public.benchmark_orders (id BIGINT, customer_id BIGINT, order_date TIMESTAMP)"

    def create_table_order_items(self) -> str:
        return "CREATE TABLE users.public.benchmark_order_items (order_id BIGINT, product_id BIGINT, quantity INTEGER)"

    def date_trunc(self, part: str, expression: str) -> str:
        return f"date_trunc('{part.lower()}', {expression})"

    def date_extract(self, part: str, expression: str) -> str:
        return f"date_part('{part.lower()}', {expression})"

    def date_diff_days(self, left: str, right: str) -> str:
        return f"date_diff('day', {right}, {left})"

    def percentile_cont(self, percentile: float, _order: str, expression: str) -> str:
        return f"PERCENTILE_CONT({expression}, {percentile})"


class SQLDialectFactory:
    @staticmethod
    def get_dialect(engine: str) -> ScratchBirdDialect:
        if engine != "scratchbird":
            raise ValueError(f"unsupported engine: {engine}")
        return ScratchBirdDialect()

    @staticmethod
    def supported_engines() -> list[str]:
        return ["scratchbird"]


class StressTestSQLGenerator:
    def __init__(self, dialect: ScratchBirdDialect) -> None:
        self.dialect = dialect

    def inner_join_simple(self) -> str:
        return "SELECT c.id FROM users.public.benchmark_customers c JOIN users.public.benchmark_orders o ON o.customer_id = c.id"

    def inner_join_large_result(self) -> str:
        return "SELECT o.id, c.country FROM users.public.benchmark_orders o JOIN users.public.benchmark_customers c ON c.id = o.customer_id"

    def left_join_all_customers(self) -> str:
        return "SELECT c.id, o.id FROM users.public.benchmark_customers c LEFT JOIN users.public.benchmark_orders o ON o.customer_id = c.id"

    def four_table_join(self) -> str:
        return "SELECT c.id, p.cost FROM users.public.benchmark_customers c JOIN users.public.benchmark_orders o ON o.customer_id = c.id JOIN users.public.benchmark_order_items oi ON oi.order_id = o.id JOIN users.public.benchmark_products p ON p.id = oi.product_id"

    def aggregation_daily_sales(self) -> str:
        return "SELECT date_trunc('day', o.order_date), COUNT(*) FROM users.public.benchmark_orders o GROUP BY date_trunc('day', o.order_date)"

    def window_function_ranking(self) -> str:
        return "SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM users.public.benchmark_orders"

    def bulk_insert_select(self) -> str:
        return "INSERT INTO users.public.benchmark_orders SELECT id, id, CURRENT_TIMESTAMP FROM users.public.benchmark_customers"

    def bulk_update_with_case(self) -> str:
        return "UPDATE users.public.benchmark_products SET cost = CASE WHEN cost > 0 THEN cost ELSE 1 END"

    def self_join_same_country(self) -> str:
        return "SELECT a.id, b.id FROM users.public.benchmark_customers a JOIN users.public.benchmark_customers b ON a.country = b.country"

    def multi_dimensional_agg(self) -> str:
        return "SELECT c.country, COUNT(*), SUM(oi.quantity) FROM users.public.benchmark_customers c JOIN users.public.benchmark_orders o ON o.customer_id = c.id JOIN users.public.benchmark_order_items oi ON oi.order_id = o.id GROUP BY c.country"

    def nested_subquery_agg(self) -> str:
        return "SELECT c.country FROM users.public.benchmark_customers c WHERE c.id IN (SELECT o.customer_id FROM users.public.benchmark_orders o)"
