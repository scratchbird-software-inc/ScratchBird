// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_SBMV_TYPED_UUID_FIELDS (owner 2026-11-26: "Yes, native-only"; built 2026-12-17).
// The SBMV v1 registered value type 5 (system_uuid: 16 bytes, non-nil RFC-variant UUIDv7; Core registries/message-vector-wire-v1.yaml, and
// wire/message_vector_value_codec.hpp ValueType::system_uuid) as a message-vector FIELD of the SBPS typed execute diagnostic trailer. The text TLV
// (type 1) stays what it was: IsPublicDiagnosticFieldAllowed still refuses a uuid-named text field. A typed UUID field is carried ONLY for a
// (diagnostic code, field name) pair registered here, ONLY to a peer that is a NATIVE session (the receipt announced as wire extension 75, the
// native admission), and is never text. The server encoder (server/sbps.cpp) and the parser decoder (parser_server_client.cpp) read this ONE table.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scratchbird::wire::message_vector {

inline constexpr std::uint16_t kTypedUuidFieldTypeCode = 5;  // ValueType::system_uuid

struct TypedUuidFieldRegistration {
  std::string_view diagnostic_code;
  std::string_view field_name;
};

// Every engine diagnostic that carries an identity the parser may receive as data, and the identity names it may carry. Nothing else is forwarded.
// SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED: the table that refused (availability_table_uuid) and the database (database_uuid).
// ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY: the database (database_uuid).
// (ROW_BOUND_EXCEEDED, RESULT_VECTOR_TYPE_NOT_BUILT and OPERATION_NOT_BUILT carry no identity.)
inline constexpr std::array<TypedUuidFieldRegistration, 3> kTypedUuidFieldRegistry{{
    {"SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "availability_table_uuid"},
    {"SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "database_uuid"},
    {"ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY", "database_uuid"},
}};

[[nodiscard]] constexpr bool IsRegisteredTypedUuidField(std::string_view diagnostic_code, std::string_view field_name) noexcept {
  for (const auto& entry : kTypedUuidFieldRegistry)
    if (entry.diagnostic_code == diagnostic_code && entry.field_name == field_name) return true;
  return false;
}

// A system_uuid value: exactly 16 bytes, non-nil, RFC variant (10xx), version 7.
[[nodiscard]] constexpr bool IsSystemUuidValue(const std::uint8_t* bytes, std::size_t size) noexcept {
  if (bytes == nullptr || size != 16) return false;
  bool nil = true;
  for (std::size_t i = 0; i < 16; ++i)
    if (bytes[i] != 0) nil = false;
  return !nil && (bytes[6] & 0xf0) == 0x70 && (bytes[8] & 0xc0) == 0x80;
}

}  // namespace scratchbird::wire::message_vector
