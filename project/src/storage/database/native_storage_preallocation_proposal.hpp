// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_storage_growth_proposal.hpp"
#include "native_storage_action_range.hpp"

namespace scratchbird::storage::database {
struct NativeStoragePreallocationDemand {
  u64 first_page=0,page_count=0;
  Uuid allocation_owner_uuid;
  u32 allocation_page_type=0;
};
enum class NativeStoragePreallocationProposalError {
  none,invalid_request,memory_binding_failure,resource_exhausted,resolution_failure,
  intent_failure,range_failure,io_failure
};
struct NativeStoragePreallocationProposalResult {
  NativeStoragePreallocationProposalError error=NativeStoragePreallocationProposalError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  NativeStorageIntentError intent_error=NativeStorageIntentError::none;
  NativeStoragePolicyResolution source_failure;
  NativeStorageRangeInspection range;
  NativeStorageIntentImage image;
  bool ok() const noexcept {
    return error==NativeStoragePreallocationProposalError::none&&range.ok()&&image.ok();
  }
};
// Exact demand resolution only, not an allocation or authority receipt. The
// request/context/allocation owner must receive separate kernel admission.
// Never substitute another range, truncate a demand, or create free capacity.
inline NativeStoragePreallocationProposalResult ResolveNativeStoragePreallocationIntentFromOpenDevices(
    const Uuid& request,const Uuid& operation,const Uuid& database,
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,const Uuid& filespace,
    u16 selector,u16 role,const NativeCatalogRelationBinding& relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,
    const NativeStorageActionContext& context,const NativeStoragePreallocationDemand& demand,
    u64 budget,NativeStorageMemory& memory) noexcept {
  using E=NativeStoragePreallocationProposalError;
  NativeStoragePreallocationProposalResult result;
  const auto fail=[&](E e){result.error=e;return std::move(result);};
  try {
    for(const auto* id:{&request,&operation,&database,&filespace,&context.policy_snapshot_uuid,
        &context.initiator_uuid,&context.request_context_uuid,&demand.allocation_owner_uuid})
      if(!core::uuid::IsEngineIdentityUuid(*id))return result;
    if(!demand.first_page||!demand.page_count||!demand.allocation_page_type||
        !disk::IsRegisteredNativePageType(demand.allocation_page_type))return result;
    result.memory_error=memory.CheckBinding(database,operation);
    if(result.memory_error==NativeStorageMemoryError::none&&
        (memory.binding().owner_uuid!=context.initiator_uuid||memory.binding().context_uuid!=context.request_context_uuid))
      result.memory_error=NativeStorageMemoryError::invalid_binding;
    if(result.memory_error!=NativeStorageMemoryError::none)return fail(E::memory_binding_failure);
    if(budget<=kNativeStorageActionIntentBytes)return fail(E::resource_exhausted);
    auto devices=supplied;
    std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    if(devices.empty())return result;
    for(std::size_t n=0;n<devices.size();++n){const auto& d=devices[n];
      if(!d.device||!core::uuid::IsEngineIdentityUuid(d.filespace_uuid)||
          !disk::FindCanonicalFilespacePageProfile(d.page_size_profile_uuid)||
          (n&&devices[n-1].filespace_uuid==d.filespace_uuid))return result;
      for(std::size_t prior=0;prior<n;++prior)if(devices[prior].device==d.device)return result;
    }
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(const auto& d:devices)guards.push_back(d.device->AcquireOperationGuard());
    auto source=ResolveNativeStoragePolicyFromOpenDevices(database,devices,checkpoint,filespace,
        selector,role,relation,reader,pin,budget-kNativeStorageActionIntentBytes);
    if(!source.ok()){result.source_failure=std::move(source);return fail(E::resolution_failure);}
    auto intent=native_storage_proposal_detail::Bind(source,request,operation,context,budget);
    // Once the selected policy is known, enforce its ceiling on this first
    // retained source as well as on the subsequent independent range read.
    u64 retained=kNativeStorageActionIntentBytes;
    for(const auto bytes:{source.capacity.retained_image_bytes,source.policy.source.source.retained_image_bytes}){
      if(bytes>intent.maximum_retained_image_bytes-retained)return fail(E::resource_exhausted);
      retained+=bytes;
    }
    intent.action=NativeStorageAction::page_preallocation;intent.intended_state=NativeStorageIntentState::preallocated;
    intent.first_page=demand.first_page;intent.page_count=demand.page_count;
    intent.allocation_owner_uuid=demand.allocation_owner_uuid;intent.allocation_page_type=demand.allocation_page_type;
    result.intent_error=ValidateNativeStorageActionIntent(intent);
    if(result.intent_error!=NativeStorageIntentError::none)return fail(E::intent_failure);
    // Construct the local candidate before physical probing, but expose it
    // only after complete range validation. No fallible sealing step may erase
    // the observations of a completed probe or publish a successful prefix.
    auto image=EncodeNativeStorageActionIntent(intent,kNativeStorageActionIntentBytes);
    if(!image.ok()){result.intent_error=image.error;return fail(E::intent_failure);}
    // Drop first-pass images before retaining the range reader's sources. The
    // complete ordered device guards span both reads. Do not double-retain an
    // uncharged snapshot or return the local candidate on a failed range read.
    source={};
    auto range=InspectNativeStorageActionRangeWithMemoryFromOpenDevices(intent,devices,
        selector,role,relation,reader,pin,budget,memory);
    if(!range.ok()){result.range=std::move(range);return fail(E::range_failure);}
    result.range=std::move(range);result.image=std::move(image);result.error=E::none;return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
