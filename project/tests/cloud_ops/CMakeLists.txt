# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_PUBLIC_P1_CLOUD_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/public_p1_cloud_foundation_gate.py"
)

if(NOT TARGET sb_engine_internal_api)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/internal_api"
    "${CMAKE_CURRENT_BINARY_DIR}/engine_internal_api"
  )
endif()

function(sb_public_p1_cloud_conformance_executable target source label)
  add_executable(${target} "${source}")
  target_link_libraries(${target} PRIVATE sb_engine_internal_api)
  target_compile_features(${target} PRIVATE cxx_std_20)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    LABELS "public_p1_cloud_foundation;${label};release_gate;mga_transaction_regression"
  )
endfunction()

sb_public_p1_cloud_conformance_executable(
  protected_material_catalog_conformance
  protected_material_catalog_conformance.cpp
  protected_material_catalog_gate
)
sb_public_p1_cloud_conformance_executable(
  cloud_provider_capability_registry_conformance
  cloud_provider_capability_registry_test.cpp
  cloud_provider_capability_registry_gate
)
sb_public_p1_cloud_conformance_executable(
  cloud_identity_kms_secretless_conformance
  cloud_identity_kms_policy_test.cpp
  cloud_identity_kms_secretless_gate
)
sb_public_p1_cloud_conformance_executable(
  edge_cache_cdn_invalidation_conformance_runtime
  edge_cache_cdn_invalidation_conformance.cpp
  edge_cache_cdn_invalidation_gate
)

add_test(
  NAME kubernetes_operator_lifecycle_runtime_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/kubernetes_operator_lifecycle_gate.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(kubernetes_operator_lifecycle_runtime_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;kubernetes_operator_lifecycle_gate;release_gate"
)

add_test(
  NAME protected_material_catalog_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode protected-material
)
set_tests_properties(protected_material_catalog_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;protected_material_catalog_gate;release_gate;mga_transaction_regression"
)

add_test(
  NAME cloud_provider_capability_registry_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode cloud-provider
)
set_tests_properties(cloud_provider_capability_registry_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;cloud_provider_capability_registry_gate;release_gate"
)

add_test(
  NAME cloud_identity_kms_secretless_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode identity-kms
)
set_tests_properties(cloud_identity_kms_secretless_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;cloud_identity_kms_secretless_gate;release_gate"
)

add_test(
  NAME kubernetes_operator_lifecycle_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode kubernetes-operator
)
set_tests_properties(kubernetes_operator_lifecycle_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;kubernetes_operator_lifecycle_gate;release_gate"
)

add_test(
  NAME edge_cache_cdn_invalidation_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode edge-cache
)
set_tests_properties(edge_cache_cdn_invalidation_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;edge_cache_cdn_invalidation_gate;release_gate;mga_transaction_regression"
)

add_test(
  NAME public_p1_cloud_management_sblr_abi_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode management-sblr-abi
)
set_tests_properties(public_p1_cloud_management_sblr_abi_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_management_sblr_abi_gate;release_gate"
)

add_test(
  NAME public_p1_cloud_operational_hardening_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode operational-hardening
)
set_tests_properties(public_p1_cloud_operational_hardening_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_operational_hardening_gate;release_gate;mga_transaction_regression"
)

add_test(
  NAME public_p1_cloud_foundation_full_route_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_PUBLIC_P1_CLOUD_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode full-route
)
set_tests_properties(public_p1_cloud_foundation_full_route_gate PROPERTIES
  LABELS "public_p1_cloud_foundation;public_p1_cloud_foundation_full_route_gate;release_gate;mga_transaction_regression"
)
