// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/test_insert_route.hpp"
#include "dml/test_optimization_profile.hpp"

#include <iostream>
#include <string_view>

namespace dml = scratchbird::engine::internal_api::dml;
using Route = dml::TestInsertRoute;

static_assert(dml::ParseTestInsertRoute(nullptr) == Route::optimized);
static_assert(dml::ParseTestInsertRoute("optimized") == Route::optimized);
static_assert(dml::ParseTestInsertRoute("staged") == Route::staged);
static_assert(dml::ParseTestInsertRoute("") == Route::invalid);
static_assert(dml::ParseTestInsertRoute("STAGED") == Route::invalid);
static_assert(dml::ParseTestInsertRoute("staged ") == Route::invalid);
static_assert(dml::ParseTestInsertRoute("reference") == Route::invalid);
using Optimization = dml::TestOptimizationProfile;
static_assert(dml::ParseTestOptimizationProfile(nullptr) == Optimization::normal);
static_assert(dml::ParseTestOptimizationProfile("") == Optimization::invalid);
static_assert(dml::ParseTestOptimizationProfile("COLD") == Optimization::invalid);
static_assert(dml::ParseTestOptimizationProfile("cold ") == Optimization::invalid);

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  const auto optimization = dml::SelectedTestOptimizationProfile();
  if (argc == 3 && optimization != dml::ParseTestOptimizationProfile(argv[2])) return 1;
  const std::string_view expected(argv[1]);
  const auto selected = dml::SelectedTestInsertRoute();
  const auto name = selected == Route::optimized ? "optimized" :
                    selected == Route::staged ? "staged" : "invalid";
  if (expected != name ||
      dml::TestInsertDirectRouteAllowed() != (selected == Route::optimized && !dml::TestScanScalarProfile())) {
    std::cerr << "insert route mismatch: expected=" << expected
              << " actual=" << name << '\n';
    return 1;
  }
  // Changing the environment after engine admission must not switch routes.
#ifdef _WIN32
  _putenv_s("SCRATCHBIRD_TEST_INSERT_ROUTE", "changed-after-admission");
  _putenv_s("SCRATCHBIRD_TEST_DML_OPTIMIZATION", "changed-after-admission");
#else
  setenv("SCRATCHBIRD_TEST_INSERT_ROUTE", "changed-after-admission", 1);
  setenv("SCRATCHBIRD_TEST_DML_OPTIMIZATION", "changed-after-admission", 1);
#endif
  if (dml::SelectedTestInsertRoute() != selected) return 1;
  if (dml::SelectedTestOptimizationProfile() != optimization) return 1;
  std::cout << "insert_route_control=" << name << ":passed\n";
}
