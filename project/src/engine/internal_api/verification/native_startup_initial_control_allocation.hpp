// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_startup_storage_identity.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeStartupInitialControlError {none,source_required,invalid_record,missing_page};
struct NativeStartupInitialControlCheck {
  NativeStartupInitialControlError error=NativeStartupInitialControlError::source_required;
  std::size_t control_pages=0;
  // Creator pages that no root, catalog page or control page reaches. Owner
  // ruling 2026-11-06 (Q17): unreachable pages are wasted space, reported
  // here and never refused.
  std::size_t wasted_pages=0;
  bool ok()const noexcept{return error==NativeStartupInitialControlError::none;}
};
// Complete first-create original-allocation membership. The selected source
// separately proves every operation-created record against immutable history.
// This comparison grants no ownership: the private verifier must also read and
// validate the selected bodies and retain continuous custody of their source.
inline NativeStartupInitialControlCheck CheckNativeStartupInitialControlAllocation(
    const storage::database::NativeCatalogCapture& capture,
    const NativeStartupStorageIdentityObservation& identity,
    const storage::database::NativeReadAllocationChain& chain) noexcept {
  namespace db=storage::database;namespace disk=storage::disk;namespace page=storage::page;
  using E=NativeStartupInitialControlError;using Uuid=core::platform::Uuid;using u64=core::platform::u64;
  if(!capture.has_initial_controls()||!capture.has_physical_pages()||!chain.ok()||
      capture.source()->source_generation!=1||capture.filespaces().size()!=1||identity.filespaces!=1||
      !identity.primary_root_count||identity.primary_root_count>identity.primary_roots.size())return {};
  // The creator is transaction 1, committed. Later transactions may exist:
  // the in-place transaction writer (SB_TRANSACTION_INVENTORY_WRITER) adds
  // them without touching the creator's pages; until allocation from the map
  // exists every record is still the creator's, checked below.
  const auto inventory=capture.inventory();
  if(!inventory||inventory->inventory.entries.empty()||inventory->inventory.entries.front().identity.local_id.value!=1||
      !transaction::mga::HasCommittedInventoryOutcome(inventory->inventory.entries.front()))return {};
  const auto creator=inventory->inventory.entries.front().identity;
  const auto& member=capture.filespaces().front();
  if(member.bootstrap.filespace_uuid!=identity.primary_filespace_uuid||member.page_zero_uuid!=identity.primary_page_uuid||
      member.page_zero_generation!=identity.primary_page_generation||member.bootstrap.page_size_profile_uuid!=identity.primary_profile_uuid)return {};
  const auto catalogs=capture.physical_pages(),controls=capture.initial_control_pages();
  const auto find=[](auto pages,const Uuid& fs,u64 n)->const db::NativeCapturedCatalogPage*{
    const auto at=std::lower_bound(pages.begin(),pages.end(),std::pair{fs,n},[](const auto& p,const auto& key){return
        p.header.filespace_uuid<key.first||(p.header.filespace_uuid==key.first&&p.header.page_number<key.second);});
    return at==pages.end()||at->header.filespace_uuid!=fs||at->header.page_number!=n?nullptr:&*at;
  };
  struct Root {disk::NativePageReference page;core::platform::u32 type;Uuid owner;};
  std::array<Root,64> additional;std::size_t additional_count=0;
  const auto add=[&](const disk::NativePageReference& ref,core::platform::u32 type,const Uuid& owner){
    if(ref.filespace_uuid!=identity.primary_filespace_uuid||ref.page_size_profile_uuid!=identity.primary_profile_uuid)return false;
    const auto* known=find(catalogs,ref.filespace_uuid,ref.page_number);
    if(!known)known=find(controls,ref.filespace_uuid,ref.page_number);
    if(known)return known->header.page_generation==ref.page_generation&&known->header.page_type==type&&
        known->header.page_size_profile_uuid==ref.page_size_profile_uuid&&known->owner_uuid==owner;
    for(const auto& prior:std::span{additional}.first(additional_count))
      if(prior.page.filespace_uuid==ref.filespace_uuid&&prior.page.page_number==ref.page_number)
        return prior.page==ref&&prior.type==type&&prior.owner==owner;
    if(additional_count==additional.size())return false;
    additional[additional_count++]={ref,type,owner};return true;
  };
  for(const auto& root:std::span{identity.primary_roots}.first(identity.primary_root_count))
    if(!add({root.filespace_uuid,root.page_number,root.page_generation,root.page_size_profile_uuid},root.page_type,root.object_uuid))return {E::invalid_record};
  std::size_t count=0,wasted=0;
  for(const auto& image:chain.pages){const auto& map=image.map;
    if(map.header.filespace_uuid!=identity.primary_filespace_uuid||map.header.page_size_profile_uuid!=identity.primary_profile_uuid||
        map.header.database_uuid!=capture.source()->database_uuid)return {E::invalid_record};
    for(const auto& record:map.records){
      if(!record.creator_operation_uuid.is_nil())continue;
      if(record.page_number<map.first_page||record.page_number-map.first_page>=map.states.size()||
          map.states[record.page_number-map.first_page]!=page::NativeAllocationState::allocated)return {E::invalid_record};
      const auto matches=[&](const db::NativeCapturedCatalogPage& p){return p.header.database_uuid==map.header.database_uuid&&
          p.header.page_size_profile_uuid==map.header.page_size_profile_uuid&&p.header.page_size_bytes==map.header.page_size_bytes&&
          p.header.page_uuid==record.page_uuid&&p.header.page_generation==record.page_generation&&
          p.header.page_type==record.page_type&&p.owner_uuid==record.owner_uuid;};
      // SB_ALLOCATION_MAP_IN_PLACE: a page allocated after the create, by a
      // later transaction or by the system. A transaction-chain page the
      // system grew (W2c) is reached through the inventory, which the capture
      // records with the controls; the fixed-root lease has already required
      // that only the pages after each chain's first are system-created.
      // Nothing else the creator wrote may reach such a page; unreached, it is
      // wasted space (owner ruling Q17 = 1).
      if(record.creator_transaction_uuid!=creator.transaction_uuid.value||record.creator_local_transaction_id!=creator.local_id.value){
        const bool system=record.creator_transaction_uuid.is_nil()&&!record.creator_local_transaction_id;
        if(system&&record.page_type==0x301)
          if(const auto* p=find(controls,map.header.filespace_uuid,record.page_number)){
            if(!matches(*p))return {E::invalid_record};++count;continue;
          }
        const auto roots=std::span{additional}.first(additional_count);
        if(record.page_number==0||find(catalogs,map.header.filespace_uuid,record.page_number)||
            find(controls,map.header.filespace_uuid,record.page_number)||
            std::any_of(roots.begin(),roots.end(),[&](const auto& r){return r.page.page_number==record.page_number;}))
          return {E::invalid_record};
        ++wasted;continue;
      }
      if(const auto* p=find(catalogs,map.header.filespace_uuid,record.page_number)){
        if(!matches(*p))return {E::invalid_record};continue;
      }
      if(record.page_number==0){
        if(record.page_type!=1||record.page_uuid!=identity.primary_page_uuid||record.page_generation!=identity.primary_page_generation||
            record.owner_uuid!=identity.primary_filespace_uuid)return {E::invalid_record};
        ++count;continue;
      }
      if(const auto* p=find(controls,map.header.filespace_uuid,record.page_number)){
        if(!matches(*p))return {E::invalid_record};++count;continue;
      }
      const auto roots=std::span{additional}.first(additional_count);
      const auto at=std::find_if(roots.begin(),roots.end(),[&](const auto& r){return r.page.page_number==record.page_number;});
      if(at==roots.end()){++wasted;continue;}
      if(at->type!=record.page_type||at->owner!=record.owner_uuid||at->page.page_generation!=record.page_generation)return {E::invalid_record};
      ++count;
    }
  }
  if(count!=controls.size()+additional_count+1)return {E::missing_page};
  return {E::none,count,wasted};
}
} // namespace scratchbird::engine::internal_api
