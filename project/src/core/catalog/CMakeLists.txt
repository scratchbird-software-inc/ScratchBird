# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# Declared catalog dispatcher composition.
#
# catalog_record_codec.cpp dispatches to one implementation per catalog family.
# Some evidence fixtures deliberately compile that dispatcher into the test
# binary rather than linking the whole catalog library, so they need the
# dispatcher plus every family implementation and nothing else.
#
# This OBJECT library is the single declaration of that composition. Production
# consumes it through sb_core_catalog; fixtures consume the same objects. A
# fixture therefore links the exact objects production composes instead of
# restating the list as its own sources, which is what previously drifted. See
# the specification rule `ARCH-BUILD-COMPOSITION-DECLARED-ONCE` and
# `decisions/DR-BUILD-COMPOSITION-001`.
#
# Adding a catalog family means adding its source here. The configure-time check
# in cmake/CatalogDispatcherClosure.cmake fails the build with the exact missing
# filename if a family implementation is added to the engine but not declared
# here, so the omission cannot reach a link error.
add_library(sb_core_catalog_dispatcher_objects OBJECT
  catalog_record_codec.cpp
  catalog_authorization_policy.cpp
  catalog_bootstrap_policy.cpp
  catalog_construction_intent.cpp
  catalog_metadata_change.cpp
  catalog_native_security_record.cpp
  catalog_language.cpp
  catalog_context_namespace.cpp
  catalog_evidence_policy.cpp
  catalog_identifier_profile.cpp
  catalog_isolation_profile.cpp
  catalog_lock_policy.cpp
  catalog_management_descriptor.cpp
  catalog_memory_configuration.cpp
  catalog_metric_descriptor.cpp
  catalog_metric_label_schema.cpp
  catalog_metric_retention_policy.cpp
  catalog_metric_series.cpp
  catalog_metric_visibility_policy.cpp
  catalog_naming_collision_policy.cpp
  catalog_name_tree_keys.cpp
  catalog_name_path.cpp
  catalog_naming_dialect_profile.cpp
  catalog_native_catalog_index.cpp
  catalog_builtin_type_domain.cpp
  catalog_policy_descriptor.cpp
  catalog_table_definition.cpp
  catalog_transaction_policy.cpp
  catalog_verification_policy.cpp
)
target_compile_features(sb_core_catalog_dispatcher_objects PUBLIC cxx_std_17)
target_include_directories(sb_core_catalog_dispatcher_objects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Compile-time requirements only. These are the same dependencies sb_core_catalog
# declares, so the dispatcher composition compiles in exactly the environment it
# did as part of the library. PRIVATE because consumers take the objects, not a
# link dependency: narrow evidence fixtures keep their own minimal link closure
# rather than inheriting the full catalog dependency set.
target_link_libraries(sb_core_catalog_dispatcher_objects PRIVATE
  sb_core_platform sb_core_uuid sb_core_hash sb_core_datatypes sb_storage_page sb_core_metrics)

add_library(sb_core_catalog
  $<TARGET_OBJECTS:sb_core_catalog_dispatcher_objects>
  cluster_catalog_access_profile.cpp
  cluster_catalog_crypto_evidence.cpp
  cluster_catalog_manifest.cpp
  cluster_catalog_record_codec.cpp
  cluster_catalog_schema_versioning.cpp
  cluster_descriptor_manifest.cpp
  cluster_schema_gating.cpp
  catalog_index_profile.cpp
  ipar_catalog_support_services.cpp
  catalog_value_codec.cpp
  catalog_database_record_codec.cpp
  catalog_filespace_record_codec.cpp
  catalog_schema_record_codec.cpp
  catalog_security_record_codec.cpp
  catalog_storage_record_codec.cpp
  catalog_schema_definition.cpp
  catalog_schema_creation.cpp
  catalog_identifier_lookup_key.cpp
  memory_configuration_import.cpp
  catalog_table_datatype_binding.cpp
  catalog_metric_binding.cpp
  catalog_metric_config_materialization.cpp
  catalog_localized_record_codec.cpp
  catalog_resource_record_codec.cpp
  catalog_name_record_codec.cpp
  catalog_name_envelope.cpp
  catalog_named_creation.cpp
  catalog_records.cpp
)

target_compile_features(sb_core_catalog PUBLIC cxx_std_17)
target_include_directories(sb_core_catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_catalog PUBLIC sb_core_platform sb_core_uuid sb_core_hash sb_core_datatypes sb_storage_page)

# Native metric catalog observations use the shared typed value codec.
target_link_libraries(sb_core_catalog PUBLIC sb_core_metrics)

# Fail configure if a catalog family implementation exists but was not declared
# in the dispatcher composition above.
sb_catalog_verify_dispatcher_closure_declared()
