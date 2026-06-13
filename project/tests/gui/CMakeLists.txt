# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME dbeaver_management_ui_proof
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dbeaver_management_ui_proof.py"
)
set_tests_properties(dbeaver_management_ui_proof PROPERTIES
  LABELS "gui;dbeaver;dbeaver_management;DBEAVER-MGMT-015;DBEAVER-MGMT-022;DBEAVER-MGMT-032;DBEAVER-MGMT-038;DBEAVER-MGMT-039;DBEAVER-MGMT-040;DBEAVER-MGMT-041;DBEAVER-MGMT-042;DBEAVER-MGMT-045"
)
