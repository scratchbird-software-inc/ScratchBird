// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_NATIVE_COMMENT_RECORD (owner Q81 = 1, Q82 = 1, 2026-11-13).
// The native comment row: one row per (target object, language), in the
// comments relation (catalog relation role 7, SB_NATIVE_COMMENT_RELATION).
// Schema 65678 version 1, record kind localized_comment, subtype
// localized_comment. Exactly four fields:
//   1 comment_uuid        engine identity (object): the row's own object
//                         identity, equal to the metadata envelope's object
//   2 target_object_uuid  engine identity (object): the object commented on
//   3 language_uuid       engine identity (object): a catalog_language constant
//   4 comment             UTF-8 text, 1..kCatalogCommentTextMaxBytes bytes
// No target kind, no comment kind and no creator transaction: the envelope
// carries the transaction. Comment rows are data (owner Q79 = 1, like name
// rows, Q50): they have no name and are not cited by the metadata-change audit.
//
// The legacy schema 65542 (catalog_localized_record_codec, language as text)
// is the legacy create's and is unchanged; the native catalog never writes it.
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::core::catalog {
inline constexpr u32 kCatalogCommentSchemaId = 65678;
inline constexpr u16 kCatalogCommentSchemaVersion = 1;
// SB_NATIVE_COMMENT_TEXT_MAX: the longest comment text, in bytes. A comment is
// conceptually a large text (varchar(max) / BLOB SUB_TYPE TEXT), but the native
// catalog keeps every row version inside one catalog leaf and has no out-of-row
// storage for catalog rows. This is the most one row can carry at the smallest
// native page size (8192), measured with the real encoders
// (catalog_comment_record_test --measure): the row body bound every catalog
// writer places rows under is the page less its 32-byte seal tail (8160);
// from it come the row-data body header, one slot, the row record's fixed
// fields (both version links included, so a successor is no larger), the
// cell header, the 384-byte metadata version header, the typed record header,
// the 24-byte SBCV block header, three 24-byte identity fields and the text
// field's 8-byte header, leaving 7136 bytes of text. A row with a comment of
// this length fits an empty leaf as a first version and as a successor; one
// byte more does not (pinned by catalog_comment_record_test). Longer text is
// refused (size_limit). An overflow store for large catalog text is an open
// item, not invented here.
inline constexpr std::size_t kCatalogCommentTextMaxBytes = 7136;

struct CatalogCommentRecord {
  Uuid comment_uuid, target_object_uuid, language_uuid;
  std::string comment;
};
// Borrowed decode: `comment` views the input, which the caller retains.
struct CatalogCommentRecordView {
  Uuid comment_uuid, target_object_uuid, language_uuid;
  std::string_view comment;
};
struct CatalogCommentDecodeResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogCommentRecordView> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};
// Refusals: invalid_value for a nil or non-UUIDv7 identity, a comment UUID
// equal to the target, an unknown language (no catalog_language constant) or
// empty text; size_limit for text over the maximum; invalid_value from the
// block codec for text that is not UTF-8.
CatalogValueEncodeResult EncodeCatalogComment(const CatalogCommentRecord&);
// Allocation-free. Refuses another schema id or version, missing, unknown or
// mistyped fields, and every encode refusal above.
CatalogCommentDecodeResult DecodeCatalogComment(std::string_view);
CatalogCommentRecord MaterializeCatalogComment(const CatalogCommentRecordView&);
// The payload carries schema 65678 (the version and the rest are the decoder's).
bool IsCatalogCommentPayload(std::string_view);
// Kind localized_comment; the header's object is the comment UUID (kind
// object), its parent the target (kind object), its row a row identity.
bool CatalogCommentMatchesHeader(const CatalogTypedRecordView&);
// The header's rules plus subtype localized_comment and no name (no default
// name, no name vector).
bool CatalogCommentMatchesMetadata(const CatalogMetadataVersionView&);
// A successor keeps the comment UUID, the target and the language; only the
// text changes (SB_NATIVE_COMMENT_SUCCESSOR, owner Q82 = 1).
bool CatalogCommentPreservesIdentity(const CatalogCommentRecordView& before, const CatalogCommentRecordView& after);
// SB_NATIVE_COMMENT_TREE: the key of a comment row version's cell in the
// comments relation's (target, language) tree: target16 | language16.
std::array<byte, 32> CatalogCommentTreeKey(const Uuid& target, const Uuid& language) noexcept;
}  // namespace scratchbird::core::catalog
