// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "plan_api.hpp"
#include "uuid.hpp"
#include <map>
#include <mutex>
#include <tuple>
namespace scratchbird::engine::internal_api {
inline EngineUuid ResolveCanonicalSampleDescriptor(
    const CanonicalSeededSampleRequest& request) {
  if ((request.method != CanonicalSeededSampleMethod::kBernoulli &&
       request.method != CanonicalSeededSampleMethod::kSystem) ||
      !request.repeatable_seed_is_bound ||
      request.sample_basis_points > 10000 ||
      (request.method == CanonicalSeededSampleMethod::kSystem &&
       request.system_block_row_count == 0) ||
      (request.method == CanonicalSeededSampleMethod::kBernoulli &&
       request.system_block_row_count != 0)) {
    return {};
  }
  using ProfileKey = std::tuple<CanonicalSeededSampleMethod, std::uint32_t,
                                std::uint64_t, std::size_t>;
  // These are live descriptor bindings, not UUIDs synthesized from profile bits.
  // Retain identities for the process lifetime so already-admitted plans remain
  // bound; refuse new profiles when the registry reaches its fixed bound.
  static std::mutex registry_mutex;
  static std::map<ProfileKey, EngineUuid> descriptors;
  const ProfileKey key{request.method, request.sample_basis_points,
                       request.repeatable_seed, request.system_block_row_count};
  const std::lock_guard lock(registry_mutex);
  if (const auto found = descriptors.find(key); found != descriptors.end())
    return found->second;
  if (descriptors.size() >= 65536) return {};
  const auto issued = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!issued.has_value()) return {};
  return descriptors.emplace(key, *issued).first->second;
}
}
