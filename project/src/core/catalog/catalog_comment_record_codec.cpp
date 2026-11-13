// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_comment_record_codec.hpp"

#include "catalog_language.hpp"
#include "catalog_object_constants.hpp"
#include "uuid.hpp"

#include <algorithm>

namespace scratchbird::core::catalog {
namespace {
using E = CatalogValueError;
using T = CatalogValueType;
constexpr CatalogValueFieldSchema kFields[]{
    {1, T::engine_identity, true, 16, UuidKind::object},  // comment_uuid
    {2, T::engine_identity, true, 16, UuidKind::object},  // target_object_uuid
    {3, T::engine_identity, true, 16, UuidKind::object},  // language_uuid (a language constant)
    {4, T::utf8_text, true, static_cast<u32>(kCatalogCommentTextMaxBytes)},  // comment
};
constexpr CatalogValueSchemaView kSchema{kCatalogCommentSchemaId, kCatalogCommentSchemaVersion, kFields};
const CatalogValueSchema& Schema() {
  static const CatalogValueSchema schema{kSchema.id, kSchema.version, {std::begin(kFields), std::end(kFields)}};
  return schema;
}
bool Id(const Uuid& id) { return uuid::IsEngineIdentityUuid(id); }
template <class Record>
E Check(const Record& r) {
  if (!Id(r.comment_uuid) || !Id(r.target_object_uuid) || r.comment_uuid == r.target_object_uuid ||
      !IsKnownCatalogLanguage(r.language_uuid) || r.comment.empty())
    return E::invalid_value;
  if (r.comment.size() > kCatalogCommentTextMaxBytes) return E::size_limit;
  return E::none;
}
bool Typed(const TypedUuid& id, UuidKind kind) { return id.kind == kind && Id(id.value); }
bool Header(const CatalogTypedRecordView& m, const CatalogCommentRecordView& r) {
  return m.header.kind == CatalogRecordKind::localized_comment && m.header.object_uuid.kind == UuidKind::object &&
         m.header.object_uuid.value == r.comment_uuid && m.header.parent_uuid.kind == UuidKind::object &&
         m.header.parent_uuid.value == r.target_object_uuid && Typed(m.header.row_uuid, UuidKind::row) &&
         m.header.row_uuid.value != r.comment_uuid && m.header.row_uuid.value != r.target_object_uuid;
}
}  // namespace

CatalogValueEncodeResult EncodeCatalogComment(const CatalogCommentRecord& r) {
  if (const auto error = Check(r); error != E::none) return {error, {}};
  return EncodeCatalogValueBlock(Schema(), {{1, TypedUuid{UuidKind::object, r.comment_uuid}},
                                            {2, TypedUuid{UuidKind::object, r.target_object_uuid}},
                                            {3, TypedUuid{UuidKind::object, r.language_uuid}},
                                            {4, r.comment}});
}

CatalogCommentDecodeResult DecodeCatalogComment(std::string_view bytes) {
  std::array<CatalogValueFieldView, 4> backing;
  const auto d = DecodeCatalogValueBlockInto(kSchema, {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, backing);
  if (!d.ok()) return {d.error, {}};
  if (d.fields.size() != 4) return {E::missing_field, {}};
  CatalogCommentRecordView r;
  for (const auto& f : d.fields) switch (f.id) {
      case 1: r.comment_uuid = f.identity()->value; break;
      case 2: r.target_object_uuid = f.identity()->value; break;
      case 3: r.language_uuid = f.identity()->value; break;
      case 4: r.comment = {reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()}; break;
      default: return {E::unknown_field, {}};
    }
  if (const auto error = Check(r); error != E::none) return {error, {}};
  return {E::none, r};
}

CatalogCommentRecord MaterializeCatalogComment(const CatalogCommentRecordView& r) {
  return {r.comment_uuid, r.target_object_uuid, r.language_uuid, std::string(r.comment)};
}

bool IsCatalogCommentPayload(std::string_view b) {
  return b.size() >= kCatalogValueBlockHeaderBytes && b.substr(0, 4) == "SBCV" &&
         platform::LoadLittle32(reinterpret_cast<const byte*>(b.data()) + 16) == kCatalogCommentSchemaId;
}
bool CatalogCommentMatchesHeader(const CatalogTypedRecordView& m) {
  const auto r = DecodeCatalogComment(m.payload);
  return r.ok() && Header(m, *r.record);
}
bool CatalogCommentMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeCatalogComment(m.record.payload);
  return r.ok() && Header(m.record, *r.record) && m.object_subtype == CatalogObjectSubtype::localized_comment &&
         !m.default_name_uuid.valid() && m.default_name_uuid.value.is_nil() && !m.name_vector_uuid.valid() &&
         m.name_vector_uuid.value.is_nil();
}
bool CatalogCommentPreservesIdentity(const CatalogCommentRecordView& a, const CatalogCommentRecordView& b) {
  return a.comment_uuid == b.comment_uuid && a.target_object_uuid == b.target_object_uuid &&
         a.language_uuid == b.language_uuid;
}
std::array<byte, 32> CatalogCommentTreeKey(const Uuid& target, const Uuid& language) noexcept {
  std::array<byte, 32> key{};
  std::copy(target.bytes.begin(), target.bytes.end(), key.begin());
  std::copy(language.bytes.begin(), language.bytes.end(), key.begin() + 16);
  return key;
}
}  // namespace scratchbird::core::catalog
