// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
enum class MemoryConfigurationFailureMode : u64 { return_error=1, fatal_status=2 };
enum class MemoryConfigurationGrowth : u64 { adaptive_with_policy_limit=1 };
enum class MemoryConfigurationWriteAuthority : u64 { mga_inventory_and_page_writeback=1 };

// Actual submitted configuration, not an effective platform envelope, grant,
// allocator installation, activation receipt or policy evaluation result.
// Text borrows immutable caller-owned backing; identities/scalars are values.
struct CatalogMemoryConfiguration {
  Uuid configuration_uuid,policy_pack_uuid,profile_uuid;
  u64 generation=0,policy_pack_generation=0,imported_schema_version=0;
  std::string_view policy_key,policy_name,provenance;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  u64 hard_limit_bytes=0,soft_limit_bytes=0,per_context_limit_bytes=0;
  u64 page_buffer_pool_limit_bytes=0,openssl_budget_bytes=0,min_startup_available_bytes=0;
  MemoryConfigurationFailureMode failure_mode=static_cast<MemoryConfigurationFailureMode>(0);
  bool track_allocations=false,zero_memory_on_allocate=false,zero_memory_on_release=false;
  bool reject_over_soft_limit=false,enable_platform_memory_probe=false,require_platform_memory_ceiling=false;
  bool adaptive_cache_enabled=false;
  MemoryConfigurationGrowth page_buffer_growth=static_cast<MemoryConfigurationGrowth>(0);
  bool index_read_optimization=false,ordinary_disconnect_heap_trim=false,dirty_writeback_required=false;
  MemoryConfigurationWriteAuthority write_flush_authority=static_cast<MemoryConfigurationWriteAuthority>(0);
  bool cache_finality_authority=false,cache_visibility_authority=false,wal_or_redo_authority=false;
};
struct CatalogMemoryConfigurationResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogMemoryConfiguration> record;
  bool ok() const noexcept {return error==CatalogValueError::none&&record.has_value();}
};
CatalogValueEncodeIntoResult EncodeCatalogMemoryConfigurationInto(const CatalogMemoryConfiguration&,std::span<byte>);
CatalogMemoryConfigurationResult DecodeCatalogMemoryConfiguration(std::string_view);
CatalogValueError ValidateCatalogMemoryConfigurationSettings(const CatalogMemoryConfiguration&);
// Bounded diagnostic data only, never a usable configuration. Owns decoded
// labels so refusal neither writes caller backing nor leaves borrowed views.
struct MemoryConfigurationImportDiagnostic {
  std::array<char,768> labels{};
  std::array<std::size_t,3> lengths{};
  std::string_view policy_name() const {return {labels.data()+lengths[0],lengths[1]};}
  std::string_view provenance() const {return {labels.data()+lengths[0]+lengths[1],lengths[2]};}
};
struct MemoryConfigurationImportResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  // Settings only: owner/pack/profile/transaction identities remain unset.
  // Text borrows the caller's character backing, never a mutable import file.
  std::optional<CatalogMemoryConfiguration> settings;
  std::optional<MemoryConfigurationImportDiagnostic> diagnostic;
  bool ok() const noexcept{return error==CatalogValueError::none&&settings.has_value();}
};
// Exact versioned import schema, including every supplied adaptive-cache field.
// No heap allocation. Complete validation precedes writes; refusal preserves
// backing and yields no usable settings. This does not install a memory policy.
MemoryConfigurationImportResult ImportMemoryConfigurationInto(std::string_view,std::span<char>);
struct BootstrapPolicyProfileImport {
  Uuid profile_uuid;
  std::string_view area,mode;
  std::optional<std::string_view> resource_file;
};
struct BootstrapPolicyProfileImportResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<BootstrapPolicyProfileImport> profile;
  bool ok() const noexcept{return error==CatalogValueError::none&&profile.has_value();}
};
// Creation/import boundary only. Optional resource absence differs from null,
// wrong type and an empty path. Source paths never become runtime authority.
BootstrapPolicyProfileImportResult ImportBootstrapPolicyProfileInto(std::string_view,std::span<char>);
bool IsCatalogMemoryConfigurationPayload(std::string_view);
bool CatalogMemoryConfigurationMatchesHeader(const CatalogTypedRecordView&);
bool CatalogMemoryConfigurationMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogMemoryConfigurationPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog
