# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# This profile changes section granularity, not optimization level, exported
# APIs, linkage/state ownership, assertions, exception handling or test coverage.
# Other platforms keep their native linker policy; never pass ELF flags to them.
set(_sb_link_pruning_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  set(_sb_link_pruning_default ON)
endif()
option(SB_ENABLE_LINK_PRUNING
  "Remove unreachable library code from optimized Linux builds" ${_sb_link_pruning_default})
unset(_sb_link_pruning_default)

function(sb_enable_link_pruning directory)
  if(NOT SB_ENABLE_LINK_PRUNING)
    return()
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
     NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "SB_ENABLE_LINK_PRUNING requires a qualified Linux GNU/Clang toolchain")
  endif()
  get_property(checked GLOBAL PROPERTY SB_LINK_PRUNING_CHECKED)
  if(NOT checked)
    include(CheckCompilerFlag)
    include(CheckLinkerFlag)
    get_property(languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    foreach(language IN ITEMS C CXX)
      if(language IN_LIST languages)
        check_compiler_flag(${language} "-ffunction-sections" SB_${language}_FUNCTION_SECTIONS)
        check_compiler_flag(${language} "-fdata-sections" SB_${language}_DATA_SECTIONS)
        check_linker_flag(${language} "LINKER:--gc-sections" SB_${language}_GC_SECTIONS)
        if(NOT SB_${language}_FUNCTION_SECTIONS OR NOT SB_${language}_DATA_SECTIONS OR
           NOT SB_${language}_GC_SECTIONS)
          message(FATAL_ERROR "SB_ENABLE_LINK_PRUNING: ${language} toolchain lacks required section support")
        endif()
      endif()
    endforeach()
    set_property(GLOBAL PROPERTY SB_LINK_PRUNING_CHECKED TRUE)
  endif()
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(applied "${target}" SB_LINK_PRUNING_APPLIED)
    if(applied)
      continue()
    endif()
    get_target_property(type "${target}" TYPE)
    # Library members often contain unrelated entry points in one translation
    # unit. Separate those without recompiling every test main just for layout.
    if(type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY)$")
      target_compile_options("${target}" PRIVATE
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-ffunction-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-fdata-sections>")
    endif()
    if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      target_link_options("${target}" PRIVATE
        "$<$<AND:$<LINK_LANGUAGE:C,CXX>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:LINKER:--gc-sections>")
    endif()
    set_property(TARGET "${target}" PROPERTY SB_LINK_PRUNING_APPLIED TRUE)
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    sb_enable_link_pruning("${child}")
  endforeach()
endfunction()
