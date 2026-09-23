// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/query/canonical_sample_descriptor_registry.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  a::CanonicalSeededSampleRequest request;
  request.sample_basis_points = 2500;
  request.repeatable_seed = 17;
  request.repeatable_seed_is_bound = true;
  const auto identity = a::ResolveCanonicalSampleDescriptor(request);
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(identity));
  Check(sizeof(identity) == 16);
  std::vector<std::thread> threads;
  for (unsigned n = 0; n < 8; ++n) threads.emplace_back([&] {
    Check(a::ResolveCanonicalSampleDescriptor(request) == identity);
  });
  for (auto& thread : threads) thread.join();
  auto changed = request;
  changed.input_row_count = 1000;
  changed.maximum_input_row_count = 2000;
  Check(a::ResolveCanonicalSampleDescriptor(changed) == identity);
  changed.repeatable_seed++;
  const auto other = a::ResolveCanonicalSampleDescriptor(changed);
  Check(other != identity && scratchbird::core::uuid::IsEngineIdentityUuid(other));
  changed.repeatable_seed_is_bound = false;
  Check(a::ResolveCanonicalSampleDescriptor(changed).is_nil());
}
