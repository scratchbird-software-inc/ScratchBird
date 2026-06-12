// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lexer/lexer.hpp"
#include "parser_ipc_common.hpp"
#include "resources/language_resource_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::vector<sbsql::Token> MeaningfulTokens(const sbsql::LexResult& lexed) {
  std::vector<sbsql::Token> tokens;
  for (const auto& token : lexed.tokens) {
    if (!sbsql::IsTriviaToken(token) && token.kind != sbsql::TokenKind::kEnd) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

const sbsql::Token& FindTokenText(const std::vector<sbsql::Token>& tokens,
                                  std::string_view text) {
  const auto found = std::find_if(tokens.begin(), tokens.end(), [&](const auto& token) {
    return token.text == text;
  });
  if (found == tokens.end()) Fail("expected token text was not found");
  return *found;
}

bool HasDiagnosticCode(const sbsql::MessageVectorSet& messages,
                       std::string_view code) {
  return std::any_of(messages.diagnostics.begin(), messages.diagnostics.end(),
                     [&](const auto& diagnostic) { return diagnostic.code == code; });
}

std::vector<std::string> IssueCodes(const sbsql::ResourceValidationResult& result) {
  std::vector<std::string> codes;
  for (const auto& issue : result.issues) {
    codes.push_back(issue.code);
  }
  return codes;
}

void RequireIssueCodes(const sbsql::ResourceValidationResult& result,
                       const std::vector<std::string>& expected,
                       std::string_view message) {
  if (IssueCodes(result) != expected) Fail(message);
}

bool IssueDetailsContain(const sbsql::ResourceValidationResult& result,
                         std::string_view needle) {
  return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
    return issue.detail.find(needle) != std::string::npos;
  });
}

void RequireStableByteSpan(const sbsql::Token& token,
                           const std::string& source,
                           const std::string& raw,
                           std::string_view message) {
  const auto expected_offset = source.find(raw);
  Require(expected_offset != std::string::npos, "test fixture raw text missing");
  Require(token.kind == sbsql::TokenKind::kIdentifier, message);
  Require(token.offset == expected_offset, "SML-087 Unicode token byte offset drifted");
  Require(token.length == raw.size(), "SML-087 Unicode token byte length drifted");
  Require(token.raw_text == raw, "SML-087 Unicode token raw source text drifted");
  Require(token.text == raw, "SML-087 Unicode token text was normalized in lexer");
}

sbsql::CanonicalElementStream UnicodeCanonicalStream(std::string_view source,
                                                     std::size_t offset,
                                                     std::size_t length) {
  sbsql::CanonicalElementStream stream;
  stream.resource_identity = "sbsql.sml087.common_resource";
  stream.language_profile_uuid = "019f1087-0000-7000-8000-000000000101";
  stream.exact_tag = "und";
  stream.dialect_profile_uuid = "sbsql.v3";
  stream.topology_profile_uuid = "topology.sbsql.canonical_svo.v1";
  stream.common_resource_hash = "common.sml087.hash";
  stream.source_hash = "source." + std::to_string(sbsql::Fnv1a64(source));

  sbsql::CanonicalElement element;
  element.kind = sbsql::CanonicalElementKind::kIdentifier;
  element.canonical_text = "E_ACUTE_NAME";
  element.canonical_id = "SBSQL.SML087.IDENTIFIER";
  element.surface_id = "SBSQL.SML087.SURFACE";
  element.slot_id = "slot.identifier";
  element.alias_id = "alias.ime.committed";
  element.topology_role = "identifier";
  element.localized_text_hash = "localized." + std::to_string(sbsql::Fnv1a64(source));
  element.source_span = sbsql::CanonicalElementSourceSpan{offset, length};
  stream.elements.push_back(std::move(element));
  return stream;
}

void VerifyUnicodeImeRtlSourceSpans() {
  const std::string cjk_identifier =
      "\xE8""\xA1""\xA8""\xE5""\x90""\x8D"; // U+8868 U+540D
  const std::string hebrew_with_combining =
      "\xD7""\xA9""\xD6""\xB8""\xD7""\x9C""\xD7""\x95""\xD7""\x9D";
  const std::string arabic_rtl =
      "\xD9""\x85""\xD8""\xB1""\xD8""\xAD""\xD8""\xA8""\xD8""\xA7";
  const std::string decomposed_ime = "e""\xCC""\x81""_name";
  const std::string source = "SELECT " + cjk_identifier + ", " +
                             hebrew_with_combining + ", " + arabic_rtl +
                             ", " + decomposed_ime + " FROM source_table;";

  const auto lexed = sbsql::Lex(source);
  Require(!lexed.messages.has_errors(),
          "SML-087 committed IME/RTL/combining source was not admitted");
  const auto tokens = MeaningfulTokens(lexed);
  Require(tokens.size() == 11, "SML-087 Unicode fixture token count drifted");

  RequireStableByteSpan(FindTokenText(tokens, cjk_identifier), source,
                        cjk_identifier,
                        "SML-087 CJK IME identifier was not an identifier token");
  RequireStableByteSpan(FindTokenText(tokens, hebrew_with_combining), source,
                        hebrew_with_combining,
                        "SML-087 Hebrew combining identifier was not an identifier token");
  RequireStableByteSpan(FindTokenText(tokens, arabic_rtl), source, arabic_rtl,
                        "SML-087 Arabic RTL identifier was not an identifier token");
  RequireStableByteSpan(FindTokenText(tokens, decomposed_ime), source,
                        decomposed_ime,
                        "SML-087 decomposed IME identifier was not an identifier token");

  const std::string composed = "\xC3""\xA9""_name";
  const auto composed_tokens = MeaningfulTokens(sbsql::Lex(composed));
  const auto decomposed_tokens = MeaningfulTokens(sbsql::Lex(decomposed_ime));
  Require(composed_tokens.size() == 1 && decomposed_tokens.size() == 1,
          "SML-087 normalization fixture did not produce one identifier each");
  Require(composed_tokens.front().raw_text != decomposed_tokens.front().raw_text,
          "SML-087 lexer normalized distinct composed/decomposed source bytes");
  Require(composed_tokens.front().length == composed.size() &&
              decomposed_tokens.front().length == decomposed_ime.size(),
          "SML-087 composed/decomposed byte spans were not preserved");

  auto stream = UnicodeCanonicalStream(decomposed_ime, 0, decomposed_ime.size());
  auto result = sbsql::ValidateCanonicalElementStream(stream);
  Require(result.accepted,
          "SML-087 normalized canonical stream with localized source span was rejected");

  stream.normalized_before_uuid_resolution = false;
  result = sbsql::ValidateCanonicalElementStream(stream);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION"),
          "SML-087 post-UUID Unicode normalization did not fail closed");
}

void VerifyUnicodeFailClosedDiagnostics() {
  const std::string orphan_combining = "\xCC""\x81""bad";
  auto lexed = sbsql::Lex(orphan_combining);
  Require(lexed.messages.has_errors(),
          "SML-087 orphan combining mark did not fail closed");
  Require(HasDiagnosticCode(lexed.messages,
                            "SBSQL.UNICODE.COMBINING_MARK_WITHOUT_BASE"),
          "SML-087 orphan combining mark diagnostic missing");

  const std::string bidi_control = "ab""\xE2""\x80""\xAE""cd";
  lexed = sbsql::Lex(bidi_control);
  Require(lexed.messages.has_errors(),
          "SML-087 bidi control did not fail closed");
  Require(HasDiagnosticCode(lexed.messages,
                            "SBSQL.UNICODE.BIDI_CONTROL_FORBIDDEN"),
          "SML-087 bidi control diagnostic missing");

  const auto public_json = sbsql::MessageVectorToJson(lexed.messages);
  Require(public_json.find(bidi_control) == std::string::npos,
          "SML-087 Unicode diagnostic disclosed raw source text");
}

sbsql::PredictiveTextResourceFootprint ValidPredictiveFootprint() {
  sbsql::PredictiveTextResourceFootprint footprint;
  footprint.resource_identity = "sbsql.predictive.sml087";
  footprint.table_bytes = 1024;
  footprint.transition_fanout = 8;
  footprint.completion_results = 16;
  footprint.generation_millis = 5;
  footprint.memory_bytes = 4096;
  footprint.nested_expansion_depth = 2;
  return footprint;
}

void VerifyPredictiveResourceLimits() {
  const sbsql::LanguageResourceLimits limits;
  auto footprint = ValidPredictiveFootprint();
  auto result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  Require(result.accepted, "SML-087 valid predictive footprint was rejected");

  footprint = ValidPredictiveFootprint();
  footprint.table_bytes = limits.max_predictive_table_bytes + 1;
  result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(result, {"SBSQL.LANG_RESOURCE.PREDICTIVE_TABLE_SIZE_LIMIT"},
                    "SML-087 predictive table size limit code drifted");

  footprint = ValidPredictiveFootprint();
  footprint.transition_fanout = limits.max_transition_fanout + 1;
  result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(result, {"SBSQL.LANG_RESOURCE.PREDICTIVE_FANOUT_LIMIT"},
                    "SML-087 predictive fanout limit code drifted");

  footprint = ValidPredictiveFootprint();
  footprint.generation_millis = limits.max_generation_millis + 1;
  result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(result, {"SBSQL.LANG_RESOURCE.PREDICTIVE_TIME_LIMIT"},
                    "SML-087 predictive time limit code drifted");

  footprint = ValidPredictiveFootprint();
  footprint.memory_bytes = limits.max_predictive_memory_bytes + 1;
  result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(result, {"SBSQL.LANG_RESOURCE.PREDICTIVE_MEMORY_LIMIT"},
                    "SML-087 predictive memory limit code drifted");

  footprint = ValidPredictiveFootprint();
  footprint.hidden_object_no_disclosure = false;
  result = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(result, {"SBSQL.LANG_RESOURCE.PREDICTIVE_NO_DISCLOSURE_REQUIRED"},
                    "SML-087 predictive no-disclosure limit code drifted");
  Require(!IssueDetailsContain(result, "hidden_table") &&
              !IssueDetailsContain(result, "policy.secret") &&
              !IssueDetailsContain(result, "provider.local_password"),
          "SML-087 predictive no-disclosure issue leaked hidden object detail");

  footprint = ValidPredictiveFootprint();
  footprint.table_bytes = limits.max_predictive_table_bytes + 1;
  footprint.transition_fanout = limits.max_transition_fanout + 1;
  footprint.completion_results = limits.max_completion_results + 1;
  footprint.generation_millis = limits.max_generation_millis + 1;
  footprint.memory_bytes = limits.max_predictive_memory_bytes + 1;
  footprint.nested_expansion_depth = limits.max_nested_expansion_depth + 1;
  footprint.deterministic_limit_enforcement = false;
  footprint.hidden_object_no_disclosure = false;

  const std::vector<std::string> expected_codes = {
      "SBSQL.LANG_RESOURCE.PREDICTIVE_TABLE_SIZE_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_FANOUT_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_COMPLETION_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_TIME_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_MEMORY_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_DEPTH_LIMIT",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_DETERMINISM_REQUIRED",
      "SBSQL.LANG_RESOURCE.PREDICTIVE_NO_DISCLOSURE_REQUIRED"};
  const auto first = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  const auto second = sbsql::ValidatePredictiveTextResourceFootprint(footprint, limits);
  RequireIssueCodes(first, expected_codes,
                    "SML-087 predictive multi-limit issue order drifted");
  Require(IssueCodes(first) == IssueCodes(second),
          "SML-087 predictive multi-limit refusal was not deterministic");
}

} // namespace

int main() {
  VerifyUnicodeImeRtlSourceSpans();
  VerifyUnicodeFailClosedDiagnostics();
  VerifyPredictiveResourceLimits();
  std::cout << "sbsql_sml087_unicode_predictive_resource_conformance=passed\n";
  return EXIT_SUCCESS;
}
