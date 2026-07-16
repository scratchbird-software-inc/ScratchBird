# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Keep the library used while configuring/building separate from the reference
# passed to dlopen()/LoadLibrary() in installed binaries. Linux resolves a
# versioned SONAME through the system loader, Windows resolves the bundled DLL
# beside the executable, and macOS QA builds use the stable Homebrew opt path.
function(sb_resolve_llvm_runtime_library output_reference output_delivery)
  set(one_value_args
    PLATFORM
    LINK_MODE
    BUILD_LIBRARY
    EXPLICIT_RUNTIME_LIBRARY
  )
  cmake_parse_arguments(SB_LLVM_RUNTIME "" "${one_value_args}" "" ${ARGN})

  if(NOT SB_LLVM_RUNTIME_PLATFORM)
    message(FATAL_ERROR "LLVM runtime resolution requires PLATFORM")
  endif()
  if(NOT SB_LLVM_RUNTIME_LINK_MODE)
    message(FATAL_ERROR "LLVM runtime resolution requires LINK_MODE")
  endif()

  if(SB_LLVM_RUNTIME_LINK_MODE STREQUAL "disabled" OR
     NOT SB_LLVM_RUNTIME_BUILD_LIBRARY)
    set(${output_reference} "" PARENT_SCOPE)
    set(${output_delivery} "not-applicable" PARENT_SCOPE)
    return()
  endif()

  get_filename_component(
    llvm_build_library_realpath
    "${SB_LLVM_RUNTIME_BUILD_LIBRARY}"
    REALPATH
  )
  get_filename_component(
    llvm_build_library_realname
    "${llvm_build_library_realpath}"
    NAME
  )
  if(NOT llvm_build_library_realname)
    get_filename_component(
      llvm_build_library_realname
      "${SB_LLVM_RUNTIME_BUILD_LIBRARY}"
      NAME
    )
  endif()

  if(SB_LLVM_RUNTIME_EXPLICIT_RUNTIME_LIBRARY)
    set(llvm_runtime_reference
      "${SB_LLVM_RUNTIME_EXPLICIT_RUNTIME_LIBRARY}")
  elseif(SB_LLVM_RUNTIME_LINK_MODE STREQUAL "static")
    set(llvm_runtime_reference "${llvm_build_library_realname}")
  elseif(SB_LLVM_RUNTIME_PLATFORM STREQUAL "macos")
    set(llvm_runtime_reference "${SB_LLVM_RUNTIME_BUILD_LIBRARY}")
  else()
    set(llvm_runtime_reference "${llvm_build_library_realname}")
  endif()

  if(llvm_runtime_reference MATCHES "[\"\n\r]")
    message(FATAL_ERROR "LLVM runtime reference contains an unsafe character")
  endif()

  if(SB_LLVM_RUNTIME_LINK_MODE STREQUAL "static")
    set(llvm_runtime_delivery "linked-static")
  elseif(SB_LLVM_RUNTIME_PLATFORM STREQUAL "windows")
    set(llvm_runtime_delivery "bundled")
  elseif(SB_LLVM_RUNTIME_PLATFORM STREQUAL "macos")
    set(llvm_runtime_delivery "external-homebrew")
  else()
    set(llvm_runtime_delivery "system-package")
  endif()

  set(${output_reference} "${llvm_runtime_reference}" PARENT_SCOPE)
  set(${output_delivery} "${llvm_runtime_delivery}" PARENT_SCOPE)
endfunction()
