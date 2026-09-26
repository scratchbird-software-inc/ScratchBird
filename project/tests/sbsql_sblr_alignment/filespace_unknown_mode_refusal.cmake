# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

if(NOT DEFINED TEST_PROGRAM OR NOT EXISTS "${TEST_PROGRAM}")
  message(FATAL_ERROR "filespace refusal test requires the actual test executable")
endif()
execute_process(
  COMMAND "${TEST_PROGRAM}" --unknown-mode-for-admission-test
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 3
)
if(NOT "${result}" STREQUAL "2")
  message(FATAL_ERROR "unknown mode returned '${result}' instead of exact admission refusal exit 2")
endif()
if(NOT "${output}" STREQUAL "" OR
   NOT "${error}" STREQUAL "FAIL unknown test mode or invalid argument count\n")
  message(FATAL_ERROR "unknown mode did not produce the exact admission refusal: stdout='${output}' stderr='${error}'")
endif()
