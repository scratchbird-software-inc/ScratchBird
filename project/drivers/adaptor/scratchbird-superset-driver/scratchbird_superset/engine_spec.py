# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Superset DB engine spec for ScratchBird."""

from __future__ import annotations

from superset.constants import TimeGrain
from superset.db_engine_specs.base import BaseEngineSpec, DatabaseCategory
from superset.sql.parse import LimitMethod


class ScratchBirdEngineSpec(BaseEngineSpec):
    engine = "scratchbird"
    engine_name = "ScratchBird"
    default_driver = "sbwp"

    supports_dynamic_schema = True
    supports_catalog = True
    allows_joins = True
    allows_subqueries = True
    allows_alias_in_select = True
    allows_alias_in_orderby = True

    limit_method = LimitMethod.FORCE_LIMIT

    metadata = {
        "description": "ScratchBird is a Firebird-inspired relational database with SBWP v1.1.",
        "logo": "scratchbird.png",
        "homepage_url": "https://scratchbird.invalid/driver",
        "categories": [
            DatabaseCategory.TRADITIONAL_RDBMS,
            DatabaseCategory.OPEN_SOURCE,
        ],
        "pypi_packages": ["scratchbird-superset", "scratchbird"],
        "version_requirements": "scratchbird>=0.1.0",
        "connection_string": "scratchbird://{username}:{password}@{host}:{port}/{database}",
        "default_port": 3092,
        "connection_examples": [
            {
                "description": "Local ScratchBird instance",
                "connection_string": "scratchbird://user:pass@localhost:3092/demo?sslmode=require",
            }
        ],
        "notes": "TLS is required. Superset uses the ScratchBird SQLAlchemy dialect.",
    }

    _time_grain_expressions = {
        None: "{col}",
        TimeGrain.SECOND: "DATE_TRUNC('second', {col})",
        TimeGrain.MINUTE: "DATE_TRUNC('minute', {col})",
        TimeGrain.HOUR: "DATE_TRUNC('hour', {col})",
        TimeGrain.DAY: "DATE_TRUNC('day', {col})",
        TimeGrain.WEEK: "DATE_TRUNC('week', {col})",
        TimeGrain.MONTH: "DATE_TRUNC('month', {col})",
        TimeGrain.QUARTER: "DATE_TRUNC('quarter', {col})",
        TimeGrain.YEAR: "DATE_TRUNC('year', {col})",
    }
