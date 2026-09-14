// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
enum class NativeRetentionKind : u16 { backup_forward=1, archive, legal_hold, cluster_replay, detached_filespace, parser_snapshot, donor_emulation, forensic };
enum class NativeRetentionAccess : u16 { recovery_only=1, audit_queryable, temporal_queryable, legal_hold, donor_emulation, forensic_debug, backup_forward_only };
struct NativeRetentionPin {
  Uuid pin_uuid, owner_uuid;
  NativeRetentionKind kind=NativeRetentionKind::backup_forward;
  NativeRetentionAccess access=NativeRetentionAccess::recovery_only;
  u32 flags=0;
  u64 start_local=0, end_local=0;
  Uuid timeline_uuid, filespace_uuid;
  u64 retain_until_local=0, retain_until_unix_ns=0, blocked_operations=1;
};
struct NativeRetentionPage {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 epoch=0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id=0, flags=0, total_pins=0, first_record=0;
  std::optional<disk::NativePageReference> next;
  std::array<byte,32> next_sha256{};
  u64 legal_hold_pins=0, lowest_start=0, highest_end=0;
  std::vector<NativeRetentionPin> records;
};
enum class NativeRetentionError {
  none, invalid_header, invalid_family, invalid_record, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, chain_mismatch, summary_mismatch, io_failure
};
struct NativeRetentionPageResult {
  NativeRetentionError error=NativeRetentionError::invalid_family;
  std::optional<NativeRetentionPage> page;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeRetentionError::none&&page.has_value();}
};
NativeRetentionPageResult EncodeNativeRetentionPage(const NativeRetentionPage&) noexcept;
NativeRetentionPageResult DecodeNativeRetentionPage(const std::vector<byte>&) noexcept;
struct NativeRetentionChainResult {
  NativeRetentionError error=NativeRetentionError::invalid_reference;
  // Root first, followed by every actual leaf in order.
  std::vector<NativeRetentionPageResult> images;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeRetentionError::none&&!images.empty();}
};
// Image/chain and summary evidence, not current checkpoint, policy/permission,
// provider, release, expiry, publication or cleanup authority.
NativeRetentionChainResult ReadNativeRetentionRootFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
    const Uuid& object_uuid,const disk::NativePageReference& root,
    u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::page
