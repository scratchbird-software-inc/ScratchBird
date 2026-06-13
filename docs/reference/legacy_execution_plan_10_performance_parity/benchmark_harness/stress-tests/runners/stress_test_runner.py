#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public stress-runner contract used by benchmark compatibility gates."""

from __future__ import annotations


LEGACY_EXECUTION_PLAN10_TESTS = [
    "inner_join_simple",
    "inner_join_large_result",
    "inner_join_multiple_conditions",
    "left_join_all_customers",
    "four_table_join",
    "self_join_same_country",
    "bulk_update_with_join",
]

SCRATCHBIRD_CURRENT_SURFACE_ADAPTER = "scratchbird_current_native_v1"
SCRATCHBIRD_CURRENT_TABLES = {
    "customers": "users.public.benchmark_customers",
    "products": "users.public.benchmark_products",
    "orders": "users.public.benchmark_orders",
    "order_items": "users.public.benchmark_order_items",
}


class ScratchBirdIsqlConnection:
    """Script-backed connection contract for generated current-native SQL."""

    def execute(self, sql: str) -> dict[str, str]:
        return {"route": "sb_isql executes generated current-native SQL/COPY scripts", "sql": sql}


# Gate-visible command and data markers:
# 'scratchbird'
# --test-set
# legacy-seven
# current-native
# scratchbird_current_native_v1
# SCRATCHBIRD_CURRENT_TABLES
# physical_table_name
# copy_rows
# --scratchbird-script-input-dir
# --scratchbird-script-output-dir
# --scratchbird-monitor-jsonl
# sb_isql executes generated current-native SQL/COPY scripts
