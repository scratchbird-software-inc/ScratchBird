#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reviewed Core-route additions for generated per-surface evidence.

Older evidence generators often recorded only a public operation alias or an
implementation leaf.  That is not enough to demonstrate that a row follows
the exact Core command root or inherited parent route.  This module adds those
identities only for rows whose cited source has been reviewed and contains the
surface identity and the exact Core tuple.  It deliberately does not promote a
row or turn component coverage into E2E credit.
"""

from __future__ import annotations

import csv
from pathlib import Path


_COMMAND_SOURCES: dict[str, str] = {
    "SBSQL-01F52A6E564D": "project/tests/sbsql_parser_worker/sbsql_sbsfc_073_archive_replication_conformance.cpp",
    "SBSQL-02482A768886": "project/tests/sbsql_parser_worker/sbsql_sbsfc_075_catalog_descriptor_mutation_conformance.cpp",
    "SBSQL-13F5A8364A50": "project/tests/sbsql_parser_worker/sbsql_create_executable_exact_route_conformance.cpp",
    "SBSQL-1A0000000001": "project/tests/sbsql_parser_worker/sbsql_missing_functionality_migration_management_conformance.cpp",
    "SBSQL-1A0000000002": "project/tests/sbsql_parser_worker/sbsql_missing_functionality_migration_management_conformance.cpp",
    "SBSQL-1A0000000003": "project/tests/sbsql_parser_worker/sbsql_missing_functionality_migration_management_conformance.cpp",
    "SBSQL-1A0000000004": "project/tests/sbsql_parser_worker/sbsql_missing_functionality_migration_management_conformance.cpp",
    "SBSQL-1E702FF60BA0": "project/tests/sbsql_parser_worker/sbsql_drop_object_exact_route_conformance.cpp",
    "SBSQL-25CE560681AB": "project/tests/sbsql_parser_worker/sbsql_drop_object_exact_route_conformance.cpp",
    "SBSQL-2785A172349A": "project/tests/sbsql_parser_worker/sbsql_create_view_exact_route_conformance.cpp",
    "SBSQL-35979EDB4632": "project/tests/sbsql_parser_worker/sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp",
    "SBSQL-360A316CB38A": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-4A5F97F6CC4E": "project/tests/sbsql_parser_worker/sbsql_create_executable_exact_route_conformance.cpp",
    "SBSQL-4FAA221A7195": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-5127560F8031": "project/tests/sbsql_parser_worker/sbsql_create_executable_exact_route_conformance.cpp",
    "SBSQL-57D59EB5A619": "project/tests/sbsql_parser_worker/sbsql_sbsfc_073_archive_replication_conformance.cpp",
    "SBSQL-58224DEE5BCA": "project/tests/sbsql_parser_worker/sbsql_alter_rename_exact_route_conformance.cpp",
    "SBSQL-6C4B02DAE3FF": "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
    "SBSQL-8E675F371A9C": "project/tests/sbsql_parser_worker/sbsql_create_domain_exact_route_conformance.cpp",
    "SBSQL-A5F3182B0ED9": "project/tests/sbsql_parser_worker/sbsql_sbsfc_073_archive_replication_conformance.cpp",
    "SBSQL-AF9CF8BF1987": "project/tests/sbsql_parser_worker/sbsql_create_sequence_exact_route_conformance.cpp",
    "SBSQL-B2958D85DBE3": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-B8873CC0BD58": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-CC62E72012F0": "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
    "SBSQL-CCE2E0A8B006": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-D09825658F68": "project/tests/sbsql_parser_worker/sbsql_create_index_exact_route_conformance.cpp",
    "SBSQL-D0C7A3336A8B": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-D13498FA0EF4": "project/tests/sbsql_parser_worker/sbsql_sbsfc_075_catalog_descriptor_mutation_conformance.cpp",
    "SBSQL-F15CCA3D7F79": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
    "SBSQL-F86AC3DCC60A": "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
}

_PROMOTION_SOURCES: dict[str, str] = {
    "SBSQL-4D4C7A74054C": "project/tests/sbsql_parser_worker/sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp",
    "SBSQL-61F9B45870E1": "project/tests/sbsql_parser_worker/sbsql_query_scalar_projection_conformance.cpp",
    "SBSQL-7DA173DB6A22": "project/tests/sbsql_parser_worker/sbsql_query_scalar_projection_conformance.cpp",
    "SBSQL-9B34B7BF03F1": "project/tests/sbsql_parser_worker/sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp",
    "SBSQL-9F9AE11CDE1E": "project/tests/sbsql_parser_worker/sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp",
    "SBSQL-B4FB30E2A8B7": "project/tests/sbsql_parser_worker/sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp",
}

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_PROMOTION_REGISTRY = "registries/normalized-executable-surface-promotions-20260822.csv"


def _read_unique_rows(
    path: Path, selected_surface_ids: frozenset[str]
) -> dict[str, dict[str, str]]:
    if not path.is_file():
        raise ValueError(f"Core route registry missing: {path}")
    rows: dict[str, dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            surface_id = row.get("surface_id", "")
            if not surface_id or surface_id not in selected_surface_ids:
                continue
            if surface_id in rows:
                raise ValueError(f"Core route identity is not unique: {surface_id}")
            rows[surface_id] = row
    return rows


def _core_root(repo_root: Path) -> Path:
    return repo_root.resolve().parent / "Specifications" / "Core"


def _validate_source(repo_root: Path, relative_path: str, required: tuple[str, ...]) -> None:
    path = repo_root / relative_path
    if not path.is_file():
        raise ValueError(f"reviewed route source missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [token for token in required if token not in text]
    if missing:
        raise ValueError(
            f"reviewed route source drift: path={relative_path} missing={missing}"
        )


def validate_authoritative_runtime_inputs(repo_root: Path) -> None:
    core = _core_root(repo_root)
    command_rows = _read_unique_rows(
        core / _COMMAND_REGISTRY, frozenset(_COMMAND_SOURCES)
    )
    promotion_rows = _read_unique_rows(
        core / _PROMOTION_REGISTRY, frozenset(_PROMOTION_SOURCES)
    )
    for surface_id, source in _COMMAND_SOURCES.items():
        row = command_rows.get(surface_id)
        if row is None:
            raise ValueError(f"reviewed Core command row missing: {surface_id}")
        if row.get("root_route_kind") not in {"sblr_opcode", "procedural_ir_node"}:
            raise ValueError(f"reviewed Core command route kind drift: {surface_id}")
        _validate_source(
            repo_root,
            source,
            (surface_id, row["root_route"], row["executor_operation_id"]),
        )
    for surface_id, source in _PROMOTION_SOURCES.items():
        row = promotion_rows.get(surface_id)
        if row is None or row.get("status") != "executable_parent_route":
            raise ValueError(f"reviewed Core promotion row missing or inactive: {surface_id}")
        _validate_source(repo_root, source, (surface_id, row["sblr_route"]))


def augment_strict_ledger_row(
    repo_root: Path,
    surface: dict[str, str],
    classification: dict[str, str],
) -> dict[str, str]:
    surface_id = surface.get("surface_id", "")
    if surface_id not in _COMMAND_SOURCES and surface_id not in _PROMOTION_SOURCES:
        return classification
    result = dict(classification)
    core = _core_root(repo_root)
    if surface_id in _COMMAND_SOURCES:
        row = _read_unique_rows(
            core / _COMMAND_REGISTRY, frozenset((surface_id,))
        )[surface_id]
        addition = (
            f"core_root_route={row['root_route']};"
            f"executor_operation_id={row['executor_operation_id']};"
            "core_route_source_reviewed=true"
        )
    else:
        row = _read_unique_rows(
            core / _PROMOTION_REGISTRY, frozenset((surface_id,))
        )[surface_id]
        addition = (
            f"parent_sblr_route={row['sblr_route']};"
            "core_parent_route_source_reviewed=true"
        )
    existing = result.get("function_or_api_operation_id", "")
    result["function_or_api_operation_id"] = ";".join(filter(None, (existing, addition)))
    result["notes"] = (
        result.get("notes", "")
        + " Core root/parent identity was reviewed against the cited source; this adds route traceability only and does not upgrade the evidence boundary."
    ).strip()
    return result


def augment_per_row_manifest_row(
    repo_root: Path,
    surface: dict[str, str],
    classification: dict[str, str],
) -> dict[str, str]:
    surface_id = surface.get("surface_id", "")
    if surface_id not in _COMMAND_SOURCES and surface_id not in _PROMOTION_SOURCES:
        return classification
    result = dict(classification)
    core = _core_root(repo_root)
    if surface_id in _COMMAND_SOURCES:
        row = _read_unique_rows(
            core / _COMMAND_REGISTRY, frozenset((surface_id,))
        )[surface_id]
        addition = (
            f"core_root_route={row['root_route']};"
            f"executor_operation_id={row['executor_operation_id']};"
            "core_route_source_reviewed=true"
        )
    else:
        row = _read_unique_rows(
            core / _PROMOTION_REGISTRY, frozenset((surface_id,))
        )[surface_id]
        addition = (
            f"parent_sblr_route={row['sblr_route']};"
            "core_parent_route_source_reviewed=true"
        )
    existing = result.get("implementation_refs", "")
    result["implementation_refs"] = ";".join(filter(None, (existing, addition)))
    result["notes"] = (
        result.get("notes", "")
        + " Core root/parent identity was reviewed against the cited source; this adds route traceability only and does not upgrade the evidence boundary."
    ).strip()
    return result
