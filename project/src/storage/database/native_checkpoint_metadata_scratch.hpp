// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_metadata_decode_scratch.hpp"
#include "native_management_operation.hpp"
#include "native_checkpoint_root.hpp"

// SB_DRIFT_REMOVAL_R4A: the checkpoint-root and management-operation decoders of
// the metadata scratch, moved verbatim out of native_metadata_decode_scratch.hpp
// (owner Q20 = 1, 2026-11-07). Only the drift layer uses them; this header is
// deleted with it.
namespace scratchbird::storage::database::detail {
struct NativeCheckpointMetadataScratch:NativeMetadataScratch {
  using NativeMetadataScratch::NativeMetadataScratch;
  auto Checkpoint(Image raw){return DecodeNativeCheckpointRootInto(raw,Array<NativeCheckpointRootReference>(16));}
  NativeManagementOperationViewWorkspace OperationWorkspace(Image raw){
    const auto count=raw.size()>=512?std::min<std::size_t>(LoadLittle32(raw.data()+456),(raw.size()-512)/256):0;
    return {Array<NativeManagementStepView>(count),Array<Uuid>(2*count+7)};
  }
};
} // namespace scratchbird::storage::database::detail
