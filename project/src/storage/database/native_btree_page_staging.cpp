// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_reserved_page_staging.hpp"

namespace scratchbird::storage::database {
namespace {
namespace mga=scratchbird::transaction::mga;
using E=NativeBtreeStageError;
NativeBtreeStageResult Fail(E error){NativeBtreeStageResult r;r.error=error;return r;}
}
NativeBtreeStageResult StageNativeBtreePageFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,const mga::TransactionIdentity& owner,
    const page::NativeBtreeDependencies& dependencies,const page::NativeBtreePage& node,u64 budget) noexcept {
  try {
    if(node.dependencies!=dependencies)return Fail(E::dependency_mismatch);
    if(node.creator_transaction_uuid!=owner.transaction_uuid.value||node.creator_local_transaction_id!=owner.local_id.value)
      return Fail(E::creator_mismatch);
    NativeBtreeStageResult result;
    auto prepared=detail::PrepareNativeReservedPage<NativeBtreeStageResult,E>(
      supplied,checkpoint,owner,node.header,node.dependencies.index_uuid,0x60u,budget,result);
    if(!prepared)return result;
    if(2*u64{node.header.page_size_bytes}>budget-prepared->retained_image_bytes)return Fail(E::resource_exhausted);
    auto encoded=page::EncodeNativeBtreePage(node);
    if(!encoded.ok()){auto r=Fail(E::page_failure);r.page_error=encoded.error;return r;}
    return detail::WriteNativeReservedPageImage<NativeBtreeStageResult,E>(*prepared,encoded.bytes,budget);
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
} // namespace scratchbird::storage::database
