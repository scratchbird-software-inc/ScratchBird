# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.25)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_REFERENCE_COMPATIBILITY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_compatibility_gate.py"
)
set(SB_REFERENCE_PARSER_START_EVIDENCE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generate_reference_parser_start_evidence.py"
)
set(SB_REFERENCE_TEST_TREE_SELF_CONTAINED_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_test_tree_self_contained_gate.py"
)
set(SB_REFERENCE_PUBLIC_PAYLOAD_POLICY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_public_payload_policy_gate.py"
)

function(sb_add_reference_gate test_name mode labels)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_REFERENCE_COMPATIBILITY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "${mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${labels};reference_compatibility;${test_name}"
  )
endfunction()

sb_add_reference_gate(
  reference_core_framework_conformance
  core
  "reference_core_framework_gate;reference_seed_gate;reference_datatype_gate;reference_index_translation_gate;reference_diagnostic_rendering_gate;reference_metadata_overlay_gate;reference_migration_cdc_gate;reference_sandbox_bridge_gate;reference_release_regression_gate"
)
sb_add_reference_gate(
  reference_seed_manifest_conformance
  seed
  "reference_seed_gate;reference_core_framework_gate"
)
sb_add_reference_gate(
  reference_surface_mapping_conformance
  surface
  "reference_datatype_gate;reference_index_translation_gate;reference_diagnostic_rendering_gate;reference_metadata_overlay_gate"
)
sb_add_reference_gate(
  reference_runtime_bridge_release_conformance
  runtime
  "reference_migration_cdc_gate;reference_sandbox_bridge_gate;reference_release_regression_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_relational_family_conformance
  relational
  "reference_relational_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_analytic_family_conformance
  analytic
  "reference_analytic_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_nosql_family_conformance
  nosql
  "reference_nosql_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_distributed_family_conformance
  distributed
  "reference_distributed_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_capability_reference_conformance
  capref
  "reference_capability_reference_gate"
)
sb_add_reference_gate(
  reference_original_regression_import_conformance
  original-regression
  "reference_original_regression_gate;reference_release_regression_gate"
)

add_test(
  NAME reference_test_tree_self_contained_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_TEST_TREE_SELF_CONTAINED_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(reference_test_tree_self_contained_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_test_asset_gate"
)

add_test(
  NAME reference_public_payload_policy_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_PUBLIC_PAYLOAD_POLICY_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(reference_public_payload_policy_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_test_asset_gate;public_release_payload_gate"
)

sb_add_reference_gate(
  reference_target_evidence_conformance
  target-evidence
  "reference_core_framework_gate;reference_relational_family_gate;reference_analytic_family_gate;reference_nosql_family_gate;reference_distributed_family_gate;reference_capability_reference_gate"
)

add_test(
  NAME reference_parser_start_evidence_manifest_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_PARSER_START_EVIDENCE_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--check"
)
set_tests_properties(reference_parser_start_evidence_manifest_conformance PROPERTIES
  SKIP_RETURN_CODE 77
  LABELS "reference_parser_gate_evidence;reference_original_regression_gate;reference_release_regression_gate"
)

add_test(
  NAME reference_parser_gate_evidence_no_go_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/reference_parser_gate_evidence_closure_gate.py"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(reference_parser_gate_evidence_no_go_conformance PROPERTIES
  SKIP_RETURN_CODE 77
  LABELS "reference_parser_gate_evidence;reference_original_regression_gate;reference_release_regression_gate"
)
