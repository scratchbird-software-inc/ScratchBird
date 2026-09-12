// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_localized_record_codec.hpp"

namespace scratchbird::core::catalog {
const CatalogValueSchema& CatalogLocalizedNameSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65541, 1, {
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::utf8_text, true, 130872}, {3, T::utf8_text, true, 130872},
      {4, T::utf8_text, true, 130872}, {5, T::unsigned_integer, true, 8},
      {6, T::unsigned_integer, true, 8}}};
  return schema;
}
const CatalogValueSchema& CatalogLocalizedCommentSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65542, 1, {
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::utf8_text, true, 130896}, {3, T::utf8_text, true, 130896},
      {4, T::unsigned_integer, true, 8}}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogLocalizedName(const CatalogLocalizedNameRecord& r) {
  if (r.name_class != 1 || r.creator_transaction_number == 0)
    return {CatalogValueError::invalid_value, {}};
  if (r.language.size() > 130872 || r.path.size() > 130872 || r.name.size() > 130872 ||
      r.language.size() + r.path.size() + r.name.size() > 130872)
    return {CatalogValueError::size_limit, {}};
  return EncodeCatalogValueBlock(CatalogLocalizedNameSchema(), {
      {1, r.target_object_uuid}, {2, r.language}, {3, r.path}, {4, r.name},
      {5, r.name_class}, {6, r.creator_transaction_number}});
}
CatalogValueEncodeResult EncodeCatalogLocalizedComment(const CatalogLocalizedCommentRecord& r) {
  if (r.creator_transaction_number == 0) return {CatalogValueError::invalid_value, {}};
  if (r.language.size() > 130896 || r.comment.size() > 130896 ||
      r.language.size() + r.comment.size() > 130896)
    return {CatalogValueError::size_limit, {}};
  return EncodeCatalogValueBlock(CatalogLocalizedCommentSchema(), {
      {1, r.target_object_uuid}, {2, r.language}, {3, r.comment},
      {4, r.creator_transaction_number}});
}
CatalogLocalizedDecodeResult<CatalogLocalizedNameRecord> DecodeCatalogLocalizedName(std::string_view bytes) {
  if (bytes.size() < 104 || bytes.size() > kCatalogLocalizedPayloadMaxBytes)
    return {CatalogValueError::invalid_framing, {}};
  const auto decoded = DecodeCatalogValueBlock(CatalogLocalizedNameSchema(),
      std::vector<byte>(bytes.begin(), bytes.end()));
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogLocalizedNameRecord r;
  r.target_object_uuid = std::get<TypedUuid>(decoded.fields[0].value);
  r.language = std::get<std::string>(decoded.fields[1].value);
  r.path = std::get<std::string>(decoded.fields[2].value);
  r.name = std::get<std::string>(decoded.fields[3].value);
  r.name_class = std::get<u64>(decoded.fields[4].value);
  r.creator_transaction_number = std::get<u64>(decoded.fields[5].value);
  if (r.name_class != 1 || r.creator_transaction_number == 0)
    return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}
CatalogLocalizedDecodeResult<CatalogLocalizedCommentRecord> DecodeCatalogLocalizedComment(std::string_view bytes) {
  if (bytes.size() < 80 || bytes.size() > kCatalogLocalizedPayloadMaxBytes)
    return {CatalogValueError::invalid_framing, {}};
  const auto decoded = DecodeCatalogValueBlock(CatalogLocalizedCommentSchema(),
      std::vector<byte>(bytes.begin(), bytes.end()));
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogLocalizedCommentRecord r;
  r.target_object_uuid = std::get<TypedUuid>(decoded.fields[0].value);
  r.language = std::get<std::string>(decoded.fields[1].value);
  r.comment = std::get<std::string>(decoded.fields[2].value);
  r.creator_transaction_number = std::get<u64>(decoded.fields[3].value);
  if (r.creator_transaction_number == 0) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}
}  // namespace scratchbird::core::catalog
