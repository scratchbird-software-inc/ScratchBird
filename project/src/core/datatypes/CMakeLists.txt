# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_core_datatypes
  datatype_advanced_family.cpp
  datatype_binary.cpp
  datatype_descriptor.cpp
  datatype_document.cpp
  datatype_exchange.cpp
  datatype_layout.cpp
  datatype_operations.cpp
  datatype_runtime_closure.cpp
  datatype_temporal_wire.cpp
  datatype_wire_metadata.cpp
)

if(NOT TARGET sb_core_hash)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../hash ${CMAKE_CURRENT_BINARY_DIR}/core_hash)
endif()

if(NOT TARGET sbl_numeric)
  add_subdirectory(${CMAKE_SOURCE_DIR}/libraries/sbl_numeric ${CMAKE_BINARY_DIR}/libraries/sbl_numeric)
endif()

target_compile_features(sb_core_datatypes PUBLIC cxx_std_17)
target_include_directories(sb_core_datatypes PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(sb_core_datatypes PUBLIC sb_core_platform sb_core_hash sbl_numeric)
