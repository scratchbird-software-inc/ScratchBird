# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Preserve dependency locations across the second configure boundary. Do not
# forward cached admission results, compiler flags, or build/test controls.
function(sb_append_nested_dependency_configure_args output_variable)
  set(_sb_dependency_args "${${output_variable}}")
  foreach(_sb_dependency_key
      CMAKE_PREFIX_PATH
      CMAKE_LIBRARY_PATH
      CMAKE_INCLUDE_PATH
      SBL_NUMERIC_MPFR_INCLUDE_DIR
      SBL_NUMERIC_MPFR_LIBRARY
      SBL_NUMERIC_GMP_INCLUDE_DIR
      SBL_NUMERIC_GMP_LIBRARY
      SBL_NUMERIC_BOOST_INCLUDE_DIR)
    if(DEFINED ${_sb_dependency_key} AND NOT "${${_sb_dependency_key}}" STREQUAL "")
      # The result is a CMake list of argv elements, not a shell command.
      string(REPLACE ";" "\\;" _sb_dependency_value "${${_sb_dependency_key}}")
      list(APPEND _sb_dependency_args "-D${_sb_dependency_key}=${_sb_dependency_value}")
    endif()
  endforeach()
  set(${output_variable} "${_sb_dependency_args}" PARENT_SCOPE)
endfunction()
