// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_allocation_map.hpp"
#include "transaction_state.hpp"
#include "node_uuid_issuer.hpp"

namespace scratchbird::storage::database {
// SB_TRANSITIONAL_LEGACY_INVENTORY (owner 2026-11-05, Q8 = 2): the
// transaction inventory a native create writes. in_place is the design: the
// compact state page plus its header page (SB_COMPACT_TRANSACTION_STATE_PAGE,
// SB_TRANSACTION_HEADER_PAGE). transitional_legacy writes the SBTINV01 page and
// no header page; it exists only so the drift-layer tests (checkpoint,
// selector and management publication) keep the starting database they decode
// until that layer is removed, and it is deleted with that layer. Production
// create never selects it. Ignored by the bootstrap-only initializer.
enum class NativeCreationInventoryFormat : core::platform::u8 { in_place, transitional_legacy };
struct NativeFilespaceInitializationRequest {
  disk::FilespaceBootstrap bootstrap;
  core::platform::Uuid operation_uuid,writer_uuid,policy_snapshot_uuid;
  transaction::mga::TransactionIdentity creator;
  core::platform::u64 total_pages=0,creation_utc_millis=0;
  NativeCreationInventoryFormat inventory_format=NativeCreationInventoryFormat::transitional_legacy;
};
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
