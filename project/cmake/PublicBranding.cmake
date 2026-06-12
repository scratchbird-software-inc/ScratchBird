# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

set(SB_PUBLIC_PRODUCT_FULL_NAME "ScratchBird Convergent Data Engine" CACHE STRING
  "Public product line name")
set(SB_PUBLIC_PRODUCT_SHORT_BRAND "SBcde" CACHE STRING
  "Public product line short brand")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(sb_public_default_target_platform linux)
elseif(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(sb_public_default_target_platform windows)
elseif(CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
  set(sb_public_default_target_platform bsd)
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" sb_public_default_target_platform)
endif()
set(SB_PUBLIC_TARGET_PLATFORM "${sb_public_default_target_platform}" CACHE STRING
  "Public artifact platform directory name")
set_property(CACHE SB_PUBLIC_TARGET_PLATFORM PROPERTY STRINGS linux windows bsd)
set(SB_PUBLIC_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/output" CACHE PATH
  "Build-tree root for public test and release artifacts")
set(SB_PUBLIC_ARTIFACT_ROOT "${SB_PUBLIC_OUTPUT_ROOT}/${SB_PUBLIC_TARGET_PLATFORM}" CACHE PATH
  "Build-tree platform directory for branded public test and release artifacts")
unset(sb_public_default_target_platform)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SB_PUBLIC_ARTIFACT_ROOT}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SB_PUBLIC_ARTIFACT_ROOT}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${SB_PUBLIC_ARTIFACT_ROOT}/lib")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${SB_PUBLIC_ARTIFACT_ROOT}/pdb")

foreach(sb_public_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${sb_public_config}" sb_public_config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${sb_public_config_upper}
    "${SB_PUBLIC_ARTIFACT_ROOT}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${sb_public_config_upper}
    "${SB_PUBLIC_ARTIFACT_ROOT}/lib")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${sb_public_config_upper}
    "${SB_PUBLIC_ARTIFACT_ROOT}/lib")
  set(CMAKE_PDB_OUTPUT_DIRECTORY_${sb_public_config_upper}
    "${SB_PUBLIC_ARTIFACT_ROOT}/pdb")
endforeach()
unset(sb_public_config)
unset(sb_public_config_upper)

file(MAKE_DIRECTORY
  "${SB_PUBLIC_ARTIFACT_ROOT}/bin"
  "${SB_PUBLIC_ARTIFACT_ROOT}/lib"
  "${SB_PUBLIC_ARTIFACT_ROOT}/pdb"
  "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird"
  "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird"
)

function(sb_public_brand_target target_name short_brand full_name scope)
  if(NOT TARGET "${target_name}")
    return()
  endif()

  set_target_properties("${target_name}" PROPERTIES
    OUTPUT_NAME "${short_brand}"
  )

  get_property(sb_public_branding_targets GLOBAL PROPERTY SB_PUBLIC_BRANDING_TARGETS)
  if(NOT sb_public_branding_targets)
    set(sb_public_branding_targets)
  endif()
  if("${target_name}" IN_LIST sb_public_branding_targets)
    return()
  endif()

  list(APPEND sb_public_branding_targets "${target_name}")
  set_property(GLOBAL PROPERTY SB_PUBLIC_BRANDING_TARGETS "${sb_public_branding_targets}")
  set_property(GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_SHORT_BRAND" "${short_brand}")
  set_property(GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_FULL_NAME" "${full_name}")
  set_property(GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_SCOPE" "${scope}")
endfunction()

function(sb_public_write_branding_manifest output_path)
  get_filename_component(sb_public_manifest_dir "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${sb_public_manifest_dir}")
  file(WRITE "${output_path}"
    "target|short_brand|scope|full_name|target_type|actual_output_name|target_platform|artifact_root\n")

  get_property(sb_public_branding_targets GLOBAL PROPERTY SB_PUBLIC_BRANDING_TARGETS)
  foreach(target_name IN LISTS sb_public_branding_targets)
    if(NOT TARGET "${target_name}")
      continue()
    endif()
    get_property(short_brand GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_SHORT_BRAND")
    get_property(full_name GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_FULL_NAME")
    get_property(scope GLOBAL PROPERTY "SB_PUBLIC_BRANDING_${target_name}_SCOPE")
    get_target_property(target_type "${target_name}" TYPE)
    get_target_property(actual_output_name "${target_name}" OUTPUT_NAME)
    if(NOT actual_output_name OR actual_output_name MATCHES "-NOTFOUND$")
      set(actual_output_name "${target_name}")
    endif()
    file(APPEND "${output_path}"
      "${target_name}|${short_brand}|${scope}|${full_name}|${target_type}|${actual_output_name}|${SB_PUBLIC_TARGET_PLATFORM}|${SB_PUBLIC_ARTIFACT_ROOT}\n")
  endforeach()
endfunction()

function(sb_public_configure_output_stage project_root python_executable)
  if(TARGET scratchbird_public_output_stage)
    return()
  endif()

  set(stage_manifest "${SB_PUBLIC_ARTIFACT_ROOT}/STANDALONE_OUTPUT_MANIFEST.json")
  file(WRITE "${stage_manifest}"
    "{\n"
    "  \"product\": \"${SB_PUBLIC_PRODUCT_FULL_NAME}\",\n"
    "  \"short_brand\": \"${SB_PUBLIC_PRODUCT_SHORT_BRAND}\",\n"
    "  \"platform\": \"${SB_PUBLIC_TARGET_PLATFORM}\",\n"
    "  \"artifact_root\": \"${SB_PUBLIC_ARTIFACT_ROOT}\",\n"
    "  \"layout\": {\n"
    "    \"bin\": \"bin\",\n"
    "    \"lib\": \"lib\",\n"
    "    \"configuration\": \"etc/scratchbird\",\n"
    "    \"resources\": \"share/scratchbird/resources\",\n"
    "    \"docs\": \"share/scratchbird/docs\",\n"
    "    \"examples\": \"share/scratchbird/examples\"\n"
    "  }\n"
    "}\n")

  add_custom_target(scratchbird_public_output_stage ALL
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SB_PUBLIC_ARTIFACT_ROOT}/bin"
            "${SB_PUBLIC_ARTIFACT_ROOT}/lib"
            "${SB_PUBLIC_ARTIFACT_ROOT}/pdb"
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird"
            "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${project_root}/resources"
            "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird/resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${project_root}/config/templates/SBsrv.conf"
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird/SBsrv.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${project_root}/config/templates/SBgate.conf"
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird/SBgate.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${project_root}/config/templates/SBmgr.conf"
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird/SBmgr.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${project_root}/config/templates/SBParser.conf"
            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird/SBParser.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${project_root}/docs/public_api"
            "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird/docs/public_api"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${project_root}/docs/release"
            "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird/docs/release"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${project_root}/examples"
            "${SB_PUBLIC_ARTIFACT_ROOT}/share/scratchbird/examples"
    COMMAND "${python_executable}"
            "${project_root}/tools/release/public_output_stage_gate.py"
            "--artifact-root=${SB_PUBLIC_ARTIFACT_ROOT}"
            "--platform=${SB_PUBLIC_TARGET_PLATFORM}"
    COMMENT "Stage ScratchBird public standalone output tree"
    VERBATIM
  )

  get_property(sb_public_output_dependencies GLOBAL PROPERTY SB_PUBLIC_BRANDING_TARGETS)
  foreach(public_output_dependency IN LISTS sb_public_output_dependencies)
    if(TARGET "${public_output_dependency}")
      add_dependencies(scratchbird_public_output_stage "${public_output_dependency}")
    endif()
  endforeach()
  unset(public_output_dependency)
  unset(sb_public_output_dependencies)
endfunction()
