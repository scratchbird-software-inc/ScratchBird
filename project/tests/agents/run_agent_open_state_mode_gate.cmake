# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

execute_process(COMMAND "${OPEN_STATE_API_PART}" RESULT_VARIABLE api_result)
if(NOT api_result EQUAL 0)
  message(FATAL_ERROR "agent_open_state_mode_api_part failed with ${api_result}")
endif()

execute_process(COMMAND "${OPEN_STATE_RUNTIME_PART}" RESULT_VARIABLE runtime_result)
if(NOT runtime_result EQUAL 0)
  message(FATAL_ERROR "agent_open_state_mode_runtime_part failed with ${runtime_result}")
endif()
