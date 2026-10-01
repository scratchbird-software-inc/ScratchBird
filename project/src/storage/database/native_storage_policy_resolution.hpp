// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_storage_policy_lookup.hpp"
#include "native_filespace_capacity.hpp"
#include "native_storage_action_intent.hpp"
#include "catalog_storage_record_codec.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <mutex>

namespace scratchbird::storage::database {
enum class NativeStoragePolicyResolutionError {
  none,invalid_request,capacity_failure,catalog_failure,missing_attachment,
  ambiguous_attachment,invalid_attachment,provisional_attachment,inactive_attachment,
  target_mismatch,missing_profile,ambiguous_profile,invalid_profile,
  provisional_profile,inactive_profile,policy_failure,resource_exhausted,io_failure,
  policy_roots_failure
};
struct NativeStoragePolicySelection {
  core::catalog::CatalogStorageActionAttachment attachment;
  core::catalog::CatalogStorageRecord profile;
  Uuid attachment_version_uuid,profile_version_uuid;
  u64 profile_generation=0;
};
struct NativeStoragePolicyResolution {
  NativeStoragePolicyResolutionError error=NativeStoragePolicyResolutionError::invalid_request;
  NativeFilespaceCapacityResult capacity;
  NativeCheckpointPolicyRootsResult policy_roots;
  NativeStoragePolicyLookupResult policy;
  std::optional<NativeStoragePolicySelection> selection;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeStoragePolicyResolutionError::none&&capacity.ok()&&policy_roots.ok()&&policy.ok()&&selection.has_value()&&retained_image_bytes;}
};
// Actual target-derived selection only. Runtime cannot choose a policy UUID.
// Own devices must remain alive; acquire no unordered subset of their guards.
// The result is not an execution lease, authenticated grant or serving decision.
inline NativeStoragePolicyResolution ResolveNativeStoragePolicyFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,const Uuid& filespace,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,u64 budget) noexcept {
  using E=NativeStoragePolicyResolutionError;namespace c=core::catalog;
  const auto fail=[](E error){NativeStoragePolicyResolution r;r.error=error;return r;};
  try {
    if(!budget||!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(filespace)||devices.empty())return fail(E::invalid_request);
    auto ordered=devices;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    for(std::size_t i=0;i<ordered.size();++i){const auto& d=ordered[i];
      if(!d.device||!core::uuid::IsEngineIdentityUuid(d.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(d.page_size_profile_uuid)||
          (i&&ordered[i-1].filespace_uuid==d.filespace_uuid))return fail(E::invalid_request);
      for(std::size_t j=0;j<i;++j)if(ordered[j].device==d.device)return fail(E::invalid_request);
    }
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(ordered.size());
    for(const auto& d:ordered)guards.push_back(d.device->AcquireOperationGuard());
    auto capacity=ReadNativeFilespaceCapacityFromOpenDevices(database,ordered,checkpoint,filespace,budget);
    if(!capacity.ok()){auto r=fail(E::capacity_failure);r.capacity=std::move(capacity);return r;}
    if(capacity.retained_image_bytes>=budget)return fail(E::resource_exhausted);
    // STORAGE-NATIVE-POLICY-ROOT-SOURCE-001. Catalog rows alone cannot stand in
    // for the checkpoint's dedicated security/configuration roots. Keep these
    // actual images and their complete nested proof under the same guards.
    auto roots=VerifyNativeCheckpointPolicyRootsFromOpenDevices(database,ordered,checkpoint,budget-capacity.retained_image_bytes);
    if(!roots.ok()){auto r=fail(E::policy_roots_failure);r.policy_roots=std::move(roots);return r;}
    const u64 retained=capacity.retained_image_bytes+roots.retained_image_bytes;
    if(retained>=budget)return fail(E::resource_exhausted);
    auto source=ReadNativePinnedCatalogVersionsFromOpenDevices(database,ordered,checkpoint,selector,role,relation,reader,pin,budget-retained);
    if(!source.ok()){auto r=fail(E::catalog_failure);r.policy.error=NativeStoragePolicyLookupError::source_failure;r.policy.source=std::move(source);return r;}
    const auto retired=[](const NativeCatalogVersionRow& row){return row.metadata.record.header.deleted||row.effective_lifecycle==c::CatalogObjectLifecycle::dropped||row.effective_status==c::CatalogObjectStatus::retired;};
    const auto active=[](const NativeCatalogVersionRow& row){return row.effective_lifecycle==c::CatalogObjectLifecycle::active&&row.effective_status==c::CatalogObjectStatus::active;};
    std::optional<NativeStoragePolicySelection> selection;
    for(const auto& row:source.rows){const auto& m=row.metadata;
      if(m.object_subtype!="storage_action_attachment"&&!c::IsCatalogStorageActionAttachmentPayload(m.record.payload))continue;
      if(!c::CatalogStorageActionAttachmentMatchesMetadata(m))return fail(E::invalid_attachment);
      const auto decoded=c::DecodeCatalogStorageActionAttachment(m.record.payload);
      const auto& a=*decoded.record;if(a.filespace_uuid!=filespace)continue;
      if(row.provisional)return fail(E::provisional_attachment);
      if(retired(row))continue;
      if(selection)return fail(E::ambiguous_attachment);
      if(!active(row))return fail(E::inactive_attachment);
      if(a.database_uuid!=database||a.page_size_profile_uuid!=capacity.observation->page_size_profile_uuid)return fail(E::target_mismatch);
      selection.emplace();selection->attachment=a;selection->attachment_version_uuid=row.version_uuid;
    }
    if(!selection)return fail(E::missing_attachment);
    const NativeCatalogVersionRow* profile=nullptr;
    for(const auto& row:source.rows)if(row.metadata.record.header.object_uuid.value==selection->attachment.storage_profile_uuid){
      if(profile)return fail(E::ambiguous_profile);profile=&row;}
    if(!profile)return fail(E::missing_profile);
    if(profile->provisional)return fail(E::provisional_profile);
    if(retired(*profile)||!active(*profile))return fail(E::inactive_profile);
    if(profile->metadata.authority_scope!=c::CatalogAuthorityScope::local||
        !c::CatalogStoragePayloadMatchesHeader(profile->metadata.record))return fail(E::invalid_profile);
    const auto descriptor=c::DecodeCatalogStorageRecord(profile->metadata.record.payload);
    if(descriptor.record->filespace_uuid.value!=filespace||descriptor.record->page_size!=capacity.observation->page_size_bytes)return fail(E::target_mismatch);
    selection->profile=*descriptor.record;selection->profile_version_uuid=profile->version_uuid;selection->profile_generation=profile->metadata.definition_version;
    const auto& a=selection->attachment;
    auto policy=native_storage_policy_detail::Select(database,std::move(source),{a.policy_uuid,filespace,a.storage_profile_uuid,a.page_size_profile_uuid,{},std::nullopt});
    if(!policy.ok()){auto r=fail(E::policy_failure);r.policy=std::move(policy);return r;}
    NativeStoragePolicyResolution result;result.error=E::none;result.capacity=std::move(capacity);
    result.retained_image_bytes=retained+policy.source.source.retained_image_bytes;
    result.policy_roots=std::move(roots);
    result.policy=std::move(policy);result.selection=std::move(selection);return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::io_failure);}
}
enum class NativeStorageIntentPolicyError {
  none,invalid_intent,resource_exhausted,resolution_failure,capacity_mismatch,
  selection_mismatch,policy_disabled,action_disallowed,limit_exceeded
};
struct NativeStorageIntentPolicyCheck {
  NativeStorageIntentPolicyError error=NativeStorageIntentPolicyError::invalid_intent;
  NativeStorageIntentError intent_error=NativeStorageIntentError::none;
  NativeStorageCapacityCheckError capacity_error=NativeStorageCapacityCheckError::none;
  NativeStoragePolicyResolution resolution;
  bool ok() const noexcept {return error==NativeStorageIntentPolicyError::none&&resolution.ok();}
};
// Fresh capacity plus exact persisted selection matching under the resolver's
// retained guards. Still NOT authentication, approval, resource admission or an
// execution lease. In particular approval=none is not security authorization.
inline NativeStorageIntentPolicyCheck CheckNativeStorageIntentPolicyFromOpenDevices(
    const NativeStorageActionIntent& intent,const std::vector<disk::NativeFilespaceDevice>& devices,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,u64 budget) noexcept {
  using E=NativeStorageIntentPolicyError;
  const auto fail=[](E e){NativeStorageIntentPolicyCheck r;r.error=e;return r;};
  const auto valid=ValidateNativeStorageActionIntent(intent);
  if(valid!=NativeStorageIntentError::none){auto r=fail(E::invalid_intent);r.intent_error=valid;return r;}
  const auto allowance=std::min(budget,intent.maximum_retained_image_bytes);
  if(allowance<=kNativeStorageActionIntentBytes)return fail(E::resource_exhausted);
  auto resolved=ResolveNativeStoragePolicyFromOpenDevices(intent.database_uuid,devices,intent.checkpoint,
    intent.filespace_uuid,selector,role,relation,reader,pin,allowance-kNativeStorageActionIntentBytes);
  if(!resolved.ok()){auto r=fail(E::resolution_failure);r.resolution=std::move(resolved);return r;}
  const auto matched=MatchNativeStorageIntentCapacityObservation(intent,*resolved.capacity.observation);
  if(matched!=NativeStorageCapacityCheckError::none){auto r=fail(E::capacity_mismatch);r.capacity_error=matched;return r;}
  const auto& s=*resolved.selection;const auto& p=*resolved.policy.policy;
  // Match the catalog guard to the verified source, not just the selected
  // objects. Unchanged policy versions cannot authorize a stale/omitted guard.
  const auto catalog_generation=resolved.policy.source.source.checkpoint.catalogs.front().root->catalog_generation;
  if(intent.catalog_generation!=catalog_generation||
      intent.attachment_uuid!=s.attachment.attachment_uuid||intent.attachment_generation!=s.attachment.generation||
      intent.attachment_version_uuid!=s.attachment_version_uuid||intent.policy_uuid!=p.policy_uuid||
      intent.policy_generation!=p.generation||intent.policy_version_uuid!=resolved.policy.version_uuid||
      intent.storage_profile_uuid!=s.profile.descriptor_uuid.value||intent.storage_profile_generation!=s.profile_generation||
      intent.storage_profile_version_uuid!=s.profile_version_uuid)return fail(E::selection_mismatch);
  if(!p.enabled)return fail(E::policy_disabled);
  if(intent.action==NativeStorageAction::physical_growth?!p.growth_allowed:!p.preallocation_allowed)return fail(E::action_disallowed);
  if(intent.maximum_total_pages>p.maximum_total_pages||intent.page_count>p.maximum_pages_per_action||
      intent.maximum_work_bytes>p.maximum_work_bytes||intent.maximum_retained_image_bytes>p.maximum_retained_image_bytes)
    return fail(E::limit_exceeded);
  NativeStorageIntentPolicyCheck r;r.error=E::none;r.resolution=std::move(resolved);return r;
}
} // namespace scratchbird::storage::database
