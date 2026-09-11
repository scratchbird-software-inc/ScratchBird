// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdlib>
#include <string_view>
#include "dml/test_optimization_profile.hpp"
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
#include <fstream>
#include <mutex>
#endif

namespace scratchbird::engine::internal_api::dml {

// Test instrumentation only. This selects an existing INSERT executor route,
// not transaction, visibility, constraint, security, or durability policy.
// The staged route is NOT yet an optimization-disabled whole-DML reference:
// scoped loaders, physical probes and hot append remain common dependencies.
enum class TestInsertRoute { optimized, staged, invalid };

constexpr TestInsertRoute ParseTestInsertRoute(const char* value) noexcept {
  if (value == nullptr) return TestInsertRoute::optimized;
  const std::string_view name(value);
  if (name == "optimized") return TestInsertRoute::optimized;
  if (name == "staged") return TestInsertRoute::staged;
  return TestInsertRoute::invalid;
}

inline TestInsertRoute SelectedTestInsertRoute() noexcept {
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
  // Process-local, immutable after first use. A client/parser cannot select or
  // change the route through SQL, options, UUID descriptors or wire packets.
  static const auto selected =
      ParseTestInsertRoute(std::getenv("SCRATCHBIRD_TEST_INSERT_ROUTE"));
  return selected;
#else
  return TestInsertRoute::optimized;
#endif
}

inline bool TestInsertDirectRouteAllowed() noexcept {
  return SelectedTestInsertRoute() == TestInsertRoute::optimized && !TestScanScalarProfile();
}

// Coverage evidence, not a success/finality oracle. Called only at actual
// successful executor exits. Missing/unwritable evidence makes the test fail;
// tracing itself must never change the result of a durable mutation.
inline void RecordTestInsertRouteExecution(TestInsertRoute route,
                                           unsigned long long rows,
                                           unsigned long long local_transaction_id) noexcept {
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
  try {
    const char* path = std::getenv("SCRATCHBIRD_TEST_INSERT_ROUTE_EVIDENCE");
    if (path == nullptr || *path == '\0') return;
    static std::mutex mutex;
    const std::lock_guard<std::mutex> guard(mutex);
    std::ofstream out(path, std::ios::app);
    out << (route == TestInsertRoute::optimized ? "direct" : "staged")
        << '\t' << rows << '\t' << local_transaction_id << '\n';
  } catch (...) {
    // Evidence cannot acquire mutation or transaction authority.
  }
#else
  (void)route;
  (void)rows;
  (void)local_transaction_id;
#endif
}

}  // namespace scratchbird::engine::internal_api::dml
