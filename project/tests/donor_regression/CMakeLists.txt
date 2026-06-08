# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.25)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_DONOR_COMPATIBILITY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/donor_compatibility_gate.py"
)
set(SB_DONOR_PARSER_START_EVIDENCE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generate_donor_parser_start_evidence.py"
)
set(SB_DONOR_TEST_TREE_SELF_CONTAINED_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/donor_test_tree_self_contained_gate.py"
)
set(SB_DONOR_PUBLIC_PAYLOAD_POLICY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/donor_public_payload_policy_gate.py"
)

function(sb_add_donor_gate test_name mode labels)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DONOR_COMPATIBILITY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "${mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${labels};donor_compatibility;${test_name}"
  )
endfunction()

sb_add_donor_gate(
  donor_core_framework_conformance
  core
  "donor_core_framework_gate;donor_seed_gate;donor_datatype_gate;donor_index_translation_gate;donor_diagnostic_rendering_gate;donor_metadata_overlay_gate;donor_migration_cdc_gate;donor_sandbox_bridge_gate;donor_release_regression_gate"
)
sb_add_donor_gate(
  donor_seed_manifest_conformance
  seed
  "donor_seed_gate;donor_core_framework_gate"
)
sb_add_donor_gate(
  donor_surface_mapping_conformance
  surface
  "donor_datatype_gate;donor_index_translation_gate;donor_diagnostic_rendering_gate;donor_metadata_overlay_gate"
)
sb_add_donor_gate(
  donor_runtime_bridge_release_conformance
  runtime
  "donor_migration_cdc_gate;donor_sandbox_bridge_gate;donor_release_regression_gate;donor_original_regression_gate"
)
sb_add_donor_gate(
  donor_relational_family_conformance
  relational
  "donor_relational_family_gate;donor_original_regression_gate"
)
sb_add_donor_gate(
  donor_analytic_family_conformance
  analytic
  "donor_analytic_family_gate;donor_original_regression_gate"
)
sb_add_donor_gate(
  donor_nosql_family_conformance
  nosql
  "donor_nosql_family_gate;donor_original_regression_gate"
)
sb_add_donor_gate(
  donor_distributed_family_conformance
  distributed
  "donor_distributed_family_gate;donor_original_regression_gate"
)
sb_add_donor_gate(
  donor_capability_reference_conformance
  capref
  "donor_capability_reference_gate"
)
sb_add_donor_gate(
  donor_original_regression_import_conformance
  original-regression
  "donor_original_regression_gate;donor_release_regression_gate"
)

add_test(
  NAME donor_test_tree_self_contained_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_DONOR_TEST_TREE_SELF_CONTAINED_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(donor_test_tree_self_contained_conformance PROPERTIES
  LABELS "donor_original_regression_gate;donor_release_regression_gate;donor_test_asset_gate"
)

add_test(
  NAME donor_public_payload_policy_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_DONOR_PUBLIC_PAYLOAD_POLICY_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(donor_public_payload_policy_conformance PROPERTIES
  LABELS "donor_original_regression_gate;donor_release_regression_gate;donor_test_asset_gate;public_release_payload_gate"
)

sb_add_donor_gate(
  donor_target_evidence_conformance
  target-evidence
  "donor_core_framework_gate;donor_relational_family_gate;donor_analytic_family_gate;donor_nosql_family_gate;donor_distributed_family_gate;donor_capability_reference_gate"
)

add_test(
  NAME donor_parser_start_evidence_manifest_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_DONOR_PARSER_START_EVIDENCE_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--check"
)
set_tests_properties(donor_parser_start_evidence_manifest_conformance PROPERTIES
  LABELS "donor_parser_gate_evidence;donor_original_regression_gate;donor_release_regression_gate"
)

add_test(
  NAME donor_parser_gate_evidence_no_go_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/donor_parser_gate_evidence_closure_gate.py"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(donor_parser_gate_evidence_no_go_conformance PROPERTIES
  LABELS "donor_parser_gate_evidence;donor_original_regression_gate;donor_release_regression_gate"
)
