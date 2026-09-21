// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_reserved_page_staging.hpp"

namespace scratchbird::storage::database {
namespace {
namespace mga=scratchbird::transaction::mga;
using E=NativeRowDataStageError;
NativeRowDataStageResult Fail(E error){NativeRowDataStageResult r;r.error=error;return r;}
}
NativeRowDataStageResult StageNativeRowDataPageFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,
    const mga::TransactionIdentity& owner,const page::NativeRowDataPage& image,u64 budget) noexcept {
  try {
    NativeRowDataStageResult result;
    constexpr u32 roles=(u32{1}<<5)|(u32{1}<<8)|(u32{1}<<10);
    auto prepared=detail::PrepareNativeReservedPage<NativeRowDataStageResult,E>(
      supplied,checkpoint,owner,image.header,image.body.relation_uuid.value,roles,budget,result);
    if(!prepared)return result;
    if(2*u64{image.header.page_size_bytes}>budget-prepared->retained_image_bytes)return Fail(E::resource_exhausted);
    auto encoded=page::EncodeNativeRowDataPage(image);
    if(!encoded.ok()){auto r=Fail(E::page_failure);r.page_error=encoded.error;return r;}
    const auto& inventory=prepared->authority.checkpoint_inventory.inventory;
    for(const auto& row:encoded.page->body.rows){
      const auto creator=mga::LookupLocalTransaction(inventory,mga::MakeLocalTransactionId(row.local_transaction_id));
      if(!creator.ok()||creator.entry.identity.transaction_uuid.value!=row.transaction_uuid.value||
          creator.entry.identity.transaction_uuid.kind!=row.transaction_uuid.kind||
          creator.entry.identity.scope!=mga::TransactionScope::local_node)return Fail(E::row_creator_mismatch);
    }
    return detail::WriteNativeReservedPageImage<NativeRowDataStageResult,E>(*prepared,encoded.bytes,budget);
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
