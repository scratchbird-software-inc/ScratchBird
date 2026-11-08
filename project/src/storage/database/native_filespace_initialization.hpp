// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_allocation_map.hpp"
#include "transaction_state.hpp"
#include "node_uuid_issuer.hpp"

namespace scratchbird::storage::database {
// The transaction inventory a native create writes is the in-place one: the
// compact state page plus its header page (SB_COMPACT_TRANSACTION_STATE_PAGE,
// SB_TRANSACTION_HEADER_PAGE). The transitional SBTINV01 choice (owner
// 2026-11-05, Q8 = 2) went with the checkpoint layer (W4d), and the request's
// inventory-format field, left with one value, went in W4d-3 (2026-11-08).
// SB_TRANSITIONAL_CHECKPOINT_GRAPH (drift removal R3; extends owner Q8 = 2):
// the genesis graph a native create writes. fixed_roots is the design: page
// zero's fixed roots name every control page and page zero, written last, is
// the commit point; there is no checkpoint, horizon-root, selector or
// watermark page. transitional_checkpoint writes the drift-era graph (genesis
// checkpoint 0x300, horizon root 0x302, selector pair 0x30E, watermark pair
// 0x500, system-state checkpoint fields). It exists only for the drift-layer
// tests until that layer is removed (R4), and is deleted with it. Production
// create never selects it. Ignored by the bootstrap-only initializer.
// W4d (2026-11-08): transitional_checkpoint was deleted with that layer.
enum class NativeCreationGenesisGraph : core::platform::u8 { fixed_roots };
struct NativeFilespaceInitializationRequest {
  disk::FilespaceBootstrap bootstrap;
  core::platform::Uuid operation_uuid,writer_uuid,policy_snapshot_uuid;
  transaction::mga::TransactionIdentity creator;
  core::platform::u64 total_pages=0,creation_utc_millis=0;
  NativeCreationGenesisGraph genesis_graph=NativeCreationGenesisGraph::fixed_roots;
};
// The filespace lifecycle a native create records in page zero, the directory
// and the catalog's filespace manifest. Page zero is written last, so a
// complete fixed-root file is an online filespace (1) (owner ruling
// 2026-11-07); the drift-era checkpoint graph keeps the request's initializing
// state. One function, so the three records cannot disagree.
inline core::platform::u16 NativeCreatedFilespaceLifecycle(const NativeFilespaceInitializationRequest& request) noexcept {
  (void)request;return core::platform::u16{1};
}
enum class NativeFilespaceInitializationError {
  none, invalid_request, invalid_device, device_not_empty, invalid_capacity,
  identity_failure, encoding_failure, resource_exhausted, hash_failure,
  io_failure, readback_mismatch, allocation_failure, cluster_requires_authority
};
struct NativeFilespaceInitializationReceipt {
  core::platform::Uuid database_uuid,filespace_uuid,page_zero_uuid,map_uuid;
  core::platform::Uuid operation_uuid,writer_uuid;
  transaction::mga::TransactionIdentity creator;
  disk::FilespaceRootReference allocation_root;
  std::array<core::platform::byte,32> page_zero_sha256{},allocation_head_sha256{};
  core::platform::u64 total_pages=0,free_pages=0,map_pages=0;
};
struct NativeFilespaceInitializationResult {
  NativeFilespaceInitializationError error=NativeFilespaceInitializationError::invalid_request;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  std::optional<NativeFilespaceInitializationReceipt> receipt;
  bool ok() const noexcept {return error==NativeFilespaceInitializationError::none&&receipt.has_value();}
};
// Creates real non-serving bootstrap/allocation storage on an empty owned
// device. No node activation, selected inventory, allocation grant or SQL success.
NativeFilespaceInitializationResult InitializeNativeFilespaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;
} // namespace scratchbird::storage::database
