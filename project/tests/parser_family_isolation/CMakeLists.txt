# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set(SB_PARSER_FAMILY_ISOLATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_isolation_gate.py"
)
set(SB_PARSER_FAMILY_OWNERSHIP_MAP
  "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_ownership.json"
)
set(SB_PARSER_FAMILY_ISOLATION_EVIDENCE
  "${CMAKE_CURRENT_BINARY_DIR}/parser_family_isolation_evidence.json"
)
set(SB_PARSER_FAMILY_BINARY_ISOLATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_binary_isolation_gate.py"
)
set(SB_PARSER_FAMILY_BINARY_ISOLATION_EVIDENCE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/binary_isolation_evidence"
)
set(SB_PARSER_FAMILY_PACKAGE_ISOLATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_package_isolation_gate.py"
)
set(SB_PARSER_FAMILY_PACKAGE_ISOLATION_EVIDENCE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/package_isolation_evidence"
)
set(SB_PARSER_FAMILY_RUNTIME_ISOLATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_runtime_isolation_gate.py"
)
set(SB_PARSER_FAMILY_RUNTIME_ISOLATION_EVIDENCE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/runtime_isolation_evidence/firebird"
)

set(SB_FIREBIRD_RUNTIME_ISOLATION_PREFIX "" CACHE PATH
  "Qualified Firebird standalone empty-prefix package for PARSER-ISO-007/008"
)
set(SB_FIREBIRD_RUNTIME_ISOLATION_SERVER "" CACHE FILEPATH
  "ScratchBird server executable for the Firebird PARSER-ISO-007/008 route"
)
set(SB_FIREBIRD_RUNTIME_ISOLATION_LISTENER "" CACHE FILEPATH
  "ScratchBird listener executable for the Firebird PARSER-ISO-007/008 route"
)
set(SB_FIREBIRD_RUNTIME_ISOLATION_ISQL "" CACHE FILEPATH
  "Genuine Firebird isql executable for the PARSER-ISO-007/008 route"
)
set(SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_DIR "" CACHE PATH
  "Optional genuine Firebird client-library directory for isql"
)
string(SHA256 SB_FIREBIRD_RUNTIME_ISOLATION_BUILD_HASH "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${SB_FIREBIRD_RUNTIME_ISOLATION_BUILD_HASH}" 0 12
  SB_FIREBIRD_RUNTIME_ISOLATION_BUILD_HASH_SHORT
)
set(SB_FIREBIRD_RUNTIME_ISOLATION_WORK_DIR
  "/tmp/sb_fbi_${SB_FIREBIRD_RUNTIME_ISOLATION_BUILD_HASH_SHORT}" CACHE PATH
  "Short disposable route work directory for Firebird PARSER-ISO-007/008 socket safety"
)

add_test(
  NAME parser_family_isolation_fixture_tests
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_isolation_gate_test.py"
    --gate-script "${SB_PARSER_FAMILY_ISOLATION_GATE}"
    --fixture-root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
set_tests_properties(parser_family_isolation_fixture_tests PROPERTIES
  LABELS "parser_family_isolation;parser_standalone;PARSER-ISO-014"
)

add_test(
  NAME parser_family_binary_identity_fixture_tests
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_binary_isolation_gate_test.py"
    --gate-script "${SB_PARSER_FAMILY_BINARY_ISOLATION_GATE}"
    --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
)
set_tests_properties(parser_family_binary_identity_fixture_tests PROPERTIES
  LABELS "parser_family_isolation;parser_family_identity"
)

if(SB_BUILD_COMPATIBILITY_PARSERS)
  add_test(
    NAME parser_family_binary_identity_checkpoint
    COMMAND
      "${Python3_EXECUTABLE}"
      -B
      "${SB_PARSER_FAMILY_BINARY_ISOLATION_GATE}"
      --repo-root "${PROJECT_SOURCE_DIR}/.."
      --build-dir "${CMAKE_BINARY_DIR}"
      --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
      --evidence-dir "${SB_PARSER_FAMILY_BINARY_ISOLATION_EVIDENCE_DIR}"
      --require-complete-registry
      --allow-no-artifacts
  )
  set_tests_properties(parser_family_binary_identity_checkpoint PROPERTIES
    DEPENDS parser_family_binary_identity_fixture_tests
    SKIP_RETURN_CODE 77
    LABELS "parser_family_isolation;parser_family_identity"
  )
endif()

add_test(
  NAME parser_family_package_isolation_fixture_tests
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_package_isolation_gate_test.py"
    --gate-script "${SB_PARSER_FAMILY_PACKAGE_ISOLATION_GATE}"
)
set_tests_properties(parser_family_package_isolation_fixture_tests PROPERTIES
  LABELS "parser_family_isolation;parser_family_package"
)

add_test(
  NAME parser_family_runtime_isolation_fixture_tests
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${CMAKE_CURRENT_SOURCE_DIR}/parser_family_runtime_isolation_gate_test.py"
    --gate-script "${SB_PARSER_FAMILY_RUNTIME_ISOLATION_GATE}"
    --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
)
set_tests_properties(parser_family_runtime_isolation_fixture_tests PROPERTIES
  LABELS "parser_family_isolation;parser_family_runtime;PARSER-ISO-014"
)

if(SB_BUILD_COMPATIBILITY_PARSERS
   AND SB_FIREBIRD_RUNTIME_ISOLATION_PREFIX
   AND SB_FIREBIRD_RUNTIME_ISOLATION_SERVER
   AND SB_FIREBIRD_RUNTIME_ISOLATION_LISTENER
   AND SB_FIREBIRD_RUNTIME_ISOLATION_ISQL)
  set(SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_ARGS)
  if(SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_DIR)
    list(APPEND SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_ARGS
      --client-library-dir "${SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_DIR}"
    )
  endif()
  add_test(
    NAME parser_family_firebird_runtime_isolation_gate
    COMMAND
      "${Python3_EXECUTABLE}"
      -B
      "${SB_PARSER_FAMILY_RUNTIME_ISOLATION_GATE}"
      --repo-root "${PROJECT_SOURCE_DIR}/.."
      --family firebird
      --source-prefix "${SB_FIREBIRD_RUNTIME_ISOLATION_PREFIX}"
      --server "${SB_FIREBIRD_RUNTIME_ISOLATION_SERVER}"
      --listener "${SB_FIREBIRD_RUNTIME_ISOLATION_LISTENER}"
      --client-tool "${SB_FIREBIRD_RUNTIME_ISOLATION_ISQL}"
      ${SB_FIREBIRD_RUNTIME_ISOLATION_CLIENT_LIBRARY_ARGS}
      --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
      --evidence-dir "${SB_PARSER_FAMILY_RUNTIME_ISOLATION_EVIDENCE_DIR}"
      --runtime-work-dir "${SB_FIREBIRD_RUNTIME_ISOLATION_WORK_DIR}"
  )
  set_tests_properties(parser_family_firebird_runtime_isolation_gate PROPERTIES
    DEPENDS parser_family_runtime_isolation_fixture_tests
    LABELS "parser_family_isolation;parser_family_runtime;PARSER-ISO-007;PARSER-ISO-008"
    TIMEOUT 180
  )
endif()

if(SB_BUILD_COMPATIBILITY_PARSERS)
  add_test(
    NAME parser_family_package_isolation_gate
    COMMAND
      "${Python3_EXECUTABLE}"
      -B
      "${SB_PARSER_FAMILY_PACKAGE_ISOLATION_GATE}"
      --repo-root "${PROJECT_SOURCE_DIR}/.."
      --build-dir "${CMAKE_BINARY_DIR}"
      --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
      --evidence-dir "${SB_PARSER_FAMILY_PACKAGE_ISOLATION_EVIDENCE_DIR}"
      --require-complete-registry
      --allow-no-artifacts
  )
  set_tests_properties(parser_family_package_isolation_gate PROPERTIES
    DEPENDS parser_family_package_isolation_fixture_tests
    SKIP_RETURN_CODE 77
    LABELS "parser_family_isolation;parser_family_package;PARSER-ISO-006"
  )
endif()

add_test(
  NAME parser_family_isolation_static_gate
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${SB_PARSER_FAMILY_ISOLATION_GATE}"
    --repo-root "${PROJECT_SOURCE_DIR}/.."
    --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
    --evidence-file "${SB_PARSER_FAMILY_ISOLATION_EVIDENCE}"
)
set_tests_properties(parser_family_isolation_static_gate PROPERTIES
  DEPENDS parser_family_isolation_fixture_tests
  LABELS "parser_family_isolation;parser_standalone;PARSER-ISO-001;PARSER-ISO-002;PARSER-ISO-003;PARSER-ISO-014"
)

add_custom_target(parser_family_isolation_static_gate_target
  COMMAND
    "${Python3_EXECUTABLE}"
    -B
    "${SB_PARSER_FAMILY_ISOLATION_GATE}"
    --repo-root "${PROJECT_SOURCE_DIR}/.."
    --ownership-map "${SB_PARSER_FAMILY_OWNERSHIP_MAP}"
    --evidence-file "${SB_PARSER_FAMILY_ISOLATION_EVIDENCE}"
  VERBATIM
)
