// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../core/platform/runtime_platform.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::wire {
struct ManagementRequestV1 {
  std::string operation_key;
  core::platform::Uuid target_uuid;
  std::string mode;
  std::string audit_reason;
  std::uint64_t timeout_ms = 30000;
  bool include_history = false;
};
inline bool ManagementTargetValid(const core::platform::Uuid& id) {
  return id.is_nil() || ((id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80);
}
inline bool DecodeManagementTarget(std::string_view bytes, core::platform::Uuid* out) {
  if (!out || bytes.size() != 16) return false;
  core::platform::Uuid value;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, value.bytes.begin());
  if (!ManagementTargetValid(value)) return false;
  *out = value;
  return true;
}
inline std::string ManagementTargetBytes(const core::platform::Uuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
// UUID-valued mode fields have a fixed extent. Delimiters inside the 16 bytes
// are data, and cannot introduce flags or truncate an identity proof.
inline bool SplitManagementMode(std::string_view mode, std::vector<std::string>* output) {
  if (!output) return false;
  std::vector<std::string> fields;
  std::set<std::string> names;
  std::size_t cursor = 0;
  const auto separator = [](char c) { return c == ';' || c == ',' || c == '\n'; };
  while (cursor < mode.size()) {
    if (separator(mode[cursor])) { ++cursor; continue; }
    const auto start = cursor;
    auto end = mode.find_first_of(";,\n", start);
    if (end == std::string_view::npos) end = mode.size();
    const auto equals = mode.find_first_of(":=", start);
    if (equals < end) {
      const auto key = mode.substr(start, equals-start);
      if (!names.emplace(key).second) return false;
      if (key.ends_with("_uuid")) {
        const auto value_start = equals+1;
        if (mode.size()-value_start < 16) return false;
        core::platform::Uuid identity;
        if (!DecodeManagementTarget(mode.substr(value_start,16), &identity)) return false;
        end = value_start+16;
        if (end < mode.size() && !separator(mode[end])) return false;
      }
    }
    fields.emplace_back(mode.substr(start, end-start));
    cursor = end;
  }
  output->swap(fields);
  return true;
}

inline bool EncodeManagementRequestV1(const ManagementRequestV1& request,
                                      std::vector<std::uint8_t>* output) {
  if (!output || request.operation_key.empty() || !ManagementTargetValid(request.target_uuid)) return false;
  std::vector<std::string> mode_fields;
  if (!SplitManagementMode(request.mode, &mode_fields)) return false;
  const std::array<std::pair<std::string_view, std::string>, 6> fields{{
      {"operation_key", request.operation_key},
      {"target_uuid", ManagementTargetBytes(request.target_uuid)},
      {"mode", request.mode}, {"audit_reason", request.audit_reason},
      {"timeout_ms", std::to_string(request.timeout_ms)},
      {"include_history", request.include_history ? "true" : "false"}}};
  std::vector<std::uint8_t> bytes{6, 0};
  const auto put = [&](std::string_view value) {
    // 65535 is reserved by SBPS for an extended string; management uses u16 extents.
    if (value.size() >= 65535) return false;
    bytes.push_back(static_cast<std::uint8_t>(value.size()));
    bytes.push_back(static_cast<std::uint8_t>(value.size() >> 8));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
  };
  for (const auto& [key, value] : fields) if (!put(key) || !put(value)) return false;
  output->swap(bytes);
  return true;
}
inline bool DecodeManagementRequestV1(std::span<const std::uint8_t> input,
                                      ManagementRequestV1* output) {
  if (!output || input.size() < 2 || input[0] != 6 || input[1] != 0) return false;
  std::size_t cursor = 2;
  const auto get = [&](std::string* value) {
    if (input.size() - cursor < 2) return false;
    const auto size = static_cast<std::size_t>(input[cursor]) |
                      (static_cast<std::size_t>(input[cursor + 1]) << 8);
    cursor += 2;
    if (size >= 65535 || size > input.size() - cursor) return false;
    value->assign(reinterpret_cast<const char*>(input.data() + cursor), size);
    cursor += size;
    return true;
  };
  std::map<std::string, std::string> fields;
  for (unsigned i = 0; i < 6; ++i) {
    std::string key, value;
    if (!get(&key) || !get(&value) || !fields.emplace(std::move(key), std::move(value)).second) return false;
  }
  if (cursor != input.size()) return false;
  for (const auto* name : {"operation_key", "target_uuid", "mode", "audit_reason", "timeout_ms", "include_history"})
    if (!fields.contains(name)) return false;
  ManagementRequestV1 result;
  result.operation_key = fields.at("operation_key");
  if (result.operation_key.empty() || !DecodeManagementTarget(fields.at("target_uuid"), &result.target_uuid)) return false;
  result.mode = fields.at("mode");
  std::vector<std::string> mode_fields;
  if (!SplitManagementMode(result.mode, &mode_fields)) return false;
  result.audit_reason = fields.at("audit_reason");
  const auto& timeout = fields.at("timeout_ms");
  const auto parsed = std::from_chars(timeout.data(), timeout.data() + timeout.size(), result.timeout_ms);
  if (parsed.ec != std::errc{} || parsed.ptr != timeout.data() + timeout.size()) return false;
  const auto& history = fields.at("include_history");
  if (history != "true" && history != "false") return false;
  result.include_history = history == "true";
  *output = std::move(result);
  return true;
}
} // namespace scratchbird::wire
