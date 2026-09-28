# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Reverse SBsql rendering is a parser-owned consumer of validated SBLR.
# No engine target links this library or contains its SQL rendering code.
if(NOT TARGET sb_engine_sblr)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/engine/sblr
                   ${CMAKE_CURRENT_BINARY_DIR}/engine_sblr)
endif()
add_library(sbl_sbsql_source_rendering STATIC sblr_to_sbsql.cpp)
target_compile_features(sbl_sbsql_source_rendering PUBLIC cxx_std_23)
target_include_directories(sbl_sbsql_source_rendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sbl_sbsql_source_rendering PUBLIC sb_engine_sblr)
get_target_property(_sb_rendering_engine_sources sb_engine_sblr SOURCES)
foreach(_sb_rendering_engine_source IN LISTS _sb_rendering_engine_sources)
  get_filename_component(_sb_rendering_engine_basename "${_sb_rendering_engine_source}" NAME)
  if(_sb_rendering_engine_basename STREQUAL "sblr_to_sbsql.cpp")
    message(FATAL_ERROR "SBsql reverse rendering cannot be compiled into the SBLR engine")
  endif()
endforeach()
if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sbl_sbsql_source_rendering
    ARCHIVE DESTINATION lib COMPONENT non_engine_runtime EXCLUDE_FROM_ALL)
endif()
