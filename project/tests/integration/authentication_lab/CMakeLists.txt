# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set(SB_AUTHENTICATION_LAB_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/lab.sh")

add_test(
  NAME authentication_lab_definition_gate
  COMMAND "${SB_AUTHENTICATION_LAB_SCRIPT}" validate
)
set_tests_properties(authentication_lab_definition_gate PROPERTIES
  LABELS "authentication_lab;external_auth_provider;container_lab;definition_gate"
  SKIP_RETURN_CODE 77
  RESOURCE_LOCK scratchbird_authentication_lab
)

add_test(
  NAME authentication_lab_start
  COMMAND "${SB_AUTHENTICATION_LAB_SCRIPT}" up "${SB_AUTHENTICATION_LAB_PROFILE}"
)
set_tests_properties(authentication_lab_start PROPERTIES
  LABELS "authentication_lab;external_auth_provider;container_lab"
  FIXTURES_SETUP scratchbird_authentication_lab_fixture
  SKIP_RETURN_CODE 77
  RESOURCE_LOCK scratchbird_authentication_lab
  TIMEOUT 900
)

add_test(
  NAME authentication_lab_smoke
  COMMAND "${SB_AUTHENTICATION_LAB_SCRIPT}" smoke "${SB_AUTHENTICATION_LAB_PROFILE}"
)
set_tests_properties(authentication_lab_smoke PROPERTIES
  LABELS "authentication_lab;external_auth_provider;container_lab;integration"
  FIXTURES_REQUIRED scratchbird_authentication_lab_fixture
  SKIP_RETURN_CODE 77
  RESOURCE_LOCK scratchbird_authentication_lab
  TIMEOUT 600
)

add_test(
  NAME authentication_lab_stop
  COMMAND "${SB_AUTHENTICATION_LAB_SCRIPT}" down
)
set_tests_properties(authentication_lab_stop PROPERTIES
  LABELS "authentication_lab;external_auth_provider;container_lab"
  FIXTURES_CLEANUP scratchbird_authentication_lab_fixture
  SKIP_RETURN_CODE 77
  RESOURCE_LOCK scratchbird_authentication_lab
  TIMEOUT 180
)
