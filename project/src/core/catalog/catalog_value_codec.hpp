// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "runtime_platform.hpp"
#include <string>
#include <variant>
#include <vector>

namespace scratchbird::core::catalog {

using platform::byte;
using platform::u8;
using platform::u16;
using platform::u32;
using platform::u64;
using platform::TypedUuid;
using platform::Uuid;
using platform::UuidKind;

// CATALOG_BINARY_VALUE_BLOCK_V1. No text identity conversion or disk I/O.
// A record reader must select its admitted schema, not trust a schema supplied
// by the input. Record/page authority and publication belong to their callers.
constexpr std::size_t kCatalogValueBlockHeaderBytes = 24;
constexpr std::size_t kCatalogValueBlockMaxBytes = 131072;
enum class CatalogValueType : u8 {
  unsigned_integer = 1, boolean = 2, utf8_text = 3, opaque_bytes = 4,
  engine_identity = 5, user_uuid_data = 6, engine_identity_list = 7,
  utf8_text_list = 8
};
enum class CatalogValueError : u8 {
  none, invalid_schema, invalid_framing, unsupported_version, unknown_field,
  missing_field, type_mismatch, invalid_value, size_limit
};
struct CatalogValueFieldSchema {
  u16 id = 0;
  CatalogValueType type = CatalogValueType::opaque_bytes;
  bool required = false;
  u32 maximum_bytes = 0;
  UuidKind identity_kind = UuidKind::unknown;
};
struct CatalogValueSchema {
  u32 id = 0;
  u16 version = 0;
  std::vector<CatalogValueFieldSchema> fields;
};
using CatalogValue = std::variant<u64, bool, std::string, std::vector<byte>,
                                  TypedUuid, Uuid, std::vector<TypedUuid>,
                                  std::vector<std::string>>;
struct CatalogValueField {
  u16 id = 0;
  CatalogValue value;
};
struct CatalogValueEncodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::vector<byte> bytes;
  bool ok() const { return error == CatalogValueError::none; }
};
struct CatalogValueDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::vector<CatalogValueField> fields;
  bool ok() const { return error == CatalogValueError::none; }
};
CatalogValueEncodeResult EncodeCatalogValueBlock(
    const CatalogValueSchema& schema, const std::vector<CatalogValueField>& fields);
CatalogValueDecodeResult DecodeCatalogValueBlock(
    const CatalogValueSchema& schema, const std::vector<byte>& bytes);

}  // namespace scratchbird::core::catalog
