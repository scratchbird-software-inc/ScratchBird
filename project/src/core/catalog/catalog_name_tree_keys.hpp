// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_lookup_key.hpp"
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
//     vector       vector UUID | collision policy UUID
//     entry key    object UUID | record kind (u16 BE) | 0x01 | language UUID
//                  | default marker (u8) | name class (u8)
//     entry        entry UUID | raw name text
//   SEARCH_KEY: SB_NATIVE_NAME_DEFAULT_INDEX (owner Q228 revisited, Q259,
//   2026-11-16: "a index that holds data as well - such as the uuid if using
//   the name index or name if using the uuid index - that is unique across
//   the language, and default"). The UUID -> name tree is that index: the
//   default marker is 0 for the object default (default_for_object, which is
//   also its language's default), 1 for a language default only, 2 for any
//   other name, so object | kind | 0x01 | language | {0, 1} holds exactly the
//   default entries of one (object, language), and the payload carries the
//   entry UUID and the name (covering: rendering reads no row). The writer
//   probes that range for the per-language default's uniqueness; the object
//   default's (across languages) is checked over the object's entries.
//   (Was: vector payload vector | default entry | collision policy UUID;
//   entry key without the marker; entry payload the entry UUID only,
//   SB_NATIVE_NAME_ROW_V4.)
//   SB_NATIVE_NAME_ROW_V4 (owner Q158 = 1, 2026-11-14): the entry payload is
//   the entry UUID only; rendering reads the row and walks its path
//   (WalkCatalogNamePath), so a rename touches no other row's cell. (Was:
//   entry UUID | LEB128 count | (LEB128 length | display bytes)*, the display
//   names of the entry's path, root side first.) The parser chooses the
//   separator and quoting (results are message vectors, owner direction
//   2026-10-21).
//
// esc(): every 0x00 byte becomes 0x00 0xFF and the field ends with 0x00 0x00,
// so byte order of the encoded keys is the order of the fields (an unsigned
// byte comparison, field by field) and no encoded field is a prefix of another.
// SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1): the own-lane key is derived from the
// row under its identifier profile (DeriveCatalogNameEntryKeys(...).own) and
// handed in; a version 4 row stores no lane. (Was: read from the row's stored
// normalized/exact key, CatalogNameEntryOwnLaneKey; and the row's
// full_path_lookup_key, the escaped own-lane keys of its path, was built by
// CatalogNameFullPathLookupKey.)
// UUIDs are their 16 raw bytes. Nothing here admits identities or policy.
void AppendCatalogNameTreeEscaped(std::vector<byte>& out, std::span<const byte> bytes);
void AppendCatalogNameTreeEscaped(std::vector<byte>& out, const CatalogNameDerivedText& text);

std::vector<byte> CatalogNameAuthoritativeTreeKey(const CatalogNameEntryView& entry, const CatalogNameDerivedText& own,
                                                  CatalogRecordKind kind);
std::vector<byte> CatalogNameAuthoritativeTreePayload(const CatalogNameEntryView& entry);
struct CatalogNameAuthoritativeTreePayloadView {
  Uuid object_uuid, entry_uuid;
};
std::optional<CatalogNameAuthoritativeTreePayloadView> DecodeCatalogNameAuthoritativeTreePayload(
    std::span<const byte> payload) noexcept;
// The authoritative key's prefix for one name in every class: scope, profile,
// language and own-lane key, without the record kind.
std::vector<byte> CatalogNameAuthoritativeTreeNamePrefix(const CatalogNameEntryView& entry,
                                                         const CatalogNameDerivedText& own);

std::vector<byte> CatalogNameUuidTreeVectorKey(const Uuid& object_uuid, CatalogRecordKind kind);
std::vector<byte> CatalogNameUuidTreeVectorPayload(const CatalogNameVectorView& vector);
struct CatalogNameUuidTreeVectorPayloadView {
  Uuid vector_uuid, collision_policy_uuid;  // (was also default_entry_uuid)
};
std::optional<CatalogNameUuidTreeVectorPayloadView> DecodeCatalogNameUuidTreeVectorPayload(
    std::span<const byte> payload) noexcept;

std::vector<byte> CatalogNameUuidTreeEntryKey(const CatalogNameEntryView& entry, CatalogRecordKind kind);
// SB_NATIVE_NAME_DEFAULT_INDEX: entry UUID | raw name text. (Was: the entry
// UUID only.)
std::vector<byte> CatalogNameUuidTreeEntryPayload(const CatalogNameEntryView& entry);
// The entry UUID; nullopt when the payload is shorter than 16 bytes.
std::optional<Uuid> DecodeCatalogNameUuidTreeEntryPayload(std::span<const byte> payload) noexcept;
// The default marker of an entry's UUID -> name key (0 object default, 1
// language default only, 2 neither).
byte CatalogNameUuidTreeDefaultMarker(const CatalogNameEntryView& entry) noexcept;
// object | kind | 0x01 | language: the prefix under which markers 0 and 1 are
// the (object, language)'s default entries.
std::vector<byte> CatalogNameUuidTreeLanguagePrefix(const Uuid& object_uuid, CatalogRecordKind kind, const Uuid& language);

// Allocation-free equality of stored bytes with what the functions above would
// encode, for the startup verifiers (which allocate nothing).
// SB_NATIVE_NAME_ROW_V4 (risk R4 of plan 35): startup verification recomputes
// the key from the derived own lane and compares it with the stored cell, so a
// derive change that would re-key a name cannot pass silently.
bool CatalogNameAuthoritativeTreeKeyEquals(const CatalogNameEntryView& entry, const CatalogNameDerivedText& own,
                                           CatalogRecordKind kind, std::span<const byte> key) noexcept;
bool CatalogNameAuthoritativeTreePayloadEquals(const CatalogNameEntryView& entry, std::span<const byte> payload) noexcept;
bool CatalogNameUuidTreeVectorKeyEquals(const Uuid& object_uuid, CatalogRecordKind kind, std::span<const byte> key) noexcept;
bool CatalogNameUuidTreeVectorPayloadEquals(const CatalogNameVectorView& vector, std::span<const byte> payload) noexcept;
bool CatalogNameUuidTreeEntryKeyEquals(const CatalogNameEntryView& entry, CatalogRecordKind kind,
                                       std::span<const byte> key) noexcept;
bool CatalogNameUuidTreeEntryPayloadEquals(const CatalogNameEntryView& entry, std::span<const byte> payload) noexcept;
}  // namespace scratchbird::core::catalog
