# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT SB_PROJECT_SOURCE_DIR)
  message(FATAL_ERROR "SB_PROJECT_SOURCE_DIR is required")
endif()

include("${SB_PROJECT_SOURCE_DIR}/cmake/ResolveLlvmRuntimeLibrary.cmake")

set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/llvm-runtime-library-gate")
file(REMOVE_RECURSE "${fixture_root}")
file(MAKE_DIRECTORY "${fixture_root}")
file(WRITE "${fixture_root}/libLLVM.so.23.1" "fixture")
file(WRITE "${fixture_root}/libLLVM-22.dll" "fixture")
file(WRITE "${fixture_root}/libLLVM.dylib" "fixture")

sb_resolve_llvm_runtime_library(linux_reference linux_delivery
  PLATFORM linux
  LINK_MODE dynamic
  BUILD_LIBRARY "${fixture_root}/libLLVM.so.23.1"
)
if(NOT linux_reference STREQUAL "libLLVM.so.23.1" OR
   NOT linux_delivery STREQUAL "system-package")
  message(FATAL_ERROR
    "Linux LLVM runtime must use a loader-resolved SONAME: "
    "reference=${linux_reference};delivery=${linux_delivery}")
endif()

sb_resolve_llvm_runtime_library(windows_reference windows_delivery
  PLATFORM windows
  LINK_MODE dynamic
  BUILD_LIBRARY "${fixture_root}/libLLVM-22.dll"
)
if(NOT windows_reference STREQUAL "libLLVM-22.dll" OR
   NOT windows_delivery STREQUAL "bundled")
  message(FATAL_ERROR
    "Windows LLVM runtime must use a bundled DLL basename: "
    "reference=${windows_reference};delivery=${windows_delivery}")
endif()

set(macos_opt_library "/opt/homebrew/opt/llvm/lib/libLLVM.dylib")
sb_resolve_llvm_runtime_library(macos_reference macos_delivery
  PLATFORM macos
  LINK_MODE dynamic
  BUILD_LIBRARY "${fixture_root}/libLLVM.dylib"
  EXPLICIT_RUNTIME_LIBRARY "${macos_opt_library}"
)
if(NOT macos_reference STREQUAL "${macos_opt_library}" OR
   NOT macos_delivery STREQUAL "external-homebrew")
  message(FATAL_ERROR
    "macOS LLVM runtime must retain the declared stable Homebrew path: "
    "reference=${macos_reference};delivery=${macos_delivery}")
endif()

sb_resolve_llvm_runtime_library(static_reference static_delivery
  PLATFORM linux
  LINK_MODE static
  BUILD_LIBRARY "${fixture_root}/libLLVM.so.23.1"
)
if(NOT static_reference STREQUAL "libLLVM.so.23.1" OR
   NOT static_delivery STREQUAL "linked-static")
  message(FATAL_ERROR "Static LLVM runtime resolution contract failed")
endif()

sb_resolve_llvm_runtime_library(disabled_reference disabled_delivery
  PLATFORM linux
  LINK_MODE disabled
  BUILD_LIBRARY "${fixture_root}/libLLVM.so.23.1"
)
if(disabled_reference OR NOT disabled_delivery STREQUAL "not-applicable")
  message(FATAL_ERROR "Disabled LLVM runtime resolution contract failed")
endif()

file(REMOVE_RECURSE "${fixture_root}")
message(STATUS "LLVM runtime library resolution gate passed")
