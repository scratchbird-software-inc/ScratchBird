// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::resources {

enum class UnicodeNormalizationStatus {
  ok, invalid_argument, invalid_resource, invalid_utf8, output_limit, allocation_failure
};

// Compiled derivative of one node's retained, immutable UnicodeData artifact.
// No file access, process-global cache, host locale or host Unicode dependency.
// A caller must independently admit the owning node/transaction/resource epoch.
class UnicodeNormalizationData final {
 public:
  // This qualified profile is Unicode 17.0.0, matching the initial UCA pack.
  // Exact upstream bytes are required, not a caller-supplied checksum of a
  // partial table. A new resource version needs independent qualification.
  static UnicodeNormalizationStatus Load17(
      std::string_view unicode_data,
      std::shared_ptr<const UnicodeNormalizationData>* output) noexcept;

  // Canonical decomposition only (NFD), not compatibility decomposition or
  // case/accent folding. byte_limit bounds the complete normalized UTF-8 value.
  // All failures preserve output, including allocation failure and aliasing.
  UnicodeNormalizationStatus NormalizeNfd(std::string_view input,
      std::size_t byte_limit, std::string* output) const noexcept;
  std::uint8_t CombiningClass(std::uint32_t scalar) const noexcept;
  bool IsAssigned(std::uint32_t scalar) const noexcept;

 private:
  struct Entry {
    std::uint32_t scalar;
    std::uint8_t combining_class;
    std::array<std::uint32_t, 2> decomposition{};
    std::uint8_t decomposition_size{0};
  };
  UnicodeNormalizationData() = default;
  const Entry* Find(std::uint32_t scalar) const noexcept;
  bool Decompose(std::uint32_t scalar, std::size_t byte_limit,
      std::size_t& bytes, std::vector<std::uint32_t>& output, unsigned depth) const;
  std::vector<Entry> entries_;
  std::vector<std::array<std::uint32_t, 2>> assigned_ranges_;
};

} // namespace scratchbird::core::resources
