// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "scratchbird/protocol/sbwp_protocol.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void U32(std::vector<uint8_t>& out, uint32_t n) {
  for (unsigned i = 0; i != 4; ++i) out.push_back(n >> (i * 8));
}
void Check(bool good) { if (!good) throw std::runtime_error("UUID ParameterStatus contract"); }
std::vector<uint8_t> Packet(uint8_t type, const std::string& value) {
  const std::string name = "session.authenticated_user_uuid";
  std::vector<uint8_t> out;
  U32(out, 1); U32(out, name.size());
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), {type, 0, 0});
  U32(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}
}
int main() {
  using scratchbird::core::Status;
  using scratchbird::protocol::parseParameterStatuses;
  // Delimiters, NUL, and non-UTF8 bytes must survive exactly; no text round trip.
  const std::array<uint8_t,16> uuid{0x01,0xa0,0,0x0a,0x0d,0x7c,0x70,0,0x80,0xff,0x5c,0x09,0,0,0,1};
  const std::string bytes(reinterpret_cast<const char*>(uuid.data()),uuid.size());
  std::vector<std::pair<std::string,std::string>> values;
  scratchbird::core::ErrorContext context;
  Check(parseParameterStatuses(Packet(4,bytes), values, &context) == Status::OK);
  Check(values.size() == 1 && values[0].second == bytes);
  for (const auto size : {0,15,17,36}) {
    Check(parseParameterStatuses(Packet(4,std::string(size,'x')), values, &context)
          == Status::PROTOCOL_VIOLATION);
    Check(values.empty());
  }
  Check(parseParameterStatuses(Packet(1,bytes), values, &context) == Status::PROTOCOL_VIOLATION);
  Check(parseParameterStatuses(Packet(1,"01a0000a-0d7c-7000-80ff-5c0900000001"), values, &context)
        == Status::PROTOCOL_VIOLATION);
  const auto legacy = [](const std::string& value) {
    const std::string name = "session.authenticated_user_uuid";
    std::vector<uint8_t> packet;
    U32(packet,name.size()); packet.insert(packet.end(),name.begin(),name.end());
    U32(packet,value.size()); packet.insert(packet.end(),value.begin(),value.end());
    return packet;
  };
  Check(parseParameterStatuses(legacy(bytes), values, &context) == Status::OK &&
        values.size() == 1 && values.front().second == bytes);
  Check(parseParameterStatuses(legacy("01a0000a-0d7c-7000-80ff-5c0900000001"), values, &context)
        == Status::PROTOCOL_VIOLATION);
  auto truncated = Packet(4,bytes); truncated.pop_back();
  Check(parseParameterStatuses(truncated, values, &context) == Status::PROTOCOL_VIOLATION);
  std::cout << "binary UUID ParameterStatus checks passed\n";
}
