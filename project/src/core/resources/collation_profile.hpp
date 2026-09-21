// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>

namespace scratchbird::core::resources {
// Durable numeric identities (Core12 RESOURCE-COLLATION-PROFILE-001).
// An imported description without a recipe is metadata, never an executable
// binary or UCA fallback. Its implementation remains a required closure item.
enum class CollationProfile : std::uint64_t {
  unbound = 0,
  utf8_binary = 1,
  uca17_root_primary = 2,
  uca17_root_secondary = 3,
  uca17_root_tertiary = 4,
  uca17_root_identical = 5
};
constexpr bool ValidCollationProfile(CollationProfile profile, bool case_insensitive,
                                     bool accent_insensitive) noexcept {
  switch (profile) {
    case CollationProfile::unbound: return true;
    case CollationProfile::utf8_binary:
    case CollationProfile::uca17_root_tertiary:
    case CollationProfile::uca17_root_identical:
      return !case_insensitive && !accent_insensitive;
    case CollationProfile::uca17_root_primary:
      return case_insensitive && accent_insensitive;
    case CollationProfile::uca17_root_secondary:
      return case_insensitive && !accent_insensitive;
  }
  return false;
}
constexpr bool UsesUnicodeRoot(CollationProfile profile) noexcept {
  return profile >= CollationProfile::uca17_root_primary &&
         profile <= CollationProfile::uca17_root_identical;
}
} // namespace scratchbird::core::resources
