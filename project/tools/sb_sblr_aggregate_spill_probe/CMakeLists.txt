# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_sblr_aggregate_spill_probe
  main.cpp
)

target_compile_features(sb_sblr_aggregate_spill_probe PRIVATE cxx_std_20)

target_include_directories(sb_sblr_aggregate_spill_probe PRIVATE
  ${CMAKE_SOURCE_DIR}/src/engine
  ${CMAKE_SOURCE_DIR}/src/engine/functions
  ${CMAKE_SOURCE_DIR}/src/engine/sblr
)

target_link_libraries(sb_sblr_aggregate_spill_probe PRIVATE
  sb_engine_functions
  sb_engine_sblr
)
