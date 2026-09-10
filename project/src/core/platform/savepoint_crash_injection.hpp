// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "whole_store_crash_injection.hpp"

namespace scratchbird::core::platform {
inline constexpr std::array<std::string_view, 12> kMgaSavepointCrashBoundaries{{
    "rollback_before_marker", "rollback_after_marker", "rollback_before_coordinator_evidence",
    "update_after_intent", "update_after_prepared", "update_after_barrier",
    "delete_after_native_marker", "delete_after_intent", "delete_after_rows",
    "delete_after_indexes", "delete_after_prepared", "delete_after_barrier"}};

// Disabled in ordinary builds. The test harness must select the exact point,
// arm the route, supply its own marker path AND insert the configured trigger
// value into the same transaction. No parser state or marker decides finality.
inline void MaybeCrashAtMgaSavepointBoundary(std::string_view point, std::uint64_t transaction) {
#if defined(SCRATCHBIRD_ENABLE_TEST_CRASH_INJECTION) && !defined(_WIN32)
  bool registered = false;
  for (const auto boundary : kMgaSavepointCrashBoundaries) registered |= point == boundary;
  if (!registered || transaction == 0 ||
      gWholeStoreRealDmlArmedTransactionId.load(std::memory_order_acquire) != transaction) return;
  const char* arm = std::getenv("SCRATCHBIRD_TEST_SAVEPOINT_CRASH_ARM");
  const char* requested = std::getenv("SCRATCHBIRD_TEST_SAVEPOINT_CRASH_POINT");
  const char* path = std::getenv("SCRATCHBIRD_TEST_SAVEPOINT_CRASH_MARKER");
  if (!arm || std::string_view(arm) != "issue8-savepoint-route" || !requested ||
      std::string_view(requested) != point || !path || !*path) return;
  std::ofstream marker(path, std::ios::binary | std::ios::trunc);
  marker << "boundary=" << point << '\n' << "local_transaction_id=" << transaction << '\n'
         << "pid=" << ::getpid() << '\n' << "authority=durable_mga_transaction_inventory\n";
  marker.flush();
  marker.close();
  if (!marker) return;
  ::kill(::getpid(), SIGKILL);
  std::_Exit(137);
#else
  (void)point; (void)transaction;
#endif
}
}  // namespace scratchbird::core::platform
