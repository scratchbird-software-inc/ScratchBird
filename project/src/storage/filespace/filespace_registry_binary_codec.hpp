// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_lifecycle.hpp"
#include "uuid.hpp"
#include <cstring>
#include <limits>
#include <set>
#include <string_view>

namespace scratchbird::storage::filespace::registry_binary {
// Private binary framing only, not MGA publication or owning-source admission.
// The caller retains immutable input and supplies its actual memory admission.
// IDs are raw16; text fields are length-framed data, never identifier codecs.
inline constexpr std::size_t maximum_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t header_bytes = 32;
inline constexpr std::string_view magic = "SBFSREG3";
enum class Error { none, header, row, identity, capacity, generation };

inline bool Identity(const TypedUuid& value, core::platform::UuidKind kind) noexcept {
  return value.kind == kind && core::uuid::IsEngineIdentityUuid(value.value);
}
inline bool Capacity(const FilespaceDescriptor& d) noexcept {
  return d.free_pages <= d.total_pages && d.preallocated_pages <= d.total_pages - d.free_pages &&
      (!d.allocation_root_page || d.allocation_root_page < d.total_pages);
}
inline Error Validate(const FilespaceRegistry& r) {
  using K = core::platform::UuidKind;
  if (!r.next_evidence_sequence || r.filespaces.size() > UINT32_MAX || r.evidence.size() > UINT32_MAX)
    return Error::row;
  std::set<std::array<core::platform::byte, 16>> identities;
  for (const auto& d : r.filespaces) {
    if (!Identity(d.database_uuid, K::database) || !Identity(d.filespace_uuid, K::filespace) ||
        !Identity(d.writer_identity_uuid, K::object) || !identities.insert(d.filespace_uuid.value.bytes).second)
      return Error::identity;
    if (!Capacity(d)) return Error::capacity;
    if (!d.header_generation) return Error::generation;
    if (static_cast<u16>(d.role) > static_cast<u16>(FilespaceRole::forbidden) ||
        static_cast<u16>(d.state) > static_cast<u16>(FilespaceState::forbidden) ||
        !disk::IsSupportedDatabasePageSize(d.page_size) || d.path.size() > UINT32_MAX ||
        d.pins.size() > UINT32_MAX) return Error::row;
    for (const auto& p : d.pins)
      if (static_cast<u16>(p.kind) > static_cast<u16>(FilespacePinKind::external) ||
          p.owner.size() > UINT32_MAX) return Error::row;
  }
  u64 prior_sequence = 0;
  for (const auto& e : r.evidence) {
    if (!Identity(e.filespace_uuid, K::filespace)) return Error::identity;
    if (!e.sequence || e.sequence <= prior_sequence || e.sequence >= r.next_evidence_sequence ||
        static_cast<u16>(e.operation) > static_cast<u16>(FilespaceOperation::salvage_filespace) ||
        static_cast<u16>(e.previous_state) > static_cast<u16>(FilespaceState::forbidden) ||
        static_cast<u16>(e.new_state) > static_cast<u16>(FilespaceState::forbidden) ||
        static_cast<u16>(e.previous_role) > static_cast<u16>(FilespaceRole::forbidden) ||
        static_cast<u16>(e.new_role) > static_cast<u16>(FilespaceRole::forbidden) ||
        e.reason.size() > UINT32_MAX || e.diagnostic_code.size() > UINT32_MAX) return Error::row;
    prior_sequence = e.sequence;
  }
  return Error::none;
}
class Writer {
 public:
  std::string value;
  bool good = true;
  void Bytes(const char* data, std::size_t size) {
    if (!good || size > maximum_bytes - value.size()) { good = false; return; }
    value.append(data, size);
  }
  void Number(u64 value, unsigned width) {
    char data[8];
    for (unsigned i = 0; i < width; ++i) data[i] = static_cast<char>(value >> (8 * i));
    Bytes(data, width);
  }
  void Uuid(const TypedUuid& id) { Bytes(reinterpret_cast<const char*>(id.value.bytes.data()), 16); }
  void Text(const std::string& data) { Number(data.size(), 4); Bytes(data.data(), data.size()); }
};
class Reader {
 public:
  explicit Reader(std::string_view bytes) : value_(bytes) {}
  std::size_t remaining() const noexcept { return value_.size() - position_; }
  bool Number(u64& value, unsigned width) noexcept {
    if (width > remaining()) return false;
    value = 0;
    for (unsigned i = 0; i < width; ++i)
      value |= static_cast<u64>(static_cast<unsigned char>(value_[position_++])) << (8 * i);
    return true;
  }
  template<class T> bool Number(T& value, unsigned width) noexcept {
    u64 decoded;
    if (!Number(decoded, width) || decoded > std::numeric_limits<T>::max()) return false;
    value = static_cast<T>(decoded); return true;
  }
  template<class T> bool Enum(T& value) noexcept {
    u64 decoded;
    if (!Number(decoded, 2)) return false;
    value = static_cast<T>(decoded); return true;
  }
  bool Uuid(TypedUuid& id, core::platform::UuidKind kind) noexcept {
    if (remaining() < 16) return false;
    id.kind = kind;
    std::memcpy(id.value.bytes.data(), value_.data() + position_, 16); position_ += 16;
    return Identity(id, kind);
  }
  bool Text(std::string& value) {
    u64 size;
    if (!Number(size, 4) || size > remaining()) return false;
    value.assign(value_.data() + position_, static_cast<std::size_t>(size));
    position_ += static_cast<std::size_t>(size); return true;
  }
 private:
  std::string_view value_;
  std::size_t position_ = 0;
};
inline Error Encode(const FilespaceRegistry& r, std::string& output) {
  const auto checked = Validate(r);
  if (checked != Error::none) return checked;
  Writer w;
  w.Bytes(magic.data(), magic.size()); w.Number(0, 8);
  w.Number(r.next_evidence_sequence, 8); w.Number(r.filespaces.size(), 4); w.Number(r.evidence.size(), 4);
  for (const auto& d : r.filespaces) {
    w.Uuid(d.database_uuid); w.Uuid(d.filespace_uuid); w.Uuid(d.writer_identity_uuid);
    w.Number(static_cast<u16>(d.role), 2); w.Number(static_cast<u16>(d.state), 2);
    w.Number(d.page_size, 4); w.Number(d.generation, 8);
    const bool bits[]{d.read_only, d.archive_owner, d.history_owner, d.startup_authority,
        d.catalog_persistence_owner, d.filespace_manifest_owner, d.recovery_evidence_owner,
        d.first_filespace, d.active};
    u16 flags = 0; for (unsigned i = 0; i < 9; ++i) flags |= u16(bits[i]) << i;
    w.Number(flags, 2); w.Number(d.physical_filespace_id, 2);
    for (u64 n : {d.total_pages, d.free_pages, d.preallocated_pages, d.allocation_root_page, d.header_generation}) w.Number(n, 8);
    w.Text(d.path); w.Number(d.pins.size(), 4);
    for (const auto& p : d.pins) { w.Number(static_cast<u16>(p.kind), 2); w.Text(p.owner); w.Number(p.count, 8); }
  }
  for (const auto& e : r.evidence) {
    w.Number(e.sequence, 8); w.Number(static_cast<u16>(e.operation), 2); w.Uuid(e.filespace_uuid);
    for (u16 n : {static_cast<u16>(e.previous_state), static_cast<u16>(e.new_state),
                 static_cast<u16>(e.previous_role), static_cast<u16>(e.new_role)}) w.Number(n, 2);
    w.Number(e.durable_state_changed, 1); w.Text(e.reason); w.Text(e.diagnostic_code);
  }
  if (!w.good) return Error::row;
  for (unsigned i = 0; i < 8; ++i) w.value[8+i] = static_cast<char>(u64(w.value.size()) >> (8*i));
  output = std::move(w.value); return Error::none;
}
inline Error Decode(std::string_view bytes, FilespaceRegistry& output) {
  using K = core::platform::UuidKind;
  if (bytes.size() < header_bytes || bytes.size() > maximum_bytes || bytes.substr(0, 8) != magic) return Error::header;
  Reader in(bytes.substr(8));
  u64 length, files, evidence;
  FilespaceRegistry r;
  if (!in.Number(length, 8) || length != bytes.size() || !in.Number(r.next_evidence_sequence, 8) ||
      !in.Number(files, 4) || !in.Number(evidence, 4) ||
      files > in.remaining() / 116 || evidence > in.remaining() / 43) return Error::row;
  for (u64 i = 0; i < files; ++i) {
    FilespaceDescriptor d;
    if (!in.Uuid(d.database_uuid, K::database) || !in.Uuid(d.filespace_uuid, K::filespace) ||
        !in.Uuid(d.writer_identity_uuid, K::object)) return Error::identity;
    u16 flags; u64 pins;
    if (!in.Enum(d.role) || !in.Enum(d.state) || !in.Number(d.page_size, 4) || !in.Number(d.generation, 8) ||
        !in.Number(flags, 2) || (flags & ~u16(0x1ff)) || !in.Number(d.physical_filespace_id, 2) ||
        !in.Number(d.total_pages, 8) || !in.Number(d.free_pages, 8) || !in.Number(d.preallocated_pages, 8) ||
        !in.Number(d.allocation_root_page, 8) || !in.Number(d.header_generation, 8) ||
        !in.Text(d.path) || !in.Number(pins, 4) || pins > in.remaining() / 14) return Error::row;
    bool* bits[]{&d.read_only, &d.archive_owner, &d.history_owner, &d.startup_authority,
        &d.catalog_persistence_owner, &d.filespace_manifest_owner, &d.recovery_evidence_owner,
        &d.first_filespace, &d.active};
    for (unsigned b = 0; b < 9; ++b) *bits[b] = (flags & (u16(1) << b)) != 0;
    for (u64 p = 0; p < pins; ++p) {
      FilespacePin pin;
      if (!in.Enum(pin.kind) || !in.Text(pin.owner) || !in.Number(pin.count, 8)) return Error::row;
      d.pins.push_back(std::move(pin));
    }
    r.filespaces.push_back(std::move(d));
  }
  if (evidence > in.remaining() / 43) return Error::row;
  for (u64 i = 0; i < evidence; ++i) {
    FilespaceEvidenceRecord e;
    u64 changed;
    if (!in.Number(e.sequence, 8) || !in.Enum(e.operation) || !in.Uuid(e.filespace_uuid, K::filespace) ||
        !in.Enum(e.previous_state) || !in.Enum(e.new_state) || !in.Enum(e.previous_role) || !in.Enum(e.new_role) ||
        !in.Number(changed, 1) || changed > 1 || !in.Text(e.reason) || !in.Text(e.diagnostic_code)) return Error::row;
    e.durable_state_changed = changed != 0; r.evidence.push_back(std::move(e));
  }
  if (in.remaining()) return Error::row;
  const auto checked = Validate(r);
  if (checked != Error::none) return checked;
  output = std::move(r); return Error::none;
}
}
