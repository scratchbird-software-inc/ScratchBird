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
} // namespace scratchbird::storage::database
