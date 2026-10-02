// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "memory.hpp"
#include <optional>
#include <span>

namespace scratchbird::core::concurrency {

// SEARCH_KEY: SB_LOCAL_LATCH_VICTIM_PREFERENCE
// Pure preference ordering for an already classified, purely local latch-only
// cycle. Not graph detection, eligibility admission or a resolution receipt.
// The live graph owner supplies current eligible candidates and revalidates
// graph/phase/authority before resolution. In particular, class5 here cannot
// prove restricted mode or authorize cancellation of recovery root repair.
using LocalLatchVictimTaskId = memory::MemoryBinaryUuid;
enum class LocalLatchVictimSafetyClass : std::uint8_t {
  non_recovery_background_before_durable = 1,
  cancellable_user_before_publication = 2,
  maintenance_before_destructive = 3,
  user_after_diagnostic_safe_rollback_point = 4,
  restricted_recovery_root_repair = 5
};
struct LocalLatchVictimCandidate {
  LocalLatchVictimTaskId task_id{};
  LocalLatchVictimSafetyClass safety_class{};
  bool operator==(const LocalLatchVictimCandidate&) const = default;
};
enum class LocalLatchVictimPreference { invalid, equivalent, left, right };

constexpr bool LocalLatchVictimCandidateValid(const LocalLatchVictimCandidate& candidate) noexcept {
  return memory::MemorySystemUuidValid(candidate.task_id) &&
      candidate.safety_class >= LocalLatchVictimSafetyClass::non_recovery_background_before_durable &&
      candidate.safety_class <= LocalLatchVictimSafetyClass::restricted_recovery_root_repair;
}
constexpr LocalLatchVictimPreference CompareLocalLatchVictimPreference(
    const LocalLatchVictimCandidate& left, const LocalLatchVictimCandidate& right) noexcept {
  using P = LocalLatchVictimPreference;
  if (!LocalLatchVictimCandidateValid(left) || !LocalLatchVictimCandidateValid(right)) return P::invalid;
  if (left.task_id == right.task_id)
    return left.safety_class == right.safety_class ? P::equivalent : P::invalid;
  if (left.safety_class != right.safety_class)
    return left.safety_class < right.safety_class ? P::left : P::right;
  // std::array<uint8_t,16> compares unsigned canonical bytes in index order,
  // independent of host endianness. No text conversion or age inference.
  return left.task_id > right.task_id ? P::left : P::right;
}

enum class LocalLatchVictimSelectionStatus { invalid, exhausted, empty, selected };
struct LocalLatchVictimSelection {
  LocalLatchVictimSelectionStatus status = LocalLatchVictimSelectionStatus::invalid;
  std::optional<LocalLatchVictimCandidate> candidate;
};
// Borrow immutable observations for this call. max_candidates is the caller's
// admitted input bound, not a resource grant issued by this helper. No heap,
// graph replacement or hidden ownership: O(n^2) bounded duplicate validation,
// O(n) preference reduction, constant extra space. Invalid input never returns
// a partial candidate. Equivalent duplicates do not change the preference.
inline LocalLatchVictimSelection SelectLocalLatchVictimPreference(
    std::span<const LocalLatchVictimCandidate> candidates, std::uint32_t max_candidates) noexcept {
  using S = LocalLatchVictimSelectionStatus;
  if (!max_candidates) return {};
  if (candidates.size() > max_candidates) return {S::exhausted, {}};
  LocalLatchVictimSelection result{S::empty, {}};
  for (std::size_t i=0; i<candidates.size(); ++i) {
    const auto& current = candidates[i];
    if (!LocalLatchVictimCandidateValid(current)) return {};
    for (std::size_t previous=0; previous<i; ++previous)
      if (current.task_id == candidates[previous].task_id &&
          current.safety_class != candidates[previous].safety_class) return {};
    if (!result.candidate ||
        CompareLocalLatchVictimPreference(current, *result.candidate) == LocalLatchVictimPreference::left)
      result = {S::selected, current};
  }
  return result;
}
} // namespace scratchbird::core::concurrency
