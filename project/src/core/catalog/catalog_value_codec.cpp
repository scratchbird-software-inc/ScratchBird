// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_value_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using Error = CatalogValueError;
using Type = CatalogValueType;
constexpr std::size_t kFieldHeader = 8;
constexpr std::size_t kMaxValue = kCatalogValueBlockMaxBytes -
    kCatalogValueBlockHeaderBytes - kFieldHeader;

std::size_t FixedWidth(Type type) {
  switch (type) {
    case Type::unsigned_integer: return 8;
    case Type::boolean: return 1;
    case Type::engine_identity:
    case Type::user_uuid_data: return 16;
    default: return 0;
  }
}
bool IdentityType(Type type) {
  return type == Type::engine_identity || type == Type::engine_identity_list;
}
bool ValidSchema(CatalogValueSchemaView schema) {
  if (!schema.id || !schema.version) return false;
  u16 previous = 0;
  for (const auto& field : schema.fields) {
    const auto type = static_cast<u8>(field.type);
    if (field.id <= previous || type < 1 || type > 8 ||
        field.maximum_bytes > kMaxValue ||
        (field.type == Type::utf8_text_list && field.maximum_bytes < 4) ||
        field.maximum_bytes < FixedWidth(field.type)) return false;
    if (IdentityType(field.type)) {
      if (!uuid::IsDurableEngineIdentityKind(field.identity_kind)) return false;
    } else if (field.identity_kind != UuidKind::unknown) {
      return false;
    }
    previous = field.id;
  }
  return true;
}
bool ValidIdentity(const TypedUuid& value, UuidKind expected) {
  return value.kind == expected && uuid::IsEngineIdentityUuid(value.value);
}
bool ValidUtf8(const byte* data, std::size_t size) {
  for (std::size_t i = 0; i < size;) {
    const u8 first = data[i++];
    if (first < 0x80) continue;
    unsigned remaining = 0;
    u32 value = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      remaining = 1; value = first & 0x1f; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      remaining = 2; value = first & 0x0f; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      remaining = 3; value = first & 7; minimum = 0x10000;
    } else return false;
    if (remaining > size - i) return false;
    while (remaining--) {
      const u8 next = data[i++];
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
void Put(std::vector<byte>& bytes, u64 value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<byte>(value >> (8 * i)));
}
u64 Get(std::span<const byte> bytes, std::size_t offset, unsigned width) {
  u64 value = 0;
  for (unsigned i = 0; i < width; ++i) value |= static_cast<u64>(bytes[offset + i]) << (8 * i);
  return value;
}
Type ValueType(const CatalogValue& value) {
  // Variant alternatives and wire tags deliberately have the same order.
  return static_cast<Type>(value.index() + 1);
}
Error ValueSize(const CatalogValue& value, const CatalogValueFieldSchema& field,
                std::size_t* size) {
  if (ValueType(value) != field.type) return Error::type_mismatch;
  *size = FixedWidth(field.type);
  switch (field.type) {
    case Type::utf8_text: {
      const auto& text = std::get<std::string>(value);
      *size = text.size();
      if (*size > field.maximum_bytes) return Error::size_limit;
      if (!ValidUtf8(reinterpret_cast<const byte*>(text.data()), text.size()))
        return Error::invalid_value;
      break;
    }
    case Type::opaque_bytes: *size = std::get<std::vector<byte>>(value).size(); break;
    case Type::utf8_text_list: {
      const auto& values = std::get<std::vector<std::string>>(value);
      if (values.size() > (field.maximum_bytes - 4) / 4) return Error::size_limit;
      *size = 4;
      for (const auto& text : values) {
        if (field.maximum_bytes - *size < 4 ||
            text.size() > field.maximum_bytes - *size - 4) return Error::size_limit;
        if (!ValidUtf8(reinterpret_cast<const byte*>(text.data()), text.size()))
          return Error::invalid_value;
        *size += 4 + text.size();
      }
      break;
    }
    case Type::engine_identity:
      if (!ValidIdentity(std::get<TypedUuid>(value), field.identity_kind))
        return Error::invalid_value;
      break;
    case Type::engine_identity_list: {
      const auto& ids = std::get<std::vector<TypedUuid>>(value);
      if (ids.size() > field.maximum_bytes / 16) return Error::size_limit;
      *size = ids.size() * 16;
      for (const auto& id : ids)
        if (!ValidIdentity(id, field.identity_kind)) return Error::invalid_value;
      break;
    }
    default: break;
  }
  return *size > field.maximum_bytes ? Error::size_limit : Error::none;
}
void PutUuid(std::vector<byte>& bytes, const Uuid& value) {
  bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
}
Uuid GetUuid(std::span<const byte> bytes, std::size_t offset) {
  Uuid value;
  std::copy_n(bytes.begin() + offset, 16, value.bytes.begin());
  return value;
}
}  // namespace

CatalogValueEncodeResult EncodeCatalogValueBlock(
    const CatalogValueSchema& schema, const std::vector<CatalogValueField>& fields) {
  if (!ValidSchema(BorrowCatalogValueSchema(schema))) return {Error::invalid_schema, {}};
  // Validate the entire request before assembling any output.
  std::size_t cursor = 0, total = kCatalogValueBlockHeaderBytes;
  u16 previous = 0;
  std::vector<std::size_t> sizes;
  if (fields.size() > schema.fields.size()) return {Error::unknown_field, {}};
  sizes.reserve(fields.size());
  for (const auto& field : fields) {
    if (field.id <= previous) return {Error::invalid_framing, {}};
    while (cursor < schema.fields.size() && schema.fields[cursor].id < field.id) {
      if (schema.fields[cursor++].required) return {Error::missing_field, {}};
    }
    if (cursor == schema.fields.size() || schema.fields[cursor].id != field.id)
      return {Error::unknown_field, {}};
    std::size_t size = 0;
    const auto error = ValueSize(field.value, schema.fields[cursor++], &size);
    if (error != Error::none) return {error, {}};
    if (kFieldHeader > kCatalogValueBlockMaxBytes - total ||
        size > kCatalogValueBlockMaxBytes - total - kFieldHeader)
      return {Error::size_limit, {}};
    total += kFieldHeader + size;
    sizes.push_back(size);
    previous = field.id;
  }
  while (cursor < schema.fields.size())
    if (schema.fields[cursor++].required) return {Error::missing_field, {}};

  CatalogValueEncodeResult result;
  auto& out = result.bytes;
  out.reserve(total);
  out.insert(out.end(), {'S', 'B', 'C', 'V'});
  Put(out, 1, 2); Put(out, kCatalogValueBlockHeaderBytes, 2);
  Put(out, total, 4); Put(out, fields.size(), 4);
  Put(out, schema.id, 4); Put(out, schema.version, 2); Put(out, 0, 2);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto& field = fields[i];
    Put(out, field.id, 2); Put(out, static_cast<u8>(ValueType(field.value)), 1);
    Put(out, 0, 1); Put(out, sizes[i], 4);
    switch (ValueType(field.value)) {
      case Type::unsigned_integer: Put(out, std::get<u64>(field.value), 8); break;
      case Type::boolean: Put(out, std::get<bool>(field.value), 1); break;
      case Type::utf8_text: {
        const auto& text = std::get<std::string>(field.value);
        out.insert(out.end(), text.begin(), text.end()); break;
      }
      case Type::opaque_bytes: {
        const auto& bytes = std::get<std::vector<byte>>(field.value);
        out.insert(out.end(), bytes.begin(), bytes.end()); break;
      }
      case Type::engine_identity: PutUuid(out, std::get<TypedUuid>(field.value).value); break;
      case Type::user_uuid_data: PutUuid(out, std::get<Uuid>(field.value)); break;
      case Type::engine_identity_list:
        for (const auto& id : std::get<std::vector<TypedUuid>>(field.value)) PutUuid(out, id.value);
        break;
      case Type::utf8_text_list: {
        const auto& values = std::get<std::vector<std::string>>(field.value);
        Put(out, values.size(), 4);
        for (const auto& text : values) {
          Put(out, text.size(), 4);
          out.insert(out.end(), text.begin(), text.end());
        }
        break;
      }
    }
  }
  return result;
}

namespace {
// One wire admission path. Neither this pass nor borrowed publication allocates.
// Owning materialization runs only after the entire block has been admitted.
struct ValidationResult {
  Error error = Error::none;
  std::size_t count = 0;
  bool ok() const { return error == Error::none; }
};
template <typename T> bool ValidRegion(std::span<T> region) {
  if (region.empty()) return true;
  const auto address = reinterpret_cast<std::uintptr_t>(region.data());
  return address && address % alignof(T) == 0 &&
      region.size() <= (std::numeric_limits<std::uintptr_t>::max() - address) / sizeof(T);
}
template <typename A, typename B>
bool Overlap(std::span<A> a, std::span<B> b) {
  if (a.empty() || b.empty()) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a.data());
  const auto y = reinterpret_cast<std::uintptr_t>(b.data());
  return x < y + b.size_bytes() && y < x + a.size_bytes();
}
ValidationResult ValidateBlock(CatalogValueSchemaView schema, std::span<const byte> bytes) {
  if (!ValidRegion(schema.fields) || !ValidRegion(bytes)) return {Error::invalid_backing, {}};
  if (!ValidSchema(schema)) return {Error::invalid_schema, {}};
  if (bytes.size() > kCatalogValueBlockMaxBytes) return {Error::size_limit, {}};
  if (bytes.size() < kCatalogValueBlockHeaderBytes ||
      bytes[0] != 'S' || bytes[1] != 'B' || bytes[2] != 'C' || bytes[3] != 'V')
    return {Error::invalid_framing, {}};
  if (Get(bytes, 4, 2) != 1) return {Error::unsupported_version, {}};
  if (Get(bytes, 6, 2) != kCatalogValueBlockHeaderBytes ||
      Get(bytes, 8, 4) != bytes.size() || Get(bytes, 22, 2) != 0)
    return {Error::invalid_framing, {}};
  if (Get(bytes, 16, 4) != schema.id || Get(bytes, 20, 2) != schema.version)
    return {Error::invalid_schema, {}};
  const auto count = Get(bytes, 12, 4);
  if (count > schema.fields.size() ||
      count > (bytes.size() - kCatalogValueBlockHeaderBytes) / kFieldHeader)
    return {Error::invalid_framing, {}};
  std::size_t offset = kCatalogValueBlockHeaderBytes, cursor = 0;
  u16 previous = 0;
  for (u64 i = 0; i < count; ++i) {
    if (bytes.size() - offset < kFieldHeader) return {Error::invalid_framing, {}};
    const auto id = static_cast<u16>(Get(bytes, offset, 2));
    const auto tag = bytes[offset + 2];
    const auto size = static_cast<std::size_t>(Get(bytes, offset + 4, 4));
    if (id <= previous || bytes[offset + 3]) return {Error::invalid_framing, {}};
    offset += kFieldHeader;
    if (size > bytes.size() - offset) return {Error::invalid_framing, {}};
    while (cursor < schema.fields.size() && schema.fields[cursor].id < id)
      if (schema.fields[cursor++].required) return {Error::missing_field, {}};
    if (cursor == schema.fields.size() || schema.fields[cursor].id != id)
      return {Error::unknown_field, {}};
    const auto& field = schema.fields[cursor++];
    if (tag != static_cast<u8>(field.type)) return {Error::type_mismatch, {}};
    if (size > field.maximum_bytes) return {Error::size_limit, {}};
    const auto fixed = FixedWidth(field.type);
    if ((fixed && fixed != size) ||
        (field.type == Type::engine_identity_list && size % 16))
      return {Error::invalid_value, {}};
    switch (field.type) {
      case Type::unsigned_integer: break;
      case Type::boolean:
        if (bytes[offset] > 1) return {Error::invalid_value, {}};
        break;
      case Type::utf8_text:
        if (!ValidUtf8(bytes.data() + offset, size)) return {Error::invalid_value, {}};
        break;
      case Type::opaque_bytes: break;
      case Type::utf8_text_list: {
        if (size < 4) return {Error::invalid_value, {}};
        const auto element_count = Get(bytes, offset, 4);
        if (element_count > (size - 4) / 4) return {Error::invalid_value, {}};
        std::size_t current = offset + 4;
        const std::size_t end = offset + size;
        for (u64 n = 0; n < element_count; ++n) {
          if (end - current < 4) return {Error::invalid_value, {}};
          const auto length = Get(bytes, current, 4);
          current += 4;
          if (length > end - current || !ValidUtf8(bytes.data() + current, length))
            return {Error::invalid_value, {}};
          current += static_cast<std::size_t>(length);
        }
        if (current != end) return {Error::invalid_value, {}};
        break;
      }
      case Type::engine_identity: {
        TypedUuid identity{field.identity_kind, GetUuid(bytes, offset)};
        if (!ValidIdentity(identity, field.identity_kind)) return {Error::invalid_value, {}};
        break;
      }
      case Type::user_uuid_data: break;
      case Type::engine_identity_list: {
        for (std::size_t n = 0; n < size; n += 16) {
          TypedUuid identity{field.identity_kind, GetUuid(bytes, offset + n)};
          if (!ValidIdentity(identity, field.identity_kind)) return {Error::invalid_value, {}};
        }
        break;
      }
    }
    offset += size;
    previous = id;
  }
  if (offset != bytes.size()) return {Error::invalid_framing, {}};
  while (cursor < schema.fields.size())
    if (schema.fields[cursor++].required) return {Error::missing_field, {}};
  return {Error::none, static_cast<std::size_t>(count)};
}

template <typename Emit>
void PublishFields(CatalogValueSchemaView schema, std::span<const byte> bytes,
                   std::size_t count, Emit emit) {
  std::size_t offset = kCatalogValueBlockHeaderBytes, cursor = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto id = static_cast<u16>(Get(bytes, offset, 2));
    const auto size = static_cast<std::size_t>(Get(bytes, offset + 4, 4));
    while (schema.fields[cursor].id < id) ++cursor;
    const auto& field = schema.fields[cursor++];
    offset += kFieldHeader;
    emit(i, CatalogValueFieldView{id, field.type, field.identity_kind, bytes.subspan(offset, size)});
    offset += size;
  }
}

CatalogValue MaterializeField(const CatalogValueFieldView& f) {
  switch (f.type) {
    case Type::unsigned_integer: return Get(f.bytes, 0, 8);
    case Type::boolean: return f.bytes[0] != 0;
    case Type::utf8_text:
      return std::string(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size());
    case Type::opaque_bytes: return std::vector<byte>(f.bytes.begin(), f.bytes.end());
    case Type::engine_identity: return TypedUuid{f.identity_kind, GetUuid(f.bytes, 0)};
    case Type::user_uuid_data: return GetUuid(f.bytes, 0);
    case Type::engine_identity_list: {
      std::vector<TypedUuid> values;
      values.reserve(f.bytes.size() / 16);
      for (std::size_t n = 0; n < f.bytes.size(); n += 16)
        values.push_back({f.identity_kind, GetUuid(f.bytes, n)});
      return values;
    }
    case Type::utf8_text_list: {
      const auto count = Get(f.bytes, 0, 4);
      std::vector<std::string> values;
      values.reserve(static_cast<std::size_t>(count));
      std::size_t offset = 4;
      for (u64 n = 0; n < count; ++n) {
        const auto length = static_cast<std::size_t>(Get(f.bytes, offset, 4));
        offset += 4;
        values.emplace_back(reinterpret_cast<const char*>(f.bytes.data() + offset), length);
        offset += length;
      }
      return values;
    }
  }
  // Unreachable for the fully validated fields produced by PublishFields.
  std::terminate();
}
}  // namespace

std::optional<u64> CatalogValueFieldView::unsigned_value() const noexcept {
  if (type != Type::unsigned_integer || bytes.size() != 8 || !ValidRegion(bytes)) return {};
  return Get(bytes, 0, 8);
}
std::optional<TypedUuid> CatalogValueFieldView::identity() const noexcept {
  if (type != Type::engine_identity || bytes.size() != 16 || !ValidRegion(bytes) ||
      !uuid::IsDurableEngineIdentityKind(identity_kind)) return {};
  TypedUuid value{identity_kind, GetUuid(bytes, 0)};
  if (!ValidIdentity(value, identity_kind)) return {};
  return value;
}

CatalogValueDecodeViewResult DecodeCatalogValueBlockInto(
    CatalogValueSchemaView schema, std::span<const byte> bytes,
    std::span<CatalogValueFieldView> backing) {
  const auto validation = ValidateBlock(schema, bytes);
  if (!validation.ok()) return {validation.error, {}};
  if (!ValidRegion(backing) || backing.size() < validation.count ||
      Overlap(backing, bytes) || Overlap(backing, schema.fields))
    return {Error::invalid_backing, {}};
  PublishFields(schema, bytes, validation.count,
      [&](std::size_t i, CatalogValueFieldView field) { backing[i] = field; });
  return {Error::none, backing.first(validation.count)};
}

CatalogValueDecodeResult DecodeCatalogValueBlock(
    const CatalogValueSchema& schema, const std::vector<byte>& bytes) {
  const auto view = BorrowCatalogValueSchema(schema);
  const auto validation = ValidateBlock(view, bytes);
  if (!validation.ok()) return {validation.error, {}};
  CatalogValueDecodeResult result;
  result.fields.reserve(validation.count);
  PublishFields(view, bytes, validation.count,
      [&](std::size_t, const CatalogValueFieldView& field) {
        result.fields.push_back({field.id, MaterializeField(field)});
      });
  return result;
}
}  // namespace scratchbird::core::catalog
