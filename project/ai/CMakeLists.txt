# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_AI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SB_AI_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/ai/artifacts")
set(SB_AI_GATE_ROOT "${CMAKE_BINARY_DIR}/ai/gates")

option(SB_AI_REQUIRE_MCP_RUNTIME
  "Fail AI MCP release validation when the Python MCP runtime is unavailable"
  OFF)
option(SB_AI_REQUIRE_LIVE_NATIVE
  "Fail AI MCP release validation when no live ScratchBird native route is configured"
  OFF)

function(sb_ai_add_gate gate_name)
  add_test(NAME "${gate_name}" ${ARGN})
  set_tests_properties("${gate_name}" PROPERTIES
    LABELS "ai;ai_current_repo_integration;ai_ctest_gate")
endfunction()

sb_ai_add_gate(ai_source_inventory_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_workplan_gate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/inventory"
    --mode inventory)

sb_ai_add_gate(ai_old_path_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_workplan_gate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/old_paths"
    --mode old-paths)

sb_ai_add_gate(ai_build_artifact_isolation_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_workplan_gate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/artifact_isolation"
    --mode artifact-isolation)

sb_ai_add_gate(ai_unit_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_component_runner.py"
    --repo-root "${SB_AI_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/unit"
    --mode unit)

sb_ai_add_gate(ai_capability_matrix_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_capability_matrix.py"
    --matrix "${SB_AI_ROOT}/capability/capability-matrix.v0.json")

sb_ai_add_gate(ai_mcp_manifest_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_ai_mcp_release_integration.py"
    --repo-root "${SB_AI_ROOT}"
    --output-json "${SB_AI_GATE_ROOT}/mcp_manifest/ai_mcp_manifest_gate.json")

sb_ai_add_gate(ai_mcp_sblr_uuid_artifact_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_ai_mcp_release_integration.py"
    --repo-root "${SB_AI_ROOT}"
    --output-json "${SB_AI_GATE_ROOT}/mcp_sblr_uuid/ai_mcp_sblr_uuid_artifact_gate.json")

sb_ai_add_gate(ai_mcp_language_resource_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_ai_mcp_release_integration.py"
    --repo-root "${SB_AI_ROOT}"
    --output-json "${SB_AI_GATE_ROOT}/mcp_language/ai_mcp_language_resource_gate.json")

sb_ai_add_gate(ai_mcp_release_integration_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_ai_mcp_release_integration.py"
    --repo-root "${SB_AI_ROOT}"
    --output-json "${SB_AI_GATE_ROOT}/mcp_release/ai_mcp_release_integration_gate.json")

sb_ai_add_gate(ai_mcp_public_cleanliness_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_workplan_gate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/mcp_cleanliness"
    --mode final)

sb_ai_add_gate(ai_http_contract_selftest_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/smoke_http_contract.py"
    --mode selftest)
set_tests_properties(ai_http_contract_selftest_gate PROPERTIES
  WORKING_DIRECTORY "${SB_AI_ROOT}")

sb_ai_add_gate(ai_conformance_generation_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/generate_ai_conformance_artifacts.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}")
set_tests_properties(ai_conformance_generation_gate PROPERTIES
  WORKING_DIRECTORY "${SB_AI_ROOT}")

sb_ai_add_gate(ai_evidence_validation_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_evidence_gates.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md)
set_tests_properties(ai_evidence_validation_gate PROPERTIES
  DEPENDS ai_conformance_generation_gate)

sb_ai_add_gate(ai_release_candidate_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_release_candidate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
    --claim-profile langchain_v0
    --claim-profile llamaindex_v0
    --claim-profile semantic_kernel_v0
    --claim-profile provider_tool_calling_v0
    --claim-profile governance_certification_v0)
set_tests_properties(ai_release_candidate_gate PROPERTIES
  DEPENDS ai_conformance_generation_gate)

sb_ai_add_gate(ai_optional_mcp_runtime_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_component_runner.py"
    --repo-root "${SB_AI_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/optional_mcp"
    --mode optional-mcp)

if(SB_AI_REQUIRE_MCP_RUNTIME)
  sb_ai_add_gate(ai_mcp_runtime_gate
    COMMAND "${Python3_EXECUTABLE}"
      -B
      "${SB_AI_ROOT}/scripts/ai_component_runner.py"
      --repo-root "${SB_AI_ROOT}"
      --output-dir "${SB_AI_GATE_ROOT}/mcp_runtime"
      --mode optional-mcp
      --require-mcp-runtime)
endif()

sb_ai_add_gate(ai_live_native_conformance_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_component_runner.py"
    --repo-root "${SB_AI_ROOT}"
    --output-dir "${SB_AI_ARTIFACT_ROOT}"
    --mode live-native-classification)

if(SB_AI_REQUIRE_LIVE_NATIVE)
  sb_ai_add_gate(ai_mcp_live_native_required_gate
    COMMAND "${Python3_EXECUTABLE}"
      -B
      "${SB_AI_ROOT}/scripts/ai_component_runner.py"
      --repo-root "${SB_AI_ROOT}"
      --output-dir "${SB_AI_ARTIFACT_ROOT}/mcp_live_native_required"
      --mode live-native-classification
      --require-live-native)
endif()

sb_ai_add_gate(ai_final_import_gate
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/scripts/ai_workplan_gate.py"
    --repo-root "${SB_AI_ROOT}"
    --artifact-root "${SB_AI_ARTIFACT_ROOT}"
    --output-dir "${SB_AI_GATE_ROOT}/final"
    --mode final)
set_tests_properties(ai_final_import_gate PROPERTIES
  DEPENDS "ai_source_inventory_gate;ai_old_path_gate;ai_build_artifact_isolation_gate;ai_conformance_generation_gate;ai_evidence_validation_gate;ai_release_candidate_gate;ai_optional_mcp_runtime_gate;ai_live_native_conformance_gate")

sb_ai_add_gate(ai_mcp_final_zero_drift_audit
  COMMAND "${Python3_EXECUTABLE}"
    -B
    "${SB_AI_ROOT}/tools/validate_ai_mcp_release_integration.py"
    --repo-root "${SB_AI_ROOT}"
    --output-json "${SB_AI_GATE_ROOT}/mcp_final/ai_mcp_final_zero_drift_audit.json")
set_tests_properties(ai_mcp_final_zero_drift_audit PROPERTIES
  DEPENDS "ai_mcp_manifest_gate;ai_mcp_sblr_uuid_artifact_gate;ai_mcp_language_resource_gate;ai_mcp_release_integration_gate;ai_mcp_public_cleanliness_gate;ai_final_import_gate")
