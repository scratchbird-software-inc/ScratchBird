# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Actual native I/O component closure. Keep the aggregate storage-linked target
# and all its tests unchanged: this target is not a full engine build gate.
# Every implementation below is the production source, including the real disk
# observer, metric registry/queue and MGA visibility/transaction code. No stubs,
# alternate codec implementations or prebuilt storage archives are admitted.
add_executable(sbsql_sblr_alignment_native_storage_components
  filespace_page_zero_test.cpp
  ../../src/server/database_ownership.cpp
  ../../src/storage/database/database_dirty_manifest.cpp
  ../../src/storage/database/physical_mga_cow_store.cpp
  ../../src/storage/database/native_system_state.cpp
  ../../src/storage/database/native_filespace_initialization.cpp
  ../../src/storage/page/catalog_page.cpp
  ../../src/storage/page/row_data_page.cpp
  ../../src/storage/page/native_row_data_page.cpp
  ../../src/storage/page/native_index_btree_page.cpp
  ../../src/storage/page/native_allocation_map.cpp
  ../../src/storage/page/native_filespace_directory.cpp
  ../../src/storage/page/native_horizon_root.cpp
  ../../src/storage/page/native_retention_root.cpp
  ../../src/storage/page/transaction_inventory_page.cpp
  ../../src/storage/disk/disk_device.cpp
  ../../src/storage/disk/filespace_page_zero.cpp
  ../../src/storage/disk/filespace_bootstrap.cpp
  ../../src/storage/disk/native_common_page_header.cpp
  ../../src/core/catalog/catalog_records.cpp
  ../../src/core/catalog/catalog_security_record_codec.cpp
  ../../src/core/catalog/catalog_storage_record_codec.cpp
  ../../src/core/catalog/catalog_value_codec.cpp
  ../../src/core/catalog/catalog_schema_definition.cpp
  ../../src/core/catalog/catalog_schema_record_codec.cpp
  ../../src/core/catalog/catalog_metric_binding.cpp
  ../../src/core/catalog/catalog_name_envelope.cpp
  ../../src/core/catalog/catalog_name_record_codec.cpp
  ../../src/core/datatypes/datatype_binary.cpp
  ../../src/core/datatypes/datatype_descriptor.cpp
  ../../src/core/datatypes/datatype_layout.cpp
  ../../src/core/metrics/metric_registry.cpp
  ../../src/core/metrics/metric_producer.cpp
  ../../src/core/metrics/metric_scalar.cpp
  ../../src/core/metrics/metric_value_update.cpp
  ../../src/core/metrics/metric_history_record.cpp
  ../../src/core/metrics/metric_retention_policy.cpp
  ../../src/core/metrics/metric_value_codec.cpp
  ../../src/core/metrics/metric_sample_codec.cpp
  ../../src/core/metrics/metric_observation_queue.cpp
  ../../src/core/metrics/metric_contracts.cpp
  ../../src/transaction/mga/transaction_inventory.cpp
  ../../src/transaction/mga/transaction_horizon.cpp
  ../../src/transaction/mga/transaction_state.cpp
  ../../src/transaction/mga/transaction_snapshot.cpp
  ../../src/transaction/mga/row_version.cpp
  ../../src/transaction/mga/copy_on_write.cpp
  ../../src/transaction/mga/current_row_map.cpp
  ../../src/transaction/mga/isolation.cpp)
target_compile_features(sbsql_sblr_alignment_native_storage_components PRIVATE cxx_std_23)
target_compile_definitions(sbsql_sblr_alignment_native_storage_components PRIVATE SB_NATIVE_ROUTE_SOURCE_TESTS=1)
target_include_directories(sbsql_sblr_alignment_native_storage_components PRIVATE
  ../../src ../../include ../../src/engine/internal_api
  ../../src/storage/database ../../src/storage/page ../../src/storage/disk
  ../../src/storage/filespace ../../src/transaction/mga ../../src/core/catalog
  ../../src/core/metrics ../../src/core/agents ../../src/core/datatypes
  ../../src/core/index ../../src/core/memory ../../src/core/resources)
target_link_libraries(sbsql_sblr_alignment_native_storage_components PRIVATE
  sb_core_platform sb_core_uuid sb_core_time sb_core_hash sbl_numeric
  OpenSSL::Crypto Threads::Threads)
target_link_libraries(sbsql_sblr_alignment_native_storage_components PRIVATE sb_core_memory sb_core_datatypes)
# The source files also contain legacy entry points outside this component.
# Link-time reachability must not replace any reachable production function.
target_compile_options(sbsql_sblr_alignment_native_storage_components PRIVATE
  -ffunction-sections -fdata-sections)
target_link_options(sbsql_sblr_alignment_native_storage_components PRIVATE
  -Wl,--wrap=_ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv
  -Wl,--gc-sections -Wl,--wrap=RAND_bytes -Wl,--wrap=EVP_Digest
  -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
  -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex
  -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync)

# W4d (2026-11-08): the staging, checkpoint, selection, directory-control and
# policy-root components were deleted with the checkpoint layer.
foreach(native_component IN ITEMS
    filespace-initialization)
  string(REPLACE "-" "_" native_component_name "${native_component}")
  add_test(NAME sbsql_sblr_alignment_native_component_${native_component_name}
    COMMAND sbsql_sblr_alignment_native_storage_components --${native_component}-only)
  set_tests_properties(sbsql_sblr_alignment_native_component_${native_component_name}
    PROPERTIES TIMEOUT 300
    LABELS "sbsql_sblr_alignment;native_actual_source_components;real_file_io;binary_uuid;not_SQL_E2E;not_full_build")
endforeach()
