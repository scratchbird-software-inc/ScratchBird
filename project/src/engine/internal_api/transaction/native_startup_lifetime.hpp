// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "time.hpp"
#include <optional>

namespace scratchbird::engine::internal_api {
enum class NativeStartupLifetimeError {
  none,invalid_request,clock_failure,clock_regressed,transaction_expired,snapshot_expired
};
// Arithmetic only. All samples must come from the same native monotonic clock;
// wall time, UUID time and a copied observation cannot admit executable work.
inline NativeStartupLifetimeError CheckNativeStartupLifetime(
    core::platform::u64 started,core::platform::u64 last,
    std::optional<core::platform::u64> snapshot,core::platform::u64 now,
    core::platform::u64 transaction_microseconds,core::platform::u64 snapshot_microseconds) noexcept {
  using E=NativeStartupLifetimeError;
  if(!transaction_microseconds||!snapshot_microseconds||last<started||
      (snapshot&&(*snapshot<started||*snapshot>last)))return E::invalid_request;
  if(now<last)return E::clock_regressed;
  // Divide elapsed nanoseconds, rather than multiplying the policy limit:
  // every finite uint64 microsecond limit remains safe without saturation.
  if((now-started)/1000>=transaction_microseconds)return E::transaction_expired;
  if(snapshot&&(now-*snapshot)/1000>=snapshot_microseconds)return E::snapshot_expired;
  return E::none;
}
} // namespace scratchbird::engine::internal_api
