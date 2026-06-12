// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ceic_036_canonical_key_ordering_fuzz.hpp"

#include "expression_index_extractor.hpp"
#include "partial_index_implication.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::engine};
}

Ceic036CanonicalKeyOrderingFuzzResult Refuse(std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  Ceic036CanonicalKeyOrderingFuzzResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeIndexKeyEncodingDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back("ceic036_fail_closed=true");
  return result;
}

TypedUuid StableUuid(UuidKind kind, unsigned seed) {
  TypedUuid typed;
  typed.kind = kind;
  for (std::size_t i = 0; i < typed.value.bytes.size(); ++i) {
    typed.value.bytes[i] =
        static_cast<byte>((seed * 41u + i * 17u + 0x35u) & 0xffu);
  }
  typed.value.bytes[6] =
      static_cast<byte>((typed.value.bytes[6] & 0x0fu) | 0x70u);
  typed.value.bytes[8] =
      static_cast<byte>((typed.value.bytes[8] & 0x3fu) | 0x80u);
  return typed;
}

std::vector<byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

std::vector<byte> SortableI64Payload(std::int64_t value) {
  const auto sortable =
      static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

int Sign(int value) {
  return (value > 0) - (value < 0);
}

int CompareBytes(const std::vector<byte>& left, const std::vector<byte>& right) {
  return left < right ? -1 : (right < left ? 1 : 0);
}

bool StartsWithBytes(const std::vector<byte>& value,
                     const std::vector<byte>& prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

enum class LogicalKind {
  signed_i64,
  text,
  binary
};

struct LogicalComponent {
  LogicalKind logical_kind = LogicalKind::text;
  IndexKeyComponentKind component_kind = IndexKeyComponentKind::scalar;
  IndexKeySortDirection direction = IndexKeySortDirection::ascending;
  IndexKeyNullPlacement null_placement = IndexKeyNullPlacement::nulls_last;
  bool is_null = false;
  bool case_folded = false;
  std::int64_t signed_value = 0;
  std::string text_value;
  std::vector<byte> binary_value;
};

LogicalComponent Number(std::int64_t value,
                        IndexKeySortDirection direction =
                            IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::signed_i64;
  component.signed_value = value;
  component.direction = direction;
  return component;
}

LogicalComponent Text(std::string value,
                      IndexKeySortDirection direction =
                          IndexKeySortDirection::ascending,
                      bool case_folded = false) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::text;
  component.component_kind = case_folded ? IndexKeyComponentKind::collation_key
                                         : IndexKeyComponentKind::text_token;
  component.text_value = std::move(value);
  component.direction = direction;
  component.case_folded = case_folded;
  return component;
}

LogicalComponent Binary(std::vector<byte> value,
                        IndexKeySortDirection direction =
                            IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::binary;
  component.component_kind = IndexKeyComponentKind::scalar;
  component.binary_value = std::move(value);
  component.direction = direction;
  return component;
}

LogicalComponent ExpressionText(std::string value) {
  auto component = Text(std::move(value));
  component.component_kind = IndexKeyComponentKind::expression;
  return component;
}

LogicalComponent Null(IndexKeyNullPlacement placement,
                      IndexKeySortDirection direction =
                          IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::text;
  component.is_null = true;
  component.null_placement = placement;
  component.direction = direction;
  return component;
}

IndexKeyEncodingComponent Component(const LogicalComponent& logical,
                                    scratchbird::core::platform::u32 ordinal) {
  IndexKeyEncodingComponent component;
  component.kind = logical.component_kind;
  component.ordinal = ordinal;
  component.type_descriptor_uuid = StableUuid(UuidKind::object, 0x70u + ordinal);
  component.sort_direction = logical.direction;
  component.null_placement = logical.null_placement;
  component.is_null = logical.is_null;
  component.case_folded = logical.case_folded;
  if (logical.is_null) {
    return component;
  }

  switch (logical.logical_kind) {
    case LogicalKind::signed_i64:
      component.payload = SortableI64Payload(logical.signed_value);
      break;
    case LogicalKind::text:
      component.payload = Bytes(logical.text_value);
      break;
    case LogicalKind::binary:
      component.payload = logical.binary_value;
      break;
  }
  return component;
}

std::vector<IndexKeyEncodingComponent> Components(
    const std::vector<LogicalComponent>& logical) {
  std::vector<IndexKeyEncodingComponent> components;
  components.reserve(logical.size());
  for (std::size_t i = 0; i < logical.size(); ++i) {
    components.push_back(
        Component(logical[i],
                  static_cast<scratchbird::core::platform::u32>(i)));
  }
  return components;
}

int NullRank(const LogicalComponent& component) {
  if (!component.is_null) {
    return 1;
  }
  return component.null_placement == IndexKeyNullPlacement::nulls_first ? 0 : 2;
}

int CompareValue(const LogicalComponent& left, const LogicalComponent& right) {
  switch (left.logical_kind) {
    case LogicalKind::signed_i64:
      return left.signed_value < right.signed_value
                 ? -1
                 : (right.signed_value < left.signed_value ? 1 : 0);
    case LogicalKind::text: {
      const auto left_text =
          left.case_folded ? LowerAscii(left.text_value) : left.text_value;
      const auto right_text =
          right.case_folded ? LowerAscii(right.text_value) : right.text_value;
      return left_text < right_text ? -1 : (right_text < left_text ? 1 : 0);
    }
    case LogicalKind::binary:
      return CompareBytes(left.binary_value, right.binary_value);
  }
  return 0;
}

int LogicalCompare(const std::vector<LogicalComponent>& left,
                   const std::vector<LogicalComponent>& right) {
  if (left.size() != right.size()) {
    return 0;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    int comparison = NullRank(left[i]) - NullRank(right[i]);
    if (comparison == 0 && !left[i].is_null && !right[i].is_null) {
      comparison = CompareValue(left[i], right[i]);
      if (left[i].direction == IndexKeySortDirection::descending) {
        comparison = -comparison;
      }
    }
    if (comparison != 0) {
      return Sign(comparison);
    }
  }
  return 0;
}

bool RequirePair(Ceic036CanonicalKeyOrderingFuzzResult* result,
                 const std::vector<LogicalComponent>& left,
                 const std::vector<LogicalComponent>& right,
                 const IndexKeySemanticProfile& profile,
                 std::string_view label) {
  const auto expected = LogicalCompare(left, right);
  const auto encoded_left = EncodeIndexKey(Components(left), profile);
  const auto encoded_right = EncodeIndexKey(Components(right), profile);
  if (!encoded_left.ok() || !encoded_right.ok()) {
    result->evidence.push_back(std::string(label) + ":encode_failed");
    return false;
  }
  const auto comparison =
      CompareEncodedIndexKeys(encoded_left.encoded, encoded_right.encoded);
  if (!comparison.ok() || Sign(comparison.comparison) != expected) {
    result->evidence.push_back(std::string(label) + ":compare_mismatch");
    return false;
  }
  const int direct_lex =
      std::lexicographical_compare(encoded_left.encoded.begin(),
                                   encoded_left.encoded.end(),
                                   encoded_right.encoded.begin(),
                                   encoded_right.encoded.end())
          ? -1
          : (std::lexicographical_compare(encoded_right.encoded.begin(),
                                          encoded_right.encoded.end(),
                                          encoded_left.encoded.begin(),
                                          encoded_left.encoded.end())
                 ? 1
                 : 0);
  if (Sign(direct_lex) != expected) {
    result->evidence.push_back(std::string(label) + ":lex_mismatch");
    return false;
  }
  result->evidence.push_back(std::string(label) + ":semantic_order=encoded_order");
  return true;
}

bool AnyAuthorityClaimed(const Ceic036AuthorityBoundaryClaims& claims,
                         std::string* detail) {
  const struct {
    bool Ceic036AuthorityBoundaryClaims::*field;
    const char* name;
  } checks[] = {
      {&Ceic036AuthorityBoundaryClaims::visibility_authority,
       "visibility_authority"},
      {&Ceic036AuthorityBoundaryClaims::authorization_security_authority,
       "authorization_security_authority"},
      {&Ceic036AuthorityBoundaryClaims::transaction_finality_authority,
       "transaction_finality_authority"},
      {&Ceic036AuthorityBoundaryClaims::recovery_authority,
       "recovery_authority"},
      {&Ceic036AuthorityBoundaryClaims::parser_authority,
       "parser_authority"},
      {&Ceic036AuthorityBoundaryClaims::reference_authority,
       "reference_authority"},
      {&Ceic036AuthorityBoundaryClaims::wal_authority,
       "wal_authority"},
      {&Ceic036AuthorityBoundaryClaims::benchmark_authority,
       "benchmark_authority"},
      {&Ceic036AuthorityBoundaryClaims::optimizer_plan_authority,
       "optimizer_plan_authority"},
      {&Ceic036AuthorityBoundaryClaims::index_finality_authority,
       "index_finality_authority"},
      {&Ceic036AuthorityBoundaryClaims::cluster_action_authority,
       "cluster_action_authority"},
      {&Ceic036AuthorityBoundaryClaims::agent_action_authority,
       "agent_action_authority"},
  };
  for (const auto& check : checks) {
    if (claims.*(check.field)) {
      if (detail != nullptr) {
        *detail = check.name;
      }
      return true;
    }
  }
  return false;
}

bool ProvePrefixBounds(Ceic036CanonicalKeyOrderingFuzzResult* result,
                       const IndexKeySemanticProfile& profile) {
  const auto prefix_components =
      Components({Binary(std::vector<byte>{'a', 0x00})});
  const auto matcher = BuildEncodedPrefixMatcher(prefix_components, profile);
  if (!matcher.ok() || matcher.matcher_prefix.empty() ||
      matcher.lower_bound != matcher.matcher_prefix ||
      matcher.upper_bound_unbounded || matcher.upper_bound.empty()) {
    result->evidence.push_back("prefix_bounds_generated=false");
    return false;
  }

  const auto matching =
      EncodeIndexKey(Components({Binary(std::vector<byte>{'a', 0x00, 'z'})}),
                     profile);
  const auto outside =
      EncodeIndexKey(Components({Binary(std::vector<byte>{'a', 0x01})}),
                     profile);
  if (!matching.ok() || !outside.ok()) {
    result->evidence.push_back("prefix_bounds_target_encoding=false");
    return false;
  }
  const auto lower_compare =
      CompareEncodedIndexKeys(matcher.lower_bound, matching.encoded);
  const auto upper_compare =
      CompareEncodedIndexKeys(matching.encoded, matcher.upper_bound);
  const bool ok = StartsWithBytes(matching.encoded, matcher.matcher_prefix) &&
                  !StartsWithBytes(outside.encoded, matcher.matcher_prefix) &&
                  lower_compare.ok() && lower_compare.comparison <= 0 &&
                  upper_compare.ok() && upper_compare.comparison < 0;
  result->evidence.push_back(std::string("prefix_bounds_generated=") +
                             (ok ? "true" : "false"));
  return ok;
}

bool ProveExpressionEnvelope(Ceic036CanonicalKeyOrderingFuzzResult* result,
                             const IndexKeySemanticProfile& profile) {
  const auto encoded =
      EncodeIndexKey(Components({ExpressionText("fn.lower_ascii(name)")}),
                     profile);
  if (!encoded.ok()) {
    result->evidence.push_back("expression_key_encoding=failed");
    return false;
  }
  const std::string encoded_text(
      reinterpret_cast<const char*>(encoded.encoded.data()),
      encoded.encoded.size());
  const auto accepted = ValidateExpressionIndexExtractorKeyEnvelope(encoded_text);
  const auto refused = ValidateExpressionIndexExtractorKeyEnvelope("bad-envelope");
  const bool ok = accepted.ok() && !refused.ok();
  result->evidence.push_back(std::string("expression_key_envelope_validated=") +
                             (ok ? "true" : "false"));
  return ok;
}

bool ProvePartialPredicate(Ceic036CanonicalKeyOrderingFuzzResult* result) {
  PartialPredicateImplicationRequest request;
  request.query_predicate_text = "tenant_id = 7 and status = 'A'";
  request.index_predicate_text = "status = 'A'";
  request.predicate_immutable = true;
  request.predicate_security_safe = true;
  request.descriptor_epoch_valid = true;
  request.resource_epoch_valid = true;
  request.collation_epoch_valid = true;
  request.function_epoch_valid = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  const auto proof = ProvePartialIndexPredicateImplication(request);
  const bool ok = proof.safe_to_consider_index && proof.predicate_implied &&
                  !proof.visibility_authority &&
                  !proof.authorization_authority &&
                  !proof.transaction_finality_authority &&
                  !proof.cleanup_authority && !proof.recovery_authority;
  result->evidence.push_back(std::string("partial_predicate_implication=") +
                             (ok ? "true" : "false"));
  result->evidence.push_back("partial_predicate_digest=" +
                             proof.canonical_index_predicate_digest);
  return ok;
}

bool ProveDeterministicPairs(Ceic036CanonicalKeyOrderingFuzzResult* result,
                             const IndexKeySemanticProfile& profile) {
  result->null_ordering_proven =
      RequirePair(result, {Null(IndexKeyNullPlacement::nulls_first)}, {Text("a")},
                  profile, "nulls_first_ascending") &&
      RequirePair(result, {Null(IndexKeyNullPlacement::nulls_last)}, {Text("a")},
                  profile, "nulls_last_ascending") &&
      RequirePair(result,
                  {Null(IndexKeyNullPlacement::nulls_first,
                        IndexKeySortDirection::descending)},
                  {Text("a", IndexKeySortDirection::descending)}, profile,
                  "nulls_first_descending") &&
      RequirePair(result,
                  {Null(IndexKeyNullPlacement::nulls_last,
                        IndexKeySortDirection::descending)},
                  {Text("a", IndexKeySortDirection::descending)}, profile,
                  "nulls_last_descending");

  result->numeric_ordering_proven =
      RequirePair(result, {Number(-10)}, {Number(4)}, profile,
                  "signed_i64_ascending") &&
      RequirePair(result, {Number(-10, IndexKeySortDirection::descending)},
                  {Number(4, IndexKeySortDirection::descending)}, profile,
                  "signed_i64_descending") &&
      RequirePair(result, {Number(2)}, {Number(10)}, profile,
                  "numeric_contract_2_before_10");

  result->text_collation_ordering_proven =
      RequirePair(result, {Text("aa")}, {Text("b")}, profile,
                  "text_variable_length") &&
      RequirePair(result, {Text("a")}, {Text("aa")}, profile,
                  "text_prefix") &&
      RequirePair(result, {Text("Alpha", IndexKeySortDirection::ascending, true)},
                  {Text("alpha", IndexKeySortDirection::ascending, true)},
                  profile, "case_folded_collation_key");

  result->binary_embedded_nul_ordering_proven =
      RequirePair(result, {Binary(std::vector<byte>{'a', 0x00, 'b'})},
                  {Binary(std::vector<byte>{'a', 0x00, 'c'})}, profile,
                  "binary_embedded_nul");

  result->composite_ordering_proven =
      RequirePair(result, {Text("tenant-1"), Number(7)},
                  {Text("tenant-1"), Number(9)}, profile,
                  "composite_text_numeric") &&
      RequirePair(result, {Text("tenant-1"), Null(IndexKeyNullPlacement::nulls_first)},
                  {Text("tenant-1"), Number(0)}, profile,
                  "composite_nulls_first");

  std::mt19937_64 rng(0xce1c0365eedull);
  bool fuzz_ok = true;
  for (int i = 0; i < 160; ++i) {
    const auto left_number =
        static_cast<std::int64_t>(rng() % 200000u) - 100000;
    const auto right_number =
        static_cast<std::int64_t>(rng() % 200000u) - 100000;
    fuzz_ok = fuzz_ok &&
              RequirePair(result,
                          {Number(left_number),
                           Text("k" + std::to_string(rng() % 113u))},
                          {Number(right_number),
                           Text("k" + std::to_string(rng() % 113u))},
                          profile, "deterministic_numeric_text_fuzz");
    const auto left_binary =
        std::vector<byte>{static_cast<byte>(rng() & 0xffu), 0x00,
                          static_cast<byte>((rng() >> 8) & 0xffu)};
    const auto right_binary =
        std::vector<byte>{static_cast<byte>(rng() & 0xffu), 0x00,
                          static_cast<byte>((rng() >> 8) & 0xffu)};
    fuzz_ok = fuzz_ok &&
              RequirePair(result,
                          {Binary(left_binary, IndexKeySortDirection::descending)},
                          {Binary(right_binary, IndexKeySortDirection::descending)},
                          profile, "deterministic_binary_desc_fuzz");
  }
  result->evidence.push_back(std::string("deterministic_fuzz_vectors=") +
                             (fuzz_ok ? "320" : "failed"));
  return result->null_ordering_proven && result->numeric_ordering_proven &&
         result->text_collation_ordering_proven &&
         result->binary_embedded_nul_ordering_proven &&
         result->composite_ordering_proven && fuzz_ok;
}

}  // namespace

Ceic036CanonicalKeyOrderingFuzzResult
ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
    const Ceic036CanonicalKeyOrderingFuzzRequest& request) {
  if (!request.typed_comparable_payload_contract_declared) {
    return Refuse("SB-CEIC-036-TYPED-COMPARABLE-CONTRACT-MISSING",
                  "ceic036.typed_comparable_payload_contract_missing");
  }
  if (!request.semantic_profile.bytewise_stable) {
    return Refuse("SB-CEIC-036-UNSTABLE-PROFILE-REFUSED",
                  "ceic036.unstable_profile_refused",
                  request.semantic_profile.profile_id);
  }
  if (request.raw_textual_numeric_ordering_claimed) {
    return Refuse("SB-CEIC-036-RAW-NUMERIC-TEXT-REFUSED",
                  "ceic036.raw_numeric_textual_ordering_refused",
                  "numeric keys require typed comparable payload bytes");
  }
  if (request.reference_comparison_recorded &&
      !request.reference_comparison_evidence_only) {
    return Refuse("SB-CEIC-036-REFERENCE-AUTHORITY-REFUSED",
                  "ceic036.reference_authority_refused",
                  "reference comparisons may be evidence only");
  }
  std::string authority_detail;
  if (AnyAuthorityClaimed(request.authority_claims, &authority_detail)) {
    return Refuse("SB-CEIC-036-AUTHORITY-CLAIM-REFUSED",
                  "ceic036.authority_claim_refused", authority_detail);
  }

  Ceic036CanonicalKeyOrderingFuzzResult result;
  result.status = OkStatus();
  result.typed_comparable_payload_contract_proven = true;
  result.raw_textual_numeric_ordering_rejected = true;
  result.reference_comparison_authority = false;
  result.authority_claims = {};
  result.evidence.push_back("ceic_search_key=CEIC_036_CANONICAL_KEY_ORDERING_ENCODING_FUZZ_GATES");
  result.evidence.push_back("typed_comparable_payload_contract=true");
  result.evidence.push_back("raw_textual_numeric_ordering_authority=false");
  result.evidence.push_back("reference_comparison_evidence_only=" +
                            std::string(request.reference_comparison_recorded
                                            ? "true"
                                            : "not_recorded"));
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("authorization_security_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("recovery_authority=false");
  result.evidence.push_back("parser_authority=false");
  result.evidence.push_back("reference_authority=false");
  result.evidence.push_back("wal_authority=false");
  result.evidence.push_back("benchmark_authority=false");
  result.evidence.push_back("optimizer_plan_authority=false");
  result.evidence.push_back("index_finality_authority=false");
  result.evidence.push_back("cluster_action_authority=false");
  result.evidence.push_back("agent_action_authority=false");

  const bool pairs_ok =
      ProveDeterministicPairs(&result, request.semantic_profile);
  result.prefix_bounds_proven =
      ProvePrefixBounds(&result, request.semantic_profile);
  result.expression_envelope_validation_proven =
      ProveExpressionEnvelope(&result, request.semantic_profile);
  result.partial_predicate_implication_evidence_proven =
      ProvePartialPredicate(&result);
  result.encoding_order_matches_semantic_order =
      pairs_ok && result.prefix_bounds_proven &&
      result.expression_envelope_validation_proven &&
      result.partial_predicate_implication_evidence_proven;
  result.canonical_ordering_proven =
      result.encoding_order_matches_semantic_order &&
      result.typed_comparable_payload_contract_proven &&
      result.raw_textual_numeric_ordering_rejected;
  if (!result.canonical_ordering_proven) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIndexKeyEncodingDiagnostic(
        result.status, "SB-CEIC-036-FUZZ-PROOF-FAILED",
        "ceic036.fuzz_proof_failed");
  }
  return result;
}

}  // namespace scratchbird::core::index
