// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../../src/wire/binary_status_packet.hpp"
namespace scratchbird::cli {
// Human-readable UUID conversion belongs exclusively to this client display
// boundary. The decoded status packet and all identity atoms remain binary.
inline std::optional<std::string> RenderBinaryStatus(std::string_view bytes) {
  std::vector<wire::public_result::Field> fields;
  if (!wire::binary_status::Decode(bytes,&fields)) return std::nullopt;
  std::string text;
  constexpr char hex[]="0123456789abcdef";
  for (std::size_t index=1;index<fields.size();++index) {
    const auto& field=fields[index];
    if (field.kind==wire::public_result::Kind::text) { text+=field.value;continue; }
    for (unsigned i=0;i<16;++i) {
      if (i==4||i==6||i==8||i==10) text+='-';
      const auto byte=static_cast<unsigned char>(field.value[i]);
      text+=hex[byte>>4];text+=hex[byte&15];
    }
  }
  return text;
}
// Client display boundary for bounded append-only server observation records.
// A standalone support bundle is already one SBRES002 status packet.
inline std::optional<std::string> RenderBinaryObservationLog(std::string_view bytes) {
  if (bytes.starts_with(wire::public_result::kMagic)) return RenderBinaryStatus(bytes);
  std::string rendered;
  while (!bytes.empty()) {
    if (bytes.size() < 16 || bytes.substr(0, 8) != "SBOBS002") return std::nullopt;
    std::uint64_t size = 0;
    for (unsigned i = 0; i < 8; ++i)
      size |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
    if (size > 64 * 1024 * 1024 || size > bytes.size() - 16) return std::nullopt;
    const auto record = RenderBinaryStatus(bytes.substr(16, size));
    if (!record) return std::nullopt;
    rendered += *record;
    rendered += '\n';
    bytes.remove_prefix(16 + size);
  }
  return rendered;
}
}  // namespace scratchbird::cli
