# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# PARSER-STANDALONE-PACKAGE-CLOSURE-GATE
#
# Register an explicit component for a compatibility parser after all parser,
# parser-support UDR, and neutral runtime targets have been declared.  The
# component is intentionally excluded from an unqualified install; isolation
# tests select exactly one component into a fresh empty prefix.
function(sb_register_standalone_compatibility_parser_package family)
  set(worker_target "sbp_${family}")
  set(pipeline_target "sbl_${family}_parser_pipeline")
  set(udr_target "sbu_${family}_parser_support")
  if(NOT TARGET "${worker_target}" OR NOT TARGET "${pipeline_target}")
    return()
  endif()

  set(component "parser_${family}_standalone")
  set(same_family_targets "${worker_target}" "${pipeline_target}")
  if(family STREQUAL "firebird" AND TARGET sbl_firebird_transaction_policy)
    list(APPEND same_family_targets sbl_firebird_transaction_policy)
  endif()
  if(TARGET "${udr_target}")
    list(APPEND same_family_targets "${udr_target}")
  endif()

  set(neutral_candidates
    sbl_listener_control_plane
    sbl_manager_protocol
    sb_udr_runtime
    sb_core_memory
    sb_core_metrics
    sb_core_platform
  )
  if(family STREQUAL "firebird")
    list(APPEND neutral_candidates
      sbl_parser_server_ipc_client
      sbl_parser_server_ipc_schema
    )
  else()
    # Legacy compatibility families have not yet completed their standalone
    # parser split.  Keep their existing build/package closure intact, but do
    # not expose that shared lexer/parser/worker archive to strict Firebird.
    list(APPEND neutral_candidates sbl_compatibility_parser_common)
  endif()
  set(neutral_targets)
  foreach(target IN LISTS neutral_candidates)
    if(TARGET "${target}")
      list(APPEND neutral_targets "${target}")
    endif()
  endforeach()

  install(TARGETS ${same_family_targets} ${neutral_targets}
    RUNTIME DESTINATION bin COMPONENT "${component}" EXCLUDE_FROM_ALL
    ARCHIVE DESTINATION lib COMPONENT "${component}" EXCLUDE_FROM_ALL
    LIBRARY DESTINATION lib COMPONENT "${component}" EXCLUDE_FROM_ALL
  )

  list(JOIN same_family_targets "\",\"" same_family_json)
  list(JOIN neutral_targets "\",\"" neutral_json)
  set(package_descriptor
    "${CMAKE_BINARY_DIR}/parser_packages/${family}/standalone-package.json")
  string(CONCAT package_descriptor_content
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"parser_family\": \"${family}\",\n"
    "  \"install_component\": \"${component}\",\n"
    "  \"same_family_targets\": [\"${same_family_json}\"],\n"
    "  \"neutral_targets\": [\"${neutral_json}\"]\n"
    "}\n"
  )
  file(GENERATE OUTPUT "${package_descriptor}"
    CONTENT "${package_descriptor_content}")
  install(FILES "${package_descriptor}"
    DESTINATION "share/scratchbird/parsers/${family}"
    RENAME standalone-package.json
    COMPONENT "${component}"
    EXCLUDE_FROM_ALL
  )
endfunction()
