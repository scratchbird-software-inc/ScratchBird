// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_MCP_PAYLOAD_MODULE

#include "manager_mcp_payload.hpp"

#include <cstddef>

namespace scratchbird::manager::node {
namespace proto = scratchbird::manager::protocol;
namespace {

std::uint32_t ReadU32Le(const proto::Bytes& data, std::size_t off) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[off + static_cast<std::size_t>(i)]) << (8 * i);
  return value;
}

bool ReadLpstr(const proto::Bytes& data, std::size_t* off, std::string* out) {
  if (!off || !out || *off + 4 > data.size()) return false;
  const auto len = ReadU32Le(data, *off);
  *off += 4;
  if (len > 4096 || *off + len > data.size()) return false;
  out->assign(data.begin() + static_cast<std::ptrdiff_t>(*off), data.begin() + static_cast<std::ptrdiff_t>(*off + len));
  *off += len;
  return true;
}

} // namespace

bool DecodeManagerCommandPayload(const proto::Bytes& payload,
                                 std::string* operation,
                                 std::string* idempotency_key,
                                 std::vector<std::pair<std::string, std::string>>* args) {
  if (!operation || !idempotency_key || !args || payload.size() < 8) return false;
  if (payload[0] != 'M' || payload[1] != 'C' || payload[2] != 'P' || payload[3] != '1') return false;
  std::size_t off = 4;
  if (!ReadLpstr(payload, &off, operation)) return false;
  if (!ReadLpstr(payload, &off, idempotency_key)) return false;
  if (off + 4 > payload.size()) return false;
  const auto count = ReadU32Le(payload, off);
  off += 4;
  if (count > 128) return false;
  args->clear();
  args->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key;
    std::string value;
    if (!ReadLpstr(payload, &off, &key) || !ReadLpstr(payload, &off, &value)) return false;
    args->push_back({std::move(key), std::move(value)});
  }
  return off == payload.size();
}

} // namespace scratchbird::manager::node
