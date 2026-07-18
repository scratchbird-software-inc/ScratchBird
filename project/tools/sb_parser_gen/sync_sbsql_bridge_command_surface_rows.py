#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Synchronize canonical SBsql bridge rows into tracked public artifacts.

The public repository deliberately does not contain the legacy canonicalization
CSV inputs or the private coverage registry that older versions of this tool
attempted to rewrite.  The complete public bridge record instead consists of
the tracked implementation backlog, batch membership, semantic-oracle map, and
batching-plan CSVs below.  This tool never opens or writes any other matrix.

Checking is the default and is read-only.  ``--update`` is required before an
existing public artifact tree can be changed.  ``--output-root`` permits a
caller to stage the four public outputs in a copied tree before applying an
in-place update to a checkout.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import os
import sys
import tempfile
from pathlib import Path
from types import ModuleType


FULL_PARSER_ARTIFACT_ROOT = Path(
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
BACKLOG = FULL_PARSER_ARTIFACT_ROOT / "SURFACE_IMPLEMENTATION_BACKLOG.csv"
BATCH_MEMBERSHIP = FULL_PARSER_ARTIFACT_ROOT / "BATCH_ROW_MEMBERSHIP.csv"
ORACLE_MAP = FULL_PARSER_ARTIFACT_ROOT / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
REGISTRY_BATCHING_PLAN = FULL_PARSER_ARTIFACT_ROOT / "REGISTRY_FAMILY_BATCHING_PLAN.csv"
BRIDGE_MODULE = Path("project/tools/sb_parser_gen/sbsql_bridge_command_surface.py")
BRIDGE_BATCH_ID = "BATCH-0077"

BACKLOG_FIELDS = (
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "source_status",
    "cluster_scope",
    "source_search_key",
    "canonical_spec",
    "sblr_operation_family",
    "parser_packet",
    "engine_packet",
    "owner_lane",
    "target_file_group",
    "parser_target_behavior",
    "udr_target_behavior",
    "server_target_behavior",
    "engine_target_behavior",
    "diagnostic_target",
    "validation_fixture_id",
    "final_acceptance_rule",
    "closure_action",
    "status",
)
BATCH_FIELDS = (
    "batch_id",
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "family",
    "surface_kind",
    "source_status",
    "cluster_scope",
    "owner_lane",
    "validation_fixture_id",
    "ctest_label",
    "source_search_key",
    "status",
)
ORACLE_FIELDS = (
    "fixture_id",
    "surface_id",
    "oracle_type",
    "oracle_source",
    "source_search_key",
    "expected_result_summary",
    "status",
)
BATCH_PLAN_FIELDS = (
    "batch_id",
    "source_matrix",
    "surface_filter",
    "row_count",
    "owner_lane",
    "parser_target",
    "udr_target",
    "server_target",
    "engine_target",
    "diagnostic_target",
    "fixture_target",
    "ctest_label",
    "max_batch_size",
    "depends_on",
    "status",
)

def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def public_file(root: Path, relative: Path, label: str) -> Path:
    """Return an existing, non-symlink tracked-file candidate under ``root``."""

    if relative.is_absolute() or ".." in relative.parts:
        fail(f"{label} has an unsafe relative path: {relative}")
    path = root / relative
    if not path.is_file():
        fail(f"required public {label} missing: {path}")
    if path.is_symlink():
        fail(f"public {label} must not be a symlink: {path}")
    try:
        path.resolve(strict=True).relative_to(root)
    except ValueError:
        fail(f"public {label} resolves outside the selected root: {path}")
    return path


def read_csv(
    path: Path,
    label: str,
    required_fields: tuple[str, ...],
) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            fail(f"required public {label} has no header: {path}")
        fieldnames = list(reader.fieldnames)
        if len(fieldnames) != len(set(fieldnames)):
            fail(f"required public {label} has duplicate header fields: {path}")
        missing = [field for field in required_fields if field not in fieldnames]
        if missing:
            fail(f"required public {label} missing header fields {missing}: {path}")
        rows: list[dict[str, str]] = []
        for line_number, row in enumerate(reader, start=2):
            if None in row or any(value is None for value in row.values()):
                fail(f"malformed public {label} CSV row {line_number}: {path}")
            rows.append(dict(row))
    return fieldnames, rows


def unique_index(
    rows: list[dict[str, str]],
    key: str,
    label: str,
) -> dict[str, int]:
    index: dict[str, int] = {}
    for row_index, row in enumerate(rows):
        value = row.get(key, "")
        if not value:
            fail(f"{label} row {row_index + 2} missing {key}")
        if value in index:
            fail(f"{label} duplicate {key} {value}")
        index[value] = row_index
    return index


def load_bridge_module(root: Path) -> ModuleType:
    """Load the public bridge definition from the selected repository root."""

    module_path = public_file(root, BRIDGE_MODULE, "bridge command surface module")
    module_name = "scratchbird_public_sbsql_bridge_command_surface"
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    if spec is None or spec.loader is None:
        fail(f"cannot load public bridge command surface module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # pragma: no cover - reports malformed checked-in module
        sys.modules.pop(module_name, None)
        fail(f"cannot execute public bridge command surface module: {exc}")

    for attribute in (
        "BRIDGE_COMMAND_SURFACES",
        "BRIDGE_CTEST",
        "CANONICAL_SPEC",
        "ENGINE_PACKET",
        "PARSER_PACKET",
        "SBLR_OPERATION_FAMILY",
    ):
        if not hasattr(module, attribute):
            fail(f"public bridge command surface module missing {attribute}")
    return module


def bridge_surfaces(bridge: ModuleType) -> tuple[object, ...]:
    surfaces = tuple(bridge.BRIDGE_COMMAND_SURFACES)
    if not surfaces:
        fail("public bridge command surface module has no rows")

    seen_surface_ids: set[str] = set()
    seen_names: set[str] = set()
    seen_uuids: set[str] = set()
    for surface in surfaces:
        for attribute in (
            "surface_id",
            "fixed_uuid_v7",
            "canonical_name",
            "surface_kind",
            "family",
            "status",
            "cluster_scope",
            "opcode",
            "effective_udr_operation",
        ):
            value = getattr(surface, attribute, None)
            if not isinstance(value, str) or not value:
                fail(f"bridge surface has missing {attribute}: {surface!r}")
        if surface.surface_id in seen_surface_ids:
            fail(f"duplicate public bridge surface id {surface.surface_id}")
        if surface.canonical_name in seen_names:
            fail(f"duplicate public bridge canonical_name {surface.canonical_name}")
        if surface.fixed_uuid_v7 in seen_uuids:
            fail(f"duplicate public bridge fixed_uuid_v7 {surface.fixed_uuid_v7}")
        if surface.family != "bridge":
            fail(f"{surface.surface_id} bridge module family drift: {surface.family}")
        if surface.surface_kind != "grammar_production":
            fail(
                f"{surface.surface_id} bridge module surface_kind drift: "
                f"{surface.surface_kind}"
            )
        if surface.status != "native_now":
            fail(f"{surface.surface_id} bridge module status drift: {surface.status}")
        seen_surface_ids.add(surface.surface_id)
        seen_names.add(surface.canonical_name)
        seen_uuids.add(surface.fixed_uuid_v7)
    return surfaces


def validation_fixture_id(surface: object) -> str:
    return f"SBSQL-SURFACE-{surface.surface_id.removeprefix('SBSQL-')}"


def backlog_row(surface: object, bridge: ModuleType) -> dict[str, str]:
    engine_behavior = "execute_bridge_udr_route_without_sql_text_or_reference_finality"
    if surface.cluster_route:
        engine_behavior = "compile_gate_to_cluster_provider_stub_or_public_unsupported_vector"
    elif surface.expected_refusal_code == "UDR.BRIDGE.SANDBOX_DENIED":
        engine_behavior = "deny_physical_page_copy_stream_without_server_local_file_or_page_access"
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": surface.surface_kind,
        "family": surface.family,
        "source_status": surface.status,
        "cluster_scope": surface.cluster_scope,
        "source_search_key": surface.surface_id,
        "canonical_spec": bridge.CANONICAL_SPEC,
        "sblr_operation_family": bridge.SBLR_OPERATION_FAMILY,
        "parser_packet": bridge.PARSER_PACKET,
        "engine_packet": bridge.ENGINE_PACKET,
        "owner_lane": "bridge parser worker",
        "target_file_group": "project/src/parsers/sbsql_worker/statements;project/src/parsers/sbsql_worker/lowering;project/src/server;project/src/engine/sblr;project/src/udr/sbsql_bridge",
        "parser_target_behavior": "parse_bind_lower_bridge_command_to_row_specific_sblr_bridge_operation",
        "udr_target_behavior": "route_trusted_bridge_operation_to_registered_udr_or_exact_policy_refusal",
        "server_target_behavior": "admit_revalidate_bridge_route_and_return_message_vector",
        "engine_target_behavior": engine_behavior,
        "diagnostic_target": "canonical_message_vector_and_parser_rendering",
        "validation_fixture_id": validation_fixture_id(surface),
        "final_acceptance_rule": "parse_bind_lower_server_engine_diagnostic_and_regression_evidence",
        "closure_action": "implement_full_route_or_exact_canonical_refusal",
        "status": "e2e_passed",
    }


def batch_row(surface: object, bridge: ModuleType) -> dict[str, str]:
    return {
        "batch_id": BRIDGE_BATCH_ID,
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "family": surface.family,
        "surface_kind": surface.surface_kind,
        "source_status": surface.status,
        "cluster_scope": surface.cluster_scope,
        "owner_lane": "bridge parser worker",
        "validation_fixture_id": validation_fixture_id(surface),
        "ctest_label": bridge.BRIDGE_CTEST,
        "source_search_key": surface.surface_id,
        "status": "ready_for_fixture_generation",
    }


def oracle_row(surface: object, bridge: ModuleType) -> dict[str, str]:
    refusal = ""
    if surface.expected_refusal_code:
        refusal = f", exact refusal {surface.expected_refusal_code}"
    return {
        "fixture_id": validation_fixture_id(surface),
        "surface_id": surface.surface_id,
        "oracle_type": "canonical_spec_plus_sblr_matrix",
        "oracle_source": bridge.CANONICAL_SPEC,
        "source_search_key": surface.surface_id,
        "expected_result_summary": (
            "expected parser bridge command route, SBLR bridge operation envelope, "
            f"opcode {surface.opcode}, UDR operation {surface.effective_udr_operation}, "
            "MGA-preserving local and remote transaction authority"
            f"{refusal}, and reference-specific rendering derived from the universal bridge ABI"
        ),
        "status": "closed_by_semantic_oracle_authority_gate",
    }


def batching_plan_row(bridge: ModuleType, row_count: int) -> dict[str, str]:
    return {
        "batch_id": BRIDGE_BATCH_ID,
        # The public implementation backlog is the only tracked, complete
        # source matrix available to this synchronizer.
        "source_matrix": BACKLOG.name,
        "surface_filter": "family=bridge;surface_kind=grammar_production;source_status=native_now;cluster_scope=all;SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING",
        "row_count": str(row_count),
        "owner_lane": "bridge parser worker",
        "parser_target": "generated parser registry plus exact bridge-command parser/lowering route",
        "udr_target": "trusted universal bridge UDR route or exact policy refusal",
        "server_target": "server admission/refusal/streaming behavior with bridge operation family",
        "engine_target": "SBLR bridge opcode route through registered UDR and cluster-provider stub gate",
        "diagnostic_target": "message-vector row and parser rendering fixture",
        "fixture_target": f"project/tests/sbsql_parser_worker/generated/{BRIDGE_BATCH_ID}",
        "ctest_label": bridge.BRIDGE_CTEST,
        "max_batch_size": "100",
        "depends_on": "SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING route and SBLR opcode proof",
        "status": "ready_for_fixture_generation",
    }


def validate_owned_rows(
    backlog_rows: list[dict[str, str]],
    batch_rows: list[dict[str, str]],
    oracle_rows: list[dict[str, str]],
    expected_surface_ids: set[str],
) -> None:
    """Refuse destructive repair of rows outside the canonical bridge set."""

    extra_backlog = [
        row["surface_id"]
        for row in backlog_rows
        if row.get("family") == "bridge" and row["surface_id"] not in expected_surface_ids
    ]
    if extra_backlog:
        fail(
            "SURFACE_IMPLEMENTATION_BACKLOG has bridge rows outside the public "
            f"bridge definition: {sorted(extra_backlog)[:8]}"
        )

    wrong_batch = [
        row["surface_id"]
        for row in batch_rows
        if row["surface_id"] in expected_surface_ids and row["batch_id"] != BRIDGE_BATCH_ID
    ]
    if wrong_batch:
        fail(
            "BATCH_ROW_MEMBERSHIP assigns public bridge rows outside "
            f"{BRIDGE_BATCH_ID}: {sorted(wrong_batch)[:8]}"
        )
    extra_batch = [
        row["surface_id"]
        for row in batch_rows
        if row["batch_id"] == BRIDGE_BATCH_ID
        and row["surface_id"] not in expected_surface_ids
    ]
    if extra_batch:
        fail(
            f"BATCH_ROW_MEMBERSHIP {BRIDGE_BATCH_ID} has non-bridge rows: "
            f"{sorted(extra_batch)[:8]}"
        )

    bridge_oracle_prefix = (
        "expected parser bridge command route, SBLR bridge operation envelope, "
    )
    extra_oracle = [
        row["surface_id"]
        for row in oracle_rows
        if row.get("expected_result_summary", "").startswith(bridge_oracle_prefix)
        and row["surface_id"] not in expected_surface_ids
    ]
    if extra_oracle:
        fail(
            "SEMANTIC_ORACLE_AUTHORITY_MAP has bridge rows outside the public "
            f"bridge definition: {sorted(extra_oracle)[:8]}"
        )


def reconcile_rows(
    rows: list[dict[str, str]],
    key: str,
    replacements: list[dict[str, str]],
    label: str,
) -> tuple[list[dict[str, str]], list[str]]:
    """Return a row-preserving update plan, without writing anything."""

    index = unique_index(rows, key, label)
    expected_keys: set[str] = set()
    candidate = [dict(row) for row in rows]
    changes: list[str] = []
    for replacement in replacements:
        value = replacement.get(key, "")
        if not value:
            fail(f"{label} generated replacement is missing {key}")
        if value in expected_keys:
            fail(f"{label} generated duplicate {key} {value}")
        expected_keys.add(value)
        existing_index = index.get(value)
        if existing_index is None:
            candidate.append(dict(replacement))
            changes.append(f"{label}:{value}:missing")
            continue

        existing = candidate[existing_index]
        existing_name = existing.get("canonical_name", "")
        replacement_name = replacement.get("canonical_name", "")
        if existing_name and replacement_name and existing_name != replacement_name:
            fail(
                f"{label} {value} collision: existing canonical_name={existing_name} "
                f"replacement={replacement_name}"
            )
        differing_fields = [
            field
            for field, expected in replacement.items()
            if existing.get(field, "") != expected
        ]
        if differing_fields:
            existing.update(replacement)
            changes.append(f"{label}:{value}:{','.join(differing_fields)}")
    return candidate, changes


def require_replacement_columns(
    header: list[str],
    replacements: list[dict[str, str]],
    label: str,
) -> None:
    available = set(header)
    generated = {field for row in replacements for field in row}
    missing = sorted(generated - available)
    if missing:
        fail(f"{label} cannot preserve generated fields absent from header: {missing}")


def write_outputs_atomically(
    outputs: list[tuple[Path, list[str], list[dict[str, str]]]],
) -> None:
    """Stage every changed file before replacing any public artifact."""

    staged: list[tuple[Path, Path]] = []
    try:
        for destination, header, rows in outputs:
            mode = destination.stat().st_mode & 0o777
            with tempfile.NamedTemporaryFile(
                mode="w",
                newline="",
                encoding="utf-8",
                dir=destination.parent,
                prefix=f".{destination.name}.",
                suffix=".tmp",
                delete=False,
            ) as handle:
                temporary = Path(handle.name)
                writer = csv.DictWriter(handle, fieldnames=header, lineterminator="\n")
                writer.writeheader()
                for row in rows:
                    writer.writerow({field: row.get(field, "") for field in header})
            os.chmod(temporary, mode)
            staged.append((destination, temporary))
        for destination, temporary in staged:
            os.replace(temporary, destination)
    finally:
        for _, temporary in staged:
            if temporary.exists():
                temporary.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, help="public ScratchBird checkout")
    parser.add_argument(
        "--output-root",
        help=(
            "existing public artifact tree to check or update; defaults to --repo-root. "
            "Use a copied tree to stage changes."
        ),
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="verify the public artifacts only (the default)",
    )
    mode.add_argument(
        "--update",
        action="store_true",
        help="apply the checked public-artifact update plan",
    )
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        fail(f"public repository root does not exist: {root}")
    output_root = Path(args.output_root) if args.output_root else root
    if not output_root.is_absolute():
        output_root = root / output_root
    output_root = output_root.resolve()
    if not output_root.is_dir():
        fail(f"public output root does not exist: {output_root}")

    bridge = load_bridge_module(root)
    surfaces = bridge_surfaces(bridge)
    expected_surface_ids = {surface.surface_id for surface in surfaces}

    backlog_path = public_file(output_root, BACKLOG, "SURFACE_IMPLEMENTATION_BACKLOG.csv")
    batch_path = public_file(output_root, BATCH_MEMBERSHIP, "BATCH_ROW_MEMBERSHIP.csv")
    oracle_path = public_file(output_root, ORACLE_MAP, "SEMANTIC_ORACLE_AUTHORITY_MAP.csv")
    batch_plan_path = public_file(
        output_root,
        REGISTRY_BATCHING_PLAN,
        "REGISTRY_FAMILY_BATCHING_PLAN.csv",
    )

    backlog_header, backlog_rows = read_csv(
        backlog_path, "SURFACE_IMPLEMENTATION_BACKLOG", BACKLOG_FIELDS
    )
    batch_header, batch_rows = read_csv(
        batch_path, "BATCH_ROW_MEMBERSHIP", BATCH_FIELDS
    )
    oracle_header, oracle_rows = read_csv(
        oracle_path, "SEMANTIC_ORACLE_AUTHORITY_MAP", ORACLE_FIELDS
    )
    batch_plan_header, batch_plan_rows = read_csv(
        batch_plan_path, "REGISTRY_FAMILY_BATCHING_PLAN", BATCH_PLAN_FIELDS
    )

    unique_index(backlog_rows, "surface_id", "SURFACE_IMPLEMENTATION_BACKLOG")
    unique_index(batch_rows, "surface_id", "BATCH_ROW_MEMBERSHIP")
    unique_index(oracle_rows, "surface_id", "SEMANTIC_ORACLE_AUTHORITY_MAP")
    unique_index(batch_plan_rows, "batch_id", "REGISTRY_FAMILY_BATCHING_PLAN")
    validate_owned_rows(backlog_rows, batch_rows, oracle_rows, expected_surface_ids)

    backlog_replacements = [backlog_row(surface, bridge) for surface in surfaces]
    batch_replacements = [batch_row(surface, bridge) for surface in surfaces]
    oracle_replacements = [oracle_row(surface, bridge) for surface in surfaces]
    batch_plan_replacements = [batching_plan_row(bridge, len(surfaces))]
    require_replacement_columns(
        backlog_header, backlog_replacements, "SURFACE_IMPLEMENTATION_BACKLOG"
    )
    require_replacement_columns(batch_header, batch_replacements, "BATCH_ROW_MEMBERSHIP")
    require_replacement_columns(
        oracle_header, oracle_replacements, "SEMANTIC_ORACLE_AUTHORITY_MAP"
    )
    require_replacement_columns(
        batch_plan_header, batch_plan_replacements, "REGISTRY_FAMILY_BATCHING_PLAN"
    )

    backlog_candidate, backlog_changes = reconcile_rows(
        backlog_rows,
        "surface_id",
        backlog_replacements,
        "SURFACE_IMPLEMENTATION_BACKLOG",
    )
    batch_candidate, batch_changes = reconcile_rows(
        batch_rows,
        "surface_id",
        batch_replacements,
        "BATCH_ROW_MEMBERSHIP",
    )
    oracle_candidate, oracle_changes = reconcile_rows(
        oracle_rows,
        "surface_id",
        oracle_replacements,
        "SEMANTIC_ORACLE_AUTHORITY_MAP",
    )
    batch_plan_candidate, batch_plan_changes = reconcile_rows(
        batch_plan_rows,
        "batch_id",
        batch_plan_replacements,
        "REGISTRY_FAMILY_BATCHING_PLAN",
    )

    all_changes = (
        backlog_changes + batch_changes + oracle_changes + batch_plan_changes
    )
    outputs = [
        (backlog_path, backlog_header, backlog_candidate, backlog_changes),
        (batch_path, batch_header, batch_candidate, batch_changes),
        (oracle_path, oracle_header, oracle_candidate, oracle_changes),
        (batch_plan_path, batch_plan_header, batch_plan_candidate, batch_plan_changes),
    ]

    if all_changes and not args.update:
        for change in all_changes[:40]:
            print(f"stale_public_bridge_artifact={change}", file=sys.stderr)
        if len(all_changes) > 40:
            print(
                f"... {len(all_changes) - 40} additional stale public bridge rows",
                file=sys.stderr,
            )
        print(
            "sbsql_bridge_command_surface_rows=stale "
            f"rows={len(surfaces)} changed={len(all_changes)}",
            file=sys.stderr,
        )
        return 1

    if args.update and all_changes:
        write_outputs_atomically(
            [(path, header, rows) for path, header, rows, changes in outputs if changes]
        )

    status = "synchronized" if args.update else "verified"
    print(
        f"sbsql_bridge_command_surface_rows={status} "
        f"rows={len(surfaces)} changed={len(all_changes)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
