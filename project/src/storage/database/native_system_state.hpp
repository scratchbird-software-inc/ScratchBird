// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
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
  invalid_filespace, binding_mismatch, io_failure
};
struct NativeSystemStateResult {
  NativeSystemStateError error=NativeSystemStateError::invalid_family;
  std::optional<NativeSystemState> state;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeSystemStateError::none&&state.has_value();}
};
NativeSystemStateResult EncodeNativeSystemState(const NativeSystemState&) noexcept;
NativeSystemStateResult DecodeNativeSystemState(const std::vector<byte>&) noexcept;
// Actual caller-owned device and exact reference, not startup/recovery admission.
NativeSystemStateResult ReadNativeSystemStateFromOpenDevice(
    disk::FileDevice&,const Uuid& database_uuid,const disk::FilespaceRootReference&) noexcept;
}  // namespace scratchbird::storage::database
