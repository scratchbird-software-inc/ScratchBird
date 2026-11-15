// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_tree_keys.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
void PutUuid(std::vector<byte>& out, const Uuid& u) { out.insert(out.end(), u.bytes.begin(), u.bytes.end()); }
void PutKind(std::vector<byte>& out, CatalogRecordKind kind) {
  const auto v = static_cast<u16>(kind);
  out.push_back(static_cast<byte>(v >> 8));
  out.push_back(static_cast<byte>(v));
}
Uuid GetUuid(std::span<const byte> in, std::size_t at) noexcept {
  Uuid u;
  std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(at), 16, u.bytes.begin());
  return u;
}
}  // namespace

void AppendCatalogNameTreeEscaped(std::vector<byte>& out, std::span<const byte> bytes) {
  for (const byte b : bytes) {
    out.push_back(b);
    if (!b) out.push_back(0xff);
  }
  out.push_back(0);
  out.push_back(0);
}

void AppendCatalogNameTreeEscaped(std::vector<byte>& out, const CatalogNameDerivedText& text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const byte b = text[i];
    out.push_back(b);
    if (!b) out.push_back(0xff);
  }
  out.push_back(0);
  out.push_back(0);
}

std::vector<byte> CatalogNameAuthoritativeTreeNamePrefix(const CatalogNameEntryView& entry,
                                                         const CatalogNameDerivedText& own) {
  std::vector<byte> out;
  PutUuid(out, entry.scope_uuid.value);
  PutUuid(out, entry.identifier_profile_uuid.value);
  PutUuid(out, entry.language_uuid.value);  // SB_CATALOG_LANGUAGE_CONSTANTS: fixed 16 bytes
  AppendCatalogNameTreeEscaped(out, own);
  return out;
}
std::vector<byte> CatalogNameAuthoritativeTreeKey(const CatalogNameEntryView& entry, const CatalogNameDerivedText& own,
                                                  CatalogRecordKind kind) {
  auto out = CatalogNameAuthoritativeTreeNamePrefix(entry, own);
  PutKind(out, kind);
  return out;
}
std::vector<byte> CatalogNameAuthoritativeTreePayload(const CatalogNameEntryView& entry) {
  std::vector<byte> out;
  PutUuid(out, entry.object_uuid.value);
  PutUuid(out, entry.name_entry_uuid.value);
  return out;
}
std::optional<CatalogNameAuthoritativeTreePayloadView> DecodeCatalogNameAuthoritativeTreePayload(
    std::span<const byte> payload) noexcept {
  if (payload.size() != 32) return std::nullopt;
  return CatalogNameAuthoritativeTreePayloadView{GetUuid(payload, 0), GetUuid(payload, 16)};
}

std::vector<byte> CatalogNameUuidTreeVectorKey(const Uuid& object_uuid, CatalogRecordKind kind) {
  std::vector<byte> out;
  PutUuid(out, object_uuid);
  PutKind(out, kind);
  out.push_back(0);
  return out;
}
std::vector<byte> CatalogNameUuidTreeVectorPayload(const CatalogNameVectorView& vector) {
  std::vector<byte> out;
  PutUuid(out, vector.name_vector_uuid.value);
  PutUuid(out, vector.default_name_entry_uuid.value);
  PutUuid(out, vector.name_collision_policy_uuid.value);
  return out;
}
std::optional<CatalogNameUuidTreeVectorPayloadView> DecodeCatalogNameUuidTreeVectorPayload(
    std::span<const byte> payload) noexcept {
  if (payload.size() != 48) return std::nullopt;
  return CatalogNameUuidTreeVectorPayloadView{GetUuid(payload, 0), GetUuid(payload, 16), GetUuid(payload, 32)};
}

std::vector<byte> CatalogNameUuidTreeEntryKey(const CatalogNameEntryView& entry, CatalogRecordKind kind) {
  std::vector<byte> out;
  PutUuid(out, entry.object_uuid.value);
  PutKind(out, kind);
  out.push_back(1);
  PutUuid(out, entry.language_uuid.value);
  out.push_back(static_cast<byte>(entry.name_class));
  return out;
}
std::vector<byte> CatalogNameUuidTreeEntryPayload(const Uuid& entry_uuid) {
  std::vector<byte> out;
  PutUuid(out, entry_uuid);
  return out;
}
std::optional<Uuid> DecodeCatalogNameUuidTreeEntryPayload(std::span<const byte> payload) noexcept {
  if (payload.size() != 16) return std::nullopt;
  return GetUuid(payload, 0);
}

namespace {
// Consumes `bytes` against what the encoders above would have written.
class Matcher {
 public:
  explicit Matcher(std::span<const byte> bytes) noexcept : bytes_(bytes) {}
  Matcher& Uuid(const struct Uuid& u) noexcept { return Raw(u.bytes); }
  Matcher& Kind(CatalogRecordKind kind) noexcept {
    const auto v = static_cast<u16>(kind);
    const byte pair[2]{static_cast<byte>(v >> 8), static_cast<byte>(v)};
    return Raw(pair);
  }
  Matcher& Byte(byte b) noexcept { return Raw(std::span<const byte>(&b, 1)); }
  Matcher& Escaped(std::span<const byte> raw) noexcept {
    for (const byte b : raw) {
      Byte(b);
      if (!b) Byte(0xff);
    }
    Byte(0);
    return Byte(0);
  }
  Matcher& Escaped(const CatalogNameDerivedText& text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const byte b = text[i];
      Byte(b);
      if (!b) Byte(0xff);
    }
    Byte(0);
    return Byte(0);
  }
  bool Done() const noexcept { return ok_ && at_ == bytes_.size(); }

 private:
  Matcher& Raw(std::span<const byte> expected) noexcept {
    if (!ok_ || expected.size() > bytes_.size() - at_ ||
        !std::equal(expected.begin(), expected.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(at_)))
      ok_ = false;
    else
      at_ += expected.size();
    return *this;
  }
  std::span<const byte> bytes_;
  std::size_t at_ = 0;
  bool ok_ = true;
};
}  // namespace

bool CatalogNameAuthoritativeTreeKeyEquals(const CatalogNameEntryView& entry, const CatalogNameDerivedText& own,
                                           CatalogRecordKind kind, std::span<const byte> key) noexcept {
  return Matcher(key)
      .Uuid(entry.scope_uuid.value)
      .Uuid(entry.identifier_profile_uuid.value)
      .Uuid(entry.language_uuid.value)
      .Escaped(own)
      .Kind(kind)
      .Done();
}
bool CatalogNameAuthoritativeTreePayloadEquals(const CatalogNameEntryView& entry, std::span<const byte> payload) noexcept {
  return Matcher(payload).Uuid(entry.object_uuid.value).Uuid(entry.name_entry_uuid.value).Done();
}
bool CatalogNameUuidTreeVectorKeyEquals(const Uuid& object_uuid, CatalogRecordKind kind, std::span<const byte> key) noexcept {
  return Matcher(key).Uuid(object_uuid).Kind(kind).Byte(0).Done();
}
bool CatalogNameUuidTreeVectorPayloadEquals(const CatalogNameVectorView& vector, std::span<const byte> payload) noexcept {
  return Matcher(payload)
      .Uuid(vector.name_vector_uuid.value)
      .Uuid(vector.default_name_entry_uuid.value)
      .Uuid(vector.name_collision_policy_uuid.value)
      .Done();
}
bool CatalogNameUuidTreeEntryKeyEquals(const CatalogNameEntryView& entry, CatalogRecordKind kind,
                                       std::span<const byte> key) noexcept {
  return Matcher(key)
      .Uuid(entry.object_uuid.value)
      .Kind(kind)
      .Byte(1)
      .Uuid(entry.language_uuid.value)
      .Byte(static_cast<byte>(entry.name_class))
      .Done();
}
bool CatalogNameUuidTreeEntryPayloadEquals(const CatalogNameEntryView& entry, std::span<const byte> payload) noexcept {
  return Matcher(payload).Uuid(entry.name_entry_uuid.value).Done();
}
}  // namespace scratchbird::core::catalog
