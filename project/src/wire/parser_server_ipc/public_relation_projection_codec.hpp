// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "public_relation_type_shape_codec.hpp"
#include "../../core/uuid/uuid.hpp"
#include <algorithm>
#include <array>
#include <string_view>
#include <charconv>
#include <set>

namespace scratchbird::parser::ipc {
inline constexpr std::uint8_t kPublicRelationProjectionKindV4 = 1;
inline constexpr std::uint8_t kPublicRelationProjectionVersionV4 = 4;
inline constexpr std::size_t kPublicRelationProjectionMaximumBytesV4 = 524288;
inline constexpr std::size_t kPublicRelationProjectionMaximumColumnsV4 = 4096;

struct PublicRelationProjectionColumnV4 {
  core::platform::Uuid column_uuid, descriptor_uuid, charset_uuid, collation_uuid, type_uuid, datatype_descriptor_uuid;
  std::uint32_t ordinal{0};
  std::string canonical_name, descriptor_kind, canonical_type_name;
  std::string scalar_metadata;
  PublicRelationTypeShapeV1 shape;
  bool nullable{false}, generated{false}, identity_column{false}, charset_variable_width{false};
  std::string charset_name, collation_name, codec_id;
  std::uint32_t character_length{0}, charset_min_bytes{0}, charset_max_bytes{0};
  std::uint64_t descriptor_generation{0}, type_generation{0}, codec_generation{0};
  std::uint16_t codec_version{0};
  std::uint32_t canonical_value_width{0};
  std::uint8_t null_encoding{0};
  bool operator==(const PublicRelationProjectionColumnV4&) const = default;
};

struct PublicRelationProjectionV4 {
  core::platform::Uuid descriptor_uuid, relation_uuid, schema_uuid, datatype_catalog_snapshot_uuid;
  std::uint64_t descriptor_generation{0}, validated_resource_epoch{0};
  std::uint64_t datatype_catalog_generation{0}, datatype_registry_generation{0};
  std::vector<PublicRelationProjectionColumnV4> columns;
  bool operator==(const PublicRelationProjectionV4&) const = default;
};

namespace relation_projection_detail {
using Uuid = core::platform::Uuid;
using View = std::span<const std::uint8_t>;
using Bytes = std::vector<std::uint8_t>;
inline View BytesOf(std::string_view text) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
inline bool Text(View bytes, bool empty) noexcept {
  if (bytes.size() > 4096 || (!empty && bytes.empty())) return false;
  for (auto value : bytes) if (value == 0) return false;
  return core::datatypes::ValidateCanonicalUtf8(bytes.data(), bytes.size());
}
inline bool ScalarMetadata(std::string_view text) noexcept {
  // Only non-identity descriptor attributes may cross this text field.
  constexpr std::array<std::string_view, 14> keys{
      "canonical", "type", "width", "precision", "scale", "timezone_profile_id",
      "nullable", "nullability", "dimension", "element_type", "text_resource_storage",
      "character_length", "encoding", "storage"};
  std::uint32_t seen = 0;
  while (!text.empty()) {
    const auto delimiter = text.find(';');
    const auto field = text.substr(0, delimiter);
    const auto equal = field.find('=');
    if (equal == std::string_view::npos || equal + 1 == field.size()) return false;
    const auto key = std::find(keys.begin(), keys.end(), field.substr(0, equal));
    if (key == keys.end()) return false;
    const auto bit = 1u << (key - keys.begin());
    if (seen & bit) return false;
    seen |= bit;
    if (delimiter == std::string_view::npos) break;
    text.remove_prefix(delimiter + 1);
    if (text.empty()) return false;
  }
  return true;
}
inline bool ScalarShapeMatches(std::string_view text,
    std::optional<std::uint32_t> width, std::optional<std::uint32_t> precision,
    std::optional<std::uint32_t> scale, std::optional<std::string_view> timezone,
    bool nullable, std::string_view canonical_type) noexcept {
  if (!ScalarMetadata(text) || (scale && (!precision || *scale > *precision))) return false;
  while (!text.empty()) {
    const auto delimiter = text.find(';');
    const auto field = text.substr(0, delimiter);
    const auto equal = field.find('=');
    const auto key = field.substr(0, equal), value = field.substr(equal + 1);
    if (key == "width" || key == "precision" || key == "scale") {
      std::uint32_t parsed = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      const auto expected = key == "width" ? width : key == "precision" ? precision : scale;
      if (error != std::errc{} || end != value.data() + value.size() || expected != parsed) return false;
    } else if (key == "timezone_profile_id" && timezone != value) return false;
    else if (key == "nullable" && value != (nullable ? "true" : "false")) return false;
    else if (key == "nullability" && value != (nullable ? "nullable" : "non_null")) return false;
    else if ((key == "canonical" || key == "type") && value != canonical_type) return false;
    if (delimiter == std::string_view::npos) break;
    text.remove_prefix(delimiter + 1);
  }
  return true;
}
inline bool ScalarShapeMatches(std::string_view text, const PublicRelationTypeShapeV1& shape,
                               bool nullable, std::string_view canonical_type) noexcept {
  return ScalarShapeMatches(text, shape.width, shape.precision, shape.scale,
      shape.timezone_profile_id ? std::optional<std::string_view>{*shape.timezone_profile_id}
                                : std::nullopt, nullable, canonical_type);
}
inline bool Identity(const Uuid& id, bool optional = false) noexcept {
  return (optional && id.is_nil()) || core::uuid::IsEngineIdentityUuid(id);
}
inline bool Header(const PublicRelationProjectionV4& p) noexcept {
  return Identity(p.descriptor_uuid) && Identity(p.relation_uuid) &&
      Identity(p.schema_uuid) && Identity(p.datatype_catalog_snapshot_uuid) &&
      p.descriptor_generation && p.validated_resource_epoch &&
      p.datatype_catalog_generation && p.datatype_registry_generation;
}
inline bool Resources(const Uuid& charset, const Uuid& collation,
                      bool charset_name_empty, bool collation_name_empty,
                      std::uint32_t length, std::uint32_t minimum,
                      std::uint32_t maximum, bool variable) noexcept {
  if (!Identity(charset, true) || !Identity(collation, true) ||
      charset.is_nil() != charset_name_empty ||
      collation.is_nil() != collation_name_empty) return false;
  if (charset.is_nil())
    return collation.is_nil() && !length && !minimum && !maximum && !variable;
  // Length zero can be an authoritative character-LOB shape. Do not infer
  // storage or variable-width semantics from a spelling or a width guess.
  return minimum && maximum >= minimum;
}
inline bool Unique(std::array<Uuid, kPublicRelationProjectionMaximumColumnsV4>& ids,
                   std::size_t count) noexcept {
  std::sort(ids.begin(), ids.begin() + count);
  return std::adjacent_find(ids.begin(), ids.begin() + count) == ids.begin() + count;
}
inline bool Measure(const PublicRelationProjectionV4& p, std::size_t limit,
                    std::size_t& size) noexcept {
  limit = std::min(limit, kPublicRelationProjectionMaximumBytesV4);
  if (limit < 100 || !Header(p) || p.columns.empty() ||
      p.columns.size() > kPublicRelationProjectionMaximumColumnsV4) return false;
  std::array<Uuid, kPublicRelationProjectionMaximumColumnsV4> ids;
  size = 100;
  for (std::size_t i = 0; i != p.columns.size(); ++i) {
    const auto& c = p.columns[i];
    if (c.ordinal != i || !Identity(c.column_uuid) || !Identity(c.descriptor_uuid) ||
        !Identity(c.datatype_descriptor_uuid) || !Identity(c.type_uuid) || !c.descriptor_generation || !c.type_generation ||
        !c.codec_generation || !c.codec_version ||
        !Resources(c.charset_uuid, c.collation_uuid, c.charset_name.empty(),
                   c.collation_name.empty(), c.character_length, c.charset_min_bytes,
                   c.charset_max_bytes, c.charset_variable_width)) return false;
    if (!ScalarShapeMatches(c.scalar_metadata, c.shape, c.nullable, c.canonical_type_name)) return false;
    ids[i] = c.column_uuid;
    std::size_t n = 192; // fixed column bytes including a 16-byte shape
    for (const auto* text : {&c.canonical_name, &c.descriptor_kind, &c.canonical_type_name,
                            &c.charset_name, &c.collation_name, &c.codec_id, &c.scalar_metadata}) {
      const bool optional = text == &c.charset_name || text == &c.collation_name || text == &c.scalar_metadata;
      if (!Text(BytesOf(*text), optional)) return false;
      n += text->size();
    }
    if (c.shape.timezone_profile_id) {
      if (!relation_shape_detail::Timezone(BytesOf(*c.shape.timezone_profile_id))) return false;
      n += 4 + c.shape.timezone_profile_id->size();
    }
    if (n > limit - size) return false;
    size += n;
  }
  return Unique(ids, p.columns.size());
}
struct Reader {
  View bytes;
  std::size_t offset{0};
  bool Take(std::size_t count, View& out) noexcept {
    if (count > bytes.size() - offset) return false;
    out = bytes.subspan(offset, count); offset += count; return true;
  }
  template<class T> bool Integer(T& value, unsigned width = sizeof(T)) noexcept {
    View part; if (!Take(width, part)) return false;
    value = 0;
    for (unsigned i = 0; i != width; ++i)
      value |= static_cast<T>(std::uint64_t(part[i]) << (8 * i));
    return true;
  }
  bool Id(Uuid& id, bool optional = false) noexcept {
    View part; if (!Take(16, part)) return false;
    std::copy(part.begin(), part.end(), id.bytes.begin());
    return Identity(id, optional);
  }
  bool String(std::string_view& text, bool empty = false) noexcept {
    std::uint32_t length; View part;
    if (!Integer(length) || !Take(length, part) || !Text(part, empty)) return false;
    text = {reinterpret_cast<const char*>(part.data()), part.size()}; return true;
  }
};
struct ColumnView {
  Uuid column, descriptor, charset, collation, type, datatype_descriptor;
  std::uint32_t ordinal{}, length{}, minimum{}, maximum{}, value_width{};
  std::uint64_t descriptor_generation{}, type_generation{}, codec_generation{};
  std::uint16_t codec_version{};
  std::uint8_t attributes{}, null_encoding{};
  std::string_view name, kind, type_name, charset_name, collation_name, codec, scalar_metadata;
  View shape;
};
inline bool ReadColumn(Reader& r, std::size_t ordinal, ColumnView& c) noexcept {
  std::uint32_t shape_size;
  if (!r.Id(c.column) || !r.Integer(c.ordinal) || c.ordinal != ordinal ||
      !r.String(c.name) || !r.Id(c.descriptor) || !r.String(c.kind) ||
      !r.String(c.type_name) || !r.Integer(shape_size) || !r.Take(shape_size, c.shape) ||
      !ValidatePublicRelationTypeShapeV1(c.shape, kPublicRelationTypeShapeMaximumBytes) ||
      !r.Integer(c.attributes) || (c.attributes & 0xf0u) ||
      !r.Id(c.charset, true) || !r.String(c.charset_name, true) ||
      !r.Id(c.collation, true) || !r.String(c.collation_name, true) ||
      !r.Integer(c.length) || !r.Integer(c.minimum) || !r.Integer(c.maximum) ||
      !Resources(c.charset, c.collation, c.charset_name.empty(), c.collation_name.empty(),
                 c.length, c.minimum, c.maximum, c.attributes & 8) ||
      !r.String(c.scalar_metadata, true) || !ScalarMetadata(c.scalar_metadata) || !r.Id(c.datatype_descriptor) ||
      !r.Integer(c.descriptor_generation) || !c.descriptor_generation || !r.Id(c.type) ||
      !r.Integer(c.type_generation) || !c.type_generation || !r.String(c.codec) ||
      !r.Integer(c.codec_version) || !c.codec_version ||
      !r.Integer(c.codec_generation) || !c.codec_generation ||
      !r.Integer(c.value_width) || !r.Integer(c.null_encoding)) return false;
  const auto optional_number = [&](unsigned bit, std::size_t offset) -> std::optional<std::uint32_t> {
    if (!(c.shape[2] & bit)) return std::nullopt;
    return relation_shape_detail::Read32(c.shape, offset);
  };
  std::optional<std::string_view> timezone;
  if (c.shape[2] & 8) timezone = std::string_view{
      reinterpret_cast<const char*>(c.shape.data() + 20), c.shape.size() - 20};
  return ScalarShapeMatches(c.scalar_metadata, optional_number(1,4), optional_number(2,8),
      optional_number(4,12), timezone, c.attributes & 1, c.type_name);
}
inline bool Inspect(View bytes, std::size_t limit, PublicRelationProjectionV4& p,
                    std::uint32_t& count) noexcept {
  if (bytes.size() < 100 || bytes.size() > limit ||
      bytes.size() > kPublicRelationProjectionMaximumBytesV4) return false;
  Reader r{bytes};
  if (!r.Id(p.descriptor_uuid) || !r.Id(p.relation_uuid) || !r.Id(p.schema_uuid) ||
      !r.Integer(p.descriptor_generation) || !r.Integer(p.validated_resource_epoch) ||
      !r.Id(p.datatype_catalog_snapshot_uuid) || !r.Integer(p.datatype_catalog_generation) ||
      !r.Integer(p.datatype_registry_generation) || !r.Integer(count) ||
      !Header(p) || !count || count > kPublicRelationProjectionMaximumColumnsV4 ||
      count > (bytes.size() - 100) / 196) return false;
  std::array<Uuid, kPublicRelationProjectionMaximumColumnsV4> ids;
  for (std::uint32_t i = 0; i != count; ++i) {
    ColumnView c;
    if (!ReadColumn(r, i, c)) return false;
    ids[i] = c.column;
  }
  return r.offset == bytes.size() && Unique(ids, count);
}
inline void Integer(Bytes& out, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i != width; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
inline void Raw(Bytes& out, View value) { out.insert(out.end(), value.begin(), value.end()); }
inline void String(Bytes& out, std::string_view text) {
  Integer(out, text.size(), 4); Raw(out, BytesOf(text));
}
} // namespace relation_projection_detail

inline bool EncodePublicRelationProjectionV4(const PublicRelationProjectionV4& p,
    std::size_t maximum_bytes, std::vector<std::uint8_t>* output) {
  namespace d = relation_projection_detail;
  std::size_t size;
  if (!output || !d::Measure(p, maximum_bytes, size)) return false;
  d::Bytes staged; staged.reserve(size);
  for (const auto& id : {p.descriptor_uuid, p.relation_uuid, p.schema_uuid}) d::Raw(staged, id.bytes);
  d::Integer(staged, p.descriptor_generation, 8); d::Integer(staged, p.validated_resource_epoch, 8);
  d::Raw(staged, p.datatype_catalog_snapshot_uuid.bytes);
  d::Integer(staged, p.datatype_catalog_generation, 8);
  d::Integer(staged, p.datatype_registry_generation, 8); d::Integer(staged, p.columns.size(), 4);
  for (const auto& c : p.columns) {
    d::Raw(staged, c.column_uuid.bytes); d::Integer(staged, c.ordinal, 4);
    d::String(staged, c.canonical_name); d::Raw(staged, c.descriptor_uuid.bytes);
    d::String(staged, c.descriptor_kind); d::String(staged, c.canonical_type_name);
    d::Bytes shape;
    if (!EncodePublicRelationTypeShapeV1(c.shape, kPublicRelationTypeShapeMaximumBytes, &shape))
      return false;
    d::Integer(staged, shape.size(), 4); d::Raw(staged, shape);
    d::Integer(staged, c.nullable | (c.generated << 1) | (c.identity_column << 2) |
                      (c.charset_variable_width << 3), 1);
    d::Raw(staged, c.charset_uuid.bytes); d::String(staged, c.charset_name);
    d::Raw(staged, c.collation_uuid.bytes); d::String(staged, c.collation_name);
    d::Integer(staged, c.character_length, 4); d::Integer(staged, c.charset_min_bytes, 4);
    d::Integer(staged, c.charset_max_bytes, 4);
    d::String(staged, c.scalar_metadata); d::Raw(staged, c.datatype_descriptor_uuid.bytes);
    d::Integer(staged, c.descriptor_generation, 8);
    d::Raw(staged, c.type_uuid.bytes); d::Integer(staged, c.type_generation, 8);
    d::String(staged, c.codec_id); d::Integer(staged, c.codec_version, 2);
    d::Integer(staged, c.codec_generation, 8); d::Integer(staged, c.canonical_value_width, 4);
    d::Integer(staged, c.null_encoding, 1);
  }
  if (staged.size() != size) return false;
  *output = std::move(staged); return true;
}

inline bool DecodePublicRelationProjectionV4(std::span<const std::uint8_t> bytes,
    std::size_t maximum_bytes, PublicRelationProjectionV4* output) {
  namespace d = relation_projection_detail;
  PublicRelationProjectionV4 staged;
  std::uint32_t count;
  if (!output || !d::Inspect(bytes, maximum_bytes, staged, count)) return false;
  staged.columns.reserve(count);
  d::Reader reader{bytes, 100};
  for (std::uint32_t i = 0; i != count; ++i) {
    d::ColumnView c;
    if (!d::ReadColumn(reader, i, c)) return false;
    PublicRelationProjectionColumnV4 column;
    column.datatype_descriptor_uuid = c.datatype_descriptor;
    column.scalar_metadata = c.scalar_metadata;
    column.column_uuid = c.column; column.descriptor_uuid = c.descriptor;
    column.charset_uuid = c.charset; column.collation_uuid = c.collation; column.type_uuid = c.type;
    column.ordinal = c.ordinal; column.canonical_name = c.name;
    column.descriptor_kind = c.kind; column.canonical_type_name = c.type_name;
    if (!DecodePublicRelationTypeShapeV1(c.shape, kPublicRelationTypeShapeMaximumBytes, &column.shape))
      return false;
    column.nullable = c.attributes & 1; column.generated = c.attributes & 2;
    column.identity_column = c.attributes & 4; column.charset_variable_width = c.attributes & 8;
    column.charset_name = c.charset_name; column.collation_name = c.collation_name;
    column.character_length = c.length; column.charset_min_bytes = c.minimum;
    column.charset_max_bytes = c.maximum; column.descriptor_generation = c.descriptor_generation;
    column.type_generation = c.type_generation; column.codec_generation = c.codec_generation;
    column.codec_version = c.codec_version; column.codec_id = c.codec;
    column.canonical_value_width = c.value_width; column.null_encoding = c.null_encoding;
    staged.columns.push_back(std::move(column));
  }
  static_assert(std::is_nothrow_move_assignable_v<PublicRelationProjectionV4>);
  *output = std::move(staged); return true;
}
} // namespace scratchbird::parser::ipc
