// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdlib>
#include <string_view>
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
#include <fstream>
#include <mutex>
#endif

namespace scratchbird::engine::internal_api::dml {

// Independent test interventions in advisory optimizations. These do not
// change transaction, snapshot, constraint or publication policy. In
// particular, relation_rows is a canonical TABLE-scoped load, never CrudState.
enum class TestOptimizationProfile {
  normal, cold, uncached, evicted, publication_evicted, relation_rows, publish_cache, scan_scalar, invalid
};

constexpr TestOptimizationProfile ParseTestOptimizationProfile(const char* value) noexcept {
  if (value == nullptr) return TestOptimizationProfile::normal;
  const std::string_view name(value);
  if (name == "normal") return TestOptimizationProfile::normal;
  if (name == "cold") return TestOptimizationProfile::cold;
  if (name == "uncached") return TestOptimizationProfile::uncached;
  if (name == "evicted") return TestOptimizationProfile::evicted;
  if (name == "publication_evicted") return TestOptimizationProfile::publication_evicted;
  if (name == "relation_rows") return TestOptimizationProfile::relation_rows;
  if (name == "publish_cache") return TestOptimizationProfile::publish_cache;
  if (name == "scan_scalar") return TestOptimizationProfile::scan_scalar;
  return TestOptimizationProfile::invalid;
}

inline TestOptimizationProfile SelectedTestOptimizationProfile() noexcept {
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
  static const auto profile = ParseTestOptimizationProfile(
      std::getenv("SCRATCHBIRD_TEST_DML_OPTIMIZATION"));
  return profile;
#else
  return TestOptimizationProfile::normal;
#endif
}

inline bool TestScanScalarProfile() noexcept {
  return SelectedTestOptimizationProfile() == TestOptimizationProfile::scan_scalar;
}

// Actual branch evidence only; neither success nor MGA finality authority.
inline void RecordTestOptimizationBranch(std::string_view branch) noexcept {
#if defined(SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION) && \
    SCRATCHBIRD_ENABLE_TEST_DML_ROUTE_SELECTION
  try {
    const char* path = std::getenv("SCRATCHBIRD_TEST_DML_OPTIMIZATION_EVIDENCE");
    if (path == nullptr || *path == '\0') return;
    static std::mutex mutex;
    const std::lock_guard<std::mutex> guard(mutex);
    std::ofstream out(path, std::ios::app);
    out << branch << '\n';
  } catch (...) {
    // Test output cannot change a mutation result.
  }
#else
  (void)branch;
#endif
}

}  // namespace scratchbird::engine::internal_api::dml
