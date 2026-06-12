// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_compact_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_compact_encoding_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

idx::IndexCompactAuthorityContext Authority() {
  idx::IndexCompactAuthorityContext authority;
  authority.exact_source_proven = true;
  authority.order_correctness_proven = true;
  authority.encoded_key_order_proven = true;
  authority.uuidv7_order_equivalence_proven = true;
  authority.mga_visibility_recheck_required = true;
  authority.security_recheck_required = true;
  return authority;
}

idx::CandidateSetAuthorityContext CandidateAuthority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

idx::CompressionPolicyRequest StrongPolicy(idx::CompressionFamily family) {
  auto policy = idx::DefaultCompressionPolicyRequest(family);
  policy.cost.cpu_cost = 1;
  policy.cost.io_savings = 80;
  policy.cost.cache_density_gain = 40;
  policy.cost.update_frequency_penalty = 0;
  policy.cost.read_hotness = 20;
  policy.cost.write_hotness = 0;
  return policy;
}

platform::TypedUuid TypedV7(platform::UuidKind kind,
                            platform::u64 millis,
                            platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::vector<platform::byte> Key(platform::u32 ordinal) {
  std::vector<platform::byte> key = {'S', 'B', 'K', 'O'};
  const std::string prefix = "tenant:alpha:compact:index:key:";
  key.insert(key.end(), prefix.begin(), prefix.end());
  const std::string number = std::to_string(1000000 + ordinal);
  key.insert(key.end(), number.begin(), number.end());
  return key;
}

std::vector<idx::ExactIndexPageCompactRecord> ExactRecords(platform::u32 count) {
  std::vector<idx::ExactIndexPageCompactRecord> records;
  for (platform::u32 i = 0; i < count; ++i) {
    idx::ExactIndexPageCompactRecord record;
    record.encoded_key = Key(i);
    record.row_uuid = TypedV7(platform::UuidKind::row, 1710000000000ull + i,
                              static_cast<platform::byte>(i));
    record.version_uuid =
        TypedV7(platform::UuidKind::row, 1710000100000ull + i,
                static_cast<platform::byte>(0x80u + (i % 100u)));
    record.row_ordinal = i + 1;
    record.flags = i % 3;
    const std::string payload = "payload-meta-" + std::to_string(i);
    record.payload_metadata.assign(payload.begin(), payload.end());
    records.push_back(std::move(record));
  }
  return records;
}

bool SameExactRecords(
    const std::vector<idx::ExactIndexPageCompactRecord>& left,
    const std::vector<idx::ExactIndexPageCompactRecord>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].encoded_key != right[i].encoded_key ||
        left[i].row_uuid.value != right[i].row_uuid.value ||
        left[i].version_uuid.value != right[i].version_uuid.value ||
        left[i].row_ordinal != right[i].row_ordinal ||
        left[i].flags != right[i].flags ||
        left[i].payload_metadata != right[i].payload_metadata) {
      return false;
    }
  }
  return true;
}

void TestExactPageRoundTripFallbackAndRepair() {
  idx::ExactIndexPageCompactRequest request;
  request.records = ExactRecords(160);
  request.authority = Authority();
  request.policy = StrongPolicy(idx::CompressionFamily::kExactIndexPage);
  auto built = idx::BuildExactIndexPageCompactEncoding(request);
  Require(built.ok(), "exact page compact build refused");
  Require(built.compressed && !built.fallback_uncompressed,
          "exact page compact policy did not accept");
  Require(built.exact_round_trip && built.order_preserved,
          "exact page compact round trip/order proof missing");
  Require(SameExactRecords(built.records, request.records),
          "exact page compact records drifted");
  Require(HasEvidence(built.evidence, "exact_page.prefix_delta_encoding=true"),
          "exact page prefix-delta evidence missing");
  Require(HasEvidence(built.evidence, "exact_fallback_equivalence_proven=true"),
          "exact page fallback equivalence evidence missing");

  auto decoded = idx::DecodeExactIndexPageCompactEncoding(built.serialized,
                                                          Authority());
  Require(decoded.ok() && SameExactRecords(decoded.records, request.records),
          "exact page decode drifted");

  idx::ExactIndexPageCompactRequest small;
  small.records = ExactRecords(1);
  small.authority = Authority();
  auto fallback = idx::BuildExactIndexPageCompactEncoding(small);
  Require(fallback.ok() && fallback.fallback_uncompressed &&
              !fallback.compressed,
          "exact page small input did not use exact fallback");
  Require(HasEvidence(fallback.evidence,
                      "exact_page.uncompressed_fallback_used=true"),
          "exact page fallback evidence missing");

  idx::ExactIndexPageCompactRequest bad_ordinal;
  bad_ordinal.records = ExactRecords(2);
  bad_ordinal.records.front().row_ordinal = 0;
  bad_ordinal.authority = Authority();
  auto bad_ordinal_result =
      idx::BuildExactIndexPageCompactEncoding(bad_ordinal);
  Require(!bad_ordinal_result.ok() && bad_ordinal_result.fail_closed,
          "exact page accepted zero row ordinal");

  auto corrupt = built.serialized;
  corrupt[corrupt.size() / 2] ^= 0x5au;
  auto refused = idx::DecodeExactIndexPageCompactEncoding(corrupt, Authority());
  Require(!refused.ok() && refused.fail_closed,
          "corrupt exact page decoded successfully");

  idx::IndexCompactRepairAdmission admission;
  admission.repair_admitted = true;
  admission.exact_source_available = true;
  admission.same_page_identity_proven = true;
  admission.order_proof_present = true;
  admission.proof_detail = "test_exact_source";
  auto repaired = idx::RepairOrValidateExactIndexPageCompactEncoding(
      corrupt, Authority(), &request.records, admission);
  Require(repaired.ok() && repaired.repaired &&
              repaired.repair_state ==
                  idx::IndexCompactRepairState::repaired_from_exact_source,
          "exact page repair did not rebuild from exact source");
  Require(SameExactRecords(repaired.records, request.records),
          "exact page repair changed records");

  idx::IndexCompactRepairAdmission no_admission;
  auto repair_refused = idx::RepairOrValidateExactIndexPageCompactEncoding(
      corrupt, Authority(), &request.records, no_admission);
  Require(!repair_refused.ok() && repair_refused.fail_closed,
          "exact page repair without admission succeeded");
}

idx::IndexPostingEntry PostingEntry(platform::u32 i) {
  idx::IndexPostingEntry entry;
  entry.locator.table_uuid = TypedV7(platform::UuidKind::object, 1710010000000ull, 1);
  entry.locator.row_uuid =
      TypedV7(platform::UuidKind::row, 1710020000000ull + i,
              static_cast<platform::byte>(i));
  entry.locator.version_uuid =
      TypedV7(platform::UuidKind::row, 1710030000000ull + i,
              static_cast<platform::byte>(0x40u + (i % 100u)));
  entry.locator.local_transaction_id = 1000 + i;
  entry.visible_from_transaction_id = 900 + i;
  entry.visible_until_transaction_id = i % 5 == 0 ? 2000 + i : 0;
  entry.flags = i % 7;
  return entry;
}

idx::IndexPostingList PostingList(platform::u32 count, bool proof) {
  idx::IndexPostingList list;
  list.index_uuid = TypedV7(platform::UuidKind::object, 1710040000000ull, 2);
  list.encoded_key = Key(777);
  list.compressed_duplicates = true;
  list.recheck_required = true;
  list.equality_proof.proof_present = proof;
  list.equality_proof.non_unique_exact = proof;
  list.equality_proof.encoded_key_bytewise_stable = proof;
  list.equality_proof.stable_row_uuid_locators = proof;
  list.equality_proof.preserves_mga_visibility_recheck = proof;
  for (platform::u32 i = 0; i < count; ++i) {
    list.entries.push_back(PostingEntry(i));
  }
  return list;
}

void TestPostingListVarintAndFallback() {
  idx::CompactPostingListRequest request;
  request.posting_list = PostingList(180, true);
  request.authority = Authority();
  request.policy = StrongPolicy(idx::CompressionFamily::kPostingList);
  auto built = idx::BuildCompactPostingListEncoding(request);
  Require(built.ok() && built.compressed && !built.fallback_uncompressed,
          "posting list compact varint build refused");
  Require(built.posting_list.entries.size() == request.posting_list.entries.size(),
          "posting compact entry count drifted");
  Require(HasEvidence(built.evidence,
                      "posting_list.varint_integer_encoding=true"),
          "posting varint evidence missing");

  auto decoded =
      idx::DecodeCompactPostingListEncoding(built.serialized, Authority());
  Require(decoded.ok() &&
              decoded.posting_list.entries.size() ==
                  request.posting_list.entries.size(),
          "posting compact decode drifted");

  auto corrupt = built.serialized;
  corrupt.back() ^= 0x22u;
  auto refused =
      idx::DecodeCompactPostingListEncoding(corrupt, Authority());
  Require(!refused.ok() && refused.fail_closed,
          "posting corrupt bytes decoded successfully");

  idx::CompactPostingListRequest no_proof;
  no_proof.posting_list = PostingList(4, false);
  no_proof.authority = Authority();
  auto fallback = idx::BuildCompactPostingListEncoding(no_proof);
  Require(fallback.ok() && fallback.fallback_uncompressed &&
              !fallback.compressed,
          "posting missing proof did not use exact fallback");
  Require(HasEvidence(fallback.evidence,
                      "posting_list.uncompressed_fallback_used=true"),
          "posting fallback evidence missing");
}

std::vector<platform::u64> Range(platform::u64 begin, platform::u64 end) {
  std::vector<platform::u64> out;
  for (platform::u64 value = begin; value < end; ++value) {
    out.push_back(value);
  }
  return out;
}

std::vector<platform::u64> SetUnion(const std::vector<platform::u64>& left,
                                    const std::vector<platform::u64>& right) {
  std::set<platform::u64> values(left.begin(), left.end());
  values.insert(right.begin(), right.end());
  return {values.begin(), values.end()};
}

std::vector<platform::u64> SetIntersect(
    const std::vector<platform::u64>& left,
    const std::vector<platform::u64>& right) {
  std::set<platform::u64> right_set(right.begin(), right.end());
  std::vector<platform::u64> out;
  for (const auto value : left) {
    if (right_set.count(value) != 0) {
      out.push_back(value);
    }
  }
  return out;
}

std::vector<platform::u64> SetSubtract(
    const std::vector<platform::u64>& left,
    const std::vector<platform::u64>& right) {
  std::set<platform::u64> right_set(right.begin(), right.end());
  std::vector<platform::u64> out;
  for (const auto value : left) {
    if (right_set.count(value) == 0) {
      out.push_back(value);
    }
  }
  return out;
}

void TestCandidateSetCompactAlgebraAndFallback() {
  std::vector<platform::u64> left = Range(0, 5000);
  std::vector<platform::u64> right = Range(2500, 7500);

  idx::CompactCandidateSetRequest request;
  request.row_ordinals = left;
  request.candidate_authority = CandidateAuthority();
  request.authority = Authority();
  request.policy = StrongPolicy(idx::CompressionFamily::kCandidateSet);
  auto built = idx::BuildCompactCandidateSetEncoding(request);
  Require(built.ok() && built.compressed,
          "candidate set compact build refused");
  Require(built.exact_ordinals == left, "candidate set compact round trip drifted");
  Require(HasEvidence(built.evidence, "candidate_set.materialized_rows=false"),
          "candidate set materialization evidence missing");

  request.row_ordinals = right;
  auto built_right = idx::BuildCompactCandidateSetEncoding(request);
  Require(built_right.ok() && built_right.compressed,
          "right candidate compact build refused");

  auto union_result = idx::UnionCandidateSets(
      built.candidate_set, built_right.candidate_set, CandidateAuthority());
  auto intersect_result = idx::IntersectCandidateSets(
      built.candidate_set, built_right.candidate_set, CandidateAuthority());
  auto subtract_result = idx::SubtractCandidateSets(
      built.candidate_set, built_right.candidate_set, CandidateAuthority());
  Require(union_result.ok() && intersect_result.ok() && subtract_result.ok(),
          "candidate compact algebra refused");
  Require(idx::ExpandCompactCandidateSetOrdinalsForProof(union_result.output) ==
              SetUnion(left, right),
          "candidate union equivalence drifted");
  Require(idx::ExpandCompactCandidateSetOrdinalsForProof(intersect_result.output) ==
              SetIntersect(left, right),
          "candidate intersection equivalence drifted");
  Require(idx::ExpandCompactCandidateSetOrdinalsForProof(subtract_result.output) ==
              SetSubtract(left, right),
          "candidate subtract equivalence drifted");

  idx::CompactCandidateSetRequest small;
  small.row_ordinals = {1, 7, 9};
  small.candidate_authority = CandidateAuthority();
  small.authority = Authority();
  auto fallback = idx::BuildCompactCandidateSetEncoding(small);
  Require(fallback.ok() && fallback.fallback_uncompressed &&
              fallback.exact_ordinals == small.row_ordinals,
          "small candidate set did not use exact fallback");

  auto corrupt = built.serialized;
  corrupt[3] ^= 0x7fu;
  auto refused = idx::DecodeCompactCandidateSetEncoding(
      corrupt, CandidateAuthority(), Authority());
  Require(!refused.ok() && refused.fail_closed,
          "candidate corrupt bytes decoded successfully");
}

std::vector<platform::TypedUuid> UuidKeys() {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(1710050000000ull);
  Require(generated.ok(), "uuidv7 block seed failed");
  std::vector<platform::TypedUuid> keys;
  for (platform::u32 i = 0; i < 512; ++i) {
    auto value = generated.value;
    value.bytes[14] = static_cast<platform::byte>((i >> 8u) & 0xffu);
    value.bytes[15] = static_cast<platform::byte>(i & 0xffu);
    auto typed = uuid::MakeTypedUuid(platform::UuidKind::row, value);
    Require(typed.ok(), "uuidv7 block key failed");
    keys.push_back(typed.value);
  }
  return keys;
}

void TestUuidV7OrderEquivalentCompactKeyBlock() {
  idx::UuidV7CompactKeyBlockRequest request;
  request.keys = UuidKeys();
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 77;
  request.authority = Authority();
  request.policy = StrongPolicy(idx::CompressionFamily::kExactIndexPage);
  auto built = idx::BuildUuidV7CompactKeyBlock(request);
  Require(built.ok() && built.compressed,
          "uuidv7 compact key block build refused");
  Require(built.order_equivalent_to_full_uuid_bytes,
          "uuidv7 order equivalence proof missing");
  Require(HasEvidence(built.evidence,
                      "uuidv7.order_equivalent_to_full_uuid_bytes=true"),
          "uuidv7 order equivalence evidence missing");

  auto decoded = idx::DecodeUuidV7CompactKeyBlock(
      built.serialized, platform::UuidKind::row, 77, Authority());
  Require(decoded.ok() && decoded.order_equivalent_to_full_uuid_bytes,
          "uuidv7 compact decode refused");

  auto corrupt = built.serialized;
  corrupt[corrupt.size() / 3] ^= 0x11u;
  auto refused = idx::DecodeUuidV7CompactKeyBlock(
      corrupt, platform::UuidKind::row, 77, Authority());
  Require(!refused.ok() && refused.fail_closed,
          "uuidv7 corrupt bytes decoded successfully");

  idx::UuidV7CompactKeyBlockRequest small;
  small.keys = {request.keys.front()};
  small.expected_kind = platform::UuidKind::row;
  small.dictionary_generation = 88;
  small.authority = Authority();
  auto fallback = idx::BuildUuidV7CompactKeyBlock(small);
  Require(fallback.ok() && fallback.fallback_uncompressed,
          "uuidv7 small key block did not fall back exactly");
}

void TestAuthorityAndRuntimeLeakageGuards() {
  auto unsafe = Authority();
  unsafe.wal_or_finality_authority = true;
  idx::ExactIndexPageCompactRequest request;
  request.records = ExactRecords(2);
  request.authority = unsafe;
  auto refused = idx::BuildExactIndexPageCompactEncoding(request);
  Require(!refused.ok() && refused.fail_closed,
          "unsafe authority context accepted");

  const std::vector<std::string> combined = {
      "compact_encoding_storage_cpu_only=true",
      "transaction_finality_authority=false",
      "parser_or_reference_authority=false"};
  for (const auto& item : combined) {
    Require(item.find("execution_plan") == std::string::npos,
            "runtime evidence leaked execution_plan marker");
    Require(item.find("stub") == std::string::npos,
            "runtime evidence leaked placeholder marker");
  }
}

}  // namespace

int main() {
  TestExactPageRoundTripFallbackAndRepair();
  TestPostingListVarintAndFallback();
  TestCandidateSetCompactAlgebraAndFallback();
  TestUuidV7OrderEquivalentCompactKeyBlock();
  TestAuthorityAndRuntimeLeakageGuards();
  std::cout << "index_compact_encoding_gate=passed\n";
  return EXIT_SUCCESS;
}
