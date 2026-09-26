// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "parser_server_client.hpp"
#include "core/platform/runtime_platform.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace ipc = scratchbird::parser::ipc;
using Bytes = std::vector<std::uint8_t>;
using Uuid = scratchbird::core::platform::Uuid;
constexpr std::size_t kTrailer = 123;
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failures; std::cerr << label << '\n'; }
}
Uuid Id(unsigned n) {
  Uuid id{{1, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0, 0}};
  id.bytes[15] = static_cast<std::uint8_t>(n);
  return id;
}
void U16(Bytes& out, unsigned value) {
  out.push_back(value & 255); out.push_back((value >> 8) & 255);
}
void U64(Bytes& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) out.push_back((value >> (i * 8)) & 255);
}
void Text(Bytes& out, const std::string& value) {
  U16(out, value.size()); out.insert(out.end(), value.begin(), value.end());
}
void Identity(Bytes& out, const Uuid& id) { out.insert(out.end(), id.bytes.begin(), id.bytes.end()); }
Bytes Base(bool cursor, std::size_t detail_size,
           const std::string& row_packet = "unchanged row packet") {
  Bytes out;
  Text(out, "accepted"); Identity(out, Id(1)); Identity(out, cursor ? Id(2) : Uuid{});
  U64(out, 0); Text(out, "engine.op.stmt_prepare"); Text(out, row_packet);
  Text(out, std::string(detail_size, 'd'));
  out.insert(out.end(), {0, 0, 0});
  Text(out, "abcdefgh"); Text(out, "");
  return out;
}
void Trailer(Bytes& out) {
  out.push_back(1); Identity(out, Id(3)); U16(out, 1); U64(out, 1);
  Identity(out, Id(2)); Identity(out, Id(4)); Identity(out, Id(5));
  Identity(out, Id(6)); Identity(out, Id(7)); U64(out, 10); U64(out, 1048576);
}
bool Decode(const Bytes& input, ipc::ServerExecutionResult* result = nullptr) {
  ipc::ServerExecutionResult local;
  local.operation_id = "unchanged-sentinel";
  local.row_count = 99;
  local.cursor_uuid = Id(99);
  ipc::MessageVectorSet messages;
  const auto ok = ipc::DecodeExecuteResultPayloadV2ForTest(input, result ? result : &local, &messages);
  if (!ok && result == nullptr) {
    Check(local.operation_id == "unchanged-sentinel" && local.row_count == 99 &&
              local.cursor_uuid == Id(99) && !local.accepted && !local.cursor_stream_descriptor.present,
          "rejected result published partial cursor or execution state");
  }
  return ok;
}
int main() {
  for (std::size_t length = 128; length < 260; ++length) {
    auto payload = Base(false, length); payload.push_back(0);
    Check(Decode(payload), "ordinary absent descriptor rejected");
    const auto offset = payload.size() - kTrailer;
    payload[offset] = 1; payload[offset + 17] = 1; payload[offset + 18] = 0;
    ipc::ServerExecutionResult result;
    Check(Decode(payload, &result), "data bytes mistaken for cursor descriptor trailer");
    Check(result.accepted && result.cursor_uuid.is_nil() && !result.cursor_stream_descriptor.present &&
              result.row_packet == "unchanged row packet" && result.transaction_outcome_detail == "abcdefgh",
          "absent cursor result changed by trailer lookalike");
    payload.back() = 2;
    Check(!Decode(payload), "unknown absent marker accepted");
  }
  auto present = Base(true, 256); Trailer(present);
  ipc::ServerExecutionResult decoded;
  Check(Decode(present, &decoded) && decoded.cursor_uuid == Id(2) &&
            decoded.cursor_stream_descriptor.stream_descriptor_uuid == Id(3),
        "valid cursor descriptor rejected");
  const auto begin = present.size() - kTrailer;
  // User data may contain UUIDs of earlier versions, nil UUIDs, or arbitrary
  // bytes. Only the cursor control identities require the system v7 contract.
  for (unsigned version = 0; version <= 8; ++version) {
    auto data_id = Id(44);
    data_id.bytes[6] = static_cast<std::uint8_t>(version << 4);
    if (version == 0) data_id = {};
    std::string data(reinterpret_cast<const char*>(data_id.bytes.data()), 16);
    data.append("\0binary-user-data", 17);
    for (const bool cursor : {false, true}) {
      auto payload = Base(cursor, 128, data);
      if (cursor) Trailer(payload); else payload.push_back(0);
      ipc::ServerExecutionResult result;
      Check(Decode(payload, &result) && result.row_packet == data,
            "binary user UUID data was rejected or changed");
    }
  }
  for (const auto relative : {19u, 107u, 115u}) {
    auto invalid = present;
    std::fill_n(invalid.begin() + begin + relative, 8, 0);
    Check(!Decode(invalid), "zero cursor descriptor generation or limit accepted");
  }
  auto wrong_version = present;
  wrong_version[begin + 17] = 2;
  Check(!Decode(wrong_version), "unknown cursor descriptor version accepted");
  for (const auto relative : {1u, 27u, 43u, 59u, 75u, 91u}) {
    auto invalid = present;
    invalid[begin + relative + 6] = 0x40;
    Check(!Decode(invalid), "non-v7 cursor control identity accepted");
    invalid = present;
    std::fill_n(invalid.begin() + begin + relative, 16, 0);
    Check(!Decode(invalid), "nil cursor control identity accepted");
  }
  for (std::size_t size = 0; size < present.size(); ++size) {
    Check(!Decode(Bytes(present.begin(), present.begin() + size)), "truncated cursor result accepted");
  }
  auto mismatch = present; mismatch[begin + 27 + 15] = 99;
  Check(!Decode(mismatch), "cursor trailer/header mismatch accepted");
  auto trailing = present; trailing.push_back(0);
  Check(!Decode(trailing), "cursor trailing byte accepted");
  auto nil_with_descriptor = Base(false, 256); Trailer(nil_with_descriptor);
  Check(!Decode(nil_with_descriptor), "present descriptor without cursor accepted");
  std::cout << "execute trailer checks=" << checks << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
