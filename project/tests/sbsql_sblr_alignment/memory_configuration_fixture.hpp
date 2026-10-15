// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "bootstrap_policy_fixture.hpp"
#include "catalog_memory_configuration.hpp"

namespace scratchbird::tests::memory_configuration {
namespace c=core::catalog;namespace p=core::platform;namespace f=bootstrap_policy;
inline c::CatalogMemoryConfiguration Values(p::TypedUuid tx) {
  c::CatalogMemoryConfiguration r;
  r.configuration_uuid=f::Id(12000);r.policy_pack_uuid=f::Id(12001);r.profile_uuid=f::Id(12002);
  r.generation=r.policy_pack_generation=r.imported_schema_version=1;
  r.policy_key="server.memory_cache.default";r.policy_name="default_local_server_memory_cache_v1";
  r.provenance="default_policy_pack:default-local-password:server_memory_cache_policy";
  r.origin_transaction_uuid=tx;r.origin_local_transaction_id=1;
  r.hard_limit_bytes=1073741824;r.soft_limit_bytes=805306368;r.per_context_limit_bytes=268435456;
  r.page_buffer_pool_limit_bytes=536870912;r.openssl_budget_bytes=4194304;r.min_startup_available_bytes=1073741824;
  r.failure_mode=c::MemoryConfigurationFailureMode::return_error;
  r.track_allocations=r.zero_memory_on_release=r.enable_platform_memory_probe=true;
  r.adaptive_cache_enabled=r.index_read_optimization=r.dirty_writeback_required=true;
  r.page_buffer_growth=c::MemoryConfigurationGrowth::adaptive_with_policy_limit;
  r.write_flush_authority=c::MemoryConfigurationWriteAuthority::mga_inventory_and_page_writeback;
  return r;
}
inline std::string Golden(const c::CatalogMemoryConfiguration& r) {
  f::Golden g(65591);g.Identity(1,r.configuration_uuid);g.Integer(2,r.generation);g.Identity(3,r.policy_pack_uuid);
  g.Integer(4,r.policy_pack_generation);g.Identity(5,r.profile_uuid);g.Integer(6,r.imported_schema_version);
  g.Field(7,3,r.policy_key);g.Field(8,3,r.policy_name);g.Field(9,3,r.provenance);
  g.Identity(10,r.origin_transaction_uuid.value);g.Integer(11,r.origin_local_transaction_id);
  g.Integer(12,r.hard_limit_bytes);g.Integer(13,r.soft_limit_bytes);g.Integer(14,r.per_context_limit_bytes);
  g.Integer(15,r.page_buffer_pool_limit_bytes);g.Integer(16,r.openssl_budget_bytes);g.Integer(17,r.min_startup_available_bytes);
  g.Integer(18,static_cast<p::u64>(r.failure_mode));g.Boolean(19,r.track_allocations);g.Boolean(20,r.zero_memory_on_allocate);
  g.Boolean(21,r.zero_memory_on_release);g.Boolean(22,r.reject_over_soft_limit);g.Boolean(23,r.enable_platform_memory_probe);
  g.Boolean(24,r.require_platform_memory_ceiling);g.Boolean(25,r.adaptive_cache_enabled);
  g.Integer(26,static_cast<p::u64>(r.page_buffer_growth));g.Boolean(27,r.index_read_optimization);
  g.Boolean(28,r.ordinary_disconnect_heap_trim);g.Boolean(29,r.dirty_writeback_required);
  g.Integer(30,static_cast<p::u64>(r.write_flush_authority));g.Boolean(31,r.cache_finality_authority);
  g.Boolean(32,r.cache_visibility_authority);g.Boolean(33,r.wal_or_redo_authority);return g.Finish();
}
inline c::CatalogMetadataVersion Metadata(const c::CatalogMemoryConfiguration& r,const p::Uuid& schema) {
  auto m=f::Metadata(0,r.origin_transaction_uuid,schema,Golden(r));
  m.record.header.kind=c::CatalogRecordKind::config_profile;
  m.record.header.object_uuid={p::UuidKind::object,r.configuration_uuid};
  m.record.header.row_uuid={p::UuidKind::row,f::Id(112000)};
  m.default_name_uuid={p::UuidKind::object,f::Id(12200)};m.name_vector_uuid={p::UuidKind::object,f::Id(12201)};
  m.object_subtype="memory_configuration";m.definition_version=r.generation;return m;
}
} // namespace scratchbird::tests::memory_configuration
