#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the EDR conformance gate matrix registered by CMake."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXPECTED_GATES = [
    {
        "edr": "EDR-001",
        "target": "database_lifecycle_execution_type_descriptor_conformance",
        "source": "execution_type_descriptor_conformance.cpp",
        "labels": ["EDR-001", "EDR-GATE-001", "execution_type_descriptor"],
    },
    {
        "edr": "EDR-002",
        "target": "database_lifecycle_execution_value_state_conformance",
        "source": "execution_value_state_conformance.cpp",
        "labels": ["EDR-002", "EDR-GATE-002", "execution_value_state"],
    },
    {
        "edr": "EDR-003",
        "target": "database_lifecycle_plain_value_payload_conformance",
        "source": "plain_value_payload_conformance.cpp",
        "labels": ["EDR-003", "EDR-GATE-003", "plain_value_payload"],
    },
    {
        "edr": "EDR-004",
        "target": "database_lifecycle_execution_data_packet_conformance",
        "source": "execution_data_packet_conformance.cpp",
        "labels": ["EDR-004", "EDR-GATE-004", "execution_data_packet"],
    },
    {
        "edr": "EDR-005",
        "target": "database_lifecycle_execution_row_batch_conformance",
        "source": "execution_row_batch_conformance.cpp",
        "labels": ["EDR-005", "EDR-GATE-005", "execution_row_batch"],
    },
    {
        "edr": "EDR-006",
        "target": "database_lifecycle_execution_result_envelope_conformance",
        "source": "execution_result_envelope_conformance.cpp",
        "labels": ["EDR-006", "EDR-GATE-006", "execution_result_envelope"],
    },
    {
        "edr": "EDR-007",
        "target": "database_lifecycle_execution_coercion_context_conformance",
        "source": "execution_coercion_context_conformance.cpp",
        "labels": ["EDR-007", "EDR-GATE-007", "execution_coercion_context"],
    },
    {
        "edr": "EDR-008",
        "target": "database_lifecycle_execution_text_temporal_semantics_conformance",
        "source": "execution_text_temporal_semantics_conformance.cpp",
        "labels": ["EDR-008", "EDR-GATE-008", "execution_text_temporal_semantics"],
    },
    {
        "edr": "EDR-009",
        "target": "database_lifecycle_execution_large_value_handle_conformance",
        "source": "execution_large_value_handle_conformance.cpp",
        "labels": ["EDR-009", "EDR-GATE-009", "execution_large_value_handle"],
    },
    {
        "edr": "EDR-010",
        "target": "database_lifecycle_execution_protected_value_conformance",
        "source": "execution_protected_value_conformance.cpp",
        "labels": ["EDR-010", "EDR-GATE-010", "execution_protected_value"],
    },
    {
        "edr": "EDR-011",
        "target": "database_lifecycle_execution_udr_bridge_value_conformance",
        "source": "execution_udr_bridge_value_conformance.cpp",
        "labels": ["EDR-011", "EDR-GATE-011", "execution_udr_bridge_value"],
    },
    {
        "edr": "EDR-012",
        "target": "database_lifecycle_execution_index_cache_identity_conformance",
        "source": "execution_index_cache_identity_conformance.cpp",
        "labels": ["EDR-012", "EDR-GATE-012", "execution_index_cache_identity"],
    },
    {
        "edr": "EDR-013",
        "target": "database_lifecycle_execution_terminology_conformance",
        "source": "execution_terminology_conformance.cpp",
        "labels": ["EDR-013", "EDR-GATE-013", "execution_terminology"],
    },
    {
        "edr": "EDR-015",
        "target": "database_lifecycle_execution_relation_descriptor_conformance",
        "source": "execution_relation_descriptor_conformance.cpp",
        "labels": ["EDR-015", "EDR-GATE-015", "execution_relation_descriptor"],
    },
    {
        "edr": "EDR-016",
        "target": "database_lifecycle_execution_cursor_handle_conformance",
        "source": "execution_cursor_handle_conformance.cpp",
        "labels": ["EDR-016", "EDR-GATE-016", "execution_cursor_handle"],
    },
    {
        "edr": "EDR-017",
        "target": "database_lifecycle_execution_rowset_value_conformance",
        "source": "execution_rowset_value_conformance.cpp",
        "labels": ["EDR-017", "EDR-GATE-017", "execution_rowset_value"],
    },
    {
        "edr": "EDR-018",
        "target": "database_lifecycle_execution_table_value_conformance",
        "source": "execution_table_value_conformance.cpp",
        "labels": ["EDR-018", "EDR-GATE-018", "execution_table_value"],
    },
    {
        "edr": "EDR-019",
        "target": "database_lifecycle_execution_routine_signature_descriptor_conformance",
        "source": "execution_routine_signature_descriptor_conformance.cpp",
        "labels": [
            "EDR-019",
            "EDR-GATE-019",
            "execution_routine_signature_descriptor",
        ],
    },
    {
        "edr": "EDR-020",
        "target": "database_lifecycle_execution_ownership_transfer_conformance",
        "source": "execution_ownership_transfer_conformance.cpp",
        "labels": [
            "EDR-020",
            "EDR-GATE-020",
            "execution_ownership_transfer",
        ],
    },
    {
        "edr": "EDR-021",
        "target": "database_lifecycle_execution_result_channel_descriptor_conformance",
        "source": "execution_result_channel_descriptor_conformance.cpp",
        "labels": [
            "EDR-021",
            "EDR-GATE-021",
            "execution_result_channel_descriptor",
        ],
    },
    {
        "edr": "EDR-022",
        "target": "database_lifecycle_execution_optimizer_safety_conformance",
        "source": "execution_optimizer_safety_conformance.cpp",
        "labels": [
            "EDR-022",
            "EDR-GATE-022",
            "execution_optimizer_safety",
        ],
    },
    {
        "edr": "EDR-023",
        "target": "database_lifecycle_execution_distributed_handle_conformance",
        "source": "execution_distributed_handle_conformance.cpp",
        "labels": [
            "EDR-023",
            "EDR-GATE-023",
            "execution_distributed_handle",
        ],
    },
    {
        "edr": "EDR-024",
        "target": "database_lifecycle_execution_domain_descriptor_conformance",
        "source": "execution_domain_descriptor_conformance.cpp",
        "labels": [
            "EDR-024",
            "EDR-GATE-024",
            "execution_domain_descriptor",
        ],
    },
    {
        "edr": "EDR-025",
        "target": "database_lifecycle_execution_domain_element_path_conformance",
        "source": "execution_domain_element_path_conformance.cpp",
        "labels": [
            "EDR-025",
            "EDR-GATE-025",
            "execution_domain_element_path",
        ],
    },
    {
        "edr": "EDR-026",
        "target": "database_lifecycle_execution_domain_cast_operation_conformance",
        "source": "execution_domain_cast_operation_conformance.cpp",
        "labels": [
            "EDR-026",
            "EDR-GATE-026",
            "execution_domain_cast_operation",
        ],
    },
    {
        "edr": "EDR-027",
        "target": "database_lifecycle_execution_type_modifier_canonicalization_conformance",
        "source": "execution_type_modifier_canonicalization_conformance.cpp",
        "labels": [
            "EDR-027",
            "EDR-GATE-027",
            "execution_type_modifier_canonicalization",
        ],
    },
    {
        "edr": "EDR-028",
        "target": "database_lifecycle_execution_enum_set_representation_conformance",
        "source": "execution_enum_set_representation_conformance.cpp",
        "labels": [
            "EDR-028",
            "EDR-GATE-028",
            "execution_enum_set_representation",
        ],
    },
    {
        "edr": "EDR-029",
        "target": "database_lifecycle_execution_range_representation_conformance",
        "source": "execution_range_representation_conformance.cpp",
        "labels": [
            "EDR-029",
            "EDR-GATE-029",
            "execution_range_representation",
        ],
    },
    {
        "edr": "EDR-030",
        "target": "database_lifecycle_execution_container_representation_conformance",
        "source": "execution_container_representation_conformance.cpp",
        "labels": [
            "EDR-030",
            "EDR-GATE-030",
            "execution_container_representation",
        ],
    },
    {
        "edr": "EDR-031",
        "target": "database_lifecycle_execution_variant_representation_conformance",
        "source": "execution_variant_representation_conformance.cpp",
        "labels": [
            "EDR-031",
            "EDR-GATE-031",
            "execution_variant_representation",
        ],
    },
    {
        "edr": "EDR-032",
        "target": "database_lifecycle_execution_polymorphic_routine_binding_conformance",
        "source": "execution_polymorphic_routine_binding_conformance.cpp",
        "labels": [
            "EDR-032",
            "EDR-GATE-032",
            "execution_polymorphic_routine_binding",
        ],
    },
    {
        "edr": "EDR-033",
        "target": "database_lifecycle_execution_comparison_key_representation_conformance",
        "source": "execution_comparison_key_representation_conformance.cpp",
        "labels": [
            "EDR-033",
            "EDR-GATE-033",
            "execution_comparison_key_representation",
        ],
    },
    {
        "edr": "EDR-034",
        "target": "database_lifecycle_execution_numeric_edge_policy_conformance",
        "source": "execution_numeric_edge_policy_conformance.cpp",
        "labels": [
            "EDR-034",
            "EDR-GATE-034",
            "execution_numeric_edge_policy",
        ],
    },
    {
        "edr": "EDR-035",
        "target": "database_lifecycle_execution_descriptor_migration_conformance",
        "source": "execution_descriptor_migration_conformance.cpp",
        "labels": [
            "EDR-035",
            "EDR-GATE-035",
            "execution_descriptor_migration",
        ],
    },
    {
        "edr": "EDR-036",
        "test_dir": "sbsql_parser_worker",
        "target": "sbsql_domain_language_expansion_conformance",
        "source": "sbsql_domain_language_expansion_conformance.cpp",
        "labels": [
            "EDR-036",
            "EDR-GATE-036",
            "EDR-DOMAIN-LANGUAGE-EXPANSION",
        ],
    },
    {
        "edr": "EDR-037",
        "target": "database_lifecycle_execution_reference_type_capability_conformance",
        "source": "execution_reference_type_capability_conformance.cpp",
        "labels": [
            "EDR-037",
            "EDR-GATE-037",
            "execution_reference_type_capability",
        ],
    },
]

DEFAULT_TEST_DIR = "database_lifecycle"
SELF_TEST = "database_lifecycle_execution_data_gate_matrix_conformance"
SELF_LABELS = ["EDR-014", "EDR-GATE-014", "execution_data_gate_matrix"]


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"failed to read {path}: {exc}") from exc


def cmake_name_pattern(name: str) -> str:
    escaped = re.escape(name)
    return rf'(?:{escaped}|"{escaped}"|\[=\[{escaped}\]=\])'


def find_call_body(text: str, command: str, name: str) -> str | None:
    pattern = re.compile(
        rf"{re.escape(command)}\(\s*{cmake_name_pattern(name)}(?=\s|\))(?P<body>.*?)\)",
        re.DOTALL,
    )
    match = pattern.search(text)
    if match:
        return match.group("body")
    return None


def find_add_test_body(text: str, name: str) -> str | None:
    pattern = re.compile(
        rf"add_test\(\s*(?:NAME\s+)?{cmake_name_pattern(name)}(?=\s|\))(?P<body>.*?)\)",
        re.DOTALL,
    )
    match = pattern.search(text)
    if match:
        return match.group("body")
    return None


def find_labels(text: str, name: str) -> set[str] | None:
    pattern = re.compile(
        rf"set_tests_properties\(\s*{cmake_name_pattern(name)}(?=\s)\s+PROPERTIES"
        rf"(?P<body>.*?)\)",
        re.DOTALL,
    )
    match = pattern.search(text)
    if not match:
        return None
    body = match.group("body")
    labels = re.search(r'LABELS\s+"(?P<labels>[^"]*)"', body)
    if not labels:
        return None
    return {label for label in labels.group("labels").split(";") if label}


def gate_test_dir(gate: dict[str, object]) -> str:
    return str(gate.get("test_dir", DEFAULT_TEST_DIR))


def validate_source_matrix(source_root: Path) -> list[str]:
    failures: list[str] = []
    seen_edr_labels: set[str] = set()
    cmake_text_by_dir: dict[str, str] = {}

    for gate in EXPECTED_GATES:
        target = gate["target"]
        source = gate["source"]
        test_dir_name = gate_test_dir(gate)
        source_dir = source_root / "tests" / test_dir_name
        cmake_text = cmake_text_by_dir.get(test_dir_name)
        if cmake_text is None:
            cmake_text = read_text(source_dir / "CMakeLists.txt")
            cmake_text_by_dir[test_dir_name] = cmake_text
        executable = find_call_body(cmake_text, "add_executable", target)
        if executable is None:
            failures.append(f"{gate['edr']}: missing add_executable for {target}")
        elif source not in executable:
            failures.append(f"{gate['edr']}: {target} does not include {source}")

        if not (source_dir / source).is_file():
            failures.append(f"{gate['edr']}: source file missing: {source}")

        if find_add_test_body(cmake_text, target) is None:
            failures.append(f"{gate['edr']}: missing add_test for {target}")

        labels = find_labels(cmake_text, target)
        if labels is None:
            failures.append(f"{gate['edr']}: missing LABELS for {target}")
        else:
            for label in gate["labels"]:
                if label not in labels:
                    failures.append(f"{gate['edr']}: {target} missing label {label}")
            seen_edr_labels.update(label for label in labels if label.startswith("EDR-GATE-"))

    database_lifecycle_cmake = cmake_text_by_dir.get(DEFAULT_TEST_DIR)
    if database_lifecycle_cmake is None:
        database_lifecycle_cmake = read_text(
            source_root / "tests" / DEFAULT_TEST_DIR / "CMakeLists.txt"
        )
    if find_add_test_body(database_lifecycle_cmake, SELF_TEST) is None:
        failures.append(f"EDR-014: missing add_test for {SELF_TEST}")
    self_labels = find_labels(database_lifecycle_cmake, SELF_TEST)
    if self_labels is None:
        failures.append(f"EDR-014: missing LABELS for {SELF_TEST}")
    else:
        for label in SELF_LABELS:
            if label not in self_labels:
                failures.append(f"EDR-014: {SELF_TEST} missing label {label}")
        seen_edr_labels.update(label for label in self_labels if label.startswith("EDR-GATE-"))

    expected_gate_labels = {
        label
        for gate in EXPECTED_GATES
        for label in gate["labels"]
        if label.startswith("EDR-GATE-")
    }
    expected_gate_labels.add("EDR-GATE-014")
    missing_gate_labels = expected_gate_labels - seen_edr_labels
    if missing_gate_labels:
        failures.append(
            "source matrix missing EDR gate labels: "
            + ", ".join(sorted(missing_gate_labels))
        )

    return failures


def validate_configured_ctest(build_root: Path) -> list[str]:
    failures: list[str] = []
    ctest_text_by_dir: dict[str, str | None] = {}
    for gate in EXPECTED_GATES:
        target = gate["target"]
        test_dir_name = gate_test_dir(gate)
        ctest_text = ctest_text_by_dir.get(test_dir_name)
        if test_dir_name not in ctest_text_by_dir:
            ctest_path = build_root / "tests" / test_dir_name / "CTestTestfile.cmake"
            ctest_text = read_text(ctest_path) if ctest_path.is_file() else None
            ctest_text_by_dir[test_dir_name] = ctest_text
        if ctest_text is None:
            continue
        if find_add_test_body(ctest_text, target) is None:
            failures.append(f"{gate['edr']}: generated CTest missing {target}")
            continue
        labels = find_labels(ctest_text, target)
        if labels is None:
            failures.append(f"{gate['edr']}: generated CTest missing labels for {target}")
            continue
        for label in gate["labels"]:
            if label not in labels:
                failures.append(f"{gate['edr']}: generated CTest missing label {label}")

    database_lifecycle_ctest = ctest_text_by_dir.get(DEFAULT_TEST_DIR)
    if database_lifecycle_ctest is None:
        ctest_path = build_root / "tests" / DEFAULT_TEST_DIR / "CTestTestfile.cmake"
        database_lifecycle_ctest = (
            read_text(ctest_path) if ctest_path.is_file() else None
        )
    if database_lifecycle_ctest is not None:
        if find_add_test_body(database_lifecycle_ctest, SELF_TEST) is None:
            failures.append(f"EDR-014: generated CTest missing {SELF_TEST}")
        self_labels = find_labels(database_lifecycle_ctest, SELF_TEST)
        if self_labels is None:
            failures.append(f"EDR-014: generated CTest missing labels for {SELF_TEST}")
        else:
            for label in SELF_LABELS:
                if label not in self_labels:
                    failures.append(f"EDR-014: generated CTest missing label {label}")
    return failures


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--build-root", required=True)
    args = parser.parse_args(argv)

    source_root = Path(args.source_root).resolve()
    build_root = Path(args.build_root).resolve()
    failures = validate_source_matrix(source_root)
    failures.extend(validate_configured_ctest(build_root))

    if failures:
        for failure in failures:
            print(f"execution_data_gate_matrix_conformance: {failure}", file=sys.stderr)
        return 1

    print("execution_data_gate_matrix_conformance=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
