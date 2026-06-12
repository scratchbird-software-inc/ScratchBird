// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-WIRE-METADATA-ANCHOR
// DSR-023 implementation of the DSR-014 native type/parameter/result metadata
// layout. Wire type ids are compact renderings; descriptor/domain UUIDs remain
// catalog authority.

#include "datatype_descriptor.hpp"

#include <array>
#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::i32;
using scratchbird::core::platform::i64;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::u8;

inline constexpr u16 kNativeWireMetadataLayoutVersion = 1;
inline constexpr u32 kCanonicalWireTypeIdBytes = 8;
inline constexpr u32 kCanonicalTypeRefBytes = 144;
inline constexpr u32 kParameterDescriptionHeaderBytes = 72;
inline constexpr u32 kParameterDescriptorFixedBytes = 172;
inline constexpr u32 kParameterDataPacketHeaderBytes = 104;
inline constexpr u32 kParameterBindingSlotDescriptorFixedBytes = 160;
inline constexpr u32 kRowDescriptionHeaderBytes = 72;
inline constexpr u32 kResultColumnDescriptorFixedBytes = 216;

inline constexpr u16 kCanonicalWireTypeFlagMask = 0x01ffu;
inline constexpr u64 kCanonicalTypeRefModifierMask = 0x7fffull;
inline constexpr u64 kWireMetadataBitmapMask = 0x0000000fffffffffull;

using WireUuidBytes = std::array<byte, 16>;
using WireHash256Bytes = std::array<byte, 32>;

enum class CanonicalWireTypeFamily : u16 {
  unknown_untyped = 0,
  boolean = 1,
  signed_integer = 2,
  unsigned_integer = 3,
  exact_numeric = 4,
  decimal_floating = 5,
  approximate_numeric = 6,
  money_currency = 7,
  text = 8,
  binary = 9,
  bit_string = 10,
  temporal = 11,
  interval = 12,
  uuid = 13,
  lob = 14,
  array_list = 15,
  map_row_composite = 16,
  range_multirange = 17,
  enum_set = 18,
  network = 19,
  document = 20,
  spatial = 21,
  vector = 22,
  time_series = 23,
  graph = 24,
  sketch = 25,
  locator = 26,
  opaque = 27,
  user_defined_domain = 28,
  cursor_rowset_table_value = 29,
  extension = 30,
};

enum class CanonicalWireTypeFlag : u16 {
  inferred_before_bind = 1u << 0,
  extension_backed = 1u << 1,
  reference_rendered_name_present = 1u << 2,
  domain_wrapped_descriptor = 1u << 3,
  unsigned_numeric_rendering = 1u << 4,
  exact_numeric_rendering = 1u << 5,
  approximate_numeric_rendering = 1u << 6,
  container_element_metadata_required = 1u << 7,
  lob_locator_metadata_required = 1u << 8,
};

enum class CanonicalTypeRefModifier : u64 {
  precision = 1ull << 0,
  scale = 1ull << 1,
  datetime_precision = 1ull << 2,
  length_chars = 1ull << 3,
  length_bytes = 1ull << 4,
  charset_uuid = 1ull << 5,
  collation_uuid = 1ull << 6,
  domain_uuid = 1ull << 7,
  element_descriptor_uuid = 1ull << 8,
  array_rank = 1ull << 9,
  timezone_policy = 1ull << 10,
  calendar_policy = 1ull << 11,
  numeric_locale_policy = 1ull << 12,
  backend_profile_required = 1ull << 13,
  policy_redacted = 1ull << 14,
};

enum class WireNullableTextState : u8 {
  absent_not_applicable = 0,
  hidden_by_policy = 1,
  unknown_by_policy = 2,
  present = 3,
};

enum class WireValueState : u8 {
  value_present = 0,
  sql_null = 1,
  default_requested = 2,
  out_unbound = 3,
  lob_handle = 4,
  structured_value = 5,
  protected_value = 6,
  error_value = 7,
};

enum class WireParameterDirection : u8 {
  in = 0,
  out = 1,
  inout = 2,
  return_value = 3,
  variadic = 4,
};

enum class WireParameterClass : u8 {
  scalar = 0,
  lob = 1,
  structured = 2,
  cursor = 3,
  rowset = 4,
  stream = 5,
};

enum class WireNullability : u8 {
  not_nullable = 0,
  nullable = 1,
  unknown_by_policy = 2,
};

enum class WireValueFormat : u8 {
  canonical_binary = 0,
  utf8_text = 1,
  driver_native = 2,
  structured_envelope = 3,
};

enum class WireParameterDescribeKind : u8 {
  statement = 0,
  routine = 1,
  portal = 2,
  metadata_probe = 3,
};

enum class WireParameterDefaultState : u8 {
  no_default = 0,
  default_available = 1,
  default_hidden = 2,
  default_required = 3,
};

enum class WireParameterPacketKind : u8 {
  bind_request = 0,
  out_parameter_result = 1,
  internal_call = 2,
  udr_call = 3,
};

enum class WireParameterBindShape : u8 {
  single_row = 0,
  array_bind_rows = 1,
  structured_single_value = 2,
};

enum class WireNameBindingState : u8 {
  positional = 0,
  named = 1,
  both = 2,
  server_assigned = 3,
};

enum class WirePayloadEncoding : u8 {
  canonical_binary = 0,
  utf8_text = 1,
  chunk_ref = 2,
  structured_envelope = 3,
  handle_ref = 4,
};

enum class WireResultSetKind : u8 {
  ordinary_rowset = 0,
  generated_keys = 1,
  out_parameter_row = 2,
  function_return_row = 3,
  metadata_only = 4,
  cursor_rowset = 5,
};

enum class WireResultColumnClass : u8 {
  projected = 0,
  base_column = 1,
  expression = 2,
  generated_key = 3,
  out_parameter = 4,
  return_value = 5,
  system = 6,
};

enum class WireResultUpdatability : u8 {
  read_only = 0,
  writable = 1,
  updatable_cursor = 2,
  unknown_by_policy = 3,
};

enum class WireGeneratedValueKind : u8 {
  none = 0,
  identity = 1,
  sequence = 2,
  computed = 3,
  default_expression = 4,
  trigger_generated = 5,
};

enum class WireMetadataBitmapBit : u8 {
  display_label_present = 0,
  column_name_present = 1,
  base_catalog_name_present = 2,
  base_schema_name_present = 3,
  base_table_name_present = 4,
  base_column_name_present = 5,
  type_name_present = 6,
  driver_type_name_present = 7,
  descriptor_uuid_visible_internal = 8,
  domain_uuid_visible_or_present = 9,
  nullability_known = 10,
  precision_valid = 11,
  scale_valid = 12,
  display_size_derivable = 13,
  character_length_valid = 14,
  octet_length_valid = 15,
  charset_visible = 16,
  collation_visible = 17,
  signedness_known = 18,
  case_sensitivity_known = 19,
  searchability_known = 20,
  currency_money_flag_known = 21,
  identity_autoincrement_metadata_present = 22,
  generated_computed_metadata_present = 23,
  generated_key_discriminator_active = 24,
  out_inout_return_discriminator_active = 25,
  writable_updatable_metadata_known = 26,
  base_source_uuids_visible_internal = 27,
  lob_locator_metadata_present = 28,
  array_container_element_metadata_present = 29,
  timezone_policy_present = 30,
  calendar_policy_present = 31,
  numeric_locale_policy_present = 32,
  backend_profile_required = 33,
  policy_redacted = 34,
  remarks_present = 35,
};

struct DatatypeWireValidationResult {
  bool ok = true;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct DatatypeWireEncodeResult {
  bool ok = true;
  std::vector<byte> bytes;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct CanonicalWireTypeId {
  u16 type_family = 0;
  u16 type_code = 0;
  u16 type_version = 0;
  u16 type_flags = 0;
};

struct CanonicalWireTypeIdDecodeResult {
  bool ok = true;
  CanonicalWireTypeId type_id;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct CanonicalTypeRef {
  CanonicalWireTypeId canonical_type_id;
  WireUuidBytes descriptor_uuid{};
  WireUuidBytes domain_uuid{};
  WireUuidBytes element_descriptor_uuid{};
  u32 descriptor_version = 0;
  u32 array_rank = 0;
  u64 modifier_bitmap = 0;
  i64 precision = 0;
  i32 scale = 0;
  i32 datetime_precision = 0;
  i64 length_chars = 0;
  i64 length_bytes = 0;
  WireUuidBytes charset_uuid{};
  WireUuidBytes collation_uuid{};
  u16 timezone_policy = 0;
  u16 calendar_policy = 0;
  u16 numeric_locale_policy = 0;
  u16 reserved = 0;
};

struct CanonicalTypeRefDecodeResult {
  bool ok = true;
  CanonicalTypeRef type_ref;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct NullableText {
  WireNullableTextState state = WireNullableTextState::absent_not_applicable;
  std::string text;
};

struct ParameterDescriptor {
  u32 ordinal = 0;
  WireParameterDirection direction = WireParameterDirection::in;
  WireParameterClass parameter_class = WireParameterClass::scalar;
  WireNullability nullability = WireNullability::unknown_by_policy;
  WireValueFormat value_format = WireValueFormat::canonical_binary;
  u64 parameter_flags = 0;
  u64 metadata_bitmap = 0;
  CanonicalTypeRef type_ref;
  WireParameterDefaultState default_state = WireParameterDefaultState::no_default;
  NullableText name;
  NullableText type_name;
  NullableText domain_name;
  NullableText default_expression;
};

struct ParameterDescriptionPacket {
  u16 layout_version = kNativeWireMetadataLayoutVersion;
  WireParameterDescribeKind describe_kind = WireParameterDescribeKind::statement;
  u8 flags = 0;
  WireUuidBytes statement_uuid{};
  WireUuidBytes signature_uuid{};
  WireHash256Bytes descriptor_snapshot_hash{};
  std::vector<ParameterDescriptor> parameters;
};

struct ParameterDescriptionDecodeResult {
  bool ok = true;
  ParameterDescriptionPacket packet;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct ParameterBindingSlotDescriptor {
  u32 ordinal = 0;
  WireParameterDirection direction = WireParameterDirection::in;
  u8 bind_flags = 0;
  WireNameBindingState name_binding_state = WireNameBindingState::positional;
  WireValueFormat value_format = WireValueFormat::canonical_binary;
  CanonicalTypeRef type_ref;
  i64 requested_size_bytes = 0;
  NullableText name;
};

struct ParameterValueCell {
  u32 slot_ordinal = 0;
  WireValueState value_state = WireValueState::value_present;
  WirePayloadEncoding payload_encoding = WirePayloadEncoding::canonical_binary;
  u16 payload_flags = 0;
  std::vector<byte> payload;
};

struct ParameterRowValueFrame {
  u32 row_ordinal = 0;
  std::vector<ParameterValueCell> values;
};

struct ParameterDataPacket {
  u16 layout_version = kNativeWireMetadataLayoutVersion;
  WireParameterPacketKind packet_kind = WireParameterPacketKind::bind_request;
  WireParameterBindShape bind_shape = WireParameterBindShape::single_row;
  u32 flags = 0;
  WireUuidBytes statement_uuid{};
  WireUuidBytes portal_uuid{};
  WireHash256Bytes descriptor_snapshot_hash{};
  WireUuidBytes redaction_map_uuid{};
  std::vector<ParameterBindingSlotDescriptor> slot_descriptors;
  std::vector<ParameterRowValueFrame> row_value_frames;
};

struct ParameterDataPacketDecodeResult {
  bool ok = true;
  ParameterDataPacket packet;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct ResultColumnDescriptor {
  u32 ordinal = 0;
  WireResultColumnClass column_class = WireResultColumnClass::projected;
  WireValueFormat value_format = WireValueFormat::canonical_binary;
  WireNullability nullability = WireNullability::unknown_by_policy;
  WireResultUpdatability updatability = WireResultUpdatability::read_only;
  u64 metadata_bitmap = 0;
  CanonicalTypeRef type_ref;
  WireUuidBytes source_table_uuid{};
  WireUuidBytes source_column_uuid{};
  WireUuidBytes source_generated_object_uuid{};
  u32 source_parameter_ordinal = 0;
  WireParameterDirection parameter_direction = WireParameterDirection::in;
  WireGeneratedValueKind generated_value_kind = WireGeneratedValueKind::none;
  NullableText display_label;
  NullableText column_name;
  NullableText base_catalog_name;
  NullableText base_schema_name;
  NullableText base_table_name;
  NullableText base_column_name;
  NullableText type_name;
  NullableText domain_name;
  NullableText driver_type_name;
  NullableText remarks;
};

struct RowDescriptionPacket {
  u16 layout_version = kNativeWireMetadataLayoutVersion;
  WireResultSetKind result_set_kind = WireResultSetKind::ordinary_rowset;
  u8 flags = 0;
  WireUuidBytes result_envelope_uuid{};
  WireUuidBytes row_shape_uuid{};
  WireHash256Bytes descriptor_snapshot_hash{};
  std::vector<ResultColumnDescriptor> columns;
};

struct RowDescriptionDecodeResult {
  bool ok = true;
  RowDescriptionPacket packet;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

constexpr u64 WireMetadataBit(WireMetadataBitmapBit bit) {
  return 1ull << static_cast<u8>(bit);
}

constexpr u64 CanonicalTypeRefModifierBit(CanonicalTypeRefModifier modifier) {
  return static_cast<u64>(modifier);
}

constexpr u16 CanonicalWireTypeFlagBit(CanonicalWireTypeFlag flag) {
  return static_cast<u16>(flag);
}

bool WireUuidIsZero(const WireUuidBytes& value);
const char* CanonicalWireTypeFamilyName(CanonicalWireTypeFamily family);
const char* WireValueStateName(WireValueState state);
const char* WireResultSetKindName(WireResultSetKind kind);
const char* WireParameterDirectionName(WireParameterDirection direction);

CanonicalWireTypeId WireTypeIdForCanonicalTypeId(CanonicalTypeId type_id);
CanonicalTypeId CanonicalTypeIdFromWireTypeId(const CanonicalWireTypeId& wire_type_id);
std::string CanonicalWireTypeCodeName(const CanonicalWireTypeId& wire_type_id);

std::array<byte, kCanonicalWireTypeIdBytes> EncodeCanonicalWireTypeId(
    const CanonicalWireTypeId& type_id);
CanonicalWireTypeIdDecodeResult DecodeCanonicalWireTypeId(const byte* data, u64 size);
DatatypeWireValidationResult ValidateCanonicalWireTypeId(const CanonicalWireTypeId& type_id);

std::array<byte, kCanonicalTypeRefBytes> EncodeCanonicalTypeRef(const CanonicalTypeRef& type_ref);
CanonicalTypeRefDecodeResult DecodeCanonicalTypeRef(const byte* data, u64 size);
DatatypeWireValidationResult ValidateCanonicalTypeRef(const CanonicalTypeRef& type_ref);

DatatypeWireValidationResult ValidateNullableText(const NullableText& text);
DatatypeWireValidationResult ValidateParameterValueCell(WireParameterDirection direction,
                                                        const ParameterValueCell& cell);
DatatypeWireValidationResult ValidateParameterDataPacket(const ParameterDataPacket& packet);
DatatypeWireValidationResult ValidateRowDescriptionPacket(const RowDescriptionPacket& packet);

DatatypeWireEncodeResult EncodeParameterDescriptionPacket(const ParameterDescriptionPacket& packet);
ParameterDescriptionDecodeResult DecodeParameterDescriptionPacket(const std::vector<byte>& bytes);

DatatypeWireEncodeResult EncodeParameterDataPacket(const ParameterDataPacket& packet);
ParameterDataPacketDecodeResult DecodeParameterDataPacket(const std::vector<byte>& bytes);

DatatypeWireEncodeResult EncodeRowDescriptionPacket(const RowDescriptionPacket& packet);
RowDescriptionDecodeResult DecodeRowDescriptionPacket(const std::vector<byte>& bytes);

}  // namespace scratchbird::core::datatypes
