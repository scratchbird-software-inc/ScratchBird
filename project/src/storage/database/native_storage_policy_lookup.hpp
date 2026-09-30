// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "physical_mga_cow_store.hpp"
#include "catalog_storage_action_policy.hpp"
#include "uuid.hpp"
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
struct NativeStoragePolicyLookupBinding {
  core::platform::Uuid policy_uuid, filespace_uuid, storage_profile_uuid, page_size_profile_uuid;
  // Optional exact version/generation guard; both absent or both present.
  core::platform::Uuid expected_version_uuid;
  std::optional<u64> expected_generation;
};
enum class NativeStoragePolicyLookupError {
  none, invalid_request, source_failure, missing, ambiguous, retired,
  provisional, inactive, family_mismatch, target_mismatch, version_mismatch, resource_exhausted
};
struct NativeStoragePolicyLookupResult {
  NativeStoragePolicyLookupError error=NativeStoragePolicyLookupError::invalid_request;
  NativePinnedCatalogReadResult source;
  std::optional<core::catalog::CatalogStorageActionPolicy> policy;
  core::platform::Uuid version_uuid;
  bool ok() const noexcept {return error==NativeStoragePolicyLookupError::none&&source.ok()&&policy.has_value();}
};
// Read from the actual pinned catalog, not a supplied definition/template.
// Caller selects the policy UUID via the owning attachment resolver separately.
// This grants no security, current attachment, resource, admission or I/O lease.
inline NativeStoragePolicyLookupResult ReadNativeStorageActionPolicyFromOpenDevices(
    const core::platform::Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,u16 selector,u16 role,
    const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,
    const NativeStoragePolicyLookupBinding& binding,u64 budget) noexcept {
  using E=NativeStoragePolicyLookupError;
  const auto fail=[](E error){NativeStoragePolicyLookupResult r;r.error=error;return r;};
  try {
    for(const auto* id:{&database,&binding.policy_uuid,&binding.filespace_uuid,&binding.storage_profile_uuid,&binding.page_size_profile_uuid})
      if(!core::uuid::IsEngineIdentityUuid(*id))return fail(E::invalid_request);
    if(!budget||binding.expected_generation.has_value()!=!binding.expected_version_uuid.is_nil()||
        (binding.expected_generation&&(!*binding.expected_generation||!core::uuid::IsEngineIdentityUuid(binding.expected_version_uuid))))
      return fail(E::invalid_request);
    auto source=ReadNativePinnedCatalogVersionsFromOpenDevices(database,devices,checkpoint,selector,role,relation,reader,pin,budget);
    if(!source.ok()){auto r=fail(E::source_failure);r.source=std::move(source);return r;}
    const NativeCatalogVersionRow* found=nullptr;
    for(const auto& row:source.rows)if(row.metadata.record.header.object_uuid.value==binding.policy_uuid){
      if(found)return fail(E::ambiguous);found=&row;
    }
    if(!found)return fail(E::missing);
    const auto& m=found->metadata;
    if(m.record.header.deleted||found->effective_lifecycle==core::catalog::CatalogObjectLifecycle::dropped||
        found->effective_status==core::catalog::CatalogObjectStatus::retired)return fail(E::retired);
    if(found->provisional)return fail(E::provisional);
    if(found->effective_lifecycle!=core::catalog::CatalogObjectLifecycle::active||
        found->effective_status!=core::catalog::CatalogObjectStatus::active)return fail(E::inactive);
    if(!core::catalog::CatalogStorageActionPolicyMatchesMetadata(m))return fail(E::family_mismatch);
    const auto decoded=core::catalog::DecodeCatalogStorageActionPolicy(m.record.payload);
    if(!decoded.ok())return fail(E::family_mismatch);
    const auto& p=*decoded.record;
    if(p.database_uuid!=database||p.filespace_uuid!=binding.filespace_uuid||
        p.storage_profile_uuid!=binding.storage_profile_uuid||p.page_size_profile_uuid!=binding.page_size_profile_uuid)
      return fail(E::target_mismatch);
    if(binding.expected_generation&&(*binding.expected_generation!=p.generation||binding.expected_version_uuid!=found->version_uuid))
      return fail(E::version_mismatch);
    NativeStoragePolicyLookupResult result;result.version_uuid=found->version_uuid;
    result.policy=p;result.source=std::move(source);result.error=E::none;return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
  catch(const std::length_error&){return fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database
