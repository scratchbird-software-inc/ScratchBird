// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "core/platform/runtime_platform.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
namespace scratchbird::parser::sbsql {
// Exact internal key bytes. No UUID formatting and no delimiter ambiguity.
class BinaryCacheKey {
 public:
  explicit BinaryCacheKey(std::string_view domain) { Text(domain); }
  void Number(std::uint64_t value) {
    for (unsigned i=0; i!=8; ++i) bytes_.push_back(static_cast<char>(value >> (8*i)));
  }
  void Text(std::string_view value) { Number(value.size()); bytes_.append(value); }
  void Uuid(const core::platform::Uuid& value) {
    bytes_.append(reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size());
  }
  void UuidSet(std::vector<core::platform::Uuid> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    Number(values.size());
    for (const auto& value : values) Uuid(value);
  }
  void Strings(const std::vector<std::string>& values) {
    Number(values.size()); for (const auto& value : values) Text(value);
  }
  std::string Finish() && { return std::move(bytes_); }
 private:
  std::string bytes_;
};
}
