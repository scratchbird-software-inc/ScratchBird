# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/udr/runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime_for_builtin_packages)
endif()

add_library(sb_udr_builtin_packages STATIC
  sb_udr_builtin_packages.cpp
)

target_compile_features(sb_udr_builtin_packages PUBLIC cxx_std_23)
target_include_directories(sb_udr_builtin_packages PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sb_udr_builtin_packages PUBLIC
  sb_udr_runtime
)

if(TARGET sb_engine_internal_api)
  target_compile_definitions(sb_udr_builtin_packages PUBLIC
    SB_UDR_BUILTIN_HAS_ENGINE_OUTBOX=1
  )
  target_link_libraries(sb_udr_builtin_packages PUBLIC
    sb_engine_internal_api
  )
endif()

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sb_udr_builtin_packages
          ARCHIVE DESTINATION lib/scratchbird/udr
          LIBRARY DESTINATION lib/scratchbird/udr
          RUNTIME DESTINATION bin)
  install(FILES sb_udr_builtin_packages.hpp
          DESTINATION include/scratchbird/udr)
endif()
