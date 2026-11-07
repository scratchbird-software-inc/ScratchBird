// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <span>
#include <vector>
namespace scratchbird::storage::database {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
enum class NativeSystemLifecycle : u16 {
  creating=1, closed=2, opening=3, ready=4, draining=5, restricted=6,
  maintenance=7, recovering=8, drop_pending=9, quarantined=10
};
enum class NativeSystemRecovery : u16 {
  clean_checkpoint=1, checkpoint_rebuild=2, repaired=3, fence_writes=4,
  corruption_stop=5, restricted_open=6, operator_review=7
};
namespace NativeSystemFlag {
inline constexpr u32 clean=1, dirty=2, write_fenced=4, cluster=8;
}
// Durable observations only: no flag certifies actual runtime or MGA effects.
struct NativeSystemState {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  u64 state_generation=0, restart_generation=0, startup_counter=0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id=0;
  NativeSystemLifecycle lifecycle=NativeSystemLifecycle::creating;
  NativeSystemRecovery recovery=NativeSystemRecovery::checkpoint_rebuild;
  u32 flags=NativeSystemFlag::dirty|NativeSystemFlag::write_fenced;
  u64 checkpoint_generation=0;
  std::optional<disk::NativePageReference> checkpoint;
  Uuid checkpoint_object_uuid;
  Uuid clean_transaction_uuid;
  u64 clean_local_transaction_id=0;
  Uuid transition_operation_uuid;
  std::optional<disk::NativePageReference> predecessor;
  std::array<byte,32> predecessor_sha256{};
};
enum class NativeSystemStateError {
  none, invalid_header, invalid_family, invalid_identity, invalid_state,
  invalid_reference, invalid_integrity, hash_failure, resource_exhausted,
  invalid_filespace, binding_mismatch, io_failure, history_mismatch
};
struct NativeSystemStateResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_family;
  std::optional<NativeSystemState> state;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&state.has_value();}
};
// SB_MGA_DATABASE_STATE (owner rulings 2026-11-07, option 1 and the mode
// table): the database's administrative mode, Firebird's model. The page is
// updated in place by careful write. It carries no clean/dirty flag, recovery
// classification, observed checkpoint, predecessor chain or transition
// operation: careful write keeps every on-disk state consistent, and a crash is
// handled by restart dead-marking of the transaction inventory (Firebird
// TRA_cleanup). Codes keep the spec's numbers where the meaning survives;
// creating(1), closed(2), opening(3), draining(5) and recovering(8) are retired.
// Family SBSYS002; SBSYS001 (NativeSystemState) remains only for the drift-era
// checkpoint graph until R4.
enum class NativeDatabaseMode : u16 {
  online=4,        // Firebird shutdown none
  restricted=6,    // owner and administrators only (shutdown multi)
  maintenance=7,   // one administrator attachment (shutdown single)
  drop_pending=9,
  quarantined=10,  // refused until the operator acts
  offline=11       // no attachment (shutdown full)
};
namespace NativeDatabaseStateFlag {
inline constexpr u32 read_only=1;  // Firebird hdr_read_only, separate from the mode
}
struct NativeDatabaseState {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id=0;
  NativeDatabaseMode mode=NativeDatabaseMode::online;
  u32 flags=0;
};
struct NativeDatabaseStateResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_family;
  std::optional<NativeDatabaseState> state;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&state.has_value();}
};
struct NativeDatabaseStateValueResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_family;
  std::optional<NativeDatabaseState> state;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&state.has_value();}
};
NativeDatabaseStateResult EncodeNativeDatabaseState(const NativeDatabaseState&) noexcept;
NativeDatabaseStateValueResult DecodeNativeDatabaseStateValue(std::span<const byte>) noexcept;
NativeSystemStateResult EncodeNativeSystemState(const NativeSystemState&) noexcept;
NativeSystemStateResult DecodeNativeSystemState(const std::vector<byte>&) noexcept;
struct NativeSystemStateValueResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_family;
  std::optional<NativeSystemState> state;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&state.has_value();}
};
// Fixed native values only; does not retain input or certify current selection.
NativeSystemStateValueResult DecodeNativeSystemStateValue(std::span<const byte>) noexcept;
// Actual caller-owned device and exact reference, not startup/recovery admission.
NativeSystemStateResult ReadNativeSystemStateFromOpenDevice(
    disk::FileDevice&,const Uuid& database_uuid,const disk::FilespaceRootReference&) noexcept;
struct NativeSystemStateHistoryResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_reference;
  std::vector<NativeSystemStateResult> pages;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&!pages.empty();}
};
// Supplied retained range only; no inference of sufficient retention, committed
// transitions, observed checkpoint truth or current runtime readiness.
NativeSystemStateHistoryResult ReadNativeSystemStateHistoryFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
    const disk::FilespaceRootReference& head,const disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::database
