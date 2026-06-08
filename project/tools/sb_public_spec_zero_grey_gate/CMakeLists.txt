# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT
  "${CMAKE_CURRENT_SOURCE_DIR}/public_spec_zero_grey_gate.py"
)
set(SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY
  "${SB_PRIVATE_REPO_ROOT}/public_audit_summary"
)
set(SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY
  "${SB_PRIVATE_REPO_ROOT}/public_audit_summary"
)
set(SB_PUBLIC_SPEC_GAP_ID_AUTHORITY
  "${SB_PRIVATE_REPO_ROOT}/public_audit_summary"
)
set(SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/public_release_foundation"
)
set(SB_PUBLIC_SINGLE_NODE_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/public_single_node"
)
set(SB_DRIVER_SERVER_RECONCILIATION_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver_server_reconciliation"
)
set(SB_PUBLIC_P1_CLOUD_FOUNDATION_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/public_p1_cloud_foundation"
)
set(SB_DRIVER_CHECKLIST_REGISTRY
  "${SB_PRIVATE_REPO_ROOT}/public_contract_snapshot"
)

add_test(
  NAME public_spec_gap_registry_sync_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" audit
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
)
set_tests_properties(public_spec_gap_registry_sync_gate PROPERTIES
  LABELS "public_spec_gap_registry;public_zero_grey;release_gate"
)

add_test(
  NAME public_spec_gap_id_authority_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" gap-id-authority
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --authority-csv "${SB_PUBLIC_SPEC_GAP_ID_AUTHORITY}"
)
set_tests_properties(public_spec_gap_id_authority_gate PROPERTIES
  LABELS "public_spec_gap_registry;public_gap_id_authority;public_zero_grey;release_gate"
)

add_test(
  NAME public_spec_closed_execution_plan_regression_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" closure-regression
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --closed-fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
          --closed-fixture-root "${SB_PUBLIC_SINGLE_NODE_FIXTURE}"
          --closed-fixture-root "${SB_PUBLIC_P1_CLOUD_FOUNDATION_FIXTURE}"
          --max-public-open 24
)
set_tests_properties(public_spec_closed_execution_plan_regression_gate PROPERTIES
  LABELS "public_spec_gap_registry;public_closed_execution_plan_regression;public_zero_grey;release_gate"
)

add_test(
  NAME public_release_foundation_target_evidence_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-evidence
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_release_foundation_target_evidence_manifest_gate PROPERTIES
  LABELS "public_release_foundation;public_release_foundation_target_gap_registry;public_release_foundation_target_evidence_manifest_gate;release_gate"
)

add_test(
  NAME public_release_foundation_hardening_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" hardening
          --fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_release_foundation_hardening_gate PROPERTIES
  LABELS "public_release_foundation;persistent_format_migration_policy_gate;fault_injection_matrix_gate;agent_write_scope_gate;mga_recovery_proof_model_gate;full_route_acceptance_fixture_gate;release_gate"
)

add_test(
  NAME sql_object_synonym_spec_authority_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" synonym-spec
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(sql_object_synonym_spec_authority_gate PROPERTIES
  LABELS "public_release_foundation;sql_object_synonym_spec_authority_gate;release_gate"
)

add_test(
  NAME sbsql_product_name_drift_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" sbsql-name-drift
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(sbsql_product_name_drift_gate PROPERTIES
  LABELS "public_spec_zero_grey;sbsql_product_name_drift_gate;release_gate"
)

add_test(
  NAME release_gate_record_conformance
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" conformance-manifest
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
)
set_tests_properties(release_gate_record_conformance PROPERTIES
  LABELS "public_release_foundation;release_gate_record_conformance;conformance_manifest_registry_gate;release_gate"
)

add_test(
  NAME public_release_foundation_target_zero_grey_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-gate
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_release_foundation_target_zero_grey_gate PROPERTIES
  LABELS "public_release_foundation;public_release_foundation_zero_grey_target_gate;public_release_foundation_target_zero_grey_gate;public_release_foundation_final_audit;release_gate"
)

add_test(
  NAME public_spec_zero_grey_release_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" gate
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
)
set_tests_properties(public_spec_zero_grey_release_gate PROPERTIES
  LABELS "public_spec_zero_grey;public_zero_grey;release_gate"
)

add_test(
  NAME global_public_zero_grey_non_target_regression_audit
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" non-target-regression
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_RELEASE_FOUNDATION_FIXTURE}"
)
set_tests_properties(global_public_zero_grey_non_target_regression_audit PROPERTIES
  LABELS "public_release_foundation;global_public_zero_grey_non_target_regression_audit;release_gate"
)

add_test(
  NAME public_single_node_target_evidence_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-evidence
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_SINGLE_NODE_FIXTURE}"
)
set_tests_properties(public_single_node_target_evidence_manifest_gate PROPERTIES
  LABELS "public_single_node_closure;public_single_node_p0;public_single_node_target_gap_registry;public_single_node_target_evidence_manifest_gate;release_gate"
)

add_test(
  NAME public_single_node_hardening_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" hardening
          --fixture-root "${SB_PUBLIC_SINGLE_NODE_FIXTURE}"
)
set_tests_properties(public_single_node_hardening_gate PROPERTIES
  LABELS "public_single_node_closure;public_single_node_p0;public_single_node_hardening_gate;public_single_node_agent_orchestration_gate;release_gate"
)

add_test(
  NAME public_p1_cloud_foundation_target_evidence_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-evidence
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_P1_CLOUD_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_p1_cloud_foundation_target_evidence_manifest_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_foundation_p0;public_p1_cloud_foundation_target_evidence_manifest_gate;release_gate"
)

add_test(
  NAME public_p1_cloud_foundation_hardening_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" hardening
          --fixture-root "${SB_PUBLIC_P1_CLOUD_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_p1_cloud_foundation_hardening_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_foundation_p0;public_p1_cloud_foundation_hardening_gate;release_gate"
)

add_test(
  NAME public_p1_cloud_foundation_target_zero_grey_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-gate
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_P1_CLOUD_FOUNDATION_FIXTURE}"
)
set_tests_properties(public_p1_cloud_foundation_target_zero_grey_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_foundation_target_zero_grey_gate;release_gate"
)

add_test(
  NAME driver_server_reconciliation_checklist_structure_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" driver-checklist
          --registry "${SB_DRIVER_CHECKLIST_REGISTRY}"
          --target-rows "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE}/artifacts/TARGET_CHECKLIST_ROWS.csv"
)
set_tests_properties(driver_server_reconciliation_checklist_structure_gate PROPERTIES
  LABELS "driver_server_reconciliation;driver_checklist_registry;release_gate"
)

add_test(
  NAME driver_server_reconciliation_hardening_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" hardening
          --fixture-root "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE}"
)
set_tests_properties(driver_server_reconciliation_hardening_gate PROPERTIES
  LABELS "driver_server_reconciliation;driver_server_reconciliation_hardening_gate;release_gate"
)

add_test(
  NAME driver_server_reconciliation_implementation_ahead_zero_grey_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" implementation-ahead
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --fixture-root "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE}"
)
set_tests_properties(driver_server_reconciliation_implementation_ahead_zero_grey_gate PROPERTIES
  LABELS "driver_server_reconciliation;driver_server_reconciliation_implementation_ahead_zero_grey_gate;DSR-042;release_gate"
)

add_test(
  NAME public_single_node_target_zero_grey_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" target-gate
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_SINGLE_NODE_FIXTURE}"
)
set_tests_properties(public_single_node_target_zero_grey_gate PROPERTIES
  LABELS "public_single_node_closure_final;public_single_node_target_zero_grey_gate;release_gate"
)

add_test(
  NAME public_single_node_non_target_regression_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_SPEC_ZERO_GREY_SCRIPT}" non-target-regression
          --inventory "${SB_PUBLIC_SPEC_ZERO_GREY_INVENTORY}"
          --registry-json "${SB_PUBLIC_SPEC_ZERO_GREY_REGISTRY}"
          --fixture-root "${SB_PUBLIC_SINGLE_NODE_FIXTURE}"
)
set_tests_properties(public_single_node_non_target_regression_gate PROPERTIES
  LABELS "public_single_node_closure_final;public_single_node_non_target_regression_gate;release_gate"
)
