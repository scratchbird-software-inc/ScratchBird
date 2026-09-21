// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "unicode_normalization.hpp"
#include "../datatypes/canonical_utf8.hpp"
#include "../hash/hash_digest.hpp"

#include <algorithm>
#include <charconv>
#include <new>
#include <stdexcept>

namespace scratchbird::core::resources {
namespace {
using Status = UnicodeNormalizationStatus;
constexpr std::size_t kUnicodeData17Bytes = 2198209;
constexpr hash::Digest256 kUnicodeData17Sha256{
  0x2e,0x1e,0xfc,0x1d,0xcb,0x59,0xc5,0x75,0xee,0xdf,0x5c,0xca,0xe6,0x0f,0x95,0x22,
  0x9f,0x70,0x6e,0xe6,0xd0,0x31,0x83,0x52,0x47,0xd8,0x43,0xc1,0x1d,0x96,0x47,0x0c};

bool Number(std::string_view text, std::uint32_t& value, int base) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool Scalar(std::uint32_t value) {
  return value <= 0x10ffff && (value < 0xd800 || value > 0xdfff);
}
unsigned Width(std::uint32_t value) {
  return value <= 0x7f ? 1 : value <= 0x7ff ? 2 : value <= 0xffff ? 3 : 4;
}
void Append(std::uint32_t scalar, std::string& out) {
  const auto width = Width(scalar);
  if (width == 1) { out.push_back(static_cast<char>(scalar)); return; }
  out.push_back(static_cast<char>((width == 2 ? 0xc0 : width == 3 ? 0xe0 : 0xf0) |
      (scalar >> (6 * (width - 1)))));
  for (unsigned i = width - 1; i > 0; --i)
    out.push_back(static_cast<char>(0x80 | ((scalar >> (6 * (i - 1))) & 0x3f)));
}
} // namespace

UnicodeNormalizationStatus UnicodeNormalizationData::Load17(
    std::string_view data, std::shared_ptr<const UnicodeNormalizationData>* output) noexcept {
  if (!output) return Status::invalid_argument;
  if (data.size() != kUnicodeData17Bytes) return Status::invalid_resource;
  try {
    const auto digest = hash::ComputeSha256Digest(
        reinterpret_cast<const platform::byte*>(data.data()), data.size());
    if (!digest.ok() || digest.digest != kUnicodeData17Sha256) return Status::invalid_resource;
    std::shared_ptr<UnicodeNormalizationData> staged(new UnicodeNormalizationData);
    // Only nonzero combining classes and canonical decompositions need entries.
    // Unassigned, private-use and noncharacters have class zero and no mapping.
    std::uint32_t previous = 0;
    bool first = true;
    bool in_range = false;
    std::uint32_t range_start = 0;
    std::string_view range_name;
    while (!data.empty()) {
      const auto newline = data.find('\n');
      if (newline == std::string_view::npos) return Status::invalid_resource;
      auto line = data.substr(0, newline);
      data.remove_prefix(newline + 1);
      std::array<std::string_view, 15> fields;
      for (std::size_t i = 0; i < fields.size() - 1; ++i) {
        const auto separator = line.find(';');
        if (separator == std::string_view::npos) return Status::invalid_resource;
        fields[i] = line.substr(0, separator); line.remove_prefix(separator + 1);
      }
      if (line.find(';') != std::string_view::npos) return Status::invalid_resource;
      fields.back() = line;
      std::uint32_t scalar = 0, combining_class = 0;
      if (!Number(fields[0], scalar, 16) || scalar > 0x10ffff ||
          (!first && scalar <= previous) ||
          !Number(fields[3], combining_class, 10) || combining_class > 255)
        return Status::invalid_resource;
      first = false; previous = scalar;
      const auto first_marker = fields[1].find(", First>");
      const auto last_marker = fields[1].find(", Last>");
      if (first_marker != std::string_view::npos) {
        if (in_range) return Status::invalid_resource;
        in_range = true; range_start = scalar; range_name = fields[1].substr(0, first_marker);
      } else {
        if (last_marker != std::string_view::npos) {
          if (!in_range || range_name != fields[1].substr(0, last_marker)) return Status::invalid_resource;
          in_range = false;
        } else {
          if (in_range) return Status::invalid_resource;
          range_start = scalar;
        }
        auto& ranges = staged->assigned_ranges_;
        if (!ranges.empty() && ranges.back()[1] + 1 == range_start) ranges.back()[1] = scalar;
        else ranges.push_back({range_start, scalar});
      }
      Entry entry{scalar, static_cast<std::uint8_t>(combining_class)};
      auto mapping = fields[5];
      // Tagged mappings are compatibility-only. Range records in this pinned
      // UCD have neither nonzero CCC nor canonical mappings (Hangul is below).
      if (!mapping.empty() && mapping.front() != '<') {
        while (!mapping.empty()) {
          const auto separator = mapping.find(' ');
          std::uint32_t part = 0;
          if (entry.decomposition_size == entry.decomposition.size() ||
              !Number(mapping.substr(0, separator), part, 16) || !Scalar(part))
            return Status::invalid_resource;
          entry.decomposition[entry.decomposition_size++] = part;
          if (separator == std::string_view::npos) break;
          mapping.remove_prefix(separator + 1);
        }
      }
      if (entry.combining_class || entry.decomposition_size) {
        if (!Scalar(scalar) || fields[1].find(", First>") != std::string_view::npos ||
            fields[1].find(", Last>") != std::string_view::npos) return Status::invalid_resource;
        staged->entries_.push_back(entry);
      }
    }
    if (in_range) return Status::invalid_resource;
    // Check graph termination and the full expansion budget before publication.
    // This is defense in depth; a modified/truncated artifact fails its digest.
    std::vector<std::uint32_t> expanded;
    for (const auto& entry : staged->entries_) {
      std::size_t bytes = 0;
      expanded.clear();
      if (!staged->Decompose(entry.scalar, 128, bytes, expanded, 0)) return Status::invalid_resource;
    }
    *output = std::move(staged);
    return Status::ok;
  } catch (const std::bad_alloc&) { return Status::allocation_failure; }
    catch (const std::length_error&) { return Status::allocation_failure; }
}

const UnicodeNormalizationData::Entry* UnicodeNormalizationData::Find(std::uint32_t scalar) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), scalar,
      [](const Entry& entry, std::uint32_t key) { return entry.scalar < key; });
  return it != entries_.end() && it->scalar == scalar ? &*it : nullptr;
}

std::uint8_t UnicodeNormalizationData::CombiningClass(std::uint32_t scalar) const noexcept {
  const auto* entry = Find(scalar);
  return entry ? entry->combining_class : 0;
}

bool UnicodeNormalizationData::IsAssigned(std::uint32_t scalar) const noexcept {
  if (!Scalar(scalar)) return false;
  const auto it = std::lower_bound(assigned_ranges_.begin(), assigned_ranges_.end(), scalar,
      [](const auto& range, auto cp) { return range[1] < cp; });
  return it != assigned_ranges_.end() && (*it)[0] <= scalar;
}

bool UnicodeNormalizationData::Decompose(std::uint32_t scalar, std::size_t limit,
    std::size_t& bytes, std::vector<std::uint32_t>& out, unsigned depth) const {
  if (depth >= 32) return false;
  // Unicode Hangul syllable decomposition: 19 leading, 21 vowel, 28 trailing
  // choices. A zero trailing index contributes no trailing Jamo.
  if (scalar >= 0xac00 && scalar <= 0xd7a3) {
    const auto index = scalar - 0xac00;
    return Decompose(0x1100 + index / 588, limit, bytes, out, depth + 1) &&
        Decompose(0x1161 + (index % 588) / 28, limit, bytes, out, depth + 1) &&
        (index % 28 == 0 || Decompose(0x11a7 + index % 28, limit, bytes, out, depth + 1));
  }
  const auto* entry = Find(scalar);
  if (entry && entry->decomposition_size) {
    for (unsigned i = 0; i < entry->decomposition_size; ++i)
      if (!Decompose(entry->decomposition[i], limit, bytes, out, depth + 1)) return false;
    return true;
  }
  const auto width = Width(scalar);
  if (bytes > limit || width > limit - bytes) return false;
  out.push_back(scalar); bytes += width;
  return true;
}

UnicodeNormalizationStatus UnicodeNormalizationData::NormalizeNfd(
    std::string_view input, std::size_t limit, std::string* output) const noexcept {
  if (!output) return Status::invalid_argument;
  try {
    std::vector<std::uint32_t> scalars;
    std::size_t offset = 0, bytes = 0;
    while (offset < input.size()) {
      std::uint32_t scalar = 0;
      if (!datatypes::DecodeCanonicalUtf8Scalar(
          reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), &offset, &scalar))
        return Status::invalid_utf8;
      if (!Decompose(scalar, limit, bytes, scalars, 0)) return Status::output_limit;
    }
    // Sort each nonstarter run stably, including a leading run. Equal classes
    // retain input order; a class-zero scalar is always a hard boundary.
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= scalars.size(); ++i) {
      if (i == scalars.size() || CombiningClass(scalars[i]) == 0) {
        std::stable_sort(scalars.begin() + begin, scalars.begin() + i,
            [this](auto lhs, auto rhs) { return CombiningClass(lhs) < CombiningClass(rhs); });
        begin = i + 1;
      }
    }
    std::string staged; staged.reserve(bytes);
    for (const auto scalar : scalars) Append(scalar, staged);
    *output = std::move(staged);
    return Status::ok;
  } catch (const std::bad_alloc&) { return Status::allocation_failure; }
    catch (const std::length_error&) { return Status::allocation_failure; }
}
} // namespace scratchbird::core::resources
