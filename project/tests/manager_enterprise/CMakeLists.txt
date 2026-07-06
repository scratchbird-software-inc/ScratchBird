# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_sbmn_enterprise_artifacts
  "${CMAKE_CURRENT_BINARY_DIR}/manager_enterprise_artifacts"
)

add_test(
  NAME sbmn_manager_enterprise_release_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/manager_enterprise_release_gate.py"
          "--repo-root" "${PROJECT_SOURCE_DIR}"
          "--build-root" "${PROJECT_BINARY_DIR}"
          "--output-root" "${_sbmn_enterprise_artifacts}"
          "--manager-exe" "$<TARGET_FILE:sbmn_manager>"
          "--protocol-unit" "$<TARGET_FILE:sbmn_manager_protocol_unit_tests>"
          "--runtime-integration" "$<TARGET_FILE:sbmn_manager_runtime_integration_tests>"
          "--fuzz-gate" "$<TARGET_FILE:sbmn_manager_protocol_fuzz_gate>"
          "--no-spin-gate" "$<TARGET_FILE:sbmn_manager_no_spin_gate>"
          "--no-spin-source" "${PROJECT_SOURCE_DIR}/src/manager"
)
set_tests_properties(sbmn_manager_enterprise_release_gate PROPERTIES
  LABELS "sbmn_manager;manager_enterprise;public_release_correctness;SBMN-GATE-004;SBMN-GATE-071;SBMN-GATE-080;SBMN-GATE-090;SBMN-GATE-900"
)

if(SB_BUILD_ENGINE_LISTENER_ENTERPRISE_GATES)
  set_tests_properties(sbmn_manager_enterprise_release_gate PROPERTIES
    DEPENDS "engine_listener_integrated_product_proof_gate"
  )
endif()
