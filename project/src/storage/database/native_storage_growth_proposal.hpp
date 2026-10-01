// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_storage_policy_resolution.hpp"

namespace scratchbird::storage::database {
enum class NativeStorageGrowthProposalError {
  none, invalid_request, resolution_failure, resource_exhausted, capacity_limit
};
enum class NativeStorageGrowthDisposition { refused, no_work, proposed };
enum class NativeStorageGrowthNoWork { none, policy_disabled, growth_disallowed, free_threshold_satisfied };
struct NativeStorageGrowthExtent {
  Uuid request_uuid, operation_uuid;
  u64 first_page=0, page_count=0, work_bytes=0;
};
struct NativeStorageGrowthProposalResult {
  NativeStorageGrowthProposalError error=NativeStorageGrowthProposalError::invalid_request;
  NativeStorageGrowthDisposition disposition=NativeStorageGrowthDisposition::refused;
  NativeStorageGrowthNoWork no_work=NativeStorageGrowthNoWork::none;
  std::optional<NativeStorageGrowthExtent> extent;
  NativeStoragePolicyResolution resolution;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {
    return error==NativeStorageGrowthProposalError::none&&resolution.ok()&&
      ((disposition==NativeStorageGrowthDisposition::proposed&&extent.has_value())||
       (disposition==NativeStorageGrowthDisposition::no_work&&!extent&&no_work!=NativeStorageGrowthNoWork::none));
  }
};
// STORAGE-NATIVE-GROWTH-PROPOSAL-001. Read-only observation, never authority.
// Own devices and published reader pins must remain alive throughout the call.
// Caller supplies no selected policy, extent, free count or permission flag.
// Reconcile unresolved original operations before any subsequent submission.
inline NativeStorageGrowthProposalResult ResolveNativeStorageGrowthProposalFromOpenDevices(
    const Uuid& request,const Uuid& operation,const Uuid& database,
    const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,const Uuid& filespace,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,
    const transaction::mga::PublishedSnapshotPin& pin,u64 budget) noexcept {
  using E=NativeStorageGrowthProposalError;
  const auto fail=[](E e){NativeStorageGrowthProposalResult r;r.error=e;return r;};
  if(!core::uuid::IsEngineIdentityUuid(request)||!core::uuid::IsEngineIdentityUuid(operation))
    return fail(E::invalid_request);
  if(budget<=kNativeStorageActionIntentBytes)return fail(E::resource_exhausted);
  auto resolved=ResolveNativeStoragePolicyFromOpenDevices(database,devices,checkpoint,filespace,
    selector,role,relation,reader,pin,budget-kNativeStorageActionIntentBytes);
  if(!resolved.ok()){
    auto r=fail(E::resolution_failure);r.resolution=std::move(resolved);return r;
  }
  const auto& p=*resolved.policy.policy;const auto& c=*resolved.capacity.observation;
  const u64 allowance=std::min(budget,p.maximum_retained_image_bytes);
  u64 retained=kNativeStorageActionIntentBytes;
  const auto charge=[&](u64 bytes){
    if(retained>allowance||bytes>allowance-retained)return false;
    retained+=bytes;return true;
  };
  if(!charge(resolved.capacity.retained_image_bytes)||
     !charge(resolved.policy.source.source.retained_image_bytes))return fail(E::resource_exhausted);
  NativeStorageGrowthNoWork reason=NativeStorageGrowthNoWork::none;
  std::optional<NativeStorageGrowthExtent> extent;
  if(!p.enabled)reason=NativeStorageGrowthNoWork::policy_disabled;
  else if(!p.growth_allowed)reason=NativeStorageGrowthNoWork::growth_disallowed;
  else if(c.total_pages>p.maximum_total_pages)return fail(E::capacity_limit);
  else if(c.state_counts[0]>=p.minimum_free_pages)reason=NativeStorageGrowthNoWork::free_threshold_satisfied;
  else {
    // Validated policy has free/target/increment <= 2^31-1; subtraction,
    // rounding and multiplication fit u64. Actual capacity reader validates
    // state counts; the selected canonical profile and work limits bound bytes.
    const u64 deficit=p.target_free_pages-c.state_counts[0];
    const u64 increments=deficit/p.growth_increment_pages+(deficit%p.growth_increment_pages!=0);
    const u64 count=std::min({increments*p.growth_increment_pages,p.maximum_pages_per_action,
      p.maximum_total_pages-c.total_pages,p.maximum_work_bytes/c.page_size_bytes});
    if(!count)return fail(E::capacity_limit);
    extent=NativeStorageGrowthExtent{request,operation,c.total_pages,count,count*c.page_size_bytes};
  }
  NativeStorageGrowthProposalResult r;r.error=E::none;r.no_work=reason;
  r.disposition=extent?NativeStorageGrowthDisposition::proposed:NativeStorageGrowthDisposition::no_work;
  r.extent=std::move(extent);r.resolution=std::move(resolved);r.retained_image_bytes=retained;return r;
}

// Owning-engine binding material, NOT an authorization context. In particular
// these guards still require current configuration/security admission. Runtime
// observations must not be converted into authority by constructing this type.
struct NativeStorageGrowthContext {
  Uuid policy_snapshot_uuid, initiator_uuid, request_context_uuid;
  u64 configuration_generation=0, security_generation=0;
};
enum class NativeStorageGrowthIntentError { none, invalid_context, proposal_failure, intent_failure };
struct NativeStorageGrowthIntentResult {
  NativeStorageGrowthIntentError error=NativeStorageGrowthIntentError::invalid_context;
  NativeStorageGrowthProposalResult proposal;
  NativeStorageIntentImage image;
  bool ok() const noexcept {
    return error==NativeStorageGrowthIntentError::none&&proposal.ok()&&
      (proposal.disposition==NativeStorageGrowthDisposition::no_work?
        !image.intent&&image.bytes.empty():image.ok());
  }
};
// STORAGE-NATIVE-GROWTH-INTENT-CONSTRUCTION-001. Resolve from actual devices,
// never from a caller-built proposal. No writes, generated identities or grants.
// Canonical request retention/retry and fresh execution admission remain owned
// by the engine. A changed result is not permission to rebind an original retry.
inline NativeStorageGrowthIntentResult ResolveNativeStorageGrowthIntentFromOpenDevices(
    const Uuid& request,const Uuid& operation,const Uuid& database,
    const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,const Uuid& filespace,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,
    const transaction::mga::PublishedSnapshotPin& pin,
    const NativeStorageGrowthContext& context,u64 budget) noexcept {
  using E=NativeStorageGrowthIntentError;
  NativeStorageGrowthIntentResult result;
  for(const auto* id:{&context.policy_snapshot_uuid,&context.initiator_uuid,&context.request_context_uuid})
    if(!core::uuid::IsEngineIdentityUuid(*id))return result;
  auto proposal=ResolveNativeStorageGrowthProposalFromOpenDevices(request,operation,database,
    devices,checkpoint,filespace,selector,role,relation,reader,pin,budget);
  if(!proposal.ok()){
    result.error=E::proposal_failure;result.proposal=std::move(proposal);return result;
  }
  if(proposal.disposition==NativeStorageGrowthDisposition::proposed){
    const auto& c=*proposal.resolution.capacity.observation;
    const auto& s=*proposal.resolution.selection;
    const auto& p=*proposal.resolution.policy.policy;
    const auto& extent=*proposal.extent;
    NativeStorageActionIntent i;
    i.request_uuid=extent.request_uuid;i.operation_uuid=extent.operation_uuid;
    i.database_uuid=c.database_uuid;i.filespace_uuid=c.filespace_uuid;
    i.locator_uuid=c.locator_uuid;i.page_zero_uuid=c.page_zero_uuid;
    i.page_size_profile_uuid=c.page_size_profile_uuid;i.page_size_bytes=c.page_size_bytes;
    i.policy_snapshot_uuid=context.policy_snapshot_uuid;i.initiator_uuid=context.initiator_uuid;
    i.request_context_uuid=context.request_context_uuid;
    i.configuration_generation=context.configuration_generation;i.security_generation=context.security_generation;
    i.catalog_generation=proposal.resolution.policy.source.source.checkpoint.catalogs.front().root->catalog_generation;
    i.checkpoint=c.checkpoint;i.allocation_root=c.allocation_root;
    i.checkpoint_sha256=c.checkpoint_sha256;i.allocation_sha256=c.allocation_sha256;
    i.checkpoint_generation=c.checkpoint_generation;i.checkpoint_root_set_generation=c.checkpoint_root_set_generation;
    i.directory_generation=c.directory_generation;i.filespace_root_set_generation=c.filespace_root_set_generation;
    i.page_zero_generation=c.page_zero_generation;i.map_generation=c.map_generation;i.capacity_generation=c.capacity_generation;
    i.attachment_uuid=s.attachment.attachment_uuid;i.attachment_version_uuid=s.attachment_version_uuid;
    i.attachment_generation=s.attachment.generation;i.storage_profile_uuid=s.profile.descriptor_uuid.value;
    i.storage_profile_version_uuid=s.profile_version_uuid;i.storage_profile_generation=s.profile_generation;
    i.policy_uuid=p.policy_uuid;i.policy_version_uuid=proposal.resolution.policy.version_uuid;i.policy_generation=p.generation;
    i.current_total_pages=c.total_pages;i.first_page=extent.first_page;i.page_count=extent.page_count;
    i.maximum_total_pages=p.maximum_total_pages;i.maximum_work_bytes=p.maximum_work_bytes;
    i.maximum_retained_image_bytes=std::min(budget,p.maximum_retained_image_bytes);
    result.image=EncodeNativeStorageActionIntent(i,kNativeStorageActionIntentBytes);
    if(!result.image.ok()){result.error=E::intent_failure;return result;}
  }
  result.error=E::none;result.proposal=std::move(proposal);return result;
}
} // namespace scratchbird::storage::database
