// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::wire::public_result {
// Public result metadata, rows, and evidence share a length-framed transport.
// UUID atoms are exactly 16 bytes; the client owns any display rendering.
inline constexpr std::string_view kMagic = "SBRES002";
inline constexpr std::size_t kMaximumFields = std::numeric_limits<std::uint32_t>::max();
enum class Kind : std::uint8_t { text = 0, bytes = 1, uuid = 2, unsigned_integer = 3, row = 4, evidence = 5 };
struct FieldView { std::string_view name; Kind kind; std::string_view value; };
struct Field { std::string name; Kind kind; std::string value; };
inline bool Valid(FieldView field) {
  const auto kind = static_cast<std::uint8_t>(field.kind);
  return !field.name.empty() && field.name.size() <= std::numeric_limits<std::uint32_t>::max() &&
         kind <= static_cast<std::uint8_t>(Kind::evidence) &&
         (field.kind != Kind::uuid || field.value.size() == 16) &&
         (field.kind != Kind::unsigned_integer || field.value.size() == 8);
}
template<class Emit>
bool Visit(const std::vector<FieldView>& fields, Emit&& emit) {
  if (fields.size() > kMaximumFields) return false;
  for (const auto& field : fields) if (!Valid(field)) return false;
  const auto number = [&](std::uint64_t value, std::size_t size) {
    std::array<char, 8> bytes{};
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(value >> (i * 8));
    emit(std::string_view(bytes.data(), size));
  };
  emit(kMagic); number(fields.size(), 4);
  for (const auto& field : fields) {
    number(field.name.size(), 4); emit(field.name);
    number(static_cast<std::uint8_t>(field.kind), 1);
    number(field.value.size(), 8); emit(field.value);
  }
  return true;
}
inline bool Encode(const std::vector<FieldView>& fields, std::string* output) {
  if (!output) return false;
  std::size_t size = 0;
  bool fits = true;
  if (!Visit(fields, [&](std::string_view part) {
        if (part.size() > output->max_size() - size) fits = false;
        else if (fits) size += part.size();
      }) || !fits) return false;
  std::string encoded; encoded.reserve(size);
  Visit(fields, [&](std::string_view part) { encoded.append(part); });
  output->swap(encoded);
  return true;
}
inline bool Encode(const std::vector<Field>& fields, std::string* output) {
  std::vector<FieldView> views; views.reserve(fields.size());
  for (const auto& field : fields) views.push_back({field.name, field.kind, field.value});
  return Encode(views, output);
}
inline bool Decode(std::string_view bytes, std::vector<Field>* output) {
  if (!output || !bytes.starts_with(kMagic)) return false;
  std::size_t offset = kMagic.size();
  const auto number = [&](std::size_t size, std::uint64_t* value) {
    if (size > bytes.size() - offset) return false;
    *value = 0;
    for (std::size_t i = 0; i < size; ++i)
      *value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++])) << (i * 8);
    return true;
  };
  std::uint64_t count = 0;
  if (!number(4, &count) || count > kMaximumFields || count > (bytes.size() - offset) / 14) return false;
  std::vector<Field> fields;
  fields.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t name_size = 0, kind = 0, value_size = 0;
    if (!number(4, &name_size) || !name_size || name_size > bytes.size() - offset) return false;
    const auto name = bytes.substr(offset, static_cast<std::size_t>(name_size)); offset += name_size;
    if (!number(1, &kind) || !number(8, &value_size) || value_size > bytes.size() - offset) return false;
    const auto value = bytes.substr(offset, static_cast<std::size_t>(value_size)); offset += value_size;
    const auto tag = static_cast<Kind>(kind);
    if (!Valid({name, tag, value})) return false;
    fields.push_back({std::string(name), tag, std::string(value)});
  }
  if (offset != bytes.size()) return false;
  *output = std::move(fields);
  return true;
}
// Scalar lookup rejects duplicates instead of accepting an ambiguous identity.
inline std::optional<Field> Find(std::string_view packet, std::string_view name) {
  std::vector<Field> fields;
  if (!Decode(packet, &fields)) return std::nullopt;
  std::optional<Field> found;
  for (auto& field : fields) {
    if (field.name != name) continue;
    if (found) return std::nullopt;
    found = std::move(field);
  }
  return found;
}
inline std::optional<std::string> Value(std::string_view packet, std::string_view name) {
  const auto field = Find(packet, name);
  return field ? std::optional<std::string>(field->value) : std::nullopt;
}
inline std::optional<Field> Evidence(std::string_view packet, std::string_view name) {
  std::vector<Field> fields;
  if (!Decode(packet, &fields)) return std::nullopt;
  std::optional<Field> found;
  for (const auto& field : fields) {
    if (field.name != "evidence" || field.kind != Kind::evidence) continue;
    std::vector<Field> evidence;
    if (!Decode(field.value, &evidence) || evidence.size() != 1) return std::nullopt;
    if (evidence[0].name != name) continue;
    if (found) return std::nullopt;
    found = std::move(evidence[0]);
  }
  return found;
}
inline std::string Unsigned(std::uint64_t value) {
  std::string bytes(8, '\0');
  for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<char>(value >> (8 * i));
  return bytes;
}
inline std::optional<std::uint64_t> AsUnsigned(const Field& field) {
  if (field.kind != Kind::unsigned_integer || field.value.size() != 8) return std::nullopt;
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(field.value[i])) << (8 * i);
  return value;
}
} // namespace scratchbird::wire::public_result
