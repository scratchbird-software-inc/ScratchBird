// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../engine/internal_api/catalog/column_metadata_codec.hpp"
#include "../core/datatypes/datatype_decimal_codec_profile.hpp"
#include "../core/datatypes/datatype_descriptor.hpp"
#include <charconv>
#include <optional>

namespace scratchbird::server {
// Project only the explicitly stored parameterized binding. The caller must
// independently resolve the supplied binary type code from the live registry
// and admit the persisted descriptor, including its column/resource fields.
// Do not confuse the base registry codec_uuid with decimal_codec_uuid.
inline bool ProjectStoredDecimalCodecProfile(
    const engine::internal_api::CatalogColumnMetadata& fields,
    std::uint32_t canonical_binary_type_code,
    std::optional<core::datatypes::DecimalCodecProfile>* output) {
  if (!output || fields.text.contains("decimal_codec_uuid") ||
      fields.identities.contains("decimal_codec_generation")) return false;
  const auto uuid = fields.identities.find("decimal_codec_uuid");
  const auto generation = fields.text.find("decimal_codec_generation");
  if (uuid == fields.identities.end() && generation == fields.text.end()) {
    output->reset();
    return true;
  }
  if (uuid == fields.identities.end() || generation == fields.text.end() ||
      canonical_binary_type_code !=
          static_cast<std::uint32_t>(core::datatypes::CanonicalTypeId::decimal)) return false;
  const auto number = [&](const char* key, auto* value) {
    const auto found = fields.text.find(key);
    if (found == fields.text.end() || found->second.empty() ||
        (found->second.size() > 1 && found->second.front() == '0')) return false;
    const auto& text = found->second;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
  };
  std::uint64_t epoch = 0;
  std::uint32_t precision = 0, scale = 0;
  if (!number("decimal_codec_generation", &epoch) ||
      !number("precision", &precision) || !number("scale", &scale) ||
      !core::datatypes::ExactDecimalCodecProfileMatches(uuid->second, epoch, precision, scale))
    return false;
  *output = core::datatypes::DecimalCodecProfile{uuid->second, epoch};
  return true;
}
} // namespace scratchbird::server
