# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_engine_sblr)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/engine/sblr ${PROJECT_BINARY_DIR}/src/engine/sblr)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(sb_engine_public_headers_api_docs_freeze_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/public_headers_api_docs_freeze_static.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)

add_test(
  NAME sb_engine_public_headers_api_docs_freeze_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/public_headers_api_docs_freeze_static.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sb_engine_public_headers_api_docs_freeze_gate PROPERTIES
  LABELS "sb_engine_public_headers_api_docs_freeze_gate;CBQ-041;CBQ_GATE_PUBLIC_HEADERS_API_DOCS_FREEZE;public_abi;api_docs;extension_boundary_manifest"
)

add_executable(sb_engine_public_abi_c_fixture
  public_abi_c_fixture.c
)
target_link_libraries(sb_engine_public_abi_c_fixture PRIVATE sb_engine)
set_source_files_properties(public_abi_c_fixture.c PROPERTIES LANGUAGE C)
add_test(NAME sb_engine_public_abi_c_fixture COMMAND sb_engine_public_abi_c_fixture)

add_executable(sb_engine_public_abi_cpp_fixture
  public_abi_cpp_fixture.cpp
)
target_compile_features(sb_engine_public_abi_cpp_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_abi_cpp_fixture PRIVATE sb_engine)
add_test(NAME sb_engine_public_abi_cpp_fixture COMMAND sb_engine_public_abi_cpp_fixture)

add_executable(sb_engine_public_sblr_admission_fixture
  public_sblr_admission_fixture.cpp
)
target_compile_features(sb_engine_public_sblr_admission_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_sblr_admission_fixture PRIVATE sb_engine)
add_test(NAME sb_engine_public_sblr_admission_fixture COMMAND sb_engine_public_sblr_admission_fixture)
set_tests_properties(sb_engine_public_sblr_admission_fixture PROPERTIES
  LABELS "sb_engine_backed_streaming_conformance;sbsql_parser_worker")

add_executable(sb_engine_sbsql_behavior_conformance_fixture
  sbsql_behavior_conformance_fixture.cpp
)
target_compile_features(sb_engine_sbsql_behavior_conformance_fixture PRIVATE cxx_std_23)
target_include_directories(sb_engine_sbsql_behavior_conformance_fixture PRIVATE
  ${PROJECT_SOURCE_DIR}/src/engine/sblr
)
target_link_libraries(sb_engine_sbsql_behavior_conformance_fixture PRIVATE sb_engine_sblr sb_engine_internal_api)
add_test(NAME sb_engine_sbsql_behavior_conformance_fixture
  COMMAND sb_engine_sbsql_behavior_conformance_fixture
    ${PROJECT_SOURCE_DIR}/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml)
set_tests_properties(sb_engine_sbsql_behavior_conformance_fixture PROPERTIES
  LABELS "sb_engine_sbsql_behavior_conformance;sbsql_parser_worker")

add_executable(sb_engine_public_diagnostic_shape_fixture
  public_diagnostic_shape_fixture.cpp
)
target_compile_features(sb_engine_public_diagnostic_shape_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_diagnostic_shape_fixture PRIVATE sb_engine)
add_test(NAME sb_engine_public_diagnostic_shape_fixture COMMAND sb_engine_public_diagnostic_shape_fixture)

add_executable(sb_engine_public_metrics_thread_fixture
  public_metrics_thread_fixture.cpp
)
find_package(Threads REQUIRED)
target_compile_features(sb_engine_public_metrics_thread_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_metrics_thread_fixture PRIVATE sb_engine Threads::Threads)
add_test(NAME sb_engine_public_metrics_thread_fixture COMMAND sb_engine_public_metrics_thread_fixture)

add_executable(sb_engine_public_source_boundary_fixture
  public_source_boundary_fixture.cpp
)
target_compile_features(sb_engine_public_source_boundary_fixture PRIVATE cxx_std_23)
add_test(NAME sb_engine_public_source_boundary_fixture
  COMMAND sb_engine_public_source_boundary_fixture ${PROJECT_SOURCE_DIR}/src/engine ${PROJECT_SOURCE_DIR}/src/server)

add_executable(sb_engine_public_wire_stability_fixture
  public_wire_stability_fixture.cpp
)
target_compile_features(sb_engine_public_wire_stability_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_wire_stability_fixture PRIVATE sb_engine)
add_test(NAME sb_engine_public_wire_stability_fixture COMMAND sb_engine_public_wire_stability_fixture)

add_executable(sb_engine_public_documentation_example_fixture
  public_documentation_example_fixture.cpp
)
target_compile_features(sb_engine_public_documentation_example_fixture PRIVATE cxx_std_23)
target_link_libraries(sb_engine_public_documentation_example_fixture PRIVATE sb_engine)
add_test(NAME sb_engine_public_documentation_example_fixture COMMAND sb_engine_public_documentation_example_fixture)

if(TARGET sb_engine_shared)
  add_executable(sb_engine_public_abi_symbol_gate
    public_abi_symbol_gate.cpp
  )
  target_compile_features(sb_engine_public_abi_symbol_gate PRIVATE cxx_std_23)
  add_dependencies(sb_engine_public_abi_symbol_gate sb_engine_shared)
  add_test(NAME sb_engine_public_abi_symbol_gate
    COMMAND sb_engine_public_abi_symbol_gate $<TARGET_FILE:sb_engine_shared>)
endif()
