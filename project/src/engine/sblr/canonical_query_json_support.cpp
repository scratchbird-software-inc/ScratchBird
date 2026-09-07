// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_json_support.hpp"

#include "canonical_query_runtime_memory_support.hpp"

#include <cctype>
#include <optional>
#include <ranges>

namespace scratchbird::engine::sblr {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_JSON_SUPPORT_AUTHORITY
namespace {

bool ScanCanonicalJsonStringEnd(const std::string_view json,
                                const std::size_t start,
                                std::size_t* end) {
  if (end == nullptr || start >= json.size() || json[start] != '"') {
    return false;
  }
  bool escaped = false;
  for (std::size_t offset = start + 1; offset < json.size(); ++offset) {
    const auto byte = json[offset];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (byte == '\\') {
      escaped = true;
      continue;
    }
    if (byte == '"') {
      *end = offset + 1;
      return true;
    }
  }
  return false;
}

bool ScanCanonicalJsonValueEnd(const std::string_view json,
                               const std::size_t start,
                               std::size_t* end) {
  if (end == nullptr || start >= json.size()) return false;
  if (json[start] == '"') return ScanCanonicalJsonStringEnd(json, start, end);
  if (json[start] != '{' && json[start] != '[') {
    auto offset = start;
    while (offset < json.size() && json[offset] != ',' &&
           json[offset] != ']' && json[offset] != '}') {
      ++offset;
    }
    if (offset == start) return false;
    *end = offset;
    return true;
  }
  std::vector<char> closing;
  closing.push_back(json[start] == '{' ? '}' : ']');
  bool in_string = false;
  bool escaped = false;
  for (std::size_t offset = start + 1; offset < json.size(); ++offset) {
    const auto byte = json[offset];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
      continue;
    }
    if (byte == '"') {
      in_string = true;
    } else if (byte == '{') {
      closing.push_back('}');
    } else if (byte == '[') {
      closing.push_back(']');
    } else if (byte == '}' || byte == ']') {
      if (closing.empty() || closing.back() != byte) return false;
      closing.pop_back();
      if (closing.empty()) {
        *end = offset + 1;
        return true;
      }
    }
  }
  return false;
}

}  // namespace

CanonicalDocumentWildcardExpansion ExpandCanonicalDocumentWildcard(
    const std::string_view canonical_json,
    const std::string_view path,
    const std::size_t maximum_rows,
    const std::uint64_t maximum_memory_bytes) {
  CanonicalDocumentWildcardExpansion result;
  constexpr std::string_view kPrefix = "$.";
  constexpr std::string_view kSuffix = "[*]";
  if (!path.starts_with(kPrefix) || !path.ends_with(kSuffix) ||
      path.size() <= kPrefix.size() + kSuffix.size()) {
    result.detail = "DOCUMENT_UNNEST path is not an exact object wildcard";
    return result;
  }
  const auto key = path.substr(
      kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
  if (!(std::isalpha(static_cast<unsigned char>(key.front())) ||
        key.front() == '_') ||
      !std::ranges::all_of(key, [](const unsigned char byte) {
        return std::isalnum(byte) || byte == '_';
      })) {
    result.detail = "DOCUMENT_UNNEST wildcard key is not canonical";
    return result;
  }
  if (canonical_json.size() < 2 || canonical_json.front() != '{' ||
      canonical_json.back() != '}') {
    result.detail = "DOCUMENT_UNNEST input is not a JSON object";
    return result;
  }

  std::size_t offset = 1;
  std::optional<std::string_view> array;
  while (offset < canonical_json.size() - 1) {
    if (canonical_json[offset] != '"') {
      result.detail = "canonical JSON object key is malformed";
      return result;
    }
    std::size_t key_end = 0;
    if (!ScanCanonicalJsonStringEnd(canonical_json, offset, &key_end) ||
        key_end >= canonical_json.size() || canonical_json[key_end] != ':') {
      result.detail = "canonical JSON object member is malformed";
      return result;
    }
    const auto encoded_key =
        canonical_json.substr(offset + 1, key_end - offset - 2);
    const auto value_start = key_end + 1;
    std::size_t value_end = 0;
    if (!ScanCanonicalJsonValueEnd(canonical_json, value_start, &value_end)) {
      result.detail = "canonical JSON object value is malformed";
      return result;
    }
    if (encoded_key == key) {
      if (array.has_value()) {
        result.detail = "DOCUMENT_UNNEST wildcard key is ambiguous";
        return result;
      }
      array = canonical_json.substr(value_start, value_end - value_start);
    }
    offset = value_end;
    if (offset == canonical_json.size() - 1) break;
    if (canonical_json[offset] != ',') {
      result.detail = "canonical JSON object delimiter is malformed";
      return result;
    }
    ++offset;
  }
  if (!array.has_value()) {
    result.ok = true;
    return result;
  }
  result.path_present = true;
  if (array->size() < 2 || array->front() != '[' || array->back() != ']') {
    result.detail = "DOCUMENT_UNNEST wildcard target is not an array";
    return result;
  }
  std::uint64_t retained_bytes = sizeof(CanonicalDocumentWildcardExpansion);
  offset = 1;
  while (offset < array->size() - 1) {
    std::size_t value_end = 0;
    if (!ScanCanonicalJsonValueEnd(*array, offset, &value_end) ||
        value_end > array->size() - 1 ||
        result.elements.size() >= maximum_rows) {
      result.detail = result.elements.size() >= maximum_rows
                          ? "DOCUMENT_UNNEST row bound was exceeded"
                          : "DOCUMENT_UNNEST array element is malformed";
      return result;
    }
    const auto element = array->substr(offset, value_end - offset);
    std::uint64_t next_bytes = 0;
    if (!CheckedAdd(retained_bytes, sizeof(std::string), &next_bytes) ||
        !CheckedAdd(next_bytes, element.size(), &retained_bytes) ||
        retained_bytes > maximum_memory_bytes) {
      result.detail = "DOCUMENT_UNNEST memory bound was exceeded";
      return result;
    }
    result.elements.emplace_back(element);
    offset = value_end;
    if (offset == array->size() - 1) break;
    if ((*array)[offset] != ',') {
      result.detail = "DOCUMENT_UNNEST array delimiter is malformed";
      return result;
    }
    ++offset;
  }
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::sblr
