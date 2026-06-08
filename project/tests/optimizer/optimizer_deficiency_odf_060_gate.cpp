// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "uuid.hpp"
#include "index_ordered_access.hpp"
#include "uuid_v7_index_encoding.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-060 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x60;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-060 typed UUIDv7 creation failed");
  return typed.value;
}

platform::TypedUuid V4AsRow() {
  auto generated = uuid::GenerateCompatibilityRandomV4();
  Require(generated.ok(), "ODF-060 UUIDv4 generation failed");
  platform::TypedUuid typed;
  typed.kind = platform::UuidKind::row;
  typed.value = generated.value;
  return typed;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireNoRuntimeDocTokens(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-060 runtime evidence leaked documentation token");
    }
  }
}

std::vector<platform::TypedUuid> CompressibleRows() {
  std::vector<platform::TypedUuid> rows;
  for (platform::byte i = 0; i < 16; ++i) {
    rows.push_back(V7(platform::UuidKind::row, 1710000000123ull,
                      static_cast<platform::byte>(0xa0u + i)));
  }
  return rows;
}

idx::IndexKeyEncodingComponent UuidV7KeyComponent(platform::u32 ordinal,
                                                  platform::TypedUuid value) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::uuid_v7;
  component.ordinal = ordinal;
  component.type_descriptor_uuid =
      V7(platform::UuidKind::object, 1710000000001ull, 0xd7);
  component.uuid_v7_value = value;
  return component;
}

void SpecializedCompareAndTimeExtraction() {
  const auto older = V7(platform::UuidKind::row, 1710000000100ull, 0x11);
  const auto newer = V7(platform::UuidKind::row, 1710000000200ull, 0x01);
  const auto older_time = uuid::ExtractUuidV7TimePrefix(older.value);
  Require(older_time.ok && older_time.unix_epoch_millis == 1710000000100ull,
          "ODF-060 UUIDv7 time prefix extraction failed");

  const auto comparison =
      uuid::CompareUuidV7ForIndex(older, newer, platform::UuidKind::row);
  Require(comparison.ok && comparison.specialized_comparator_used &&
              !comparison.fallback_to_uncompressed_uuid &&
              comparison.comparison < 0,
          "ODF-060 specialized UUIDv7 comparator was not used");

  const auto fallback =
      uuid::CompareUuidV7ForIndex(older, V4AsRow(), platform::UuidKind::row);
  Require(!fallback.ok && fallback.fallback_to_uncompressed_uuid &&
              fallback.refusal_reason == "not_uuid_v7",
          "ODF-060 non-v7 UUID did not fall back exactly");

  auto wrong_kind = newer;
  wrong_kind.kind = platform::UuidKind::object;
  const auto kind_refusal =
      uuid::CompareUuidV7ForIndex(older, wrong_kind, platform::UuidKind::row);
  Require(!kind_refusal.ok && kind_refusal.refusal_reason == "kind_mismatch",
          "ODF-060 UUID kind mismatch did not refuse specialization");
}

void OrderedIndexPathRoutesUuidV7KeyComponents() {
  idx::IndexKeySemanticProfile profile;
  profile.profile_id = "sb_native_uuid_v7";
  profile.bytewise_stable = true;
  const auto older =
      UuidV7KeyComponent(0, V7(platform::UuidKind::row, 1710000000100ull, 0x11));
  const auto newer =
      UuidV7KeyComponent(0, V7(platform::UuidKind::row, 1710000000200ull, 0x01));

  const auto encoded_older = idx::EncodeIndexKey({older}, profile);
  const auto encoded_newer = idx::EncodeIndexKey({newer}, profile);
  Require(encoded_older.ok() && encoded_newer.ok() &&
              encoded_older.uuid_v7_specialized &&
              encoded_newer.uuid_v7_specialized,
          "ODF-060 index key encoding did not select UUIDv7 specialization");
  Require(EvidenceHas(encoded_older.evidence, "uuidv7_index_key_component=true") &&
              EvidenceHas(encoded_older.evidence,
                          "uuid_ordering_finality_authority=false") &&
              EvidenceHas(encoded_older.evidence, "visibility_authority=false"),
          "ODF-060 UUIDv7 index key specialization evidence missing");
  const auto key_compare =
      idx::CompareEncodedIndexKeys(encoded_older.encoded, encoded_newer.encoded);
  Require(key_compare.ok() && key_compare.comparison < 0,
          "ODF-060 encoded UUIDv7 index keys were not time ordered");

  idx::OrderedAccessRequest request;
  request.family = idx::IndexFamily::btree;
  request.intent = idx::OrderedAccessIntent::range;
  request.semantic_profile = profile;
  request.lower_bound.kind = idx::OrderedBoundKind::inclusive;
  request.lower_bound.components = {older};
  request.upper_bound.kind = idx::OrderedBoundKind::inclusive;
  request.upper_bound.components = {newer};
  const auto plan = idx::PlanOrderedBTreeAccess(request);
  Require(plan.ok() && plan.lower_key.uuid_v7_specialized &&
              plan.upper_key.uuid_v7_specialized &&
              plan.decision == idx::OrderedAccessDecision::admitted_exact,
          "ODF-060 ordered index access did not route UUIDv7 key bounds");
  RequireNoRuntimeDocTokens(encoded_older.evidence);
  RequireNoRuntimeDocTokens(encoded_newer.evidence);
}

void DictionaryPrefixDeltaRoundTripAndEvidence() {
  idx::UuidV7IndexEncodeRequest request;
  request.uuids = CompressibleRows();
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 6001;

  const auto encoded = idx::BuildUuidV7IndexPageEncoding(request);
  Require(encoded.ok && encoded.compressed &&
              !encoded.fallback_to_uncompressed_uuid,
          "ODF-060 UUIDv7 index page compression was not selected");
  Require(encoded.dictionary.self_describing &&
              encoded.dictionary.prefix_bytes >= 6 &&
              encoded.dictionary.dictionary_checksum != 0,
          "ODF-060 page dictionary/prefix evidence missing");
  Require(encoded.bytes_saved > 0,
          "ODF-060 prefix/delta encoding did not save bytes");
  Require(EvidenceHas(encoded.evidence, "page_dictionary_present=true") &&
              EvidenceHas(encoded.evidence, "time_prefix_delta_encoding=true") &&
              EvidenceHas(encoded.evidence, "external_identity_remains_uuid=true") &&
              EvidenceHas(encoded.evidence, "uuid_ordering_finality_authority=false") &&
              EvidenceHas(encoded.evidence, "visibility_authority=false") &&
              EvidenceHas(encoded.evidence, "finality_authority=false"),
          "ODF-060 compression evidence missing");
  RequireNoRuntimeDocTokens(encoded.evidence);

  const auto decoded = idx::DecodeUuidV7IndexPageEncoding(
      encoded.serialized, platform::UuidKind::row, 6001);
  Require(decoded.ok && decoded.decoded_round_trip.size() == request.uuids.size(),
          "ODF-060 UUIDv7 compressed page decode failed");
  for (std::size_t i = 0; i < decoded.decoded_round_trip.size(); ++i) {
    Require(SameUuid(decoded.decoded_round_trip[i], request.uuids[i]),
            "ODF-060 UUIDv7 compressed round trip changed UUID identity");
  }
  Require(EvidenceHas(decoded.evidence, "round_trip_decode_valid=true"),
          "ODF-060 decode round-trip evidence missing");
  RequireNoRuntimeDocTokens(decoded.evidence);
}

void CorruptStaleAndUnsupportedDictionariesFailClosed() {
  idx::UuidV7IndexEncodeRequest request;
  request.uuids = CompressibleRows();
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 6002;
  const auto encoded = idx::BuildUuidV7IndexPageEncoding(request);
  Require(encoded.ok, "ODF-060 corruption setup failed");

  auto corrupt = encoded.serialized;
  corrupt[corrupt.size() - 1] ^= 0xffu;
  const auto corrupt_decoded =
      idx::DecodeUuidV7IndexPageEncoding(corrupt, platform::UuidKind::row, 6002);
  Require(!corrupt_decoded.ok &&
              corrupt_decoded.fallback_to_uncompressed_uuid &&
              corrupt_decoded.refusal_reason == "dictionary_checksum_mismatch",
          "ODF-060 corrupt dictionary did not fail closed");

  const auto stale = idx::DecodeUuidV7IndexPageEncoding(
      encoded.serialized, platform::UuidKind::row, 7002);
  Require(!stale.ok && stale.fallback_to_uncompressed_uuid &&
              stale.refusal_reason == "stale_dictionary_generation",
          "ODF-060 stale dictionary did not fail closed");

  const auto wrong_kind = idx::DecodeUuidV7IndexPageEncoding(
      encoded.serialized, platform::UuidKind::object, 6002);
  Require(!wrong_kind.ok && wrong_kind.fallback_to_uncompressed_uuid &&
              wrong_kind.refusal_reason == "incompatible_dictionary_kind",
          "ODF-060 incompatible dictionary kind did not fail closed");

  auto unsupported = request;
  unsupported.expected_kind = platform::UuidKind::unknown;
  const auto unsupported_result = idx::BuildUuidV7IndexPageEncoding(unsupported);
  Require(!unsupported_result.ok &&
              unsupported_result.refusal_reason == "unsupported_kind",
          "ODF-060 unsupported UUID kind was guessed instead of refused");

  auto zero_generation = request;
  zero_generation.dictionary_generation = 0;
  const auto zero_generation_result =
      idx::BuildUuidV7IndexPageEncoding(zero_generation);
  Require(!zero_generation_result.ok &&
              zero_generation_result.refusal_reason ==
                  "dictionary_generation_required",
          "ODF-060 zero dictionary generation was accepted");
}

void SparseUuidPageFallsBackWhenCompressionIsNotBeneficial() {
  idx::UuidV7IndexEncodeRequest request;
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 6004;
  request.uuids = {
      V7(platform::UuidKind::row, 1710000000123ull, 0x01),
      V7(platform::UuidKind::row, 1710000001123ull, 0xf0),
  };
  request.uuids[1].value.bytes[9] = 0xf1;
  request.uuids[1].value.bytes[10] = 0xf2;

  const auto encoded = idx::BuildUuidV7IndexPageEncoding(request);
  Require(!encoded.ok && encoded.fallback_to_uncompressed_uuid &&
              encoded.refusal_reason == "compressed_page_not_smaller",
          "ODF-060 sparse UUIDv7 page did not fall back to raw UUIDs");
  Require(EvidenceHas(encoded.evidence, "fallback_to_uncompressed_uuid=true") &&
              EvidenceHas(encoded.evidence,
                          "uuid_ordering_finality_authority=false"),
          "ODF-060 sparse fallback evidence missing");
  RequireNoRuntimeDocTokens(encoded.evidence);
}

void TimePrefixPruningIsCandidateOnly() {
  idx::UuidV7IndexEncodeRequest request;
  request.uuids = CompressibleRows();
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 6003;
  const auto encoded = idx::BuildUuidV7IndexPageEncoding(request);
  Require(encoded.ok, "ODF-060 pruning setup failed");

  idx::UuidV7TimeRangePredicate matching;
  matching.lower_present = true;
  matching.upper_present = true;
  matching.lower_unix_epoch_millis = 1710000000123ull;
  matching.upper_unix_epoch_millis = 1710000000123ull;
  const auto scan = idx::PlanUuidV7TimePrefixPrune(
      encoded.dictionary, matching, true);
  Require(scan.ok && scan.candidate_range_selected &&
              scan.decision == idx::UuidV7TimePruneDecisionKind::scan &&
              !scan.finality_authority && !scan.visibility_authority,
          "ODF-060 time-prefix pruning did not select candidate range safely");
  Require(EvidenceHas(scan.evidence, "uuid_ordering_finality_authority=false") &&
              EvidenceHas(scan.evidence, "mga_visibility_recheck=required"),
          "ODF-060 pruning evidence lost non-authority/MGA recheck");

  idx::UuidV7TimeRangePredicate outside;
  outside.lower_present = true;
  outside.upper_present = true;
  outside.lower_unix_epoch_millis = 1710000000999ull;
  outside.upper_unix_epoch_millis = 1710000001999ull;
  const auto pruned = idx::PlanUuidV7TimePrefixPrune(
      encoded.dictionary, outside, true);
  Require(pruned.ok &&
              pruned.decision == idx::UuidV7TimePruneDecisionKind::prune &&
              !pruned.finality_authority,
          "ODF-060 disjoint time-prefix range was not pruned");

  const auto refused = idx::PlanUuidV7TimePrefixPrune(
      encoded.dictionary, matching, false);
  Require(!refused.ok &&
              refused.decision == idx::UuidV7TimePruneDecisionKind::fallback_uncompressed &&
              refused.refusal_reason == "mga_recheck_required",
          "ODF-060 pruning accepted missing MGA recheck");
  RequireNoRuntimeDocTokens(scan.evidence);
  RequireNoRuntimeDocTokens(pruned.evidence);
  RequireNoRuntimeDocTokens(refused.evidence);
}

}  // namespace

int main() {
  SpecializedCompareAndTimeExtraction();
  OrderedIndexPathRoutesUuidV7KeyComponents();
  DictionaryPrefixDeltaRoundTripAndEvidence();
  CorruptStaleAndUnsupportedDictionariesFailClosed();
  SparseUuidPageFallsBackWhenCompressionIsNotBeneficial();
  TimePrefixPruningIsCandidateOnly();
  return 0;
}
