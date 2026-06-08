# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_core_platform
  runtime_platform.cpp
  runtime_capabilities.cpp
  runtime_topology.cpp
)

include(CheckCXXSourceCompiles)

set(SB_SAVED_CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
set(CMAKE_REQUIRED_LIBRARIES quadmath)
check_cxx_source_compiles("
  #include <quadmath.h>
  int main() {
    __float128 value = static_cast<__float128>(1.0);
    char buffer[64];
    return quadmath_snprintf(buffer, sizeof(buffer), \"%Qg\", value) < 0;
  }
" SB_HAS_REAL128_QUADMATH)
set(CMAKE_REQUIRED_LIBRARIES "${SB_SAVED_CMAKE_REQUIRED_LIBRARIES}")

# SEARCH_KEY: OEIC_LLVM_DYNAMIC_LOADER
set(SB_LLVM_PROJECT_ROOT "$ENV{HOME}/local workspace/llvm" CACHE PATH
  "Preferred local LLVM source root for ScratchBird LLVM integration")
set(SB_LLVM_FALLBACK_PROJECT_ROOT "$ENV{HOME}/local workspace/llvm-project" CACHE PATH
  "Compatibility fallback local LLVM source root used when SB_LLVM_PROJECT_ROOT is not present")
set(SB_LLVM_TOOLS_ROOT "${CMAKE_SOURCE_DIR}/tools/llvm" CACHE PATH
  "Private staged LLVM tool dependency root for ScratchBird")
set(SB_LLVM_MIN_MAJOR 23 CACHE STRING
  "Minimum LLVM major version accepted by release-complete ScratchBird builds")
set(SB_LLVM_LIBRARY "" CACHE FILEPATH
  "ScratchBird LLVM library; leave empty to auto-detect from tools/local/system paths")
# SEARCH_KEY: OEIC_LLVM_STATIC_LINK_OPTION
set(SB_LLVM_LINK_MODE "dynamic" CACHE STRING
  "ScratchBird LLVM integration link mode: dynamic, static, or disabled")
set_property(CACHE SB_LLVM_LINK_MODE PROPERTY STRINGS dynamic static disabled)
option(SB_LLVM_BUILD_FROM_SOURCE_IF_MISSING
  "Create a helper target to stage LLVM from the configured source root when no library is present"
  OFF)
set(SB_LLVM_STAGING_BUILD_DIR "${CMAKE_BINARY_DIR}/llvm-scratchbird-stage" CACHE PATH
  "ScratchBird-controlled LLVM staging build directory")

set(SB_LLVM_LINK_MODE_VALID_VALUES dynamic static disabled)
if(NOT SB_LLVM_LINK_MODE IN_LIST SB_LLVM_LINK_MODE_VALID_VALUES)
  message(FATAL_ERROR "SB_LLVM_LINK_MODE must be one of: dynamic, static, disabled")
endif()

set(SB_LLVM_LOCAL_SOURCE_AVAILABLE OFF)
set(SB_LLVM_EFFECTIVE_PROJECT_ROOT "${SB_LLVM_PROJECT_ROOT}")
if(NOT EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/CMakeLists.txt" AND
   NOT EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/CMakeLists.txt" AND
   EXISTS "${SB_LLVM_FALLBACK_PROJECT_ROOT}/llvm/CMakeLists.txt")
  set(SB_LLVM_EFFECTIVE_PROJECT_ROOT "${SB_LLVM_FALLBACK_PROJECT_ROOT}")
endif()
if(EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/CMakeLists.txt" OR
   EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/CMakeLists.txt")
  set(SB_LLVM_LOCAL_SOURCE_AVAILABLE ON)
endif()

set(SB_LLVM_TOOLS_LIBRARY_AVAILABLE OFF)
if(EXISTS "${SB_LLVM_TOOLS_ROOT}/lib")
  set(SB_LLVM_TOOLS_LIBRARY_AVAILABLE ON)
endif()

if(SB_LLVM_LINK_MODE STREQUAL "disabled")
  set(SB_LLVM_LIBRARY "" CACHE FILEPATH "ScratchBird LLVM library" FORCE)
elseif(NOT SB_LLVM_LIBRARY)
  if(EXISTS "${SB_LLVM_TOOLS_ROOT}/lib/libLLVM-23.so")
    set(SB_LLVM_LIBRARY "${SB_LLVM_TOOLS_ROOT}/lib/libLLVM-23.so" CACHE FILEPATH
      "ScratchBird LLVM library" FORCE)
  else()
    find_library(SB_LLVM_LIBRARY
      NAMES LLVM LLVM-23 LLVM-21 LLVM-20 LLVM-19 LLVM-18 LLVM-15
      HINTS
        "${SB_LLVM_TOOLS_ROOT}/lib"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib64"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib64"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib"
        "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib64"
    )
    if(NOT SB_LLVM_LIBRARY)
      foreach(SB_LLVM_SONAME_CANDIDATE IN ITEMS
          "${SB_LLVM_TOOLS_ROOT}/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_TOOLS_ROOT}/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build-scratchbird-tools/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/build/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/build/lib64/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "/usr/lib/x86_64-linux-gnu/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "/usr/lib/x86_64-linux-gnu/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "/usr/lib/llvm-${SB_LLVM_MIN_MAJOR}/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "/usr/lib/llvm-${SB_LLVM_MIN_MAJOR}/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}"
          "/usr/local/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}.0"
          "/usr/local/lib/libLLVM.so.${SB_LLVM_MIN_MAJOR}")
        if(EXISTS "${SB_LLVM_SONAME_CANDIDATE}")
          set(SB_LLVM_LIBRARY "${SB_LLVM_SONAME_CANDIDATE}" CACHE FILEPATH
            "ScratchBird LLVM library" FORCE)
          break()
        endif()
      endforeach()
    endif()
  endif()
endif()

set(SB_LLVM_VERSION_KNOWN OFF)
set(SB_LLVM_DETECTED_MAJOR 0)
set(SB_LLVM_VERSION_OK OFF)
if(SB_LLVM_LIBRARY)
  get_filename_component(SB_LLVM_LIBRARY_NAME "${SB_LLVM_LIBRARY}" NAME)
  get_filename_component(SB_LLVM_LIBRARY_REALPATH "${SB_LLVM_LIBRARY}" REALPATH)
  get_filename_component(SB_LLVM_LIBRARY_REALNAME "${SB_LLVM_LIBRARY_REALPATH}" NAME)
  foreach(SB_LLVM_VERSION_CANDIDATE IN ITEMS
      "${SB_LLVM_LIBRARY_NAME}"
      "${SB_LLVM_LIBRARY_REALNAME}")
    if(NOT SB_LLVM_VERSION_KNOWN)
      string(REGEX MATCH "LLVM-([0-9]+)" SB_LLVM_VERSION_MATCH
             "${SB_LLVM_VERSION_CANDIDATE}")
      if(CMAKE_MATCH_1)
        set(SB_LLVM_VERSION_KNOWN ON)
        set(SB_LLVM_DETECTED_MAJOR "${CMAKE_MATCH_1}")
      endif()
    endif()
    if(NOT SB_LLVM_VERSION_KNOWN)
      string(REGEX MATCH "LLVM[.]so[.]([0-9]+)" SB_LLVM_VERSION_MATCH
             "${SB_LLVM_VERSION_CANDIDATE}")
      if(CMAKE_MATCH_1)
        set(SB_LLVM_VERSION_KNOWN ON)
        set(SB_LLVM_DETECTED_MAJOR "${CMAKE_MATCH_1}")
      endif()
    endif()
  endforeach()
  if(SB_LLVM_VERSION_KNOWN AND NOT SB_LLVM_DETECTED_MAJOR LESS SB_LLVM_MIN_MAJOR)
    set(SB_LLVM_VERSION_OK ON)
  endif()
endif()

target_compile_features(sb_core_platform PUBLIC cxx_std_17)
target_include_directories(sb_core_platform PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
)

if(SB_HAS_REAL128_QUADMATH)
  target_compile_definitions(sb_core_platform PUBLIC
    SCRATCHBIRD_HAS_REAL128_LIBRARY=1
    SCRATCHBIRD_REAL128_PROVIDER="libquadmath::__float128"
  )
  target_link_libraries(sb_core_platform PUBLIC quadmath)
endif()

if(SB_LLVM_LIBRARY)
  if(SB_LLVM_TOOLS_LIBRARY_AVAILABLE)
    set(SB_LLVM_PROVIDER "link-mode=${SB_LLVM_LINK_MODE}|tools-root=${SB_LLVM_TOOLS_ROOT}|local-source=${SB_LLVM_EFFECTIVE_PROJECT_ROOT}|library=${SB_LLVM_LIBRARY}")
  elseif(SB_LLVM_LOCAL_SOURCE_AVAILABLE)
    set(SB_LLVM_PROVIDER "link-mode=${SB_LLVM_LINK_MODE}|local-source=${SB_LLVM_EFFECTIVE_PROJECT_ROOT}|library=${SB_LLVM_LIBRARY}")
  else()
    set(SB_LLVM_PROVIDER "link-mode=${SB_LLVM_LINK_MODE}|library=${SB_LLVM_LIBRARY}")
  endif()
else()
  set(SB_LLVM_PROVIDER "link-mode=${SB_LLVM_LINK_MODE}|missing|min-major=${SB_LLVM_MIN_MAJOR}|local-source=${SB_LLVM_EFFECTIVE_PROJECT_ROOT}")
endif()

target_compile_definitions(sb_core_platform PUBLIC
  SCRATCHBIRD_LLVM_LIBRARY_FOUND=$<BOOL:${SB_LLVM_LIBRARY}>
  SCRATCHBIRD_LLVM_VERSION_OK=$<BOOL:${SB_LLVM_VERSION_OK}>
  SCRATCHBIRD_LLVM_DETECTED_MAJOR=${SB_LLVM_DETECTED_MAJOR}
  SCRATCHBIRD_LLVM_MIN_MAJOR=${SB_LLVM_MIN_MAJOR}
  SCRATCHBIRD_LLVM_LINK_MODE="${SB_LLVM_LINK_MODE}"
  SCRATCHBIRD_LLVM_LIBRARY_PATH="${SB_LLVM_LIBRARY}"
  SCRATCHBIRD_LLVM_SOURCE_ROOT="${SB_LLVM_EFFECTIVE_PROJECT_ROOT}"
  SCRATCHBIRD_LLVM_TOOLS_ROOT="${SB_LLVM_TOOLS_ROOT}"
  SCRATCHBIRD_LLVM_STAGING_BUILD_DIR="${SB_LLVM_STAGING_BUILD_DIR}"
  SCRATCHBIRD_LLVM_PROVIDER="${SB_LLVM_PROVIDER}"
)

if(SB_LLVM_LIBRARY AND SB_LLVM_VERSION_OK)
  target_compile_definitions(sb_core_platform PUBLIC
    SCRATCHBIRD_HAS_LLVM=1
  )
  if(SB_LLVM_LINK_MODE STREQUAL "static")
    target_link_libraries(sb_core_platform PUBLIC ${SB_LLVM_LIBRARY})
  endif()
else()
  target_compile_definitions(sb_core_platform PUBLIC
    SCRATCHBIRD_HAS_LLVM=0
  )
endif()

if(SB_LLVM_BUILD_FROM_SOURCE_IF_MISSING AND NOT SB_LLVM_LIBRARY)
  if(EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm/CMakeLists.txt")
    set(SB_LLVM_SOURCE_CMAKE_DIR "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/llvm")
  elseif(EXISTS "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}/CMakeLists.txt")
    set(SB_LLVM_SOURCE_CMAKE_DIR "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}")
  else()
    set(SB_LLVM_SOURCE_CMAKE_DIR "")
  endif()
  if(NOT SB_LLVM_SOURCE_CMAKE_DIR)
    message(FATAL_ERROR
      "SB_LLVM_BUILD_FROM_SOURCE_IF_MISSING=ON but no LLVM CMake source root was found at "
      "${SB_LLVM_EFFECTIVE_PROJECT_ROOT}")
  endif()
  add_custom_target(sb_stage_llvm_from_source
    COMMAND "${SB_LLVM_TOOLS_ROOT}/stage_llvm_from_source.sh"
            "${SB_LLVM_SOURCE_CMAKE_DIR}"
            "${SB_LLVM_STAGING_BUILD_DIR}"
            "${SB_LLVM_TOOLS_ROOT}"
            "${SB_LLVM_MIN_MAJOR}"
    COMMENT "OEIC_LLVM_DYNAMIC_LOADER: staging LLVM from source for ScratchBird dynamic loading"
    VERBATIM)
endif()

if(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE)
  if(NOT SB_HAS_REAL128_QUADMATH)
    message(FATAL_ERROR
      "release-complete non-cluster engine requires mandatory real128 support; "
      "configure with SB_NONCLUSTER_ENGINE_PROFILE=bootstrap or emergency only for degraded non-release builds")
  endif()
  if(NOT SB_LLVM_LIBRARY)
    message(FATAL_ERROR
      "release-complete non-cluster engine requires mandatory LLVM support; "
      "stage LLVM under project/tools/llvm or configure SB_LLVM_PROJECT_ROOT/SB_LLVM_TOOLS_ROOT, "
      "or use SB_NONCLUSTER_ENGINE_PROFILE=bootstrap or emergency for degraded non-release builds")
  endif()
  if(SB_LLVM_LINK_MODE STREQUAL "disabled")
    message(FATAL_ERROR
      "release-complete non-cluster engine cannot disable LLVM integration; use dynamic or static")
  endif()
  if(NOT SB_LLVM_VERSION_KNOWN)
    message(FATAL_ERROR
      "release-complete non-cluster engine requires a versioned LLVM library name; "
      "expected libLLVM-<major> or libLLVM.so.<major> with major >= "
      "${SB_LLVM_MIN_MAJOR}, got ${SB_LLVM_LIBRARY}")
  endif()
  if(NOT SB_LLVM_VERSION_OK)
    message(FATAL_ERROR
      "release-complete non-cluster engine requires LLVM major >= ${SB_LLVM_MIN_MAJOR}; "
      "detected ${SB_LLVM_DETECTED_MAJOR} at ${SB_LLVM_LIBRARY}")
  endif()
endif()
