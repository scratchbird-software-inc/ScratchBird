# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Call after target enrollment. Directory-wide flags miss tests registered by
# functions invoked from a different directory. Classify by the actual source
# location, not a target-name convention, and leave production-only targets alone.
function(sb_enable_test_assertions directory test_root)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(target_type "${target}" TYPE)
    if(NOT target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
      continue()
    endif()
    get_target_property(sources "${target}" SOURCES)
    get_target_property(source_dir "${target}" SOURCE_DIR)
    foreach(source IN LISTS sources)
      if(source MATCHES "\\$<")
        continue()
      endif()
      cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_dir}" NORMALIZE
                 OUTPUT_VARIABLE absolute_source)
      cmake_path(IS_PREFIX test_root "${absolute_source}" NORMALIZE is_test_source)
      if(is_test_source)
        if(MSVC)
          target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/UNDEBUG>")
        else()
          target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-UNDEBUG>")
        endif()
        break()
      endif()
    endforeach()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    sb_enable_test_assertions("${child}" "${test_root}")
  endforeach()
endfunction()
