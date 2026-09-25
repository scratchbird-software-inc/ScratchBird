// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/wire/public_result_packet.hpp"
#include <stdexcept>
namespace scratchbird::tests {
// Test CLIENT display boundary. Decode framing before formatting UUID atoms;
// the driver, wire packet, engine and retained result remain binary.
inline std::string DisplayPublicResultPacket(std::string_view packet,
                                             unsigned depth = 0) {
  namespace result = wire::public_result;
  std::vector<result::Field> fields;
  if (depth > 2 || packet.size() > 64 * 1024 * 1024 || !result::Decode(packet, &fields))
    throw std::invalid_argument("client result packet framing invalid");
  std::string text;
  constexpr char hex[] = "0123456789abcdef";
  for (const auto& field : fields) {
    if (!text.empty()) text += depth == 0 ? '\n' : ';';
    text += field.name;
    text += '=';
    if (field.kind == result::Kind::row || field.kind == result::Kind::evidence) {
      text += DisplayPublicResultPacket(field.value, depth + 1);
    } else if (field.kind == result::Kind::text) {
      text += field.value;
    } else if (field.kind == result::Kind::unsigned_integer) {
      const auto number = result::AsUnsigned(field);
      if (!number) throw std::invalid_argument("client result integer invalid");
      text += std::to_string(*number);
    } else if (field.kind == result::Kind::uuid || field.kind == result::Kind::bytes) {
      if (field.kind == result::Kind::bytes) text += "hex:";
      for (std::size_t i = 0; i < field.value.size(); ++i) {
        if (field.kind == result::Kind::uuid && (i == 4 || i == 6 || i == 8 || i == 10))
          text += '-';
        const auto byte = static_cast<unsigned char>(field.value[i]);
        text += hex[byte >> 4];
        text += hex[byte & 15];
      }
    } else throw std::invalid_argument("client result field kind invalid");
  }
  if (depth == 0) text += '\n';
  return text;
}
}
