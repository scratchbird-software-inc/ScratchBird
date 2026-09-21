// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "unicode_collation.hpp"
#include "../datatypes/canonical_utf8.hpp"
#include "../hash/hash_digest.hpp"

#include <algorithm>
#include <charconv>
#include <new>
#include <stdexcept>

namespace scratchbird::core::resources {
namespace {
using Status = UnicodeNormalizationStatus;
std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
bool Hex(std::string_view text, std::uint32_t& value) {
  text = Trim(text);
  if (text.empty()) return false;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
bool Range(std::string_view text, std::uint32_t& first, std::uint32_t& last) {
  const auto split = text.find("..");
  if (!Hex(text.substr(0, split), first)) return false;
  if (split == std::string_view::npos) last = first;
  else if (!Hex(text.substr(split + 2), last)) return false;
  return first <= last && last <= 0x10ffff;
}
bool Pinned(std::string_view bytes, std::size_t size, std::string_view sha) {
  if (bytes.size() != size) return false;
  const auto digest = hash::ComputeSha256Digest(reinterpret_cast<const platform::byte*>(bytes.data()), bytes.size());
  if (!digest.ok() || sha.size() != 64) return false;
  // Compare raw digest bytes: stream-based hex rendering may swallow an
  // allocation exception and misclassify OOM as corrupt resource data.
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < digest.digest.size(); ++i)
    if (digest.digest[i] != 16 * nibble(sha[2 * i]) + nibble(sha[2 * i + 1])) return false;
  return true;
}
std::string_view Line(std::string_view& input) {
  const auto end = input.find('\n');
  auto line = input.substr(0, end);
  input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
  return Trim(line.substr(0, line.find('#')));
}
void Word(std::uint16_t value, std::string& output) {
  output.push_back(static_cast<char>(value >> 8)); output.push_back(static_cast<char>(value & 255));
}
} // namespace

bool UnicodeCollationData::Key::operator<(const Key& other) const noexcept {
  return std::lexicographical_compare(scalars.begin(), scalars.begin() + size,
      other.scalars.begin(), other.scalars.begin() + other.size);
}

UnicodeNormalizationStatus UnicodeCollationData::Load17(std::string_view allkeys,
    std::string_view properties, std::shared_ptr<const UnicodeNormalizationData> normalization,
    std::shared_ptr<const UnicodeCollationData>* output) noexcept {
  if (!output || !normalization) return Status::invalid_argument;
  try {
    if (!Pinned(allkeys, 2304434, "2503d09367c2639a4fb8fd55e81aaacb0d9fb4ea26600333329bd12456b99ecd") ||
        !Pinned(properties, 145465, "130dcddcaadaf071008bdfce1e7743e04fdfbc910886f017d9f9ac931d8c64dd"))
      return Status::invalid_resource;
    std::shared_ptr<UnicodeCollationData> staged(new UnicodeCollationData);
    staged->normalization_ = std::move(normalization);
    staged->mappings_.reserve(39749); staged->elements_.reserve(45860);
    bool version = false;
    while (!allkeys.empty()) {
      auto line = Line(allkeys);
      if (line.empty()) continue;
      if (line == "@version 17.0.0") { if (version) return Status::invalid_resource; version = true; continue; }
      const auto semicolon = line.find(';');
      if (!version || semicolon == std::string_view::npos) return Status::invalid_resource;
      auto source = Trim(line.substr(0, semicolon)), weights = Trim(line.substr(semicolon + 1));
      if (source.substr(0, 17) == "@implicitweights ") {
        ImplicitRange range{}; std::uint32_t base;
        if (!Range(Trim(source.substr(17)), range.first, range.last) || !Hex(weights, base) || base > 65535)
          return Status::invalid_resource;
        range.base = static_cast<std::uint16_t>(base); range.origin = range.first;
        staged->implicit_ranges_.push_back(range); continue;
      }
      Mapping mapping{};
      while (!source.empty()) {
        const auto separator = source.find_first_of(" \t");
        std::uint32_t scalar;
        if (mapping.key.size == mapping.key.scalars.size() || !Hex(source.substr(0, separator), scalar) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return Status::invalid_resource;
        mapping.key.scalars[mapping.key.size++] = scalar;
        source = separator == std::string_view::npos ? std::string_view{} : Trim(source.substr(separator));
      }
      mapping.offset = static_cast<std::uint32_t>(staged->elements_.size());
      while (!weights.empty()) {
        if (weights.size() < 17 || weights[0] != '[' || (weights[1] != '.' && weights[1] != '*') ||
            weights[6] != '.' || weights[11] != '.' || weights[16] != ']') return Status::invalid_resource;
        Element element{};
        for (unsigned i = 0; i < 3; ++i) {
          std::uint32_t value;
          if (!Hex(weights.substr(2 + 5 * i, 4), value)) return Status::invalid_resource;
          element.weights[i] = static_cast<std::uint16_t>(value);
        }
        staged->elements_.push_back(element); ++mapping.count;
        weights = Trim(weights.substr(17));
      }
      if (!mapping.key.size || !mapping.count) return Status::invalid_resource;
      staged->mappings_.push_back(mapping);
    }
    if (!version || staged->mappings_.size() != 39749 || staged->elements_.size() != 45860 ||
        staged->implicit_ranges_.size() != 6) return Status::invalid_resource;
    std::sort(staged->mappings_.begin(), staged->mappings_.end(),
        [](const auto& a, const auto& b) { return a.key < b.key; });
    for (std::size_t i = 1; i < staged->mappings_.size(); ++i)
      if (!(staged->mappings_[i - 1].key < staged->mappings_[i].key)) return Status::invalid_resource;
    for (std::size_t i = 0; i < staged->mappings_.size(); ++i) {
      auto& prefix = staged->mappings_[i];
      for (std::size_t j = i + 1; j < staged->mappings_.size(); ++j) {
        const auto& child = staged->mappings_[j].key;
        if (child.size <= prefix.key.size || !std::equal(prefix.key.scalars.begin(),
            prefix.key.scalars.begin() + prefix.key.size, child.scalars.begin())) break;
        if (child.size == prefix.key.size + 1)
          prefix.extension_class = std::max(prefix.extension_class,
              staged->normalization_->CombiningClass(child.scalars[child.size - 1]));
      }
    }
    for (auto& range : staged->implicit_ranges_)
      for (const auto& other : staged->implicit_ranges_)
        if (range.base == other.base) range.origin = std::min(range.origin, other.first);
    while (!properties.empty()) {
      const auto line = Line(properties); const auto semicolon = line.find(';');
      if (semicolon == std::string_view::npos || Trim(line.substr(semicolon + 1)) != "Unified_Ideograph") continue;
      std::array<std::uint32_t, 2> range{};
      if (!Range(Trim(line.substr(0, semicolon)), range[0], range[1])) return Status::invalid_resource;
      if (!staged->unified_ideographs_.empty() && range[0] <= staged->unified_ideographs_.back()[1])
        return Status::invalid_resource;
      staged->unified_ideographs_.push_back(range);
    }
    if (staged->unified_ideographs_.empty()) return Status::invalid_resource;
    *output = std::move(staged); return Status::ok;
  } catch (const std::bad_alloc&) { return Status::allocation_failure; }
    catch (const std::length_error&) { return Status::allocation_failure; }
}

const UnicodeCollationData::Mapping* UnicodeCollationData::Find(const Key& key) const noexcept {
  const auto it = std::lower_bound(mappings_.begin(), mappings_.end(), key,
      [](const auto& mapping, const auto& candidate) { return mapping.key < candidate; });
  return it != mappings_.end() && !(key < it->key) ? &*it : nullptr;
}

std::array<UnicodeCollationData::Element, 2> UnicodeCollationData::Implicit(std::uint32_t cp) const noexcept {
  for (const auto& range : implicit_ranges_) {
    if (cp >= range.first && cp <= range.last && normalization_->IsAssigned(cp))
      return {Element{{range.base, 0x20, 2}}, Element{{static_cast<std::uint16_t>((cp - range.origin) | 0x8000), 0, 0}}};
  }
  const auto han = std::lower_bound(unified_ideographs_.begin(), unified_ideographs_.end(), cp,
      [](const auto& range, auto value) { return range[1] < value; });
  std::uint16_t base = 0xfbc0;
  if (han != unified_ideographs_.end() && (*han)[0] <= cp)
    base = ((cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff)) ? 0xfb40 : 0xfb80;
  return {Element{{static_cast<std::uint16_t>(base + (cp >> 15)), 0x20, 2}},
          Element{{static_cast<std::uint16_t>((cp & 0x7fff) | 0x8000), 0, 0}}};
}

UnicodeNormalizationStatus UnicodeCollationData::MakeSortKey(std::string_view input,
    UnicodeCollationStrength strength, UnicodeCollationLimits limits, std::string* output) const noexcept {
  const unsigned levels = std::min(3u, static_cast<unsigned>(strength));
  if (!output || !levels || static_cast<unsigned>(strength) > 4) return Status::invalid_argument;
  try {
    std::string normalized;
    const auto status = normalization_->NormalizeNfd(input, limits.normalized_bytes, &normalized);
    if (status != Status::ok) return status;
    std::vector<std::uint32_t> scalars;
    for (std::size_t offset = 0; offset < normalized.size();) {
      std::uint32_t scalar;
      if (!datatypes::DecodeCanonicalUtf8Scalar(reinterpret_cast<const std::uint8_t*>(normalized.data()),
          normalized.size(), &offset, &scalar)) return Status::invalid_utf8;
      scalars.push_back(scalar);
    }
    // Linked positions permit S2.1.3 removal without quadratic string erases.
    std::vector<std::size_t> next(scalars.size());
    std::vector<std::size_t> previous(scalars.size()), next_class(scalars.size());
    std::vector<bool> removed(scalars.size());
    for (std::size_t i = 0; i < next.size(); ++i) {
      next[i] = i + 1; previous[i] = i ? i - 1 : scalars.size();
    }
    for (std::size_t i = scalars.size(); i > 0;) {
      --i; next_class[i] = i + 1;
      const auto ccc = normalization_->CombiningClass(scalars[i]);
      if (ccc && i + 1 < scalars.size() && ccc == normalization_->CombiningClass(scalars[i + 1]))
        next_class[i] = next_class[i + 1];
    }
    std::array<std::string, 3> weights;
    std::size_t used = 2 * (levels - 1);
    if (strength == UnicodeCollationStrength::identical) {
      if (normalized.size() > limits.sort_key_bytes || limits.sort_key_bytes - normalized.size() < used + 2)
        return Status::output_limit;
      used += 2 + normalized.size();
    }
    if (used > limits.sort_key_bytes) return Status::output_limit;
    const auto emit = [&](const Element& element) {
      for (unsigned level = 0; level < levels; ++level) {
        if (!element.weights[level]) continue;
        if (limits.sort_key_bytes - used < 2) return false;
        Word(element.weights[level], weights[level]); used += 2;
      }
      return true;
    };
    for (std::size_t position = 0; position < scalars.size();) {
      Key key; const Mapping* match = nullptr; std::size_t tail = next[position];
      auto scan = position;
      while (scan < scalars.size() && key.size < key.scalars.size()) {
        key.scalars[key.size++] = scalars[scan];
        if (const auto* candidate = Find(key)) { match = candidate; tail = next[scan]; }
        scan = next[scan];
      }
      if (match) {
        key = match->key;
        scan = tail; unsigned blocked_class = 0;
        while (scan < scalars.size() && key.size < key.scalars.size() && match->extension_class > blocked_class) {
          const unsigned ccc = normalization_->CombiningClass(scalars[scan]);
          if (!ccc) break;
          Key candidate = key; candidate.scalars[candidate.size++] = scalars[scan];
          const auto* extended = ccc > blocked_class ? Find(candidate) : nullptr;
          if (extended) {
            match = extended; key = candidate;
            if (scan == tail) tail = next[scan];
            else next[previous[scan]] = next[scan];
            if (next[scan] < scalars.size()) previous[next[scan]] = previous[scan];
            removed[scan] = true; scan = next[scan];
          } else {
            blocked_class = std::max(blocked_class, ccc);
            // Equal-class followers are blocked, so skip their entire run.
            // This also bounds Tibetan nonstarter-prefix lookahead: a long
            // run of U+0F71 must not rescan its full suffix for every scalar.
            scan = next_class[scan];
            while (scan < scalars.size() && removed[scan]) scan = next[scan];
          }
        }
        for (unsigned i = 0; i < match->count; ++i)
          if (!emit(elements_[match->offset + i])) return Status::output_limit;
      } else {
        for (const auto& element : Implicit(scalars[position])) if (!emit(element)) return Status::output_limit;
      }
      position = tail;
    }
    std::string staged; staged.reserve(used);
    for (unsigned level = 0; level < levels; ++level) {
      if (level) Word(0, staged);
      staged += weights[level];
    }
    if (strength == UnicodeCollationStrength::identical) { Word(0, staged); staged += normalized; }
    *output = std::move(staged); return Status::ok;
  } catch (const std::bad_alloc&) { return Status::allocation_failure; }
    catch (const std::length_error&) { return Status::allocation_failure; }
}

UnicodeNormalizationStatus UnicodeCollationData::Compare(std::string_view left, std::string_view right,
    UnicodeCollationStrength strength, UnicodeCollationLimits limits, int* output) const noexcept {
  if (!output) return Status::invalid_argument;
  std::string lhs, rhs;
  auto status = MakeSortKey(left, strength, limits, &lhs);
  if (status != Status::ok) return status;
  status = MakeSortKey(right, strength, limits, &rhs);
  if (status != Status::ok) return status;
  const auto comparison = lhs.compare(rhs);
  *output = (comparison > 0) - (comparison < 0); return Status::ok;
}
} // namespace scratchbird::core::resources
