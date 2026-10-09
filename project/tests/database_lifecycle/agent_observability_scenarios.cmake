# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
# A process owns exactly one node metric queue. Never reset that ownership to
# accommodate independent component/durable/partial-admission fixtures.
if(NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR "Agent observability executable is missing")
endif()
set(failed FALSE)
foreach(scenario component durable partial)
  execute_process(COMMAND "${TEST_EXECUTABLE}" "${scenario}"
    RESULT_VARIABLE result TIMEOUT 300)
  message(STATUS "agent observability ${scenario}: ${result}")
  if(NOT result STREQUAL "0")
    set(failed TRUE)
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR "Agent observability scenario failures; all scenarios were attempted")
endif()
