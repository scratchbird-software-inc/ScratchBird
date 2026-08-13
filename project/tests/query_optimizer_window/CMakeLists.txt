# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# QOW-CMAKE-TST-001-HARNESS-V1
# QOW_NATIVE_HARNESS_CMAKE
if(NOT SB_BUILD_QOW_CLOSURE_TESTS)
  message(FATAL_ERROR
    "tests/query_optimizer_window is reachable only through SB_BUILD_QOW_CLOSURE_TESTS")
endif()
if(SB_BUILD_TESTS OR SB_BUILD_SBSQL_PARSER_WORKER_TESTS)
  message(FATAL_ERROR
    "the isolated QOW harness cannot use either broad test switch")
endif()

add_executable(qow_native_harness qow_native_harness.cpp)
target_compile_features(qow_native_harness PRIVATE cxx_std_23)

add_executable(
  qow_tst_001_harness_test_v1
  qow_tst_001_harness.cpp
)
target_compile_features(qow_tst_001_harness_test_v1 PRIVATE cxx_std_23)
target_compile_definitions(qow_tst_001_harness_test_v1 PRIVATE
  QOW_BUILD_QOW_CLOSURE_TESTS=$<BOOL:${SB_BUILD_QOW_CLOSURE_TESTS}>
  QOW_BUILD_TESTS=$<BOOL:${SB_BUILD_TESTS}>
  QOW_BUILD_SBSQL_PARSER_WORKER_TESTS=$<BOOL:${SB_BUILD_SBSQL_PARSER_WORKER_TESTS}>
)

add_test(
  NAME qow_tst_001_harness_v1
  COMMAND qow_tst_001_harness_test_v1
)
set_tests_properties(qow_tst_001_harness_v1 PROPERTIES
  LABELS "qow_closure;QOW-022;QOW-LEAF-TST-001-HARNESS;RCP-081A;case_free_proof_record;development_candidate"
)

set(SB_QOW_NATIVE_HARNESS_ENROLLED TRUE PARENT_SCOPE)
