# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(
  optimizer_deficiency_odf_117_gate
  optimizer_deficiency_odf_117_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_117_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_117_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
    sb_core_index
)
add_test(
  NAME optimizer_deficiency_odf_117_gate
  COMMAND optimizer_deficiency_odf_117_gate
)
set_tests_properties(optimizer_deficiency_odf_117_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_117_gate;ODF-117;optimizer_deficiency;concurrency;performance;benchmark_clean;mga_transaction_regression"
)

if(NOT TARGET sb_core_memory)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${CMAKE_BINARY_DIR}/core_memory_for_concurrency_tests
  )
endif()

add_executable(
  memory_sanitizer_soak_concurrency_gate
  memory_sanitizer_soak_concurrency_gate.cpp
)
target_compile_features(memory_sanitizer_soak_concurrency_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_sanitizer_soak_concurrency_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_sanitizer_soak_concurrency_gate
  COMMAND memory_sanitizer_soak_concurrency_gate
)
set_tests_properties(memory_sanitizer_soak_concurrency_gate PROPERTIES
  LABELS "memory_sanitizer_soak_concurrency_gate;MMCH-100;MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;memory;soak;concurrency;sanitizer;mga_transaction_regression;linux_proof"
)
