# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_real128_special_value_probe main.cpp)
target_link_libraries(sb_real128_special_value_probe PRIVATE sb_core_datatypes)
target_compile_features(sb_real128_special_value_probe PRIVATE cxx_std_20)

add_test(
  NAME sb_real128_special_value_probe
  COMMAND sb_real128_special_value_probe
)
set_tests_properties(sb_real128_special_value_probe PROPERTIES
  LABELS "public_release_correctness;real128_special_value_probe;numeric_128_backend_gate;real128_backend_truth"
)
