// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_storage_policy_resolution.hpp"
#include "hash_digest.hpp"

namespace scratchbird::storage::database {
enum class NativeStorageRangeError {
  none, invalid_request, policy_failure, resource_exhausted, allocation_failure,
  allocation_mismatch, page_not_free, page_not_empty, io_failure, hash_failure
};
struct NativeStorageRangeInspection {
  NativeStorageRangeError error=NativeStorageRangeError::invalid_request;
  NativeStorageIntentPolicyCheck policy;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  std::optional<u64> blocked_page;
  u64 inspected_pages=0, retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeStorageRangeError::none&&policy.ok();}
};
// STORAGE-NATIVE-ACTION-RANGE-001. Fresh observation, NOT an allocation lease,
// authorization, serving-state check or resource grant. Device ownership stays
// with the caller. All guards remain held through policy, map and zero-byte
// checks, then release on return. Execution must revalidate under its own lease.
// Growth checks its exact observed end; it never creates or reserves capacity.
// Preallocation accepts only native free/recordless and physically zero pages;
// reusable, quarantined, reserved or dirty-free pages need their owning recovery.
inline NativeStorageRangeInspection InspectNativeStorageActionRangeFromOpenDevices(
    const NativeStorageActionIntent& intent,const std::vector<disk::NativeFilespaceDevice>& supplied,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,u64 budget) noexcept {
  using E=NativeStorageRangeError;
  NativeStorageRangeInspection result;
  const auto fail=[&](E e){result.error=e;return std::move(result);};
  try {
    auto devices=supplied;
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    if(devices.empty())return fail(E::invalid_request);
    for(std::size_t i=0;i<devices.size();++i){const auto& d=devices[i];
      if(!d.device||!core::uuid::IsEngineIdentityUuid(d.filespace_uuid)||
          !disk::FindCanonicalFilespacePageProfile(d.page_size_profile_uuid)||
          (i&&devices[i-1].filespace_uuid==d.filespace_uuid))return fail(E::invalid_request);
      for(std::size_t j=0;j<i;++j)if(devices[j].device==d.device)return fail(E::invalid_request);
    }
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(const auto& d:devices)guards.push_back(d.device->AcquireOperationGuard());
    auto policy=CheckNativeStorageIntentPolicyFromOpenDevices(intent,devices,selector,role,relation,reader,pin,budget);
    if(!policy.ok()){result.policy=std::move(policy);return fail(E::policy_failure);}
    const u64 allowance=std::min(budget,intent.maximum_retained_image_bytes);
    u64 retained=kNativeStorageActionIntentBytes;
    const auto charge=[&](u64 bytes){if(bytes>allowance-retained)return false;retained+=bytes;return true;};
    if(!charge(policy.resolution.capacity.retained_image_bytes)||
        !charge(policy.resolution.policy.source.source.retained_image_bytes))return fail(E::resource_exhausted);
    if(intent.action==NativeStorageAction::page_preallocation){
      const auto target=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.filespace_uuid==intent.filespace_uuid;});
      if(target==devices.end())return fail(E::invalid_request);
      // Reserve allowance for the one reusable physical probe before retaining
      // another allocation chain. No multiplication by a caller page count.
      if(!charge(intent.page_size_bytes))return fail(E::resource_exhausted);
      auto allocation=page::ReadNativeAllocationChainAtRootFromOpenDevice(*target->device,
        {intent.database_uuid,intent.filespace_uuid,intent.page_size_profile_uuid},intent.allocation_root,allowance-retained);
      if(!allocation.ok()){result.allocation_error=allocation.error;return fail(E::allocation_failure);}
      if(!charge(allocation.retained_image_bytes))return fail(E::resource_exhausted);
      const auto hash=core::hash::ComputeSha256Digest(allocation.pages.front().bytes);
      if(!hash.ok())return fail(E::hash_failure);
      const auto& head=*allocation.pages.front().map;
      if(hash.digest!=intent.allocation_sha256||head.map_generation!=intent.map_generation||
          head.capacity_generation!=intent.capacity_generation||head.total_pages!=intent.current_total_pages||
          allocation.state_counts!=policy.resolution.capacity.observation->state_counts)return fail(E::allocation_mismatch);
      // Check the whole logical range first: no data probing is needed to reject
      // allocated/control pages, and no non-free state is promoted to free.
      const u64 end=intent.first_page+intent.page_count;
      u64 covered=0;
      for(const auto& image:allocation.pages){const auto& map=*image.map;
        const u64 first=std::max(intent.first_page,map.first_page),last=std::min(end,map.first_page+map.states.size());
        auto record=std::lower_bound(map.records.begin(),map.records.end(),first,[](const auto& r,u64 n){return r.page_number<n;});
        for(u64 n=first;n<last;++n){
          if(map.states[n-map.first_page]!=page::NativeAllocationState::free||
              (record!=map.records.end()&&record->page_number==n)){
            result.blocked_page=n;return fail(E::page_not_free);
          }
          ++covered;
        }
      }
      if(covered!=intent.page_count)return fail(E::allocation_mismatch);
      std::vector<byte> scratch(intent.page_size_bytes);
      for(u64 n=intent.first_page;n<end;++n){
        const auto io=target->device->ReadAt(n*intent.page_size_bytes,scratch.data(),scratch.size());
        if(!io.ok()||io.bytes_transferred!=scratch.size()){result.blocked_page=n;return fail(E::io_failure);}
        ++result.inspected_pages;
        if(std::any_of(scratch.begin(),scratch.end(),[](byte b){return b!=0;})){
          result.blocked_page=n;return fail(E::page_not_empty);
        }
      }
    }
    result.error=E::none;result.retained_image_bytes=retained;result.policy=std::move(policy);return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
