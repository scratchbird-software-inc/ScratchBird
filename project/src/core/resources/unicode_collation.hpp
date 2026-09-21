// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "unicode_normalization.hpp"

namespace scratchbird::core::resources {

enum class UnicodeCollationStrength : std::uint8_t { primary = 1, secondary = 2, tertiary = 3, identical = 4 };
struct UnicodeCollationLimits {
  std::size_t normalized_bytes;
  std::size_t sort_key_bytes;
};

// Untailored DUCET17, non-ignorable variable weighting, forward secondary
// weights. This data is not authority for a donor/locale tailoring. The owner
// must select the exact admitted profile and node/resource cohort separately.
class UnicodeCollationData final {
 public:
  static UnicodeNormalizationStatus Load17(std::string_view allkeys,
      std::string_view properties, std::shared_ptr<const UnicodeNormalizationData> normalization,
      std::shared_ptr<const UnicodeCollationData>* output) noexcept;
  UnicodeNormalizationStatus MakeSortKey(std::string_view input,
      UnicodeCollationStrength strength, UnicodeCollationLimits limits, std::string* output) const noexcept;
  UnicodeNormalizationStatus Compare(std::string_view left, std::string_view right,
      UnicodeCollationStrength strength, UnicodeCollationLimits limits, int* output) const noexcept;

 private:
  struct Element { std::array<std::uint16_t, 3> weights; };
  struct Key {
    std::array<std::uint32_t, 3> scalars{};
    std::uint8_t size{0};
    bool operator<(const Key& other) const noexcept;
  };
  struct Mapping { Key key; std::uint32_t offset, count; std::uint8_t extension_class{0}; };
  struct ImplicitRange { std::uint32_t first, last, origin; std::uint16_t base; };
  UnicodeCollationData() = default;
  const Mapping* Find(const Key& key) const noexcept;
  std::array<Element, 2> Implicit(std::uint32_t scalar) const noexcept;
  std::shared_ptr<const UnicodeNormalizationData> normalization_;
  std::vector<Mapping> mappings_;
  std::vector<Element> elements_;
  std::vector<ImplicitRange> implicit_ranges_;
  std::vector<std::array<std::uint32_t, 2>> unified_ideographs_;
};
} // namespace scratchbird::core::resources
