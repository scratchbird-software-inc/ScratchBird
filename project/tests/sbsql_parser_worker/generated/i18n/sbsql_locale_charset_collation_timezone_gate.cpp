// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_registry.hpp"
#include "datatype_temporal_wire.hpp"
#include "lexer/lexer.hpp"
#include "resource_seed_pack.hpp"
#include "sblr_operator_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SB_FSP012E_SEED_PACK_ROOT
#define SB_FSP012E_SEED_PACK_ROOT ""
#endif

namespace datatypes = scratchbird::core::datatypes;
namespace engine = scratchbird::engine::internal_api;
namespace parser = scratchbird::parser::sbsql;
namespace resources = scratchbird::core::resources;
namespace sblr = scratchbird::engine::sblr;

namespace {

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string message) {
    if (condition) return;
    ok = false;
    if (failures < 100) std::cerr << message << '\n';
    ++failures;
  }
};

std::vector<parser::Token> MeaningfulTokens(const parser::LexResult& lexed) {
  std::vector<parser::Token> out;
  for (const auto& token : lexed.tokens) {
    if (!parser::IsTriviaToken(token) && token.kind != parser::TokenKind::kEnd) {
      out.push_back(token);
    }
  }
  return out;
}

bool HasToken(const parser::LexResult& lexed,
              parser::TokenKind kind,
              std::string_view text = {},
              std::string_view family = {}) {
  for (const auto& token : lexed.tokens) {
    if (token.kind != kind) continue;
    if (!text.empty() && token.text != text) continue;
    if (!family.empty() && token.literal_family != family) continue;
    return true;
  }
  return false;
}

bool HasQuotedIdentifier(const parser::LexResult& lexed, std::string_view text) {
  for (const auto& token : lexed.tokens) {
    if (token.kind == parser::TokenKind::kIdentifier && token.quoted &&
        token.text == text) {
      return true;
    }
  }
  return false;
}

bool HasCommentContaining(const parser::LexResult& lexed, std::string_view text) {
  for (const auto& token : lexed.tokens) {
    if (token.kind == parser::TokenKind::kComment &&
        token.raw_text.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const sblr::SblrResult& result, std::string_view diagnostic_id) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [diagnostic_id](const sblr::SblrRuntimeDiagnostic& diagnostic) {
                       return diagnostic.diagnostic_id == diagnostic_id;
                     });
}

sblr::SblrValue TextValue(std::string value,
                          std::string charset = "UTF-8",
                          std::string collation = "unicode_ci") {
  sblr::SblrValue out;
  out.descriptor_id = "text";
  out.text_value = std::move(value);
  out.encoded_value = out.text_value;
  out.charset_name = std::move(charset);
  out.collation_name = std::move(collation);
  out.payload_kind = sblr::SblrValuePayloadKind::text;
  out.is_null = false;
  return out;
}

sblr::SblrValue TemporalValue(std::string descriptor_id, std::string encoded_value) {
  sblr::SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.encoded_value = std::move(encoded_value);
  out.text_value = out.encoded_value;
  out.payload_kind = sblr::SblrValuePayloadKind::temporal_text;
  out.is_null = false;
  return out;
}

datatypes::TimezoneSeedAuthority TimezoneSeedFromImage(
    const resources::ResourceSeedCatalogImage& image) {
  datatypes::TimezoneSeedAuthority seed;
  seed.active = image.active;
  seed.seed_pack_name = image.seed_pack_name;
  seed.seed_pack_version = image.seed_pack_version;
  seed.content_hash = image.content_hash;
  seed.timezone_records = image.timezone_records;
  seed.timezone_transition_records = image.timezone_transition_records;
  seed.timezone_leap_second_records = image.timezone_leap_second_records;
  for (const auto& alias : image.aliases) {
    if (alias.family != resources::ResourceSeedFamily::timezone_tables) continue;
    if (std::find(seed.timezone_names.begin(), seed.timezone_names.end(),
                  alias.alias) == seed.timezone_names.end()) {
      seed.timezone_names.push_back(alias.alias);
    }
    if (std::find(seed.timezone_names.begin(), seed.timezone_names.end(),
                  alias.canonical_name) == seed.timezone_names.end()) {
      seed.timezone_names.push_back(alias.canonical_name);
    }
  }
  return seed;
}

void ValidateParserLocaleSurface(Harness* harness) {
  const auto lexed = parser::Lex(
      "SELECT \"Résumé\", École, café FROM \"schéma\".таблица "
      "/* locale: fr_CA Café */ WHERE name COLLATE unicode_ci = N'café' "
      "AND payload = X'CAFE' AND flags = B'0101' "
      "AND born = DATE '2024-02-29' "
      "AND seen = TIMESTAMP '2024-11-03T01:30:00-04:00' "
      "AND duration = INTERVAL 'P1DT2H';");

  harness->Check(!lexed.messages.has_errors(),
                 "localized parser fixture emitted diagnostics");
  harness->Check(HasQuotedIdentifier(lexed, "Résumé"),
                 "quoted UTF-8 identifier was not preserved");
  harness->Check(HasToken(lexed, parser::TokenKind::kIdentifier, "École"),
                 "unquoted UTF-8 identifier was not tokenized");
  harness->Check(HasToken(lexed, parser::TokenKind::kIdentifier, "таблица"),
                 "non-Latin UTF-8 identifier was not tokenized");
  harness->Check(HasCommentContaining(lexed, "fr_CA Café"),
                 "localized comment trivia was not preserved");
  harness->Check(HasToken(lexed, parser::TokenKind::kStringLiteral, "café",
                          "national_string"),
                 "national string literal was not preserved");
  harness->Check(HasToken(lexed, parser::TokenKind::kBinaryLiteral, "CAFE",
                          "hex_binary"),
                 "hex binary literal was not preserved");
  harness->Check(HasToken(lexed, parser::TokenKind::kBinaryLiteral, "0101",
                          "bit_binary"),
                 "bit binary literal was not preserved");
  harness->Check(HasToken(lexed, parser::TokenKind::kTemporalLiteral,
                          "2024-02-29", "DATE"),
                 "DATE literal was not tokenized as temporal");
  harness->Check(HasToken(lexed, parser::TokenKind::kTemporalLiteral,
                          "2024-11-03T01:30:00-04:00", "TIMESTAMP"),
                 "timezone timestamp literal was not tokenized as temporal");
  harness->Check(HasToken(lexed, parser::TokenKind::kTemporalLiteral,
                          "P1DT2H", "INTERVAL"),
                 "INTERVAL literal was not tokenized as temporal");
  harness->Check(HasToken(lexed, parser::TokenKind::kKeyword, "COLLATE"),
                 "COLLATE keyword was not preserved in locale fixture");

  const auto meaningful = MeaningfulTokens(lexed);
  harness->Check(!meaningful.empty() && meaningful.back().kind ==
                                      parser::TokenKind::kStatementTerminator,
                 "localized fixture did not preserve statement terminator");
}

void ValidateEngineNameProfiles(Harness* harness) {
  engine::EngineRequestContext context;
  context.language_context.default_language_tag = "fr-CA";
  context.language_context.language_tag.clear();
  context.identifier_profile_uuid = "sbsql_v3";
  context.catalog_generation_id = 7;
  context.resource_epoch = 11;
  context.name_resolution_epoch = 13;
  context.local_transaction_id = 17;

  harness->Check(engine::NameRegistryDefaultLanguage(context) == "fr-CA",
                 "name registry default language fallback regressed");
  harness->Check(engine::NameRegistrySessionLanguage(context) == "fr-CA",
                 "name registry session language fallback regressed");
  harness->Check(engine::NameRegistryLookupKey("MixedCase", "sbsql_v3", false) ==
                     "MIXEDCASE",
                 "SBSQL identifier profile did not fold unquoted names upper");
  harness->Check(engine::NameRegistryLookupKey("MixedCase", "postgresql_family", false) ==
                     "mixedcase",
                 "PostgreSQL identifier profile did not fold unquoted names lower");
  harness->Check(engine::NameRegistryLookupKey("MixedCase", "mysql_case_insensitive", false) ==
                     "mixedcase",
                 "MySQL insensitive identifier profile did not fold lower");
  harness->Check(engine::NameRegistryLookupKey("MixedCase", "sbsql_v3", true) ==
                     "MixedCase",
                 "quoted/exact identifier lookup did not preserve case");

  engine::EngineLocalizedName localized;
  localized.name = "École";
  localized.raw_name_text = "École";
  localized.display_name = "École";
  localized.was_quoted = true;
  localized.default_name = true;
  localized.name_class = "primary";
  const auto entry = engine::MakeNameRegistryEntry(
      context, "019e07f3-012e-7000-8000-000000000001", "table",
      "019e07f3-012e-7000-8000-000000000002", localized, "fallback");
  harness->Check(entry.language_tag == "fr-CA",
                 "localized name entry did not inherit default language");
  harness->Check(entry.requires_exact_match && entry.exact_lookup_key == "École",
                 "quoted localized name did not require exact lookup");
  harness->Check(entry.normalized_lookup_key == "ÉCOLE",
                 "localized name normalized lookup did not preserve UTF-8 and fold ASCII profile");
  harness->Check(entry.resource_epoch == context.resource_epoch &&
                     entry.name_resolution_epoch == context.name_resolution_epoch,
                 "localized name entry did not retain authority epochs");
}

resources::ResourceSeedCatalogImage ValidateSeedPack(Harness* harness) {
  resources::ResourceSeedLoadConfig config;
  config.seed_pack_root = SB_FSP012E_SEED_PACK_ROOT;
  const auto loaded = resources::LoadResourceSeedPack(config);
  harness->Check(loaded.ok(), "initial resource seed pack did not load");
  if (!loaded.ok()) return {};

  const auto& image = loaded.image;
  harness->Check(image.active, "resource seed image is not active");
  harness->Check(!image.content_hash.empty(), "resource seed image lacks content hash");
  harness->Check(image.charset_records > 50, "charset seed rows are too small");
  harness->Check(image.charset_alias_records > 50, "charset alias rows are too small");
  harness->Check(image.collation_records > 100, "collation seed rows are too small");
  harness->Check(image.timezone_records > 100, "timezone seed rows are too small");
  harness->Check(image.timezone_transition_records > 100,
                 "timezone transition rows are too small");
  harness->Check(image.timezone_leap_second_records > 1,
                 "timezone leap-second rows are too small");

  const auto utf8 = resources::ResolveResourceSeedAlias(
      image, resources::ResourceSeedFamily::charset, "utf8mb4");
  harness->Check(utf8.ok() && utf8.alias.canonical_name == "UTF-8",
                 "UTF-8 charset alias did not resolve");
  const auto collation = resources::ResolveResourceSeedAlias(
      image, resources::ResourceSeedFamily::collation, "c.utf8");
  harness->Check(collation.ok() && collation.alias.canonical_name == "C.utf8",
                 "C.utf8 collation alias did not resolve case-insensitively");
  const auto eastern = resources::ResolveResourceSeedAlias(
      image, resources::ResourceSeedFamily::timezone_tables, "US/Eastern");
  harness->Check(eastern.ok() && eastern.alias.canonical_name == "America/New_York",
                 "timezone link alias did not resolve");
  const auto utc = resources::ResolveResourceSeedAlias(
      image, resources::ResourceSeedFamily::timezone_tables, "Etc/UTC");
  harness->Check(utc.ok() && utc.alias.canonical_name == "Etc/UTC",
                 "Etc/UTC timezone alias did not resolve");

  return image;
}

void ValidateDonorTemporalProfiles(
    const resources::ResourceSeedCatalogImage& image,
    Harness* harness) {
  const auto seed = TimezoneSeedFromImage(image);

  datatypes::DonorTemporalWireProfileRequest request;
  request.donor_engine = "postgresql";
  request.donor_type_or_family = "timestamptz";
  request.wire_profile = "timestamp_with_time_zone";
  request.encoded_value = "2024-11-03T01:30:00-04:00";
  request.timezone_seed = seed;
  const auto offset_result = datatypes::ValidateDonorTemporalWireProfile(request);
  harness->Check(offset_result.ok() &&
                     offset_result.timezone_identifier == "-04:00" &&
                     offset_result.timezone_offset_minutes == -240,
                 "offset timestamp donor wire profile did not normalize");

  request.encoded_value = "2024-03-10T02:30:00 America/New_York";
  const auto named_result = datatypes::ValidateDonorTemporalWireProfile(request);
  harness->Check(named_result.ok() &&
                     named_result.used_timezone_seed &&
                     named_result.timezone_identifier == "America/New_York",
                 "named timezone donor wire profile did not use seed authority");

  request.timezone_seed = {};
  const auto missing_seed = datatypes::ValidateDonorTemporalWireProfile(request);
  harness->Check(!missing_seed.ok() &&
                     missing_seed.diagnostic.diagnostic_code ==
                         "SB_DATATYPE_WIRE_CONVERSION_REJECTED",
                 "missing timezone seed authority did not fail closed");

  request.timezone_seed = seed;
  request.wire_profile = "date_wire";
  request.donor_type_or_family = "date";
  request.encoded_value = "2024-05-08Z";
  const auto forbidden_zone = datatypes::ValidateDonorTemporalWireProfile(request);
  harness->Check(!forbidden_zone.ok(), "DATE wire profile accepted a timezone");
}

void ValidateSblrI18nRuntime(Harness* harness) {
  const sblr::SblrExecutionContext context;

  const auto ci_like = sblr::EvaluateSblrStringOperator(
      "op_like", TextValue("Cafe"), TextValue("cafe"), context);
  harness->Check(ci_like.ok() && !ci_like.scalar_values.empty() &&
                     ci_like.scalar_values.front().has_int64_value &&
                     ci_like.scalar_values.front().int64_value == 1,
                 "case-insensitive collation did not affect LIKE");

  const auto concat = sblr::EvaluateSblrStringOperator(
      "op_concat", TextValue("pre", "UTF-8", "unicode_ci"),
      TextValue("fix", "UTF-8", "unicode_ci"), context);
  harness->Check(concat.ok() && !concat.scalar_values.empty() &&
                     concat.scalar_values.front().charset_name == "UTF-8" &&
                     concat.scalar_values.front().collation_name == "unicode_ci" &&
                     concat.scalar_values.front().text_value == "prefix",
                 "text concat did not preserve charset/collation metadata");

  const auto mismatch = sblr::EvaluateSblrStringOperator(
      "op_concat", TextValue("a", "UTF-8", "unicode_ci"),
      TextValue("b", "UTF-8", "unicode_cs"), context);
  harness->Check(!mismatch.ok() &&
                     HasDiagnostic(mismatch, "SBLR.CHARSET_COLLATION_MISMATCH"),
                 "collation mismatch did not fail closed");

  const auto temporal = sblr::EvaluateSblrArithmetic(
      "op_add", TemporalValue("timestamp_tz", "2024-03-10T01:30:00-05:00"),
      TemporalValue("interval", "3600"), context);
  harness->Check(temporal.ok() && !temporal.scalar_values.empty() &&
                     temporal.scalar_values.front().descriptor_id == "timestamp_tz" &&
                     temporal.scalar_values.front().encoded_value == "2024-03-10T07:30:00Z",
                 "timezone-aware temporal arithmetic did not normalize to UTC");

  const auto interval = sblr::EvaluateSblrArithmetic(
      "op_add", TemporalValue("interval", "P1DT2H"),
      TemporalValue("interval", "3600"), context);
  harness->Check(interval.ok() && !interval.scalar_values.empty() &&
                     interval.scalar_values.front().encoded_value == "97200",
                 "ISO interval arithmetic did not normalize to seconds");
}

}  // namespace

int main() {
  Harness harness;
  ValidateParserLocaleSurface(&harness);
  ValidateEngineNameProfiles(&harness);
  const auto image = ValidateSeedPack(&harness);
  if (harness.ok) {
    ValidateDonorTemporalProfiles(image, &harness);
  }
  ValidateSblrI18nRuntime(&harness);
  return harness.ok ? 0 : 1;
}
