// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_name_record_codec.hpp"
#include "catalog_records.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_NATIVE_NAME_TREES (owner rulings 2026-11-09: Q34 page-zero
// root kinds, Q35 = 2 key order, Q37/Q38 covering SBBTP002 cells, Q42
// superseding stamps; "the name should be indexed for fast uuid<->name
// conversions ... and it should hold the full path as part of its row").
// The keys and payloads of the two native name trees over the names relation
// (role 2). Both are SBBTP002 B-trees whose cells carry the entry or vector
// row's row and version UUIDs (the tie-break), its base leaf, its creator and
// any superseding stamp; one cell per row version.
//
//   Authoritative name tree (name -> UUID; the spec's authoritative index,
//   fields reordered by Q35 = 2 so that one name in every class of a scope sits
//   together, FirebirdSQL's meeting point for unique keys):
//     key     scope UUID | identifier profile UUID | esc(language tag)
//             | esc(the entry's own-lane lookup key) | record kind (u16, big endian)
//     payload object UUID | entry UUID
//   The own lane is the exact key when the entry requires an exact match, else
//   the normalized key: the key the collision rule compares.
//
//   UUID -> name tree (rendering without reading the entry rows):
//     vector key   object UUID | record kind (u16 BE) | 0x00
//     vector       vector UUID | default entry UUID | collision policy UUID
//     entry key    object UUID | record kind (u16 BE) | 0x01 | esc(language tag)
//                  | name class (u8)
//     entry        entry UUID | LEB128 count | (LEB128 length | display bytes)*
//   The entry payload's components are the display names of the entry's path
//   from below the database root to the entry itself; the parser chooses the
//   separator and quoting (results are message vectors, owner direction
//   2026-10-21).
//
// esc(): every 0x00 byte becomes 0x00 0xFF and the field ends with 0x00 0x00,
// so byte order of the encoded keys is the order of the fields (an unsigned
// byte comparison, field by field) and no encoded field is a prefix of another.
// The entry row's full_path_lookup_key is esc() of each component's own-lane
// lookup key, concatenated; path_component_count counts them.
// UUIDs are their 16 raw bytes. Nothing here admits identities or policy.
void AppendCatalogNameTreeEscaped(std::vector<byte>& out, std::span<const byte> bytes);
std::span<const byte> CatalogNameEntryOwnLaneKey(const CatalogNameEntryView& entry) noexcept;

std::vector<byte> CatalogNameAuthoritativeTreeKey(const CatalogNameEntryView& entry, CatalogRecordKind kind);
std::vector<byte> CatalogNameAuthoritativeTreePayload(const CatalogNameEntryView& entry);
struct CatalogNameAuthoritativeTreePayloadView {
  Uuid object_uuid, entry_uuid;
};
std::optional<CatalogNameAuthoritativeTreePayloadView> DecodeCatalogNameAuthoritativeTreePayload(
    std::span<const byte> payload) noexcept;
// The authoritative key's prefix for one name in every class: scope, profile,
// language and own-lane key, without the record kind.
std::vector<byte> CatalogNameAuthoritativeTreeNamePrefix(const CatalogNameEntryView& entry);

std::vector<byte> CatalogNameUuidTreeVectorKey(const Uuid& object_uuid, CatalogRecordKind kind);
std::vector<byte> CatalogNameUuidTreeVectorPayload(const CatalogNameVectorView& vector);
struct CatalogNameUuidTreeVectorPayloadView {
  Uuid vector_uuid, default_entry_uuid, collision_policy_uuid;
};
std::optional<CatalogNameUuidTreeVectorPayloadView> DecodeCatalogNameUuidTreeVectorPayload(
    std::span<const byte> payload) noexcept;

std::vector<byte> CatalogNameUuidTreeEntryKey(const CatalogNameEntryView& entry, CatalogRecordKind kind);
std::vector<byte> CatalogNameUuidTreeEntryPayload(const Uuid& entry_uuid,
                                                  std::span<const std::string_view> display_components);
struct CatalogNameUuidTreeEntryPayloadView {
  Uuid entry_uuid;
  std::vector<std::string_view> display_components;  // borrow the payload
};
std::optional<CatalogNameUuidTreeEntryPayloadView> DecodeCatalogNameUuidTreeEntryPayload(
    std::span<const byte> payload);

std::vector<byte> CatalogNameFullPathLookupKey(std::span<const std::span<const byte>> component_keys);

// Allocation-free equality of stored bytes with what the functions above would
// encode, for the startup verifiers (which allocate nothing).
bool CatalogNameAuthoritativeTreeKeyEquals(const CatalogNameEntryView& entry, CatalogRecordKind kind,
                                           std::span<const byte> key) noexcept;
bool CatalogNameAuthoritativeTreePayloadEquals(const CatalogNameEntryView& entry, std::span<const byte> payload) noexcept;
bool CatalogNameUuidTreeVectorKeyEquals(const Uuid& object_uuid, CatalogRecordKind kind, std::span<const byte> key) noexcept;
bool CatalogNameUuidTreeVectorPayloadEquals(const CatalogNameVectorView& vector, std::span<const byte> payload) noexcept;
bool CatalogNameUuidTreeEntryKeyEquals(const CatalogNameEntryView& entry, CatalogRecordKind kind,
                                       std::span<const byte> key) noexcept;
// Decodes a UUID-to-name entry payload into caller storage: the entry UUID and
// up to components.size() display components. False when malformed or when
// there are more components than the storage holds.
bool DecodeCatalogNameUuidTreeEntryPayloadInto(std::span<const byte> payload, Uuid& entry_uuid,
                                               std::span<std::string_view> components, std::size_t& count) noexcept;
}  // namespace scratchbird::core::catalog
