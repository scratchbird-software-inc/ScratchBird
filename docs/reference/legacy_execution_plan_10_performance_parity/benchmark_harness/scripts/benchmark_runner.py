#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Minimal public ScratchBird benchmark runner contract for compatibility gates."""

from __future__ import annotations


SCHEMA_SQL = {
    "scratchbird": """
CREATE TABLE users.public.sbsfc021_stream_table (
    id BIGINT,
    payload TEXT
)
"""
}

MICRO_BENCHMARKS = {
    "single_insert": {
        "sql": {"scratchbird": "COPY users.public.sbsfc021_stream_table FROM STDIN"},
        "copy_data": {"scratchbird": "id={id};payload=benchmark\n"},
    },
    "single_select": {
        "sql": {"scratchbird": "SELECT id, payload FROM users.public.sbsfc021_stream_table WHERE id = ?"},
        "copy_data": {"scratchbird": "id={id};payload=benchmark\n"},
    },
}

ENGINE_CONFIGS = {
    "scratchbird": {
        "wire": "sbwp_v1_1",
        "route": "listener/parser/server/engine",
    }
}


class ScratchBirdConnector:
    """Script-backed connector shape used by the public benchmark gate."""

    def _resolve_sb_isql(self) -> str:
        return "build/output/current/bin/sb_isql"

    def execute_benchmark(self, sql: str) -> dict[str, str]:
        return {"engine": "scratchbird", "route": "sb_isql script", "sql": sql}


def get_connector(engine: str) -> ScratchBirdConnector:
    if engine != "scratchbird":
        raise ValueError(f"unsupported engine: {engine}")
    return ScratchBirdConnector()


def render_benchmark_params(copy_data: dict[str, str] | str, offset: int, base_id: int) -> bytes:
    template = copy_data["scratchbird"] if isinstance(copy_data, dict) else copy_data
    return template.format(id=base_id + offset).encode("utf-8")


# Public monitoring options required by the compatibility gate:
# --scratchbird-script-input-dir
# --scratchbird-script-output-dir
# --scratchbird-monitor-jsonl
# BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR
# BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR
# BENCHMARK_SCRATCHBIRD_MONITOR_JSONL
