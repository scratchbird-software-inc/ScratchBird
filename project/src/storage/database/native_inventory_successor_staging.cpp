// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_reserved_page_staging.hpp"
#include "transaction_inventory_validation.hpp"

namespace scratchbird::storage::database {
namespace mga=scratchbird::transaction::mga;
using Uuid=scratchbird::core::platform::Uuid;
using byte=scratchbird::core::platform::byte;
namespace {
using E=NativeInventoryStageError;
using Result=NativeInventoryStageResult;
struct PageResult : Result {std::optional<NativeInventoryStageReceipt> receipt;};
Result Fail(E error){Result r;r.error=error;return r;}
disk::NativePageReference Ref(const page::NativeTransactionInventoryPage& p){
  const auto& h=p.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};
}
}
NativeInventoryStageResult StageNativeInventorySuccessorFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,const mga::TransactionIdentity& owner,
    const std::vector<page::NativeTransactionInventoryPage>& chain,u64 budget) noexcept {
  try {
    if(chain.empty()||supplied.empty()||!budget)return Fail(E::invalid_request);
    auto devices=supplied;std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;std::vector<std::unique_lock<std::recursive_mutex>> guards;
    for(std::size_t i=0;i<devices.size();++i){const auto& f=devices[i];
      if(!f.device||!core::uuid::IsEngineIdentityUuid(f.filespace_uuid)||!handles.insert(f.device).second||
        (i&&devices[i-1].filespace_uuid==f.filespace_uuid))return Fail(E::invalid_request);
      guards.push_back(f.device->AcquireOperationGuard());
    }
    u64 image_bytes=0;std::vector<std::vector<byte>> images;images.reserve(chain.size());
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;
    mga::LocalTransactionInventory combined;
    combined.next_local_transaction_id=chain.front().inventory.next_local_transaction_id;
    combined.next_commit_sequence=chain.front().inventory.next_commit_sequence;
    for(std::size_t i=0;i<chain.size();++i){const auto& p=chain[i];const auto& h=p.header;
      if(h.page_size_bytes>budget-image_bytes)return Fail(E::resource_exhausted);
      image_bytes+=h.page_size_bytes;
      const auto expected_previous=i?std::optional{Ref(chain[i-1])}:std::nullopt;
      const auto expected_next=i+1<chain.size()?std::optional{Ref(chain[i+1])}:std::nullopt;
      if(h.database_uuid!=chain.front().header.database_uuid||p.object_uuid!=chain.front().object_uuid||
        p.inventory_generation!=chain.front().inventory_generation||p.previous!=expected_previous||p.next!=expected_next||
        p.inventory.next_local_transaction_id!=combined.next_local_transaction_id||p.inventory.next_commit_sequence!=combined.next_commit_sequence||
        !slots.emplace(h.filespace_uuid,h.page_number).second||!page_ids.insert(h.page_uuid).second)return Fail(E::chain_mismatch);
      if(!combined.entries.empty()&&!p.inventory.entries.empty()&&
        p.inventory.entries.front().identity.local_id.value<=combined.entries.back().identity.local_id.value)return Fail(E::chain_mismatch);
      auto encoded=page::EncodeNativeTransactionInventoryPage(p);
      if(!encoded.ok()){auto r=Fail(E::inventory_failure);r.inventory_error=encoded.error;return r;}
      combined.entries.insert(combined.entries.end(),p.inventory.entries.begin(),p.inventory.entries.end());
      images.push_back(std::move(encoded.bytes));
    }
    if(*mga::ValidateLocalTransactionInventoryStructure(combined))return Fail(E::inventory_failure);
    // Full preflight precedes all writes. All guards remain held for both passes.
    for(std::size_t i=0;i<chain.size();++i){const auto& p=chain[i];PageResult failure;
      auto prepared=detail::PrepareNativeReservedPage<PageResult,E>(devices,checkpoint,owner,p.header,p.object_uuid,0x1e,budget-image_bytes,failure);
      if(!prepared)return failure;
      const auto& current=prepared->authority.checkpoint_inventory;
      if(current.checkpoint->roots.front().object_uuid!=p.object_uuid)return Fail(E::root_mismatch);
      if(p.inventory_generation<=current.inventory_generation)return Fail(E::generation_mismatch);
      if(combined.next_local_transaction_id<current.inventory.next_local_transaction_id||
        combined.next_commit_sequence<current.inventory.next_commit_sequence)return Fail(E::counter_regression);
      if(i==0&&*mga::ValidateLocalTransactionInventoryEvolution(current.inventory,combined))return Fail(E::inventory_failure);
      if(p.header.page_size_bytes>budget-image_bytes-prepared->retained_image_bytes)return Fail(E::resource_exhausted);
      std::vector<byte> before(p.header.page_size_bytes);
      const auto io=prepared->target->ReadAt(p.header.page_number*u64{p.header.page_size_bytes},before.data(),before.size());
      if(!io.ok()||io.bytes_transferred!=before.size())return Fail(E::io_failure);
      if(before!=images[i]&&!std::all_of(before.begin(),before.end(),[](byte b){return b==0;}))return Fail(E::destination_not_empty);
    }
    Result result;result.receipts.reserve(chain.size());
    for(std::size_t i=0;i<chain.size();++i){const auto& p=chain[i];PageResult failure;
      auto prepared=detail::PrepareNativeReservedPage<PageResult,E>(devices,checkpoint,owner,p.header,p.object_uuid,0x1e,budget-image_bytes,failure);
      if(!prepared)return failure;
      // The candidate bytes are already charged. Common writer adds one scratch
      // image, and computes each receipt from actual reservation/image bytes.
      auto written=detail::WriteNativeReservedPageImage<PageResult,E>(*prepared,images[i],budget-image_bytes+p.header.page_size_bytes);
      if(!written.receipt)return written;
      result.receipts.push_back(*written.receipt);
    }
    result.error=E::none;return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
