// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_NATIVE_VERIFICATION_CLASSES. The verification class and
// authority enums catalog-based verification policy selection uses, moved
// verbatim out of native_verification_operation.hpp (a checkpoint-layer
// header, owner ruling Q14 = 1) so policy selection reaches no drift header.
#include "runtime_platform.hpp"
#include "native_decoded_storage_ranges.hpp"
#include <cstdint>
#include <span>

namespace scratchbird::engine::internal_api {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u16;
using core::platform::u32;
using core::platform::u64;

enum class NativeVerificationClass : u16 {
  startup_light=1,startup_full,online_consistency,offline_consistency,maintenance,
  targeted,cluster,restore,backup_package
};
enum class NativeVerificationAuthority : u16 {
  startup_recovery=1,local_sysarch,cluster_authority,scheduler,restore_operation,administrator_review
};

// Moved verbatim from native_verification_operation.hpp with the enums.
namespace native_verification_state_detail {
template<class T>bool Region(std::span<T> input) noexcept {
  return storage::disk::detail::DecodedRange(input).valid&&
      (input.empty()||reinterpret_cast<std::uintptr_t>(input.data())%alignof(T)==0);
}
}  // namespace native_verification_state_detail
}  // namespace scratchbird::engine::internal_api
