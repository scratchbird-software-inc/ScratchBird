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
void PutLeb(std::vector<byte>& out, u64 v) {
  do {
    byte b = static_cast<byte>(v & 0x7f);
    v >>= 7;
    if (v) b |= 0x80;
    out.push_back(b);
  } while (v);
}
std::optional<u64> GetLeb(std::span<const byte> in, std::size_t& at) noexcept {
  u64 v = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (at >= in.size()) return std::nullopt;
    const byte x = in[at++];
    if (i == 9 && (x & 0xfe)) return std::nullopt;
    v |= u64(x & 0x7f) << (7 * i);
    if (!(x & 0x80)) {
      if (i && !x) return std::nullopt;
      return v;
    }
  }
  return std::nullopt;
}
Uuid GetUuid(std::span<const byte> in, std::size_t at) noexcept {
  Uuid u;
  std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(at), 16, u.bytes.begin());
  return u;
}
std::span<const byte> Bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const byte*>(text.data()), text.size()};
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

std::span<const byte> CatalogNameEntryOwnLaneKey(const CatalogNameEntryView& entry) noexcept {
  return entry.requires_exact_match ? entry.exact_lookup_key : entry.normalized_lookup_key;
}

std::vector<byte> CatalogNameAuthoritativeTreeNamePrefix(const CatalogNameEntryView& entry) {
  std::vector<byte> out;
  PutUuid(out, entry.scope_uuid.value);
  PutUuid(out, entry.identifier_profile_uuid.value);
  AppendCatalogNameTreeEscaped(out, Bytes(entry.language_tag));
  AppendCatalogNameTreeEscaped(out, CatalogNameEntryOwnLaneKey(entry));
  return out;
}
std::vector<byte> CatalogNameAuthoritativeTreeKey(const CatalogNameEntryView& entry, CatalogRecordKind kind) {
  auto out = CatalogNameAuthoritativeTreeNamePrefix(entry);
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
  AppendCatalogNameTreeEscaped(out, Bytes(entry.language_tag));
  out.push_back(static_cast<byte>(entry.name_class));
  return out;
}
std::vector<byte> CatalogNameUuidTreeEntryPayload(const Uuid& entry_uuid,
                                                  std::span<const std::string_view> display_components) {
  std::vector<byte> out;
  PutUuid(out, entry_uuid);
  PutLeb(out, display_components.size());
  for (const auto component : display_components) {
    PutLeb(out, component.size());
    const auto bytes = Bytes(component);
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}
std::optional<CatalogNameUuidTreeEntryPayloadView> DecodeCatalogNameUuidTreeEntryPayload(
    std::span<const byte> payload) {
  if (payload.size() < 17) return std::nullopt;
  CatalogNameUuidTreeEntryPayloadView out;
  out.entry_uuid = GetUuid(payload, 0);
  std::size_t at = 16;
  const auto count = GetLeb(payload, at);
  if (!count || *count > payload.size() - at) return std::nullopt;
  out.display_components.reserve(static_cast<std::size_t>(*count));
  for (u64 i = 0; i < *count; ++i) {
    const auto length = GetLeb(payload, at);
    if (!length || *length > payload.size() - at) return std::nullopt;
    out.display_components.emplace_back(reinterpret_cast<const char*>(payload.data() + at),
                                        static_cast<std::size_t>(*length));
    at += static_cast<std::size_t>(*length);
  }
  if (at != payload.size()) return std::nullopt;
  return out;
}

std::vector<byte> CatalogNameFullPathLookupKey(std::span<const std::span<const byte>> component_keys) {
  std::vector<byte> out;
  for (const auto key : component_keys) AppendCatalogNameTreeEscaped(out, key);
  return out;
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
  Matcher& Escaped(std::string_view text) noexcept { return Escaped(Bytes(text)); }
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

bool CatalogNameAuthoritativeTreeKeyEquals(const CatalogNameEntryView& entry, CatalogRecordKind kind,
                                           std::span<const byte> key) noexcept {
  return Matcher(key)
      .Uuid(entry.scope_uuid.value)
      .Uuid(entry.identifier_profile_uuid.value)
      .Escaped(entry.language_tag)
      .Escaped(CatalogNameEntryOwnLaneKey(entry))
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
      .Escaped(entry.language_tag)
      .Byte(static_cast<byte>(entry.name_class))
      .Done();
}
bool DecodeCatalogNameUuidTreeEntryPayloadInto(std::span<const byte> payload, Uuid& entry_uuid,
                                               std::span<std::string_view> components, std::size_t& count) noexcept {
  count = 0;
  if (payload.size() < 17) return false;
  entry_uuid = GetUuid(payload, 0);
  std::size_t at = 16;
  const auto n = GetLeb(payload, at);
  if (!n || *n > payload.size() - at || *n > components.size()) return false;
  for (u64 i = 0; i < *n; ++i) {
    const auto length = GetLeb(payload, at);
    if (!length || *length > payload.size() - at) return false;
    components[count++] = {reinterpret_cast<const char*>(payload.data() + at), static_cast<std::size_t>(*length)};
    at += static_cast<std::size_t>(*length);
  }
  return at == payload.size();
}
}  // namespace scratchbird::core::catalog
