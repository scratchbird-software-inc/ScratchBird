// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string_view>

#if defined(SCRATCHBIRD_ENABLE_TEST_CRASH_INJECTION) && !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace scratchbird::core::platform {

// SEARCH_KEY: SB_WHOLE_STORE_REAL_DML_CRASH_BOUNDARY_REGISTRY
// These are test-only process termination points. They are deliberately
// code-owned so the full-route recovery gate and the engine cannot silently
// disagree about which durable transitions require kill/restart coverage.
inline constexpr std::array<std::string_view, 11>
    kWholeStoreRealDmlCrashBoundaries{{
        "allocation",
        "partial_page_write",
        "page_sync",
        "directory_mutation",
        "index_write",
        "index_sync",
        "catalog_trigger_effect",
        "mutation_manifest_publication",
        "transaction_inventory_publication",
        "final_sync",
        "recovery_cleanup",
    }};

inline bool IsWholeStoreRealDmlCrashBoundary(std::string_view point) {
  for (const auto registered : kWholeStoreRealDmlCrashBoundaries) {
    if (registered == point) {
      return true;
    }
  }
  return false;
}

inline thread_local bool gWholeStoreRealDmlCommitCrashScope = false;
inline thread_local std::uint64_t gWholeStoreRealDmlTransactionId = 0;
inline std::atomic<std::uint64_t> gWholeStoreRealDmlArmedTransactionId{0};

inline void ArmWholeStoreRealDmlCrashForObservedValue(
    std::uint64_t local_transaction_id,
    std::string_view observed_value) {
#if defined(SCRATCHBIRD_ENABLE_TEST_CRASH_INJECTION) && !defined(_WIN32)
  const char* trigger =
      std::getenv("SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_TRIGGER_VALUE");
  if (local_transaction_id != 0 && trigger != nullptr && *trigger != '\0' &&
      std::string_view(trigger) == observed_value) {
    gWholeStoreRealDmlArmedTransactionId.store(local_transaction_id,
                                               std::memory_order_release);
  }
#else
  (void)local_transaction_id;
  (void)observed_value;
#endif
}

class WholeStoreRealDmlCommitCrashScope {
 public:
  explicit WholeStoreRealDmlCommitCrashScope(
      bool enabled = true, std::uint64_t local_transaction_id = 0)
      : previous_(gWholeStoreRealDmlCommitCrashScope),
        previous_transaction_id_(gWholeStoreRealDmlTransactionId) {
    if (enabled) {
      gWholeStoreRealDmlCommitCrashScope = true;
      gWholeStoreRealDmlTransactionId = local_transaction_id;
    }
  }

  ~WholeStoreRealDmlCommitCrashScope() {
    gWholeStoreRealDmlCommitCrashScope = previous_;
    gWholeStoreRealDmlTransactionId = previous_transaction_id_;
  }

  WholeStoreRealDmlCommitCrashScope(
      const WholeStoreRealDmlCommitCrashScope&) = delete;
  WholeStoreRealDmlCommitCrashScope& operator=(
      const WholeStoreRealDmlCommitCrashScope&) = delete;

 private:
  bool previous_{false};
  std::uint64_t previous_transaction_id_{0};
};

// A crash requires all three test-only environment values. Ordinary builds
// compile this function as a no-op; diagnostic builds must additionally opt in
// with SB_ENABLE_TEST_CRASH_INJECTION. The marker is flushed before SIGKILL so
// the parent harness can distinguish a reached boundary from an unrelated
// server failure. No exception or parser-owned finality substitute is used.
inline void MaybeCrashAtWholeStoreRealDmlBoundary(
    std::string_view point,
    bool require_commit_scope = false) {
#if defined(SCRATCHBIRD_ENABLE_TEST_CRASH_INJECTION) && !defined(_WIN32)
  if (!IsWholeStoreRealDmlCrashBoundary(point) ||
      (require_commit_scope && !gWholeStoreRealDmlCommitCrashScope)) {
    return;
  }
  const char* armed =
      std::getenv("SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_ARM");
  const char* requested =
      std::getenv("SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_POINT");
  const char* marker =
      std::getenv("SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_MARKER");
  if (armed == nullptr || requested == nullptr || marker == nullptr ||
      std::string_view(armed) != "issue6-real-dml-route" ||
      std::string_view(requested) != point || *marker == '\0') {
    return;
  }
  const std::uint64_t armed_transaction_id =
      gWholeStoreRealDmlArmedTransactionId.load(std::memory_order_acquire);
  if (point != "recovery_cleanup" &&
      (armed_transaction_id == 0 ||
       (require_commit_scope &&
        gWholeStoreRealDmlTransactionId != armed_transaction_id))) {
    return;
  }

  std::ofstream output(marker, std::ios::binary | std::ios::trunc);
  if (!output) {
    return;
  }
  output << "boundary=" << point << '\n'
         << "pid=" << static_cast<unsigned long long>(::getpid()) << '\n'
         << "local_transaction_id=" << armed_transaction_id << '\n'
         << "authority=durable_mga_transaction_inventory\n"
         << "parser_finality=false\n"
         << "wal_authority=false\n";
  output.flush();
  output.close();
  if (!output) {
    return;
  }
  ::kill(::getpid(), SIGKILL);
  std::_Exit(137);
#else
  (void)point;
  (void)require_commit_scope;
#endif
}

}  // namespace scratchbird::core::platform
