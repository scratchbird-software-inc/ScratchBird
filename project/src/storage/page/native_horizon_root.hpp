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
enum class NativeHorizonKind : u16 {
  oit=1, oat, ost, recovery, archive, legal_hold, backup, management, oct, olr,
  cluster_transaction, cluster_snapshot, cluster_limbo, cluster_archive,
  cluster_backup, cluster_legal_hold, cluster_recovery, cluster_conflict_resolution
};
enum class NativeHorizonOwner : u16 { engine=1, parser, backup, archive, cluster, legal_hold, detached_filespace, donor_bridge };
struct NativeHorizonRecord {
  NativeHorizonKind kind=NativeHorizonKind::oit;
  NativeHorizonOwner owner_kind=NativeHorizonOwner::engine;
  u32 flags=0;
  // Cluster rows require provider-authorized participant-to-local projection.
  u64 local_boundary=0;
  Uuid owner_uuid, pin_uuid, checkpoint_object_uuid;
  u64 checkpoint_generation=0;
  Uuid diagnostic_uuid;
  std::optional<disk::NativePageReference> checkpoint;
  Uuid horizon_uuid, timeline_uuid;
};
struct NativeHorizonRoot {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 epoch=0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id=0, flags=0, total_records=0, first_record=0;
  disk::NativePageReference retention;
  Uuid retention_object_uuid;
  std::array<byte,32> retention_sha256{};
  std::optional<disk::NativePageReference> next;
  std::array<byte,32> next_sha256{};
  // Image-local summary only, not cleanup eligibility.
  u64 minimum_blocker=0;
  std::vector<NativeHorizonRecord> records;
};
enum class NativeHorizonError {
  none, invalid_header, invalid_family, invalid_record, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, chain_mismatch, io_failure
};
struct NativeHorizonResult {
  NativeHorizonError error=NativeHorizonError::invalid_family;
  std::optional<NativeHorizonRoot> root;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeHorizonError::none&&root.has_value();}
};
NativeHorizonResult EncodeNativeHorizonRoot(const NativeHorizonRoot&) noexcept;
NativeHorizonResult DecodeNativeHorizonRoot(const std::vector<byte>&) noexcept;
struct NativeHorizonChainResult {
  NativeHorizonError error=NativeHorizonError::invalid_reference;
  std::vector<NativeHorizonResult> pages;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeHorizonError::none&&!pages.empty();}
};
// Actual image/chain evidence only. No inferred current horizon, committed
// publication, provider authority, target-content proof or cleanup grant.
NativeHorizonChainResult ReadNativeHorizonRootFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
    const Uuid& object_uuid,const disk::NativePageReference& head,
    u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::page
