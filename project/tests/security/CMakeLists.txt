# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME dbeaver_management_model_security_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dbeaver_management_model_security_test.py"
)
set_tests_properties(dbeaver_management_model_security_test PROPERTIES
  LABELS "security;dbeaver;dbeaver_management;server_admission;secret_redaction;sblr_uuid_authority"
)
