# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_auth_provider_dependency_inventory_probe main.cpp)
target_compile_features(sb_auth_provider_dependency_inventory_probe PRIVATE cxx_std_20)
add_test(
  NAME sb_auth_provider_dependency_inventory_probe
  COMMAND sb_auth_provider_dependency_inventory_probe
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/AUTH_PROVIDER_DEPENDENCY_INVENTORY.csv"
)
