# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Threads REQUIRED)

if(NOT TARGET sbl_listener_control_plane)
  add_library(sbl_listener_control_plane
    listener_diagnostics.cpp
    control_plane.cpp
  )

  target_include_directories(sbl_listener_control_plane PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(sbl_listener_control_plane PUBLIC cxx_std_23)
  target_link_libraries(sbl_listener_control_plane PUBLIC sbl_manager_protocol)
  if(WIN32)
    target_link_libraries(sbl_listener_control_plane PUBLIC ws2_32)
  endif()
endif()

if(SB_BUILD_SB_LISTENER)
  if(NOT TARGET sbl_listener_runtime)
    add_library(sbl_listener_runtime
      listener_config.cpp
      listener_metrics.cpp
      listener_socket_identity.cpp
      listener_support_bundle.cpp
      listener_tls_policy.cpp
      dbbt_lpreface.cpp
      handoff_claim_reader.cpp
      sbps_preauth.cpp
      parser_pool.cpp
      listener_runtime.cpp
    )

    target_include_directories(sbl_listener_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(sbl_listener_runtime PUBLIC cxx_std_23)
    target_link_libraries(sbl_listener_runtime PUBLIC sbl_listener_control_plane sbl_manager_protocol Threads::Threads)
    if(WIN32)
      target_link_libraries(sbl_listener_runtime PUBLIC ws2_32)
    endif()
  endif()

  if(NOT TARGET sb_listener)
    add_executable(sb_listener main.cpp)
    target_link_libraries(sb_listener PRIVATE sbl_listener_runtime)
  endif()

  if(NOT TARGET sb_parser_dummy)
    add_executable(sb_parser_dummy
      ../parser_dummy/main.cpp
    )

    target_compile_features(sb_parser_dummy PRIVATE cxx_std_23)
    target_link_libraries(sb_parser_dummy PRIVATE sbl_listener_control_plane Threads::Threads)
  endif()
endif()
