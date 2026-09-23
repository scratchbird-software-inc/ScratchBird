// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../core/platform/runtime_platform.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
namespace scratchbird::wire {
struct ParserMetricSnapshotV1 {
  core::platform::Uuid parser_uuid;
  core::platform::Uuid connection_uuid;
  core::platform::Uuid session_uuid;
  std::string attributes_json;
};
inline constexpr std::size_t kParserMetricSnapshotMaximumBytes = 1024 * 1024;
inline std::string EncodeParserMetricSnapshotV1(const ParserMetricSnapshotV1& snapshot) {
  if (snapshot.attributes_json.size() > kParserMetricSnapshotMaximumBytes - 60)
    throw std::length_error("parser_metric_snapshot_size_limit");
  std::string bytes("SBPMET01", 8);
  for (const auto& identity : {snapshot.parser_uuid, snapshot.connection_uuid, snapshot.session_uuid})
    bytes.append(reinterpret_cast<const char*>(identity.bytes.data()), 16);
  const auto size = static_cast<std::uint32_t>(snapshot.attributes_json.size());
  for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(size >> (8 * i)));
  bytes += snapshot.attributes_json;
  return bytes;
}
inline std::optional<ParserMetricSnapshotV1> DecodeParserMetricSnapshotV1(std::string_view bytes) {
  if (bytes.size() < 60 || bytes.size() > kParserMetricSnapshotMaximumBytes ||
      bytes.substr(0, 8) != "SBPMET01") return std::nullopt;
  std::uint32_t size = 0;
  for (unsigned i = 0; i < 4; ++i) size |= std::uint32_t(static_cast<unsigned char>(bytes[56 + i])) << (8 * i);
  if (size != bytes.size() - 60) return std::nullopt;
  ParserMetricSnapshotV1 snapshot;
  std::size_t offset = 8;
  for (auto* identity : {&snapshot.parser_uuid, &snapshot.connection_uuid, &snapshot.session_uuid}) {
    std::copy_n(reinterpret_cast<const unsigned char*>(bytes.data() + offset), 16, identity->bytes.begin());
    offset += 16;
  }
  snapshot.attributes_json.assign(bytes.substr(60));
  return snapshot;
}
} // namespace scratchbird::wire
