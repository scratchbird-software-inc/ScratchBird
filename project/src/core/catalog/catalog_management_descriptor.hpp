// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <array>

namespace scratchbird::core::catalog {
// Registered implementation/profile codes are closed discriminators. Actual
// operation and profile identity is the separately allocated catalog UUID.
enum class CatalogManagementProfileCode : u64 {
  verification_family=1,verify_start_procedure,database_target,target_dryrun_parameters,
  operation_state_result,maintenance_authorization,verification_policy,
  verification_evidence,verification_metrics,verify_start_conformance
};
enum class CatalogManagementOperationCode : u64 {verify_start=1};
struct CatalogManagementProfile {
  Uuid profile_uuid,database_uuid;
  u64 generation=0;
  CatalogManagementProfileCode code=static_cast<CatalogManagementProfileCode>(0);
  TypedUuid origin_transaction_uuid,creating_transaction_uuid;
  u64 origin_local_transaction_id=0,creating_local_transaction_id=0;
};
struct CatalogManagementOperationDescriptor {
  Uuid descriptor_uuid,database_uuid;
  u64 generation=0;
  CatalogManagementOperationCode code=static_cast<CatalogManagementOperationCode>(0);
  // Exact cataloged profiles in CatalogManagementProfileCode order. The
  // owning resolver must load every row, its actual version and native origin.
  std::array<Uuid,10> profiles;
  // durable_state_required, dry_run, pause, resume, cancel. No other bits.
  u64 flags=0;
  TypedUuid origin_transaction_uuid,creating_transaction_uuid;
  u64 origin_local_transaction_id=0,creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogManagementProfileSchemaId=65825;
inline constexpr u32 kCatalogManagementDescriptorSchemaId=65826;
inline constexpr std::size_t kCatalogManagementProfileMaxBytes=184;
inline constexpr std::size_t kCatalogManagementDescriptorMaxBytes=440;
template<class T>struct CatalogManagementDecodeResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<T> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
using CatalogManagementProfileResult=CatalogManagementDecodeResult<CatalogManagementProfile>;
using CatalogManagementDescriptorResult=CatalogManagementDecodeResult<CatalogManagementOperationDescriptor>;

// Data-only native declarations. No registered execution, effective permission,
// policy activation, durable operation or successful verification is implied.
std::optional<std::size_t> CatalogManagementProfileEncodedBytes(const CatalogManagementProfile&);
std::optional<std::size_t> CatalogManagementDescriptorEncodedBytes(const CatalogManagementOperationDescriptor&);
CatalogValueEncodeIntoResult EncodeCatalogManagementProfileInto(const CatalogManagementProfile&,std::span<byte>);
CatalogValueEncodeIntoResult EncodeCatalogManagementDescriptorInto(const CatalogManagementOperationDescriptor&,std::span<byte>);
CatalogManagementProfileResult DecodeCatalogManagementProfile(std::string_view);
CatalogManagementDescriptorResult DecodeCatalogManagementDescriptor(std::string_view);
bool IsCatalogManagementProfilePayload(std::string_view);
bool IsCatalogManagementDescriptorPayload(std::string_view);
bool CatalogManagementProfileMatchesHeader(const CatalogTypedRecordView&);
bool CatalogManagementDescriptorMatchesHeader(const CatalogTypedRecordView&);
bool CatalogManagementProfileMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogManagementDescriptorMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogManagementDefinitionMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&);
bool CatalogManagementDefinitionPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog
