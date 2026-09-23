// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/engine/internal_api/catalog/binary_catalog_metadata.hpp"
namespace scratchbird::tests {
// Inspect text attributes only. UUID alternatives are never formatted as text.
inline std::string EvidenceTextFields(const engine::internal_api::EngineEvidenceValue& value) {
  const auto* text = std::get_if<std::string>(&value);
  if (!text) return {};
  if (!text->starts_with("SBMETA02")) return *text;
  engine::internal_api::BinaryCatalogMetadata fields;
  if (!engine::internal_api::DecodeBinaryCatalogMetadata(*text, "crud.index_evidence.v2", &fields))
    throw std::invalid_argument("invalid_binary_index_evidence");
  std::string result;
  for (const auto& [key, field] : fields.text) {
    if (!result.empty()) result += '|';
    result += key + "=" + field;
  }
  return result;
}
// Text searches never match the UUID alternative, even for an empty needle.
inline bool EvidenceTextEquals(const engine::internal_api::EngineEvidenceValue& value,
                               std::string_view expected) {
  return std::holds_alternative<std::string>(value) && EvidenceTextFields(value) == expected;
}
template<class Needle>
inline std::size_t EvidenceTextFind(const engine::internal_api::EngineEvidenceValue& value,
                                   const Needle& needle, std::size_t position = 0) {
  return std::holds_alternative<std::string>(value)
      ? EvidenceTextFields(value).find(needle, position) : std::string::npos;
}
template<class Needle>
inline std::size_t EvidenceTextRfind(const engine::internal_api::EngineEvidenceValue& value,
                                    const Needle& needle, std::size_t position = std::string::npos) {
  return std::holds_alternative<std::string>(value)
      ? EvidenceTextFields(value).rfind(needle, position) : std::string::npos;
}
inline bool EvidenceTextStartsWith(const engine::internal_api::EngineEvidenceValue& value,
                                   std::string_view prefix) {
  return std::holds_alternative<std::string>(value) && EvidenceTextFields(value).starts_with(prefix);
}
inline bool EvidenceIdentityEquals(const engine::internal_api::EngineEvidenceValue& value,
                                   const core::platform::Uuid& expected) {
  if (const auto* id = std::get_if<core::platform::Uuid>(&value)) return *id == expected;
  const auto* bytes = std::get_if<std::string>(&value);
  engine::internal_api::BinaryCatalogMetadata fields;
  if (!bytes || !engine::internal_api::DecodeBinaryCatalogMetadata(*bytes, "crud.index_evidence.v2", &fields)) return false;
  const auto found = fields.identities.find("index_uuid");
  return found != fields.identities.end() && found->second == expected;
}
} // namespace scratchbird::tests
