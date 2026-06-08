# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_DOCUMENTATION_BASELINE_GENERATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/documentation_baseline_generation_gate.py"
)

add_test(
  NAME documentation_baseline_generation_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DOCUMENTATION_BASELINE_GENERATION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(documentation_baseline_generation_gate PROPERTIES
  LABELS "documentation;documentation_baseline;source_authority;release_proof_substrate"
)

add_custom_target(documentation_baseline_generation_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DOCUMENTATION_BASELINE_GENERATION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Generating and validating the ScratchBird documentation baseline"
)
