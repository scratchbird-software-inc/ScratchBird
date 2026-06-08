// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_wire_metadata.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr u64 kParameterFlagsMask = 0x7ffull;
constexpr u8 kParameterDescriptionFlagsMask = 0x03u;
constexpr u8 kRowDescriptionFlagsMask = 0x03u;
constexpr u8 kBindFlagsMask = 0x0fu;
constexpr u16 kPayloadFlagsMask = 0x0000u;

DatatypeWireValidationResult Ok() { return {}; }

DatatypeWireValidationResult Failure(std::string code, std::string detail = {}) {
  DatatypeWireValidationResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  return result;
}

DatatypeWireEncodeResult EncodeFailure(std::string code, std::string detail = {}) {
  DatatypeWireEncodeResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  return result;
}

bool ModifierSet(const CanonicalTypeRef& type_ref, CanonicalTypeRefModifier modifier) {
  return (type_ref.modifier_bitmap & CanonicalTypeRefModifierBit(modifier)) != 0;
}

bool BitmapSet(u64 bitmap, WireMetadataBitmapBit bit) {
  return (bitmap & WireMetadataBit(bit)) != 0;
}

bool ArrayIsZero(const WireUuidBytes& value) {
  return std::all_of(value.begin(), value.end(), [](byte current) { return current == 0; });
}

bool HashIsZero(const WireHash256Bytes& value) {
  return std::all_of(value.begin(), value.end(), [](byte current) { return current == 0; });
}

void AppendU8(std::vector<byte>* out, u8 value) { out->push_back(value); }

void AppendU16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

void AppendI32(std::vector<byte>* out, i32 value) {
  u32 stored = 0;
  std::memcpy(&stored, &value, sizeof(value));
  AppendU32(out, stored);
}

void AppendI64(std::vector<byte>* out, i64 value) {
  u64 stored = 0;
  std::memcpy(&stored, &value, sizeof(value));
  AppendU64(out, stored);
}

void AppendBytes(std::vector<byte>* out, const byte* data, std::size_t size) {
  out->insert(out->end(), data, data + size);
}

void AppendUuid(std::vector<byte>* out, const WireUuidBytes& value) {
  AppendBytes(out, value.data(), value.size());
}

void AppendHash(std::vector<byte>* out, const WireHash256Bytes& value) {
  AppendBytes(out, value.data(), value.size());
}

u32 CheckedU32Size(std::size_t size) {
  return size > std::numeric_limits<u32>::max() ? std::numeric_limits<u32>::max()
                                                : static_cast<u32>(size);
}

u64 CheckedU64Size(std::size_t size) { return static_cast<u64>(size); }

struct Cursor {
  const byte* data = nullptr;
  u64 size = 0;
  u64 offset = 0;
  std::string diagnostic_code;

  bool Remaining(u64 bytes) const { return offset <= size && bytes <= (size - offset); }

  bool ReadU8(u8* value) {
    if (!Remaining(1)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    *value = data[offset++];
    return true;
  }

  bool ReadU16(u16* value) {
    if (!Remaining(2)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    *value = LoadLittle16(data + offset);
    offset += 2;
    return true;
  }

  bool ReadU32(u32* value) {
    if (!Remaining(4)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    *value = LoadLittle32(data + offset);
    offset += 4;
    return true;
  }

  bool ReadU64(u64* value) {
    if (!Remaining(8)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    *value = LoadLittle64(data + offset);
    offset += 8;
    return true;
  }

  bool ReadI32(i32* value) {
    u32 stored = 0;
    if (!ReadU32(&stored)) { return false; }
    std::memcpy(value, &stored, sizeof(stored));
    return true;
  }

  bool ReadI64(i64* value) {
    u64 stored = 0;
    if (!ReadU64(&stored)) { return false; }
    std::memcpy(value, &stored, sizeof(stored));
    return true;
  }

  bool ReadBytes(byte* target, u64 bytes) {
    if (!Remaining(bytes)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    std::memcpy(target, data + offset, static_cast<std::size_t>(bytes));
    offset += bytes;
    return true;
  }

  bool Skip(u64 bytes) {
    if (!Remaining(bytes)) { diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED"; return false; }
    offset += bytes;
    return true;
  }
};

void EncodeNullableTextInto(std::vector<byte>* out, const NullableText& text) {
  AppendU8(out, static_cast<u8>(text.state));
  if (text.state == WireNullableTextState::present) {
    AppendU32(out, CheckedU32Size(text.text.size()));
    AppendBytes(out,
                reinterpret_cast<const byte*>(text.text.data()),
                text.text.size());
  } else {
    AppendU32(out, 0);
  }
}

bool DecodeNullableTextFrom(Cursor* cursor, NullableText* text) {
  u8 state = 0;
  u32 length = 0;
  if (!cursor->ReadU8(&state) || !cursor->ReadU32(&length)) { return false; }
  text->state = static_cast<WireNullableTextState>(state);
  text->text.clear();
  if (length != 0) {
    if (!cursor->Remaining(length)) {
      cursor->diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED";
      return false;
    }
    text->text.assign(reinterpret_cast<const char*>(cursor->data + cursor->offset), length);
    cursor->offset += length;
  }
  return true;
}

void EncodeCanonicalTypeRefInto(std::vector<byte>* out, const CanonicalTypeRef& type_ref) {
  const auto type_id = EncodeCanonicalWireTypeId(type_ref.canonical_type_id);
  AppendBytes(out, type_id.data(), type_id.size());
  AppendUuid(out, type_ref.descriptor_uuid);
  AppendUuid(out, type_ref.domain_uuid);
  AppendUuid(out, type_ref.element_descriptor_uuid);
  AppendU32(out, type_ref.descriptor_version);
  AppendU32(out, type_ref.array_rank);
  AppendU64(out, type_ref.modifier_bitmap);
  AppendI64(out, type_ref.precision);
  AppendI32(out, type_ref.scale);
  AppendI32(out, type_ref.datetime_precision);
  AppendI64(out, type_ref.length_chars);
  AppendI64(out, type_ref.length_bytes);
  AppendUuid(out, type_ref.charset_uuid);
  AppendUuid(out, type_ref.collation_uuid);
  AppendU16(out, type_ref.timezone_policy);
  AppendU16(out, type_ref.calendar_policy);
  AppendU16(out, type_ref.numeric_locale_policy);
  AppendU16(out, type_ref.reserved);
}

bool DecodeCanonicalTypeRefFrom(Cursor* cursor, CanonicalTypeRef* type_ref) {
  if (!cursor->Remaining(kCanonicalTypeRefBytes)) {
    cursor->diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED";
    return false;
  }
  const auto decoded = DecodeCanonicalWireTypeId(cursor->data + cursor->offset,
                                                 kCanonicalWireTypeIdBytes);
  if (!decoded.ok) {
    cursor->diagnostic_code = decoded.diagnostic_code;
    return false;
  }
  type_ref->canonical_type_id = decoded.type_id;
  cursor->offset += kCanonicalWireTypeIdBytes;
  if (!cursor->ReadBytes(type_ref->descriptor_uuid.data(), type_ref->descriptor_uuid.size()) ||
      !cursor->ReadBytes(type_ref->domain_uuid.data(), type_ref->domain_uuid.size()) ||
      !cursor->ReadBytes(type_ref->element_descriptor_uuid.data(), type_ref->element_descriptor_uuid.size()) ||
      !cursor->ReadU32(&type_ref->descriptor_version) ||
      !cursor->ReadU32(&type_ref->array_rank) ||
      !cursor->ReadU64(&type_ref->modifier_bitmap) ||
      !cursor->ReadI64(&type_ref->precision) ||
      !cursor->ReadI32(&type_ref->scale) ||
      !cursor->ReadI32(&type_ref->datetime_precision) ||
      !cursor->ReadI64(&type_ref->length_chars) ||
      !cursor->ReadI64(&type_ref->length_bytes) ||
      !cursor->ReadBytes(type_ref->charset_uuid.data(), type_ref->charset_uuid.size()) ||
      !cursor->ReadBytes(type_ref->collation_uuid.data(), type_ref->collation_uuid.size()) ||
      !cursor->ReadU16(&type_ref->timezone_policy) ||
      !cursor->ReadU16(&type_ref->calendar_policy) ||
      !cursor->ReadU16(&type_ref->numeric_locale_policy) ||
      !cursor->ReadU16(&type_ref->reserved)) {
    return false;
  }
  return true;
}

CanonicalWireTypeId WireId(CanonicalWireTypeFamily family, u16 code, u16 flags = 0) {
  CanonicalWireTypeId type_id;
  type_id.type_family = static_cast<u16>(family);
  type_id.type_code = code;
  type_id.type_version = 1;
  type_id.type_flags = flags;
  return type_id;
}

bool IsConcreteTypeId(const CanonicalWireTypeId& type_id) {
  return type_id.type_family != 0 || type_id.type_code != 0 ||
         type_id.type_version != 0 || type_id.type_flags != 0;
}

bool IsOutDirection(WireParameterDirection direction) {
  return direction == WireParameterDirection::out ||
         direction == WireParameterDirection::inout ||
         direction == WireParameterDirection::return_value;
}

DatatypeWireValidationResult ValidateParameterDescriptor(const ParameterDescriptor& descriptor) {
  if (descriptor.ordinal == 0) {
    return Failure("NATIVE_WIRE.PARAMETER.ORDINAL_INVALID");
  }
  if ((descriptor.parameter_flags & ~kParameterFlagsMask) != 0) {
    return Failure("NATIVE_WIRE.PARAMETER.FLAGS_RESERVED_BITS");
  }
  if ((descriptor.metadata_bitmap & ~kWireMetadataBitmapMask) != 0) {
    return Failure("NATIVE_WIRE.RESULT.METADATA_BITMAP_INVALID");
  }
  const auto type_ref = ValidateCanonicalTypeRef(descriptor.type_ref);
  if (!type_ref.ok) { return type_ref; }
  const auto name = ValidateNullableText(descriptor.name);
  if (!name.ok) { return name; }
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::type_name_present)) {
    const auto checked = ValidateNullableText(descriptor.type_name);
    if (!checked.ok) { return checked; }
  }
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::domain_uuid_visible_or_present)) {
    const auto checked = ValidateNullableText(descriptor.domain_name);
    if (!checked.ok) { return checked; }
  }
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::generated_computed_metadata_present)) {
    const auto checked = ValidateNullableText(descriptor.default_expression);
    if (!checked.ok) { return checked; }
  }
  return Ok();
}

DatatypeWireValidationResult ValidateSlotDescriptor(const ParameterBindingSlotDescriptor& descriptor) {
  if (descriptor.ordinal == 0) {
    return Failure("NATIVE_WIRE.PARAMETER.ORDINAL_INVALID");
  }
  if ((descriptor.bind_flags & ~kBindFlagsMask) != 0) {
    return Failure("NATIVE_WIRE.PARAMETER.BIND_FLAGS_RESERVED_BITS");
  }
  const auto type_ref = ValidateCanonicalTypeRef(descriptor.type_ref);
  if (!type_ref.ok) { return type_ref; }
  return ValidateNullableText(descriptor.name);
}

DatatypeWireValidationResult ValidateColumnDescriptor(const ResultColumnDescriptor& column,
                                                       WireResultSetKind result_set_kind) {
  if (column.ordinal == 0) {
    return Failure("NATIVE_WIRE.RESULT.COLUMN_ORDINAL_INVALID");
  }
  if ((column.metadata_bitmap & ~kWireMetadataBitmapMask) != 0) {
    return Failure("NATIVE_WIRE.RESULT.METADATA_BITMAP_INVALID");
  }
  const auto type_ref = ValidateCanonicalTypeRef(column.type_ref);
  if (!type_ref.ok) { return type_ref; }
  const auto display_label = ValidateNullableText(column.display_label);
  if (!display_label.ok) { return display_label; }

  if (result_set_kind == WireResultSetKind::generated_keys) {
    if (column.column_class != WireResultColumnClass::generated_key ||
        !BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::generated_key_discriminator_active)) {
      return Failure("NATIVE_WIRE.RESULT.GENERATED_KEYS_DISCRIMINATOR_MISSING");
    }
  } else if (column.column_class == WireResultColumnClass::generated_key ||
             BitmapSet(column.metadata_bitmap,
                       WireMetadataBitmapBit::generated_key_discriminator_active)) {
    return Failure("NATIVE_WIRE.RESULT.GENERATED_KEYS_DISCRIMINATOR_MISSING");
  }
  if (result_set_kind == WireResultSetKind::out_parameter_row) {
    if (column.column_class != WireResultColumnClass::out_parameter ||
        column.source_parameter_ordinal == 0 ||
        !IsOutDirection(column.parameter_direction) ||
        !BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::out_inout_return_discriminator_active)) {
      return Failure("NATIVE_WIRE.RESULT.OUT_RETURN_PATH_INVALID");
    }
  } else if (column.column_class == WireResultColumnClass::out_parameter ||
             BitmapSet(column.metadata_bitmap,
                       WireMetadataBitmapBit::out_inout_return_discriminator_active)) {
    return Failure("NATIVE_WIRE.RESULT.OUT_RETURN_PATH_INVALID");
  }
  if (result_set_kind == WireResultSetKind::function_return_row &&
      column.column_class != WireResultColumnClass::return_value) {
    return Failure("NATIVE_WIRE.RESULT.OUT_RETURN_PATH_INVALID");
  }
  return Ok();
}

void EncodeParameterDescriptorInto(std::vector<byte>* out, const ParameterDescriptor& descriptor) {
  AppendU32(out, descriptor.ordinal);
  AppendU8(out, static_cast<u8>(descriptor.direction));
  AppendU8(out, static_cast<u8>(descriptor.parameter_class));
  AppendU8(out, static_cast<u8>(descriptor.nullability));
  AppendU8(out, static_cast<u8>(descriptor.value_format));
  AppendU64(out, descriptor.parameter_flags);
  AppendU64(out, descriptor.metadata_bitmap);
  EncodeCanonicalTypeRefInto(out, descriptor.type_ref);
  AppendU8(out, static_cast<u8>(descriptor.default_state));
  AppendU8(out, 0);
  AppendU8(out, 0);
  AppendU8(out, 0);
  EncodeNullableTextInto(out, descriptor.name);
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::type_name_present)) {
    EncodeNullableTextInto(out, descriptor.type_name);
  }
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::domain_uuid_visible_or_present)) {
    EncodeNullableTextInto(out, descriptor.domain_name);
  }
  if (BitmapSet(descriptor.metadata_bitmap, WireMetadataBitmapBit::generated_computed_metadata_present)) {
    EncodeNullableTextInto(out, descriptor.default_expression);
  }
}

bool DecodeParameterDescriptorFrom(Cursor* cursor, ParameterDescriptor* descriptor) {
  u8 direction = 0;
  u8 parameter_class = 0;
  u8 nullability = 0;
  u8 value_format = 0;
  u8 default_state = 0;
  u8 reserved = 0;
  if (!cursor->ReadU32(&descriptor->ordinal) ||
      !cursor->ReadU8(&direction) ||
      !cursor->ReadU8(&parameter_class) ||
      !cursor->ReadU8(&nullability) ||
      !cursor->ReadU8(&value_format) ||
      !cursor->ReadU64(&descriptor->parameter_flags) ||
      !cursor->ReadU64(&descriptor->metadata_bitmap) ||
      !DecodeCanonicalTypeRefFrom(cursor, &descriptor->type_ref) ||
      !cursor->ReadU8(&default_state) ||
      !cursor->ReadU8(&reserved)) {
    return false;
  }
  if (reserved != 0) { cursor->diagnostic_code = "NATIVE_WIRE.PARAMETER.RESERVED_NONZERO"; return false; }
  if (!cursor->ReadU8(&reserved)) { return false; }
  if (reserved != 0) { cursor->diagnostic_code = "NATIVE_WIRE.PARAMETER.RESERVED_NONZERO"; return false; }
  if (!cursor->ReadU8(&reserved)) { return false; }
  if (reserved != 0) { cursor->diagnostic_code = "NATIVE_WIRE.PARAMETER.RESERVED_NONZERO"; return false; }
  descriptor->direction = static_cast<WireParameterDirection>(direction);
  descriptor->parameter_class = static_cast<WireParameterClass>(parameter_class);
  descriptor->nullability = static_cast<WireNullability>(nullability);
  descriptor->value_format = static_cast<WireValueFormat>(value_format);
  descriptor->default_state = static_cast<WireParameterDefaultState>(default_state);
  if (!DecodeNullableTextFrom(cursor, &descriptor->name)) { return false; }
  if (BitmapSet(descriptor->metadata_bitmap, WireMetadataBitmapBit::type_name_present) &&
      !DecodeNullableTextFrom(cursor, &descriptor->type_name)) {
    return false;
  }
  if (BitmapSet(descriptor->metadata_bitmap, WireMetadataBitmapBit::domain_uuid_visible_or_present) &&
      !DecodeNullableTextFrom(cursor, &descriptor->domain_name)) {
    return false;
  }
  if (BitmapSet(descriptor->metadata_bitmap, WireMetadataBitmapBit::generated_computed_metadata_present) &&
      !DecodeNullableTextFrom(cursor, &descriptor->default_expression)) {
    return false;
  }
  return true;
}

void EncodeSlotDescriptorInto(std::vector<byte>* out,
                              const ParameterBindingSlotDescriptor& descriptor) {
  AppendU32(out, descriptor.ordinal);
  AppendU8(out, static_cast<u8>(descriptor.direction));
  AppendU8(out, descriptor.bind_flags);
  AppendU8(out, static_cast<u8>(descriptor.name_binding_state));
  AppendU8(out, static_cast<u8>(descriptor.value_format));
  EncodeCanonicalTypeRefInto(out, descriptor.type_ref);
  AppendI64(out, descriptor.requested_size_bytes);
  EncodeNullableTextInto(out, descriptor.name);
}

bool DecodeSlotDescriptorFrom(Cursor* cursor, ParameterBindingSlotDescriptor* descriptor) {
  u8 direction = 0;
  u8 name_binding_state = 0;
  u8 value_format = 0;
  if (!cursor->ReadU32(&descriptor->ordinal) ||
      !cursor->ReadU8(&direction) ||
      !cursor->ReadU8(&descriptor->bind_flags) ||
      !cursor->ReadU8(&name_binding_state) ||
      !cursor->ReadU8(&value_format) ||
      !DecodeCanonicalTypeRefFrom(cursor, &descriptor->type_ref) ||
      !cursor->ReadI64(&descriptor->requested_size_bytes) ||
      !DecodeNullableTextFrom(cursor, &descriptor->name)) {
    return false;
  }
  descriptor->direction = static_cast<WireParameterDirection>(direction);
  descriptor->name_binding_state = static_cast<WireNameBindingState>(name_binding_state);
  descriptor->value_format = static_cast<WireValueFormat>(value_format);
  return true;
}

void EncodeValueCellInto(std::vector<byte>* out, const ParameterValueCell& cell) {
  AppendU32(out, cell.slot_ordinal);
  AppendU8(out, static_cast<u8>(cell.value_state));
  AppendU8(out, static_cast<u8>(cell.payload_encoding));
  AppendU16(out, cell.payload_flags);
  AppendU64(out, CheckedU64Size(cell.payload.size()));
  AppendBytes(out, cell.payload.data(), cell.payload.size());
}

bool DecodeValueCellFrom(Cursor* cursor, ParameterValueCell* cell) {
  u8 value_state = 0;
  u8 payload_encoding = 0;
  u64 payload_size = 0;
  if (!cursor->ReadU32(&cell->slot_ordinal) ||
      !cursor->ReadU8(&value_state) ||
      !cursor->ReadU8(&payload_encoding) ||
      !cursor->ReadU16(&cell->payload_flags) ||
      !cursor->ReadU64(&payload_size)) {
    return false;
  }
  if (!cursor->Remaining(payload_size)) {
    cursor->diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED";
    return false;
  }
  cell->value_state = static_cast<WireValueState>(value_state);
  cell->payload_encoding = static_cast<WirePayloadEncoding>(payload_encoding);
  cell->payload.resize(static_cast<std::size_t>(payload_size));
  if (payload_size != 0 && !cursor->ReadBytes(cell->payload.data(), payload_size)) {
    return false;
  }
  return true;
}

void EncodeColumnDescriptorInto(std::vector<byte>* out, const ResultColumnDescriptor& column) {
  AppendU32(out, column.ordinal);
  AppendU8(out, static_cast<u8>(column.column_class));
  AppendU8(out, static_cast<u8>(column.value_format));
  AppendU8(out, static_cast<u8>(column.nullability));
  AppendU8(out, static_cast<u8>(column.updatability));
  AppendU64(out, column.metadata_bitmap);
  EncodeCanonicalTypeRefInto(out, column.type_ref);
  AppendUuid(out, column.source_table_uuid);
  AppendUuid(out, column.source_column_uuid);
  AppendUuid(out, column.source_generated_object_uuid);
  AppendU32(out, column.source_parameter_ordinal);
  AppendU8(out, static_cast<u8>(column.parameter_direction));
  AppendU8(out, static_cast<u8>(column.generated_value_kind));
  AppendU16(out, 0);
  EncodeNullableTextInto(out, column.display_label);
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::column_name_present)) {
    EncodeNullableTextInto(out, column.column_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::base_catalog_name_present)) {
    EncodeNullableTextInto(out, column.base_catalog_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::base_schema_name_present)) {
    EncodeNullableTextInto(out, column.base_schema_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::base_table_name_present)) {
    EncodeNullableTextInto(out, column.base_table_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::base_column_name_present)) {
    EncodeNullableTextInto(out, column.base_column_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::type_name_present)) {
    EncodeNullableTextInto(out, column.type_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::domain_uuid_visible_or_present)) {
    EncodeNullableTextInto(out, column.domain_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::driver_type_name_present)) {
    EncodeNullableTextInto(out, column.driver_type_name);
  }
  if (BitmapSet(column.metadata_bitmap, WireMetadataBitmapBit::remarks_present)) {
    EncodeNullableTextInto(out, column.remarks);
  }
}

bool DecodeColumnDescriptorFrom(Cursor* cursor, ResultColumnDescriptor* column) {
  u8 column_class = 0;
  u8 value_format = 0;
  u8 nullability = 0;
  u8 updatability = 0;
  u8 parameter_direction = 0;
  u8 generated_value_kind = 0;
  u16 reserved = 0;
  if (!cursor->ReadU32(&column->ordinal) ||
      !cursor->ReadU8(&column_class) ||
      !cursor->ReadU8(&value_format) ||
      !cursor->ReadU8(&nullability) ||
      !cursor->ReadU8(&updatability) ||
      !cursor->ReadU64(&column->metadata_bitmap) ||
      !DecodeCanonicalTypeRefFrom(cursor, &column->type_ref) ||
      !cursor->ReadBytes(column->source_table_uuid.data(), column->source_table_uuid.size()) ||
      !cursor->ReadBytes(column->source_column_uuid.data(), column->source_column_uuid.size()) ||
      !cursor->ReadBytes(column->source_generated_object_uuid.data(),
                         column->source_generated_object_uuid.size()) ||
      !cursor->ReadU32(&column->source_parameter_ordinal) ||
      !cursor->ReadU8(&parameter_direction) ||
      !cursor->ReadU8(&generated_value_kind) ||
      !cursor->ReadU16(&reserved)) {
    return false;
  }
  if (reserved != 0) {
    cursor->diagnostic_code = "NATIVE_WIRE.RESULT.RESERVED_NONZERO";
    return false;
  }
  column->column_class = static_cast<WireResultColumnClass>(column_class);
  column->value_format = static_cast<WireValueFormat>(value_format);
  column->nullability = static_cast<WireNullability>(nullability);
  column->updatability = static_cast<WireResultUpdatability>(updatability);
  column->parameter_direction = static_cast<WireParameterDirection>(parameter_direction);
  column->generated_value_kind = static_cast<WireGeneratedValueKind>(generated_value_kind);
  if (!DecodeNullableTextFrom(cursor, &column->display_label)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::column_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->column_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::base_catalog_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->base_catalog_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::base_schema_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->base_schema_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::base_table_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->base_table_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::base_column_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->base_column_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::type_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->type_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::domain_uuid_visible_or_present) &&
      !DecodeNullableTextFrom(cursor, &column->domain_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::driver_type_name_present) &&
      !DecodeNullableTextFrom(cursor, &column->driver_type_name)) { return false; }
  if (BitmapSet(column->metadata_bitmap, WireMetadataBitmapBit::remarks_present) &&
      !DecodeNullableTextFrom(cursor, &column->remarks)) { return false; }
  return true;
}

}  // namespace

bool WireUuidIsZero(const WireUuidBytes& value) { return ArrayIsZero(value); }

const char* CanonicalWireTypeFamilyName(CanonicalWireTypeFamily family) {
  switch (family) {
    case CanonicalWireTypeFamily::unknown_untyped: return "unknown_untyped";
    case CanonicalWireTypeFamily::boolean: return "boolean";
    case CanonicalWireTypeFamily::signed_integer: return "signed_integer";
    case CanonicalWireTypeFamily::unsigned_integer: return "unsigned_integer";
    case CanonicalWireTypeFamily::exact_numeric: return "exact_numeric";
    case CanonicalWireTypeFamily::decimal_floating: return "decimal_floating";
    case CanonicalWireTypeFamily::approximate_numeric: return "approximate_numeric";
    case CanonicalWireTypeFamily::money_currency: return "money_currency";
    case CanonicalWireTypeFamily::text: return "text";
    case CanonicalWireTypeFamily::binary: return "binary";
    case CanonicalWireTypeFamily::bit_string: return "bit_string";
    case CanonicalWireTypeFamily::temporal: return "temporal";
    case CanonicalWireTypeFamily::interval: return "interval";
    case CanonicalWireTypeFamily::uuid: return "uuid";
    case CanonicalWireTypeFamily::lob: return "lob";
    case CanonicalWireTypeFamily::array_list: return "array_list";
    case CanonicalWireTypeFamily::map_row_composite: return "map_row_composite";
    case CanonicalWireTypeFamily::range_multirange: return "range_multirange";
    case CanonicalWireTypeFamily::enum_set: return "enum_set";
    case CanonicalWireTypeFamily::network: return "network";
    case CanonicalWireTypeFamily::document: return "document";
    case CanonicalWireTypeFamily::spatial: return "spatial";
    case CanonicalWireTypeFamily::vector: return "vector";
    case CanonicalWireTypeFamily::time_series: return "time_series";
    case CanonicalWireTypeFamily::graph: return "graph";
    case CanonicalWireTypeFamily::sketch: return "sketch";
    case CanonicalWireTypeFamily::locator: return "locator";
    case CanonicalWireTypeFamily::opaque: return "opaque";
    case CanonicalWireTypeFamily::user_defined_domain: return "user_defined_domain";
    case CanonicalWireTypeFamily::cursor_rowset_table_value: return "cursor_rowset_table_value";
    case CanonicalWireTypeFamily::extension: return "extension";
  }
  return "unknown";
}

const char* WireValueStateName(WireValueState state) {
  switch (state) {
    case WireValueState::value_present: return "value_present";
    case WireValueState::sql_null: return "sql_null";
    case WireValueState::default_requested: return "default_requested";
    case WireValueState::out_unbound: return "out_unbound";
    case WireValueState::lob_handle: return "lob_handle";
    case WireValueState::structured_value: return "structured_value";
    case WireValueState::protected_value: return "protected_value";
    case WireValueState::error_value: return "error_value";
  }
  return "unknown";
}

const char* WireResultSetKindName(WireResultSetKind kind) {
  switch (kind) {
    case WireResultSetKind::ordinary_rowset: return "ordinary_rowset";
    case WireResultSetKind::generated_keys: return "generated_keys";
    case WireResultSetKind::out_parameter_row: return "out_parameter_row";
    case WireResultSetKind::function_return_row: return "function_return_row";
    case WireResultSetKind::metadata_only: return "metadata_only";
    case WireResultSetKind::cursor_rowset: return "cursor_rowset";
  }
  return "unknown";
}

const char* WireParameterDirectionName(WireParameterDirection direction) {
  switch (direction) {
    case WireParameterDirection::in: return "in";
    case WireParameterDirection::out: return "out";
    case WireParameterDirection::inout: return "inout";
    case WireParameterDirection::return_value: return "return";
    case WireParameterDirection::variadic: return "variadic";
  }
  return "unknown";
}

CanonicalWireTypeId WireTypeIdForCanonicalTypeId(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::null_type:
    case CanonicalTypeId::unknown:
      return {};
    case CanonicalTypeId::boolean:
      return WireId(CanonicalWireTypeFamily::boolean, 1);
    case CanonicalTypeId::int8:
      return WireId(CanonicalWireTypeFamily::signed_integer, 1);
    case CanonicalTypeId::int16:
      return WireId(CanonicalWireTypeFamily::signed_integer, 2);
    case CanonicalTypeId::int32:
      return WireId(CanonicalWireTypeFamily::signed_integer, 3);
    case CanonicalTypeId::int64:
      return WireId(CanonicalWireTypeFamily::signed_integer, 4);
    case CanonicalTypeId::int128:
      return WireId(CanonicalWireTypeFamily::signed_integer,
                    5,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::exact_numeric_rendering));
    case CanonicalTypeId::uint8:
      return WireId(CanonicalWireTypeFamily::unsigned_integer,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::unsigned_numeric_rendering));
    case CanonicalTypeId::uint16:
      return WireId(CanonicalWireTypeFamily::unsigned_integer,
                    2,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::unsigned_numeric_rendering));
    case CanonicalTypeId::uint32:
      return WireId(CanonicalWireTypeFamily::unsigned_integer,
                    3,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::unsigned_numeric_rendering));
    case CanonicalTypeId::uint64:
      return WireId(CanonicalWireTypeFamily::unsigned_integer,
                    4,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::unsigned_numeric_rendering));
    case CanonicalTypeId::uint128:
      return WireId(CanonicalWireTypeFamily::unsigned_integer,
                    5,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::unsigned_numeric_rendering) |
                        CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::exact_numeric_rendering));
    case CanonicalTypeId::bfloat16:
      return WireId(CanonicalWireTypeFamily::approximate_numeric,
                    5,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::approximate_numeric_rendering));
    case CanonicalTypeId::real16:
      return WireId(CanonicalWireTypeFamily::approximate_numeric,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::approximate_numeric_rendering));
    case CanonicalTypeId::real32:
      return WireId(CanonicalWireTypeFamily::approximate_numeric,
                    2,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::approximate_numeric_rendering));
    case CanonicalTypeId::real64:
      return WireId(CanonicalWireTypeFamily::approximate_numeric,
                    3,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::approximate_numeric_rendering));
    case CanonicalTypeId::real128:
      return WireId(CanonicalWireTypeFamily::approximate_numeric,
                    4,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::approximate_numeric_rendering));
    case CanonicalTypeId::decimal:
      return WireId(CanonicalWireTypeFamily::exact_numeric,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::exact_numeric_rendering));
    case CanonicalTypeId::decimal_float:
      return WireId(CanonicalWireTypeFamily::decimal_floating, 1);
    case CanonicalTypeId::uuid:
      return WireId(CanonicalWireTypeFamily::uuid, 1);
    case CanonicalTypeId::ip_address:
      return WireId(CanonicalWireTypeFamily::network, 1);
    case CanonicalTypeId::network_prefix:
      return WireId(CanonicalWireTypeFamily::network, 2);
    case CanonicalTypeId::mac_address:
      return WireId(CanonicalWireTypeFamily::network, 3);
    case CanonicalTypeId::character:
      return WireId(CanonicalWireTypeFamily::text, 1);
    case CanonicalTypeId::binary:
      return WireId(CanonicalWireTypeFamily::binary, 1);
    case CanonicalTypeId::bit_string:
      return WireId(CanonicalWireTypeFamily::bit_string, 1);
    case CanonicalTypeId::date:
      return WireId(CanonicalWireTypeFamily::temporal, 1);
    case CanonicalTypeId::time:
      return WireId(CanonicalWireTypeFamily::temporal, 2);
    case CanonicalTypeId::timestamp:
      return WireId(CanonicalWireTypeFamily::temporal, 3);
    case CanonicalTypeId::interval:
      return WireId(CanonicalWireTypeFamily::interval, 1);
    case CanonicalTypeId::blob:
      return WireId(CanonicalWireTypeFamily::lob,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::lob_locator_metadata_required));
    case CanonicalTypeId::document:
      return WireId(CanonicalWireTypeFamily::document, 1);
    case CanonicalTypeId::json_document:
      return WireId(CanonicalWireTypeFamily::document, 2);
    case CanonicalTypeId::binary_json_document:
      return WireId(CanonicalWireTypeFamily::document, 3);
    case CanonicalTypeId::bson_document:
      return WireId(CanonicalWireTypeFamily::document, 4);
    case CanonicalTypeId::xml_document:
      return WireId(CanonicalWireTypeFamily::document, 5);
    case CanonicalTypeId::hstore_document:
      return WireId(CanonicalWireTypeFamily::document, 6);
    case CanonicalTypeId::object_document:
      return WireId(CanonicalWireTypeFamily::document, 7);
    case CanonicalTypeId::flattened_object_document:
      return WireId(CanonicalWireTypeFamily::document, 8);
    case CanonicalTypeId::enum_value:
      return WireId(CanonicalWireTypeFamily::enum_set, 1);
    case CanonicalTypeId::set_value:
      return WireId(CanonicalWireTypeFamily::enum_set, 2);
    case CanonicalTypeId::array:
      return WireId(CanonicalWireTypeFamily::array_list,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::container_element_metadata_required));
    case CanonicalTypeId::list:
      return WireId(CanonicalWireTypeFamily::array_list,
                    2,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::container_element_metadata_required));
    case CanonicalTypeId::map:
      return WireId(CanonicalWireTypeFamily::map_row_composite,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::container_element_metadata_required));
    case CanonicalTypeId::row:
      return WireId(CanonicalWireTypeFamily::map_row_composite, 2);
    case CanonicalTypeId::composite:
      return WireId(CanonicalWireTypeFamily::map_row_composite, 3);
    case CanonicalTypeId::variant:
      return WireId(CanonicalWireTypeFamily::map_row_composite, 4);
    case CanonicalTypeId::range:
      return WireId(CanonicalWireTypeFamily::range_multirange,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::container_element_metadata_required));
    case CanonicalTypeId::multirange:
      return WireId(CanonicalWireTypeFamily::range_multirange,
                    2,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::container_element_metadata_required));
    case CanonicalTypeId::geometry:
      return WireId(CanonicalWireTypeFamily::spatial, 1);
    case CanonicalTypeId::geography:
      return WireId(CanonicalWireTypeFamily::spatial, 2);
    case CanonicalTypeId::point:
      return WireId(CanonicalWireTypeFamily::spatial, 3);
    case CanonicalTypeId::shape:
      return WireId(CanonicalWireTypeFamily::spatial, 4);
    case CanonicalTypeId::raster:
      return WireId(CanonicalWireTypeFamily::spatial, 5);
    case CanonicalTypeId::vector:
      return WireId(CanonicalWireTypeFamily::vector, 1);
    case CanonicalTypeId::dense_vector:
      return WireId(CanonicalWireTypeFamily::vector, 2);
    case CanonicalTypeId::sparse_vector:
      return WireId(CanonicalWireTypeFamily::vector, 3);
    case CanonicalTypeId::binary_vector:
      return WireId(CanonicalWireTypeFamily::vector, 4);
    case CanonicalTypeId::quantized_vector:
      return WireId(CanonicalWireTypeFamily::vector, 5);
    case CanonicalTypeId::graph_node:
      return WireId(CanonicalWireTypeFamily::graph, 1);
    case CanonicalTypeId::graph_edge:
      return WireId(CanonicalWireTypeFamily::graph, 2);
    case CanonicalTypeId::graph_path:
      return WireId(CanonicalWireTypeFamily::graph, 3);
    case CanonicalTypeId::time_series_value:
      return WireId(CanonicalWireTypeFamily::time_series, 1);
    case CanonicalTypeId::hll_sketch:
      return WireId(CanonicalWireTypeFamily::sketch, 1);
    case CanonicalTypeId::bloom_filter:
      return WireId(CanonicalWireTypeFamily::sketch, 2);
    case CanonicalTypeId::quantile_sketch:
      return WireId(CanonicalWireTypeFamily::sketch, 3);
    case CanonicalTypeId::histogram_sketch:
      return WireId(CanonicalWireTypeFamily::sketch, 4);
    case CanonicalTypeId::ranking_summary:
      return WireId(CanonicalWireTypeFamily::sketch, 5);
    case CanonicalTypeId::vector_summary:
      return WireId(CanonicalWireTypeFamily::sketch, 6);
    case CanonicalTypeId::lob_locator:
      return WireId(CanonicalWireTypeFamily::locator,
                    1,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::lob_locator_metadata_required));
    case CanonicalTypeId::external_file_locator:
      return WireId(CanonicalWireTypeFamily::locator, 2);
    case CanonicalTypeId::remote_object_locator:
      return WireId(CanonicalWireTypeFamily::locator, 3);
    case CanonicalTypeId::bridge_handle:
      return WireId(CanonicalWireTypeFamily::locator, 4);
    case CanonicalTypeId::cursor_handle:
      return WireId(CanonicalWireTypeFamily::locator, 5);
    case CanonicalTypeId::system_reference:
      return WireId(CanonicalWireTypeFamily::locator, 6);
    case CanonicalTypeId::opaque_extension:
      return WireId(CanonicalWireTypeFamily::opaque, 1);
    case CanonicalTypeId::cursor:
      return WireId(CanonicalWireTypeFamily::cursor_rowset_table_value, 1);
    case CanonicalTypeId::result_set:
      return WireId(CanonicalWireTypeFamily::cursor_rowset_table_value, 2);
    case CanonicalTypeId::table_value:
      return WireId(CanonicalWireTypeFamily::cursor_rowset_table_value, 3);
    case CanonicalTypeId::token_stream:
      return WireId(CanonicalWireTypeFamily::extension, 101,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::search_query:
      return WireId(CanonicalWireTypeFamily::extension, 102,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::search_rank_feature:
      return WireId(CanonicalWireTypeFamily::extension, 103,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::search_completion:
      return WireId(CanonicalWireTypeFamily::extension, 104,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::search_percolator:
      return WireId(CanonicalWireTypeFamily::extension, 105,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::columnar_segment:
      return WireId(CanonicalWireTypeFamily::extension, 201,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
    case CanonicalTypeId::aggregate_state:
      return WireId(CanonicalWireTypeFamily::extension, 202,
                    CanonicalWireTypeFlagBit(CanonicalWireTypeFlag::extension_backed));
  }
  return {};
}

CanonicalTypeId CanonicalTypeIdFromWireTypeId(const CanonicalWireTypeId& wire_type_id) {
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    const auto candidate = WireTypeIdForCanonicalTypeId(descriptor.type_id);
    if (candidate.type_family == wire_type_id.type_family &&
        candidate.type_code == wire_type_id.type_code &&
        candidate.type_version == wire_type_id.type_version) {
      return descriptor.type_id;
    }
  }
  if (wire_type_id.type_family == 0 && wire_type_id.type_code == 0 &&
      wire_type_id.type_version == 0 && wire_type_id.type_flags == 0) {
    return CanonicalTypeId::unknown;
  }
  return CanonicalTypeId::unknown;
}

std::string CanonicalWireTypeCodeName(const CanonicalWireTypeId& wire_type_id) {
  const auto type_id = CanonicalTypeIdFromWireTypeId(wire_type_id);
  if (type_id == CanonicalTypeId::unknown) {
    if (wire_type_id.type_family == 0 && wire_type_id.type_code == 0) {
      return "UNKNOWN_UNTYPED";
    }
    return "extension_or_unknown";
  }
  return CanonicalTypeName(type_id);
}

std::array<byte, kCanonicalWireTypeIdBytes> EncodeCanonicalWireTypeId(
    const CanonicalWireTypeId& type_id) {
  std::array<byte, kCanonicalWireTypeIdBytes> encoded{};
  StoreLittle16(encoded.data(), type_id.type_family);
  StoreLittle16(encoded.data() + 2, type_id.type_code);
  StoreLittle16(encoded.data() + 4, type_id.type_version);
  StoreLittle16(encoded.data() + 6, type_id.type_flags);
  return encoded;
}

CanonicalWireTypeIdDecodeResult DecodeCanonicalWireTypeId(const byte* data, u64 size) {
  CanonicalWireTypeIdDecodeResult result;
  if (data == nullptr || size < kCanonicalWireTypeIdBytes) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED";
    return result;
  }
  result.type_id.type_family = LoadLittle16(data);
  result.type_id.type_code = LoadLittle16(data + 2);
  result.type_id.type_version = LoadLittle16(data + 4);
  result.type_id.type_flags = LoadLittle16(data + 6);
  const auto checked = ValidateCanonicalWireTypeId(result.type_id);
  if (!checked.ok) {
    result.ok = false;
    result.diagnostic_code = checked.diagnostic_code;
    result.diagnostic_detail = checked.diagnostic_detail;
  }
  return result;
}

DatatypeWireValidationResult ValidateCanonicalWireTypeId(const CanonicalWireTypeId& type_id) {
  if ((type_id.type_flags & ~kCanonicalWireTypeFlagMask) != 0) {
    return Failure("NATIVE_WIRE.TYPE.CANONICAL_ID_RESERVED_BITS");
  }
  if (type_id.type_family == 0 || type_id.type_code == 0 || type_id.type_version == 0) {
    const bool unknown_untyped = type_id.type_family == 0 && type_id.type_code == 0 &&
                                 type_id.type_version == 0 && type_id.type_flags == 0;
    if (!unknown_untyped) {
      return Failure("NATIVE_WIRE.TYPE.CANONICAL_ID_INVALID");
    }
  }
  return Ok();
}

std::array<byte, kCanonicalTypeRefBytes> EncodeCanonicalTypeRef(const CanonicalTypeRef& type_ref) {
  std::vector<byte> encoded;
  encoded.reserve(kCanonicalTypeRefBytes);
  EncodeCanonicalTypeRefInto(&encoded, type_ref);
  std::array<byte, kCanonicalTypeRefBytes> fixed{};
  std::copy(encoded.begin(), encoded.end(), fixed.begin());
  return fixed;
}

CanonicalTypeRefDecodeResult DecodeCanonicalTypeRef(const byte* data, u64 size) {
  CanonicalTypeRefDecodeResult result;
  if (data == nullptr || size < kCanonicalTypeRefBytes) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.DECODE.TRUNCATED";
    return result;
  }
  Cursor cursor{data, size, 0};
  if (!DecodeCanonicalTypeRefFrom(&cursor, &result.type_ref)) {
    result.ok = false;
    result.diagnostic_code = cursor.diagnostic_code;
    return result;
  }
  const auto checked = ValidateCanonicalTypeRef(result.type_ref);
  if (!checked.ok) {
    result.ok = false;
    result.diagnostic_code = checked.diagnostic_code;
    result.diagnostic_detail = checked.diagnostic_detail;
  }
  return result;
}

DatatypeWireValidationResult ValidateCanonicalTypeRef(const CanonicalTypeRef& type_ref) {
  const auto type_check = ValidateCanonicalWireTypeId(type_ref.canonical_type_id);
  if (!type_check.ok) { return type_check; }
  if ((type_ref.modifier_bitmap & ~kCanonicalTypeRefModifierMask) != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_RESERVED_BITS");
  }
  if (type_ref.reserved != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_RESERVED_BITS", "reserved");
  }

  const bool redacted = ModifierSet(type_ref, CanonicalTypeRefModifier::policy_redacted);
  if (IsConcreteTypeId(type_ref.canonical_type_id) && !redacted &&
      WireUuidIsZero(type_ref.descriptor_uuid)) {
    return Failure("NATIVE_WIRE.TYPE.DESCRIPTOR_UUID_MISSING");
  }

  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::precision) && type_ref.precision != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "precision");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::scale) && type_ref.scale != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "scale");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::datetime_precision) &&
      type_ref.datetime_precision != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "datetime_precision");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::length_chars) && type_ref.length_chars != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "length_chars");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::length_bytes) && type_ref.length_bytes != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "length_bytes");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::charset_uuid) &&
      !WireUuidIsZero(type_ref.charset_uuid)) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "charset_uuid");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::collation_uuid) &&
      !WireUuidIsZero(type_ref.collation_uuid)) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "collation_uuid");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::domain_uuid) &&
      !WireUuidIsZero(type_ref.domain_uuid)) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "domain_uuid");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::element_descriptor_uuid) &&
      !WireUuidIsZero(type_ref.element_descriptor_uuid)) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "element_descriptor_uuid");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::array_rank) && type_ref.array_rank != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "array_rank");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::timezone_policy) &&
      type_ref.timezone_policy != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "timezone_policy");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::calendar_policy) &&
      type_ref.calendar_policy != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "calendar_policy");
  }
  if (!ModifierSet(type_ref, CanonicalTypeRefModifier::numeric_locale_policy) &&
      type_ref.numeric_locale_policy != 0) {
    return Failure("NATIVE_WIRE.TYPE.MODIFIER_FIELD_WITHOUT_BIT", "numeric_locale_policy");
  }
  return Ok();
}

DatatypeWireValidationResult ValidateNullableText(const NullableText& text) {
  switch (text.state) {
    case WireNullableTextState::absent_not_applicable:
    case WireNullableTextState::hidden_by_policy:
    case WireNullableTextState::unknown_by_policy:
      if (!text.text.empty()) {
        return Failure("NATIVE_WIRE.TEXT.NULLABLE_STATE_LENGTH_INVALID");
      }
      return Ok();
    case WireNullableTextState::present:
      return Ok();
  }
  return Failure("NATIVE_WIRE.TEXT.NULLABLE_STATE_INVALID");
}

DatatypeWireValidationResult ValidateParameterValueCell(WireParameterDirection direction,
                                                        const ParameterValueCell& cell) {
  if (cell.slot_ordinal == 0) {
    return Failure("NATIVE_WIRE.PARAMETER.ORDINAL_INVALID");
  }
  if ((cell.payload_flags & ~kPayloadFlagsMask) != 0) {
    return Failure("NATIVE_WIRE.PARAMETER.PAYLOAD_RESERVED_BITS");
  }
  const bool payload_empty = cell.payload.empty();
  switch (cell.value_state) {
    case WireValueState::value_present:
      if (direction == WireParameterDirection::out) {
        return Failure("NATIVE_WIRE.PARAMETER.OUT_BIND_HAS_VALUE");
      }
      return Ok();
    case WireValueState::sql_null:
    case WireValueState::default_requested:
    case WireValueState::out_unbound:
      if (!payload_empty) {
        return Failure("NATIVE_WIRE.VALUE.NULL_EMPTY_AMBIGUOUS");
      }
      break;
    case WireValueState::lob_handle:
    case WireValueState::structured_value:
    case WireValueState::protected_value:
      if (direction == WireParameterDirection::out) {
        return Failure("NATIVE_WIRE.PARAMETER.OUT_BIND_HAS_VALUE");
      }
      return Ok();
    case WireValueState::error_value:
      return Failure("NATIVE_WIRE.PARAMETER.VALUE_STATE_INVALID");
  }
  if (direction == WireParameterDirection::out) {
    if (cell.value_state != WireValueState::out_unbound) {
      return Failure("NATIVE_WIRE.PARAMETER.VALUE_STATE_INVALID");
    }
    return Ok();
  }
  if (direction == WireParameterDirection::return_value) {
    return Failure("NATIVE_WIRE.PARAMETER.VALUE_STATE_INVALID");
  }
  return Ok();
}

DatatypeWireValidationResult ValidateParameterDataPacket(const ParameterDataPacket& packet) {
  if (packet.layout_version != kNativeWireMetadataLayoutVersion) {
    return Failure("NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED");
  }
  if (packet.bind_shape != WireParameterBindShape::array_bind_rows &&
      packet.row_value_frames.size() != 1) {
    return Failure("NATIVE_WIRE.PARAMETER.ROW_COUNT_INVALID");
  }
  if (packet.bind_shape == WireParameterBindShape::array_bind_rows &&
      packet.row_value_frames.empty()) {
    return Failure("NATIVE_WIRE.PARAMETER.ROW_COUNT_INVALID");
  }
  if (packet.flags != 0) {
    return Failure("NATIVE_WIRE.PARAMETER.FLAGS_RESERVED_BITS");
  }
  for (const auto& slot : packet.slot_descriptors) {
    const auto checked = ValidateSlotDescriptor(slot);
    if (!checked.ok) { return checked; }
  }
  for (const auto& row : packet.row_value_frames) {
    if (row.values.size() != packet.slot_descriptors.size()) {
      return Failure("NATIVE_WIRE.PARAMETER.VALUE_STATE_INVALID", "value_count");
    }
    for (const auto& value : row.values) {
      auto slot = std::find_if(packet.slot_descriptors.begin(),
                               packet.slot_descriptors.end(),
                               [&value](const ParameterBindingSlotDescriptor& candidate) {
                                 return candidate.ordinal == value.slot_ordinal;
                               });
      if (slot == packet.slot_descriptors.end()) {
        return Failure("NATIVE_WIRE.PARAMETER.VALUE_STATE_INVALID", "slot_ordinal");
      }
      const auto checked = ValidateParameterValueCell(slot->direction, value);
      if (!checked.ok) { return checked; }
    }
  }
  return Ok();
}

DatatypeWireValidationResult ValidateRowDescriptionPacket(const RowDescriptionPacket& packet) {
  if (packet.layout_version != kNativeWireMetadataLayoutVersion) {
    return Failure("NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED");
  }
  if ((packet.flags & ~kRowDescriptionFlagsMask) != 0) {
    return Failure("NATIVE_WIRE.RESULT.METADATA_BITMAP_INVALID", "header_flags");
  }
  for (const auto& column : packet.columns) {
    const auto checked = ValidateColumnDescriptor(column, packet.result_set_kind);
    if (!checked.ok) { return checked; }
  }
  return Ok();
}

DatatypeWireEncodeResult EncodeParameterDescriptionPacket(const ParameterDescriptionPacket& packet) {
  if (packet.layout_version != kNativeWireMetadataLayoutVersion) {
    return EncodeFailure("NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED");
  }
  if ((packet.flags & ~kParameterDescriptionFlagsMask) != 0) {
    return EncodeFailure("NATIVE_WIRE.PARAMETER.FLAGS_RESERVED_BITS");
  }
  for (const auto& parameter : packet.parameters) {
    const auto checked = ValidateParameterDescriptor(parameter);
    if (!checked.ok) { return EncodeFailure(checked.diagnostic_code, checked.diagnostic_detail); }
  }

  DatatypeWireEncodeResult result;
  AppendU16(&result.bytes, packet.layout_version);
  AppendU8(&result.bytes, static_cast<u8>(packet.describe_kind));
  AppendU8(&result.bytes, packet.flags);
  AppendUuid(&result.bytes, packet.statement_uuid);
  AppendUuid(&result.bytes, packet.signature_uuid);
  AppendHash(&result.bytes, packet.descriptor_snapshot_hash);
  AppendU32(&result.bytes, CheckedU32Size(packet.parameters.size()));
  for (const auto& parameter : packet.parameters) {
    EncodeParameterDescriptorInto(&result.bytes, parameter);
  }
  return result;
}

ParameterDescriptionDecodeResult DecodeParameterDescriptionPacket(const std::vector<byte>& bytes) {
  ParameterDescriptionDecodeResult result;
  Cursor cursor{bytes.data(), CheckedU64Size(bytes.size()), 0};
  u8 describe_kind = 0;
  u32 parameter_count = 0;
  if (!cursor.ReadU16(&result.packet.layout_version) ||
      !cursor.ReadU8(&describe_kind) ||
      !cursor.ReadU8(&result.packet.flags) ||
      !cursor.ReadBytes(result.packet.statement_uuid.data(), result.packet.statement_uuid.size()) ||
      !cursor.ReadBytes(result.packet.signature_uuid.data(), result.packet.signature_uuid.size()) ||
      !cursor.ReadBytes(result.packet.descriptor_snapshot_hash.data(),
                        result.packet.descriptor_snapshot_hash.size()) ||
      !cursor.ReadU32(&parameter_count)) {
    result.ok = false;
    result.diagnostic_code = cursor.diagnostic_code;
    return result;
  }
  result.packet.describe_kind = static_cast<WireParameterDescribeKind>(describe_kind);
  for (u32 index = 0; index < parameter_count; ++index) {
    ParameterDescriptor descriptor;
    if (!DecodeParameterDescriptorFrom(&cursor, &descriptor)) {
      result.ok = false;
      result.diagnostic_code = cursor.diagnostic_code;
      return result;
    }
    result.packet.parameters.push_back(std::move(descriptor));
  }
  if (cursor.offset != cursor.size) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.DECODE.TRAILING_BYTES";
    return result;
  }
  const auto encoded = EncodeParameterDescriptionPacket(result.packet);
  if (!encoded.ok) {
    result.ok = false;
    result.diagnostic_code = encoded.diagnostic_code;
    result.diagnostic_detail = encoded.diagnostic_detail;
  }
  return result;
}

DatatypeWireEncodeResult EncodeParameterDataPacket(const ParameterDataPacket& packet) {
  const auto checked = ValidateParameterDataPacket(packet);
  if (!checked.ok) { return EncodeFailure(checked.diagnostic_code, checked.diagnostic_detail); }

  std::vector<byte> slots;
  for (const auto& slot : packet.slot_descriptors) {
    EncodeSlotDescriptorInto(&slots, slot);
  }

  DatatypeWireEncodeResult result;
  AppendU16(&result.bytes, packet.layout_version);
  AppendU8(&result.bytes, static_cast<u8>(packet.packet_kind));
  AppendU8(&result.bytes, static_cast<u8>(packet.bind_shape));
  AppendU32(&result.bytes, packet.flags);
  AppendUuid(&result.bytes, packet.statement_uuid);
  AppendUuid(&result.bytes, packet.portal_uuid);
  AppendU32(&result.bytes, CheckedU32Size(packet.slot_descriptors.size()));
  AppendU32(&result.bytes, CheckedU32Size(packet.row_value_frames.size()));
  AppendU32(&result.bytes, CheckedU32Size(slots.size()));
  AppendU32(&result.bytes, 0);
  AppendHash(&result.bytes, packet.descriptor_snapshot_hash);
  AppendUuid(&result.bytes, packet.redaction_map_uuid);
  AppendBytes(&result.bytes, slots.data(), slots.size());
  for (const auto& row : packet.row_value_frames) {
    AppendU32(&result.bytes, row.row_ordinal);
    AppendU32(&result.bytes, CheckedU32Size(row.values.size()));
    for (const auto& value : row.values) {
      EncodeValueCellInto(&result.bytes, value);
    }
  }
  return result;
}

ParameterDataPacketDecodeResult DecodeParameterDataPacket(const std::vector<byte>& bytes) {
  ParameterDataPacketDecodeResult result;
  Cursor cursor{bytes.data(), CheckedU64Size(bytes.size()), 0};
  u8 packet_kind = 0;
  u8 bind_shape = 0;
  u32 parameter_count = 0;
  u32 row_count = 0;
  u32 slot_descriptor_bytes = 0;
  u32 reserved = 0;
  if (!cursor.ReadU16(&result.packet.layout_version) ||
      !cursor.ReadU8(&packet_kind) ||
      !cursor.ReadU8(&bind_shape) ||
      !cursor.ReadU32(&result.packet.flags) ||
      !cursor.ReadBytes(result.packet.statement_uuid.data(), result.packet.statement_uuid.size()) ||
      !cursor.ReadBytes(result.packet.portal_uuid.data(), result.packet.portal_uuid.size()) ||
      !cursor.ReadU32(&parameter_count) ||
      !cursor.ReadU32(&row_count) ||
      !cursor.ReadU32(&slot_descriptor_bytes) ||
      !cursor.ReadU32(&reserved) ||
      !cursor.ReadBytes(result.packet.descriptor_snapshot_hash.data(),
                        result.packet.descriptor_snapshot_hash.size()) ||
      !cursor.ReadBytes(result.packet.redaction_map_uuid.data(),
                        result.packet.redaction_map_uuid.size())) {
    result.ok = false;
    result.diagnostic_code = cursor.diagnostic_code;
    return result;
  }
  if (reserved != 0) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.PARAMETER.RESERVED_NONZERO";
    return result;
  }
  result.packet.packet_kind = static_cast<WireParameterPacketKind>(packet_kind);
  result.packet.bind_shape = static_cast<WireParameterBindShape>(bind_shape);
  const u64 slot_end = cursor.offset + slot_descriptor_bytes;
  for (u32 index = 0; index < parameter_count; ++index) {
    ParameterBindingSlotDescriptor slot;
    if (!DecodeSlotDescriptorFrom(&cursor, &slot)) {
      result.ok = false;
      result.diagnostic_code = cursor.diagnostic_code;
      return result;
    }
    result.packet.slot_descriptors.push_back(std::move(slot));
  }
  if (cursor.offset != slot_end) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.PARAMETER.SLOT_BYTES_MISMATCH";
    return result;
  }
  for (u32 row_index = 0; row_index < row_count; ++row_index) {
    ParameterRowValueFrame row;
    u32 value_count = 0;
    if (!cursor.ReadU32(&row.row_ordinal) || !cursor.ReadU32(&value_count)) {
      result.ok = false;
      result.diagnostic_code = cursor.diagnostic_code;
      return result;
    }
    for (u32 value_index = 0; value_index < value_count; ++value_index) {
      ParameterValueCell cell;
      if (!DecodeValueCellFrom(&cursor, &cell)) {
        result.ok = false;
        result.diagnostic_code = cursor.diagnostic_code;
        return result;
      }
      row.values.push_back(std::move(cell));
    }
    result.packet.row_value_frames.push_back(std::move(row));
  }
  if (cursor.offset != cursor.size) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.DECODE.TRAILING_BYTES";
    return result;
  }
  const auto checked = ValidateParameterDataPacket(result.packet);
  if (!checked.ok) {
    result.ok = false;
    result.diagnostic_code = checked.diagnostic_code;
    result.diagnostic_detail = checked.diagnostic_detail;
  }
  return result;
}

DatatypeWireEncodeResult EncodeRowDescriptionPacket(const RowDescriptionPacket& packet) {
  const auto checked = ValidateRowDescriptionPacket(packet);
  if (!checked.ok) { return EncodeFailure(checked.diagnostic_code, checked.diagnostic_detail); }

  DatatypeWireEncodeResult result;
  AppendU16(&result.bytes, packet.layout_version);
  AppendU8(&result.bytes, static_cast<u8>(packet.result_set_kind));
  AppendU8(&result.bytes, packet.flags);
  AppendU32(&result.bytes, CheckedU32Size(packet.columns.size()));
  AppendUuid(&result.bytes, packet.result_envelope_uuid);
  AppendUuid(&result.bytes, packet.row_shape_uuid);
  AppendHash(&result.bytes, packet.descriptor_snapshot_hash);
  for (const auto& column : packet.columns) {
    EncodeColumnDescriptorInto(&result.bytes, column);
  }
  return result;
}

RowDescriptionDecodeResult DecodeRowDescriptionPacket(const std::vector<byte>& bytes) {
  RowDescriptionDecodeResult result;
  Cursor cursor{bytes.data(), CheckedU64Size(bytes.size()), 0};
  u8 result_set_kind = 0;
  u32 column_count = 0;
  if (!cursor.ReadU16(&result.packet.layout_version) ||
      !cursor.ReadU8(&result_set_kind) ||
      !cursor.ReadU8(&result.packet.flags) ||
      !cursor.ReadU32(&column_count) ||
      !cursor.ReadBytes(result.packet.result_envelope_uuid.data(),
                        result.packet.result_envelope_uuid.size()) ||
      !cursor.ReadBytes(result.packet.row_shape_uuid.data(), result.packet.row_shape_uuid.size()) ||
      !cursor.ReadBytes(result.packet.descriptor_snapshot_hash.data(),
                        result.packet.descriptor_snapshot_hash.size())) {
    result.ok = false;
    result.diagnostic_code = cursor.diagnostic_code;
    return result;
  }
  result.packet.result_set_kind = static_cast<WireResultSetKind>(result_set_kind);
  for (u32 index = 0; index < column_count; ++index) {
    ResultColumnDescriptor column;
    if (!DecodeColumnDescriptorFrom(&cursor, &column)) {
      result.ok = false;
      result.diagnostic_code = cursor.diagnostic_code;
      return result;
    }
    result.packet.columns.push_back(std::move(column));
  }
  if (cursor.offset != cursor.size) {
    result.ok = false;
    result.diagnostic_code = "NATIVE_WIRE.DECODE.TRAILING_BYTES";
    return result;
  }
  const auto checked = ValidateRowDescriptionPacket(result.packet);
  if (!checked.ok) {
    result.ok = false;
    result.diagnostic_code = checked.diagnostic_code;
    result.diagnostic_detail = checked.diagnostic_detail;
  }
  return result;
}

}  // namespace scratchbird::core::datatypes
