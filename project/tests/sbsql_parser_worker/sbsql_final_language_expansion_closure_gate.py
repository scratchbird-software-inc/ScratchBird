#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the current Core SBsql/SBLR authority and obligation closure.

This gate deliberately distinguishes semantic authority from implementation
evidence.  Manifest-admitted files under ``Specifications/Core`` define the
language and SBLR contracts.  The active alignment workplan is checked only as
an exact implementation/test-obligation inventory; it is never used to infer
runtime behavior or final implementation acceptance.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from pathlib import Path
import re
import sys
import uuid

import yaml


CORE_REGISTRY_FILES = {
    "surface": "registries/sbsql-consolidated-surface-registry.csv",
    "surface_to_sblr": "registries/sbsql-consolidated-surface-to-sblr.csv",
    "command_closure": "registries/sbsql-command-sblr-zero-grey-closure.csv",
    "opcode_closure": "registries/sblr-opcode-executor-zero-grey-closure.csv",
    "operation_matrix": "registries/sblr-operation-matrix.yaml",
    "result_shapes": "registries/result-shape-registry.yaml",
}

WORKPLAN_FILES = {
    "implementation": "IMPLEMENTATION_LEDGER.csv",
    "tests": "TEST_LEDGER.csv",
    "areas": "AREA_MATRIX.csv",
    "tracker": "TRACKER.csv",
    "validation_report": "VALIDATION_REPORT.md",
}

SURFACE_COLUMNS = {
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "status",
    "cluster_scope",
    "sblr_operation_family",
    "documentation_family",
}

SURFACE_TO_SBLR_COLUMNS = {
    "surface_id",
    "canonical_name",
    "sblr_operation_family",
    "ingress_envelope",
    "required_context",
    "binding_steps",
    "result_shape",
    "diagnostics",
}

COMMAND_COLUMNS = {
    "surface_id",
    "canonical_name",
    "surface_kind",
    "family",
    "language_intent_status",
    "specification_state",
    "implementation_state",
    "grammar_key",
    "ast_node_kind",
    "bound_ast_node_kind",
    "root_route_kind",
    "root_route",
    "descriptor_contract",
    "executor_operation_id",
    "result_shape",
    "required_right",
    "mga_profile",
    "transaction_effect",
    "resource_contract",
    "cluster_contract",
    "diagnostic_key",
    "conformance_fixture",
    "canonical_contract",
    "semantic_dependency",
    "closure_notes",
}

OPCODE_COLUMNS = {
    "opcode_name",
    "opcode_code",
    "opcode_family",
    "registry_status",
    "specification_state",
    "implementation_state",
    "operand_contract",
    "result_contract",
    "transaction_effect",
    "security_class",
    "semantic_search_key",
    "executor_binding_requirement",
    "admission_behavior",
    "missing_evidence_diagnostic",
    "unsupported_diagnostic",
    "canonical_contract",
}

SBLR_STATUS_CONTRACT = {
    (
        "required",
        "specified_admitted",
    ): "require_exact_accepted_executor_evidence_else_reject_before_dispatch",
    (
        "required",
        "specified_cluster_gated",
    ): "require_cluster_provider_capability_else_fail_closed",
    ("deferred", "specified_deferred"): "reject_before_dispatch",
    (
        "optional",
        "specified_optional",
    ): "policy_or_provider_gate_then_dispatch_else_exact_refusal",
}

EXPECTED_CLUSTER_GATED_OPCODES = frozenset(
    {
        "SBLR_CLUSTER_AGENT_CONTROL",
        "SBLR_CLUSTER_AGENT_GET",
        "SBLR_CLUSTER_AGENT_LIST",
        "SBLR_CLUSTER_CONTROL_CLUSTER",
        "SBLR_CLUSTER_INSPECT_REPLICATION",
        "SBLR_CLUSTER_INSPECT_ROUTING_PLAN",
        "SBLR_CLUSTER_INSPECT_STATE",
        "SBLR_CLUSTER_PLACE_OBJECT",
        "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT",
        "SBLR_CLUSTER_SYS_AGENTS",
        "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE",
    }
)

IMPLEMENTATION_COLUMNS = {
    "item_type",
    "item_id",
    "area_id",
    "family",
    "specification_anchors",
    "expected_route",
    "primary_code_seams",
    "existing_code_evidence",
    "existing_test_evidence",
    "implementation_status",
    "required_action",
    "review_status",
}

TEST_COLUMNS = {
    "test_id",
    "item_type",
    "item_id",
    "area_id",
    "test_family",
    "required_layer",
    "controlling_authority",
    "planned_ctest_label",
    "existing_test_evidence",
    "implementation_status",
}

REQUIRED_SOURCE_TOKENS = {
    "project/tests/sbsql_parser_worker/fixtures/final_sblr_sbsql_closure/SBSQL_CONTEXT_SENSITIVE_KEYWORD_POLICY.csv": [
        "SBSQL is context-sensitive",
        "near-empty global reserved-word set",
        "sbsql_scalar_syntax_exact_route_conformance",
    ],
    "project/src/parsers/sbsql_worker/lexer/lexer.cpp": [
        "SBsql is context-sensitive",
        "not define a broad SQL reserved",
        "contextual_native",
        "contextual_literal",
    ],
    "project/src/parsers/shared/sbsql_v3_ast/sbsql_v3_ast_catalog.cpp": [
        "sbsql.private_cluster",
        "PrivateClusterAst",
        "sbsql.observability",
        "ArchiveReplicationMigrationAst",
    ],
    "project/src/parsers/shared/sbsql_v3_binding/sbsql_v3_binding_catalog.cpp": [
        "sbsql.private_cluster",
        "CLUSTER_AUTHORITY_REQUIRED",
        "private_cluster_catalog",
    ],
    "project/src/parsers/shared/sbsql_v3_api_mapping/sbsql_v3_api_mapping_catalog.cpp": [
        "SBSQL_V3_RAW_SQL_FALLBACK_FORBIDDEN",
        "SBSQL_V3_CLUSTER_AUTHORITY_REQUIRED",
        "cluster authority mappings must fail closed",
    ],
    "project/src/parsers/sbsql_worker/statement/statement_catalog.cpp": [
        "sbsql.emulated.backup_restore_non_file",
        "SBSQL.EMULATION.NON_FILE_OPERATION",
        "observability",
    ],
}

REQUIRED_CTEST_NAMES = {
    "sbsql_surface_to_sblr_function_coverage_gate",
    "sbsql_no_stub_source_integrity_gate",
    "sbsql_sblr_binary_round_trip_fixture_gate",
    "sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance",
    "sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance",
    "sbsql_cluster_private_fail_closed_conformance",
    "sbsql_non_core_optional_provider_classification_conformance",
    "sbsql_observability_exact_route_conformance",
    "sbsql_final_language_expansion_closure_gate",
}

IMPLEMENTATION_STATUSES = {
    "absent",
    "partial",
    "externally_owned_coordination_only",
}

TEST_STATUSES = {
    "test_required",
    "audit_candidate",
    "externally_owned_coordination_only",
}

TEST_FAMILIES = {
    "sbsql_command": {
        "e2e": "canonical_or_exact_refusal",
        "contract": "malformed_boundary",
        "integration": "authorization_non_disclosure",
        "fault": "resource_cancellation",
    },
    "sblr_opcode": {
        "e2e": "canonical_or_exact_refusal",
        "contract": "malformed_operand",
        "integration": "missing_executor_evidence",
        "fault": "cancellation_atomicity",
    },
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: Path, required_columns: set[str], label: str) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fieldnames = set(reader.fieldnames or ())
            rows = list(reader)
    except FileNotFoundError:
        fail(f"missing {label}: {path}")
    missing = sorted(required_columns - fieldnames)
    require(not missing, f"{label} missing columns: {missing}")
    require(rows, f"{label} has no rows: {path}")
    return rows


def require_nonempty(rows: list[dict[str, str]], columns: set[str], label: str) -> None:
    for row_number, row in enumerate(rows, 2):
        for column in columns:
            require(row[column].strip(), f"{label} row {row_number} has empty {column}")


def unique_index(
    rows: list[dict[str, str]], column: str, label: str
) -> dict[str, dict[str, str]]:
    index: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[column]
        require(value not in index, f"{label} duplicate {column}: {value}")
        index[value] = row
    return index


def resolve_control_roots(repo_root: Path) -> tuple[Path, Path, Path]:
    root = repo_root.resolve()
    require((root / "project").is_dir(), f"repo root does not contain project/: {root}")
    workspace = root.parent
    core_root = workspace / "Specifications/Core"
    workplan_root = workspace / "Workplans/sbsql-sblr-implementation-alignment"
    require(core_root.is_dir(), f"canonical Core root missing: {core_root}")
    require(workplan_root.is_dir(), f"active alignment workplan missing: {workplan_root}")
    return workspace, core_root, workplan_root


def manifest_authority_paths(core_root: Path) -> set[str]:
    manifest = core_root / "MANIFEST.yaml"
    require(manifest.is_file(), f"Core manifest missing: {manifest}")
    try:
        document = yaml.safe_load(manifest.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        fail(f"Core manifest is invalid YAML: {exc}")
    require(isinstance(document, dict), "Core manifest root is not a mapping")
    entries = document.get("authority_files")
    require(isinstance(entries, list) and entries, "Core manifest authority_files is empty")
    require(
        all(isinstance(entry, str) and entry for entry in entries),
        "Core manifest authority_files contains a non-path entry",
    )
    require(len(entries) == len(set(entries)), "Core manifest has duplicate authority_files")
    core_resolved = core_root.resolve()
    for relative in entries:
        path = (core_root / relative).resolve()
        require(
            path == core_resolved or core_resolved in path.parents,
            f"Core manifest authority escapes Core root: {relative}",
        )
        require(path.is_file(), f"Core manifest authority file missing: {relative}")

    admitted = set(entries)
    for relative in {"AUTHORITY.md", "README.md", *CORE_REGISTRY_FILES.values()}:
        require(relative in admitted, f"required Core authority is not manifest-admitted: {relative}")
    return admitted


def validate_surface_registries(
    core_root: Path,
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    surfaces = read_csv(
        core_root / CORE_REGISTRY_FILES["surface"], SURFACE_COLUMNS, "SBsql surface registry"
    )
    mappings = read_csv(
        core_root / CORE_REGISTRY_FILES["surface_to_sblr"],
        SURFACE_TO_SBLR_COLUMNS,
        "SBsql surface-to-SBLR registry",
    )
    require_nonempty(surfaces, SURFACE_COLUMNS, "SBsql surface registry")
    require_nonempty(mappings, SURFACE_TO_SBLR_COLUMNS, "SBsql surface-to-SBLR registry")

    surface_by_id = unique_index(surfaces, "surface_id", "SBsql surface registry")
    mapping_by_id = unique_index(mappings, "surface_id", "SBsql surface-to-SBLR registry")
    require(
        set(surface_by_id) == set(mapping_by_id),
        "SBsql surface and surface-to-SBLR identity sets differ",
    )

    seen_uuids: set[str] = set()
    for surface_id, surface in surface_by_id.items():
        require(surface_id.startswith("SBSQL-"), f"invalid SBsql surface id: {surface_id}")
        try:
            fixed_uuid = uuid.UUID(surface["fixed_uuid_v7"])
        except ValueError:
            fail(f"invalid fixed UUID for {surface_id}: {surface['fixed_uuid_v7']}")
        require(fixed_uuid.version == 7, f"fixed UUID is not UUIDv7 for {surface_id}")
        require(
            surface["fixed_uuid_v7"] not in seen_uuids,
            f"duplicate fixed UUIDv7: {surface['fixed_uuid_v7']}",
        )
        seen_uuids.add(surface["fixed_uuid_v7"])

        mapping = mapping_by_id[surface_id]
        require(
            mapping["canonical_name"] == surface["canonical_name"],
            f"surface name drift for {surface_id}",
        )
        if surface["status"] != "refused":
            require(
                mapping["sblr_operation_family"] == surface["sblr_operation_family"],
                f"surface operation-family drift for {surface_id}",
            )
        require(
            mapping["ingress_envelope"]
            in {"SBLRExecutionEnvelope.v3", "sbsql_domain_ddl_or_typed_value"},
            f"surface ingress envelope drift for {surface_id}",
        )
        context = {part.strip() for part in mapping["required_context"].split(";")}
        require(
            len(context) >= 4,
            f"surface required-context closure drift for {surface_id}",
        )
        binding = {part.strip() for part in mapping["binding_steps"].split(";")}
        require(
            len(binding) >= 3,
            f"surface binding/lowering closure drift for {surface_id}",
        )

    return surfaces, mappings


def validate_sblr_authority(
    core_root: Path,
) -> tuple[list[dict[str, str]], list[dict[str, str]], set[str]]:
    commands = read_csv(
        core_root / CORE_REGISTRY_FILES["command_closure"],
        COMMAND_COLUMNS,
        "SBsql command zero-grey closure",
    )
    opcodes = read_csv(
        core_root / CORE_REGISTRY_FILES["opcode_closure"],
        OPCODE_COLUMNS,
        "SBLR opcode zero-grey closure",
    )
    require_nonempty(commands, COMMAND_COLUMNS, "SBsql command zero-grey closure")
    require_nonempty(opcodes, OPCODE_COLUMNS, "SBLR opcode zero-grey closure")

    opcode_by_name = unique_index(opcodes, "opcode_name", "SBLR opcode closure")
    executor_bindings: set[str] = set()
    cluster_gated_opcodes: set[str] = set()
    for opcode in opcodes:
        name = opcode["opcode_name"]
        require(name.startswith("SBLR_"), f"invalid SBLR opcode name: {name}")
        require(opcode["opcode_code"].isdigit(), f"invalid opcode code for {name}")
        require(
            opcode["executor_binding_requirement"] not in executor_bindings,
            f"duplicate SBLR executor binding: {opcode['executor_binding_requirement']}",
        )
        executor_bindings.add(opcode["executor_binding_requirement"])
        status_key = (opcode["registry_status"], opcode["specification_state"])
        require(
            status_key in SBLR_STATUS_CONTRACT,
            "unknown SBLR registry-status/specification-state contract "
            f"for {name}: {status_key[0]} + {status_key[1]}",
        )
        expected_admission = SBLR_STATUS_CONTRACT[status_key]
        require(
            opcode["admission_behavior"] == expected_admission,
            f"SBLR admission-policy drift for {name}",
        )
        if status_key == ("required", "specified_cluster_gated"):
            cluster_gated_opcodes.add(name)
        require(
            opcode["implementation_state"] == "evidence_required",
            f"SBLR authority improperly asserts implementation for {name}",
        )

    missing_cluster_gated = EXPECTED_CLUSTER_GATED_OPCODES - cluster_gated_opcodes
    unexpected_cluster_gated = cluster_gated_opcodes - EXPECTED_CLUSTER_GATED_OPCODES
    require(
        not missing_cluster_gated and not unexpected_cluster_gated,
        "SBLR cluster-gated opcode set drift: "
        f"missing={sorted(missing_cluster_gated)} "
        f"unexpected={sorted(unexpected_cluster_gated)}",
    )

    seen_command_rows: set[tuple[str, str]] = set()
    allowed_route_kinds = {
        "diagnostic_refusal",
        "sblr_opcode",
        "sblr_procedural_node",
        "syntax_dispatch",
    }
    for command in commands:
        row_key = (command["surface_id"], command["canonical_name"])
        require(row_key not in seen_command_rows, f"duplicate SBsql command row: {row_key}")
        seen_command_rows.add(row_key)
        require(
            command["root_route_kind"] in allowed_route_kinds,
            f"unknown SBsql command route kind for {row_key}: {command['root_route_kind']}",
        )
        require(
            command["implementation_state"] == "evidence_required",
            f"SBsql authority improperly asserts implementation for {row_key}",
        )
        if command["root_route_kind"] == "sblr_opcode":
            require(
                command["root_route"] in opcode_by_name,
                f"SBsql command route has no canonical SBLR opcode: {row_key}",
            )
        elif command["root_route_kind"] == "diagnostic_refusal":
            require(
                command["root_route"] == "SBLR_DIAGNOSTIC_REFUSAL",
                f"SBsql refusal route drift for {row_key}",
            )
            require(
                command["executor_operation_id"] == "not_admitted",
                f"SBsql refusal exposes executor operation for {row_key}",
            )
        elif command["root_route_kind"] == "syntax_dispatch":
            require(
                command["root_route"] == "child_surface_id_exact_dispatch",
                f"SBsql syntax-dispatch route drift for {row_key}",
            )
        else:
            require(
                command["root_route"].startswith("sblr.psql.node."),
                f"SBsql procedural route drift for {row_key}",
            )

    operation_matrix = (
        core_root / CORE_REGISTRY_FILES["operation_matrix"]
    ).read_text(encoding="utf-8")
    try:
        main_section = operation_matrix.split("envelope_families:", 1)[1].split(
            "\nenvelope_to_opcode_family_binding:", 1
        )[0]
        binding_section = operation_matrix.split(
            "envelope_to_opcode_family_binding:", 1
        )[1].split("\nenvelope_family_count_assertion:", 1)[0]
        priority_d_section = operation_matrix.split(
            "priority_D_envelope_family_additions:", 1
        )[1].split("\npriority_D_count_assertions:", 1)[0]
    except IndexError:
        fail("SBLR operation matrix is missing an envelope closure section")

    main_envelopes = set(
        re.findall(r"^  (sblr\.[A-Za-z0-9_.]+):", main_section, flags=re.MULTILINE)
    )
    binding_envelopes = set(
        re.findall(r"^  (sblr\.[A-Za-z0-9_.]+):", binding_section, flags=re.MULTILINE)
    )
    priority_d_envelopes = set(
        re.findall(r"^  - id: (sblr\.[A-Za-z0-9_.]+)", priority_d_section, flags=re.MULTILINE)
    )
    require(main_envelopes, "SBLR operation matrix has no envelope families")
    require(
        main_envelopes == binding_envelopes,
        "SBLR envelope-family/binding identity sets differ",
    )
    require(
        not (main_envelopes & priority_d_envelopes),
        "SBLR Priority-D envelope duplicates a primary envelope",
    )
    envelopes = main_envelopes | priority_d_envelopes
    total_match = re.search(r"operation_envelope_families:\s*([0-9]+)", operation_matrix)
    require(total_match is not None, "SBLR operation matrix has no total envelope assertion")
    require(
        len(envelopes) == int(total_match.group(1)),
        "SBLR operation envelope population does not match its Core assertion",
    )
    return commands, opcodes, envelopes


def validate_workplan_obligations(
    core_authorities: set[str],
    workplan_root: Path,
    commands: list[dict[str, str]],
    opcodes: list[dict[str, str]],
    envelopes: set[str],
) -> dict[str, int | str]:
    implementation = read_csv(
        workplan_root / WORKPLAN_FILES["implementation"],
        IMPLEMENTATION_COLUMNS,
        "implementation obligation ledger",
    )
    tests = read_csv(
        workplan_root / WORKPLAN_FILES["tests"], TEST_COLUMNS, "test obligation ledger"
    )
    areas = read_csv(
        workplan_root / WORKPLAN_FILES["areas"], {"area_id", "status"}, "area matrix"
    )
    tracker = read_csv(
        workplan_root / WORKPLAN_FILES["tracker"],
        {"phase_id", "status", "exit_criterion"},
        "alignment tracker",
    )
    require_nonempty(implementation, IMPLEMENTATION_COLUMNS, "implementation obligation ledger")
    require_nonempty(tests, TEST_COLUMNS, "test obligation ledger")

    implementation_by_key: dict[tuple[str, str], dict[str, str]] = {}
    for row in implementation:
        key = (row["item_type"], row["item_id"])
        require(key not in implementation_by_key, f"duplicate implementation obligation: {key}")
        require(
            row["implementation_status"] in IMPLEMENTATION_STATUSES,
            f"implementation obligation has unsupported/final status: {key} {row['implementation_status']}",
        )
        if row["area_id"] == "IA-EXT-OPT":
            require(
                row["implementation_status"] == "externally_owned_coordination_only",
                f"optimizer-owned implementation obligation loses ownership boundary: {key}",
            )
        elif row["implementation_status"] == "externally_owned_coordination_only":
            fail(f"non-optimizer implementation obligation marked externally owned: {key}")
        implementation_by_key[key] = row

    command_families: dict[str, set[str]] = defaultdict(set)
    for command in commands:
        command_families[command["surface_id"]].add(command["family"])
    expected_commands = {("sbsql_command", item_id) for item_id in command_families}
    opcode_by_name = {row["opcode_name"]: row for row in opcodes}
    expected_opcodes = {("sblr_opcode", name) for name in opcode_by_name}
    expected_envelopes = {("sblr_envelope", name) for name in envelopes}
    expected_implementation = expected_commands | expected_opcodes | expected_envelopes
    require(
        set(implementation_by_key) == expected_implementation,
        "active implementation ledger does not exactly cover Core command/opcode/envelope identities",
    )

    for _, item_id in expected_commands:
        row = implementation_by_key[("sbsql_command", item_id)]
        require(
            row["family"] in command_families[item_id],
            f"SBsql command family drift in implementation ledger: {item_id}",
        )
    for _, item_id in expected_opcodes:
        row = implementation_by_key[("sblr_opcode", item_id)]
        require(
            row["family"] == opcode_by_name[item_id]["opcode_family"],
            f"SBLR opcode family drift in implementation ledger: {item_id}",
        )
    for _, item_id in expected_envelopes:
        row = implementation_by_key[("sblr_envelope", item_id)]
        require(
            row["family"] == item_id,
            f"SBLR envelope family drift in implementation ledger: {item_id}",
        )

    test_ids: set[str] = set()
    tests_by_item: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in tests:
        test_id = row["test_id"]
        require(test_id not in test_ids, f"duplicate test obligation id: {test_id}")
        test_ids.add(test_id)
        key = (row["item_type"], row["item_id"])
        require(key in implementation_by_key, f"test obligation has no implementation item: {key}")
        require(
            row["item_type"] in TEST_FAMILIES,
            f"unexpected directly tested item type: {row['item_type']}",
        )
        require(
            row["implementation_status"] in TEST_STATUSES,
            f"test obligation has unsupported/final status: {test_id} {row['implementation_status']}",
        )
        require(
            row["area_id"] == implementation_by_key[key]["area_id"],
            f"test/implementation area drift for {test_id}",
        )
        if row["area_id"] == "IA-EXT-OPT":
            require(
                row["implementation_status"] == "externally_owned_coordination_only"
                and row["planned_ctest_label"] == "external_optimizer_proof_chain",
                f"optimizer-owned test obligation loses ownership boundary: {test_id}",
            )
        else:
            require(
                row["area_id"] in row["planned_ctest_label"]
                and row["required_layer"] in row["planned_ctest_label"],
                f"test obligation label loses area/layer identity: {test_id}",
            )
        for authority in row["controlling_authority"].split("|"):
            authority_path = authority.split("#", 1)[0]
            require(
                authority_path in core_authorities,
                f"test obligation cites non-admitted Core authority: {test_id} {authority_path}",
            )
        tests_by_item[key].append(row)

    directly_tested_items = expected_commands | expected_opcodes
    require(
        set(tests_by_item) == directly_tested_items,
        "test ledger does not exactly cover every Core command and opcode identity",
    )
    required_layers = {"contract", "integration", "fault", "e2e"}
    for key, item_tests in tests_by_item.items():
        require(len(item_tests) == 4, f"item does not have four test obligations: {key}")
        by_layer = {row["required_layer"]: row for row in item_tests}
        require(set(by_layer) == required_layers, f"item layer closure drift: {key}")
        expected_families = TEST_FAMILIES[key[0]]
        for layer, row in by_layer.items():
            require(
                row["test_family"] == expected_families[layer],
                f"item test-family drift: {key} {layer}",
            )

    area_ids = {row["area_id"] for row in areas}
    require(
        {row["area_id"] for row in implementation} <= area_ids,
        "implementation ledger contains an area absent from AREA_MATRIX.csv",
    )
    tracker_by_phase = unique_index(tracker, "phase_id", "alignment tracker")
    require(
        tracker_by_phase.get("IA-14", {}).get("status") == "pending",
        "alignment tracker no longer records the open full-corpus acceptance phase",
    )
    require(
        tracker_by_phase.get("IA-15", {}).get("status") == "pending",
        "alignment tracker no longer records the open owner-acceptance phase",
    )
    report = (workplan_root / WORKPLAN_FILES["validation_report"]).read_text(encoding="utf-8")
    require("Status: in progress" in report, "alignment validation report loses in-progress status")
    require(
        "not final workplan acceptance" in report,
        "alignment validation report no longer disclaims final implementation acceptance",
    )

    return {
        "implementation_items": len(implementation),
        "test_obligations": len(tests),
        "command_identities": len(expected_commands),
        "opcode_identities": len(expected_opcodes),
        "envelope_identities": len(expected_envelopes),
        "workplan_status": "in_progress",
    }


def validate_source_anchors(root: Path) -> None:
    for relative, tokens in REQUIRED_SOURCE_TOKENS.items():
        path = root / relative
        require(path.is_file(), f"source anchor missing: {relative}")
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in tokens:
            require(token in text, f"{relative} missing token {token}")

    lexer = (root / "project/src/parsers/sbsql_worker/lexer/lexer.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    forbidden_lexer_tokens = (
        "static const std::unordered_set<std::string> reserved",
        "Contains(reserved",
        'return "reserved_native"',
        '"reserved_native",',
    )
    for token in forbidden_lexer_tokens:
        require(token not in lexer, f"SBsql lexer encodes a broad reserved-word model: {token}")
    require(
        '"SELECT"' in lexer and '"WHERE"' in lexer,
        "SBsql lexer lost core command-word classification evidence",
    )
    require(
        'return "contextual_native"' in lexer,
        "SBsql lexer does not classify command words as contextual",
    )


def generated_ctest_names(build_root: Path) -> set[str]:
    names: set[str] = set()
    for ctest_file in build_root.rglob("CTestTestfile.cmake"):
        text = ctest_file.read_text(encoding="utf-8", errors="replace")
        names.update(
            re.findall(r"add_test\(\[=*\[([^]]+)\]=*\]", text, flags=re.MULTILINE)
        )
    return names


def validate_ctest_registration(root: Path, build_root: Path | None) -> None:
    if build_root is not None:
        require(build_root.is_dir(), f"build root missing: {build_root}")
        names = generated_ctest_names(build_root)
        require(names, f"build root has no generated CTest inventory: {build_root}")
        missing = sorted(REQUIRED_CTEST_NAMES - names)
        require(not missing, f"generated CTest inventory missing SBsql closure tests: {missing}")
        return

    text = (root / "project/tests/sbsql_parser_worker/CMakeLists.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    for name in REQUIRED_CTEST_NAMES:
        require(name in text, f"CTest source registration missing {name}")


def validate_all(root: Path, build_root: Path | None) -> dict[str, int | str]:
    _, core_root, workplan_root = resolve_control_roots(root)
    core_authorities = manifest_authority_paths(core_root)
    surfaces, _ = validate_surface_registries(core_root)
    commands, opcodes, envelopes = validate_sblr_authority(core_root)
    summary = validate_workplan_obligations(
        core_authorities, workplan_root, commands, opcodes, envelopes
    )
    summary["manifest_authorities"] = len(core_authorities)
    summary["surface_rows"] = len(surfaces)
    summary["command_rows"] = len(commands)
    validate_source_anchors(root)
    validate_ctest_registration(root, build_root)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve() if args.build_root else None
    summary = validate_all(root, build_root)
    print(
        "sbsql_final_language_expansion_closure_gate=passed "
        f"manifest_authorities={summary['manifest_authorities']} "
        f"surfaces={summary['surface_rows']} "
        f"command_rows={summary['command_rows']} "
        f"command_identities={summary['command_identities']} "
        f"sblr_opcodes={summary['opcode_identities']} "
        f"sblr_envelopes={summary['envelope_identities']} "
        f"test_obligations={summary['test_obligations']} "
        f"workplan_status={summary['workplan_status']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
