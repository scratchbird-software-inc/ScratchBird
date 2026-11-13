// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_COMMENT_RECORD (step 4, owner Q81 = 1, 2026-11-13).
// The native comment row codec, schema 65678 version 1, against an
// independent byte oracle (bootstrap_policy_fixture's Golden, which uses no
// production schema or encoder):
//   - round trips: ASCII, multi-byte UTF-8, every language constant, the
//     maximum length; decode is heap-free;
//   - every refusal: unknown language, nil / non-v7 identities, comment UUID
//     equal to the target, empty text, text over the maximum, text that is not
//     UTF-8, another schema id, another version, a missing field, an extra
//     (unknown) field, a field of the wrong type, truncation;
//   - the metadata binding (header object = comment UUID, parent = target,
//     subtype localized_comment, no name) and its refusals, through the
//     metadata validator every catalog row passes;
//   - identity preservation for successors; the tree key;
//   - SB_NATIVE_COMMENT_TEXT_MAX: a row carrying a maximum-length comment, as
//     a first version and as a successor with both version links, fits an empty
//     catalog leaf at the smallest native page size (8192), measured with the
//     real leaf encoder; the same row one byte larger does not fit the row
//     page body bound the writers place rows under.
#include "catalog_comment_record_codec.hpp"
#include "catalog_language.hpp"
#include "catalog_object_constants.hpp"
#include "catalog_native_catalog_index.hpp"
#include "catalog_localized_record_codec.hpp"
#include "native_catalog_leaf_page.hpp"
#include "filespace_bootstrap.hpp"
#include "bootstrap_policy_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

thread_local bool heap_denied = false;
void* operator new(std::size_t size) {
  if (heap_denied) throw std::bad_alloc();
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace f = scratchbird::tests::bootstrap_policy;
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
unsigned checks = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) {
    heap_denied = false;
    throw std::runtime_error(why);
  }
}
c::CatalogCommentRecord Value(std::string text = "Orders placed by the web shop.") {
  return {f::Id(700), f::Id(1), c::kCatalogLanguage_en, std::move(text)};
}
// Independent field assembly: SBCV header, then (tag, type, length, bytes).
// Types: 3 UTF-8 text, 5 engine identity.
std::string Golden(const c::CatalogCommentRecord& r, unsigned schema = 65678, unsigned version = 1) {
  f::Golden g(schema, version);
  g.Identity(1, r.comment_uuid);
  g.Identity(2, r.target_object_uuid);
  g.Identity(3, r.language_uuid);
  g.Field(4, 3, r.comment);
  return g.Finish();
}
std::string AsString(const std::vector<p::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

void RoundTrip(const c::CatalogCommentRecord& r, const std::string& label) {
  const auto wire = Golden(r);
  const auto encoded = c::EncodeCatalogComment(r);
  Check(encoded.ok() && AsString(encoded.bytes) == wire, label + ": encodes to the oracle's bytes");
  heap_denied = true;
  const auto decoded = c::DecodeCatalogComment(wire);
  heap_denied = false;
  Check(decoded.ok() && decoded.record->comment_uuid == r.comment_uuid &&
            decoded.record->target_object_uuid == r.target_object_uuid && decoded.record->language_uuid == r.language_uuid &&
            decoded.record->comment == r.comment,
        label + ": decodes heap-free to the same record");
  Check(Golden(c::MaterializeCatalogComment(*decoded.record)) == wire, label + ": materializes back to the same bytes");
  Check(c::IsCatalogCommentPayload(wire), label + ": recognized by its schema id");
}

void Refusals() {
  using E = c::CatalogValueError;
  const auto refuses = [](c::CatalogCommentRecord r, E error, const std::string& label) {
    const auto encoded = c::EncodeCatalogComment(r);
    Check(!encoded.ok() && encoded.error == error && encoded.bytes.empty(), label + ": encode refused");
    // The same record as the oracle writes it is refused by the decoder too
    // (where the oracle can write it at all).
    const auto decoded = c::DecodeCatalogComment(Golden(r));
    Check(!decoded.ok() && !decoded.record, label + ": decode refused");
  };
  auto r = Value();
  // An unknown language: a v7 UUID that is no catalog_language constant.
  r.language_uuid = f::Id(9);
  refuses(r, E::invalid_value, "unknown language");
  r = Value();
  r.language_uuid = {};
  refuses(r, E::invalid_value, "nil language");
  r = Value();
  r.comment_uuid = {};
  refuses(r, E::invalid_value, "nil comment UUID");
  r = Value();
  r.target_object_uuid = {};
  refuses(r, E::invalid_value, "nil target");
  r = Value();
  r.target_object_uuid.bytes[6] = 0x40;  // a v4 UUID is not an engine identity
  refuses(r, E::invalid_value, "non-v7 target");
  r = Value();
  r.target_object_uuid = r.comment_uuid;
  refuses(r, E::invalid_value, "comment UUID equal to the target");
  refuses(Value(""), E::invalid_value, "empty text");
  refuses(Value(std::string(c::kCatalogCommentTextMaxBytes + 1, 'x')), E::size_limit, "text over the maximum");
  {
    // Not UTF-8: a lone continuation byte, an overlong form, a truncated
    // sequence, a surrogate.
    for (const std::string bad : {std::string("a\x80z"), std::string("\xc0\xaf"), std::string("ok\xe2\x82"),
                                  std::string("\xed\xa0\x80")}) {
      const auto encoded = c::EncodeCatalogComment(Value(bad));
      Check(!encoded.ok() && encoded.error == E::invalid_value, "invalid UTF-8 refused by the encoder");
      Check(!c::DecodeCatalogComment(Golden(Value(bad))).ok(), "invalid UTF-8 refused by the decoder");
    }
  }
  const auto wire = Golden(Value());
  Check(!c::DecodeCatalogComment(Golden(Value(), 65542)).ok(), "another schema id (the legacy 65542) refused");
  Check(!c::DecodeCatalogComment(Golden(Value(), 65679)).ok(), "another schema id refused");
  Check(!c::DecodeCatalogComment(Golden(Value(), 65678, 2)).ok(), "another version refused");
  Check(!c::IsCatalogCommentPayload(Golden(Value(), 65542)), "the legacy schema is not a native comment payload");
  {
    f::Golden g(65678);
    g.Identity(1, f::Id(700));
    g.Identity(2, f::Id(1));
    g.Field(4, 3, "no language");
    Check(!c::DecodeCatalogComment(g.Finish()).ok(), "a missing field refused");
  }
  {
    f::Golden g(65678);
    g.Identity(1, f::Id(700));
    g.Identity(2, f::Id(1));
    g.Identity(3, c::kCatalogLanguage_en);
    g.Field(4, 3, "text");
    g.Integer(5, 1);  // e.g. a comment kind or a creator transaction: not in this schema
    Check(!c::DecodeCatalogComment(g.Finish()).ok(), "an extra field refused");
  }
  {
    f::Golden g(65678);
    g.Identity(1, f::Id(700));
    g.Identity(2, f::Id(1));
    g.Field(3, 3, "en");  // the language as text, as 65542 has it
    g.Field(4, 3, "text");
    Check(!c::DecodeCatalogComment(g.Finish()).ok(), "the language as text refused");
  }
  for (std::size_t n = 0; n < wire.size(); ++n) {
    heap_denied = true;
    const auto cut = c::DecodeCatalogComment(std::string_view(wire).substr(0, n));
    heap_denied = false;
    Check(!cut.ok() && !cut.record, "every truncation refused without heap");
  }
  // The legacy record is untouched: 65542 still round trips its own way.
  c::CatalogLocalizedCommentRecord legacy;
  legacy.target_object_uuid = {p::UuidKind::object, f::Id(1)};
  legacy.language = "en";
  legacy.comment = "legacy";
  legacy.creator_transaction_number = 1;
  const auto legacy_bytes = c::EncodeCatalogLocalizedComment(legacy);
  Check(legacy_bytes.ok() && c::DecodeCatalogLocalizedComment(AsString(legacy_bytes.bytes)).ok() &&
            !c::DecodeCatalogComment(AsString(legacy_bytes.bytes)).ok(),
        "the legacy 65542 codec is unchanged and is not the native record");
}

c::CatalogMetadataVersion Metadata(const c::CatalogCommentRecord& r, const std::string& payload) {
  auto m = f::Metadata(0, {p::UuidKind::transaction, f::Id(5)}, f::Id(60), payload);
  m.record.header.kind = c::CatalogRecordKind::localized_comment;
  m.record.header.object_uuid = {p::UuidKind::object, r.comment_uuid};
  m.record.header.row_uuid = {p::UuidKind::row, f::Id(701)};
  m.record.header.parent_uuid = {p::UuidKind::object, r.target_object_uuid};
  m.object_subtype = c::CatalogObjectSubtype::localized_comment;
  m.default_name_uuid = {};
  m.name_vector_uuid = {};
  return m;
}
bool Valid(const c::CatalogMetadataVersion& m) {
  return !c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m)).has_value();
}

void MetadataBinding() {
  const auto r = Value();
  const auto wire = Golden(r);
  const auto m = Metadata(r, wire);
  Check(Valid(m) && c::CatalogCommentMatchesMetadata(c::BorrowCatalogMetadataVersion(m)) &&
            c::CatalogCommentMatchesHeader(c::BorrowCatalogMetadataVersion(m).record),
        "the comment row's metadata binds");
  Check(c::EncodeCatalogMetadataVersion(m).ok(), "and encodes as a catalog row version");
  auto x = m;
  x.record.header.object_uuid = {p::UuidKind::object, f::Id(702)};
  Check(!Valid(x), "an envelope object other than the comment UUID refused");
  x = m;
  x.record.header.parent_uuid = {p::UuidKind::object, f::Id(2)};
  Check(!Valid(x), "a parent other than the target refused");
  x = m;
  x.record.header.kind = c::CatalogRecordKind::policy;
  Check(!Valid(x), "another record kind refused");
  x = m;
  x.object_subtype = c::CatalogObjectSubtype::name_entry;
  Check(!Valid(x), "another subtype refused");
  x = m;
  x.default_name_uuid = {p::UuidKind::object, f::Id(703)};
  x.name_vector_uuid = {p::UuidKind::object, f::Id(704)};
  Check(!Valid(x), "a named comment refused (comment rows have no name)");
  x = m;
  x.record.payload = Golden(Value(), 65542);
  Check(!Valid(x), "the localized_comment subtype with another payload schema refused");
  x = m;
  x.record.header.row_uuid = {p::UuidKind::row, r.comment_uuid};
  Check(!Valid(x), "a row identity equal to the comment UUID refused");
  // Successors keep the identity; only the text may change.
  const auto a = c::DecodeCatalogComment(wire);
  auto changed = r;
  changed.comment = "Edited.";
  const auto changed_wire = Golden(changed);
  const auto b = c::DecodeCatalogComment(changed_wire);
  Check(a.ok() && b.ok() && c::CatalogCommentPreservesIdentity(*a.record, *b.record), "an edit keeps the identity");
  for (int i = 0; i < 3; ++i) {
    auto other = r;
    if (i == 0) other.comment_uuid = f::Id(705);
    if (i == 1) other.target_object_uuid = f::Id(2);
    if (i == 2) other.language_uuid = c::kCatalogLanguage_fr_CA;
    const auto other_wire = Golden(other);
    const auto o = c::DecodeCatalogComment(other_wire);
    Check(o.ok() && !c::CatalogCommentPreservesIdentity(*a.record, *o.record),
          "a change of comment UUID, target or language is not an edit");
  }
  const auto key = c::CatalogCommentTreeKey(r.target_object_uuid, r.language_uuid);
  Check(std::equal(r.target_object_uuid.bytes.begin(), r.target_object_uuid.bytes.end(), key.begin()) &&
            std::equal(r.language_uuid.bytes.begin(), r.language_uuid.bytes.end(), key.begin() + 16),
        "the tree key is target16 | language16");
  // The subtype constant (28b, appended 2026-11-13, pending owner confirmation).
  Check(static_cast<unsigned>(c::CatalogObjectSubtype::localized_comment) == 58 &&
            c::CatalogObjectSubtypeIdentity(c::CatalogObjectSubtype::localized_comment).has_value(),
        "subtype localized_comment is code 58 with a fixed identity");
}

// SB_NATIVE_COMMENT_TEXT_MAX.
db::NativeCatalogLeafPage EmptyLeaf(const d::CanonicalFilespacePageProfile& profile) {
  db::NativeCatalogLeafPage leaf;
  leaf.header = {profile.page_size_bytes, 6, f::Id(1), f::Id(2), f::Id(3), 40, 1, 0, profile.uuid};
  leaf.body.relation_uuid = {p::UuidKind::object, c::kNativeCommentCatalogRelationUuid};
  leaf.body.segment_id = leaf.body.segment_generation = leaf.body.page_generation = leaf.body.compaction_generation = 1;
  leaf.body.page_number = 40;
  return leaf;
}
pg::RowDataRecord Row(std::vector<p::byte> cell, bool successor) {
  pg::RowDataRecord row;
  row.row_uuid = {p::UuidKind::row, f::Id(701)};
  row.version_uuid = f::Id(801);
  row.transaction_uuid = {p::UuidKind::transaction, f::Id(5)};
  row.local_transaction_id = 1;
  row.internal_row_ordinal = row.stable_slot_id = 1;
  row.row_version = row.storage_generation = 1;
  if (successor) {  // both links: back to a predecessor, forward to a later version
    row.row_version = 2;
    row.previous_version_uuid = f::Id(800);
    row.previous_row_version = 1;
    row.next_version_uuid = f::Id(802);
    row.next_row_version = 3;
  }
  scratchbird::core::datatypes::DatatypeBinaryValue value;
  value.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
  value.payload = std::move(cell);
  row.cells.push_back({1, std::move(value), {}});
  return row;
}
void MaximumLength() {
  const auto& smallest = d::kCanonicalFilespacePageProfiles.front();
  Check(smallest.page_size_bytes == 8192, "the smallest native page size is 8192");
  for (const bool multibyte : {false, true}) {
    // Multi-byte text counts bytes, not characters: 3-byte U+20AC repeated,
    // padded with ASCII to exactly the maximum.
    std::string text;
    if (multibyte) {
      while (text.size() + 3 <= c::kCatalogCommentTextMaxBytes) text += "\xe2\x82\xac";
    }
    text.append(c::kCatalogCommentTextMaxBytes - text.size(), 'm');
    const auto r = Value(text);
    RoundTrip(r, multibyte ? "maximum length, multi-byte" : "maximum length");
    const auto m = Metadata(r, Golden(r));
    const auto encoded = c::EncodeCatalogMetadataVersion(m);
    Check(encoded.ok(), "a maximum-length comment row version encodes");
    for (const bool successor : {false, true}) {
      auto leaf = EmptyLeaf(smallest);
      leaf.body.rows.push_back(Row(encoded.bytes, successor));
      const auto fits = db::EncodeNativeCatalogLeaf(leaf);
      Check(fits.ok(), std::string("a maximum-length comment row fits an empty 8192 leaf") +
                           (successor ? " (successor with both links)" : " (first version)"));
      Check(pg::BuildRowDataPageBody(leaf.body, smallest.page_size_bytes - 32).ok(),
            "and the row page body bound the writers place rows under");
      // One byte more: the row no longer fits that bound.
      auto bigger = encoded.bytes;
      bigger.push_back(0);
      auto over = EmptyLeaf(smallest);
      over.body.rows.push_back(Row(bigger, successor));
      const auto body = pg::BuildRowDataPageBody(over.body, smallest.page_size_bytes - 32);
      Check(!body.ok() && body.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE",
            "a row one byte larger does not fit an empty 8192 leaf: the maximum is exact");
    }
  }
}
// --measure: the derivation of kCatalogCommentTextMaxBytes. The largest row
// cell that fits an empty 8192 leaf (successor form, both links), by the leaf
// encoder with a real comment row version, found by search over text length
// with the codec's own maximum lifted (the payload is assembled by the oracle
// and the metadata is checked by the metadata header's fixed layout only).
int Measure() {
  const auto& smallest = d::kCanonicalFilespacePageProfiles.front();
  const auto probe = [&](std::size_t text, bool successor) {
    const auto r = Value(std::string(std::min<std::size_t>(text, 100), 'm'));
    auto m = Metadata(r, Golden(r));
    auto encoded = c::EncodeCatalogMetadataVersion(m);
    Check(encoded.ok(), "a probe row encodes");
    // Grow the cell by the text beyond 100 bytes: the oracle's field length is
    // fixed width, so the encoded size grows byte for byte.
    auto cell = encoded.bytes;
    if (text > 100) cell.insert(cell.end(), text - 100, 0);
    auto leaf = EmptyLeaf(smallest);
    leaf.body.rows.push_back(Row(cell, successor));
    return pg::BuildRowDataPageBody(leaf.body, smallest.page_size_bytes - 32).ok();
  };
  for (const bool successor : {false, true}) {
    std::size_t lo = 100, hi = 9000;
    while (lo + 1 < hi) {
      const auto mid = (lo + hi) / 2;
      (probe(mid, successor) ? lo : hi) = mid;
    }
    std::cout << (successor ? "successor" : "first") << ": largest text fitting the row body bound = " << lo << '\n';
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--measure") return Measure();
  try {
    RoundTrip(Value(), "ASCII");
    RoundTrip(Value("Commande pass\xc3\xa9\x65 \xe2\x82\xac \xf0\x9f\x93\xa6"), "multi-byte UTF-8");
    RoundTrip(Value("x"), "one byte");
    for (const auto& language : c::kCatalogLanguages) {
      auto r = Value();
      r.language_uuid = language.uuid;
      RoundTrip(r, "language constant");
    }
    Refusals();
    MetadataBinding();
    MaximumLength();
    std::cout << "PASS catalog comment record (schema 65678, max " << c::kCatalogCommentTextMaxBytes
              << " bytes) checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL catalog comment record: " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
