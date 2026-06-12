// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "datatype_binary.hpp"
#include "index_backup_restore.hpp"
#include "index_compact_encoding.hpp"
#include "index_key_encoding.hpp"
#include "index_posting.hpp"
#include "index_route_capability.hpp"
#include "row_page_compact_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-242 gate failure: " << message << '\n';
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

idx::CompressionPolicyRequest StrongPolicy(idx::CompressionFamily family) {
  auto policy = idx::DefaultCompressionPolicyRequest(family);
  policy.cost.cpu_cost = 1;
  policy.cost.io_savings = 96;
  policy.cost.cache_density_gain = 64;
  policy.cost.update_frequency_penalty = 0;
  policy.cost.read_hotness = 40;
  policy.cost.write_hotness = 0;
  return policy;
}

idx::IndexCompactAuthorityContext IndexAuthority() {
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

idx::RowPageCompactAuthorityContext RowAuthority() {
  idx::RowPageCompactAuthorityContext authority;
  authority.authoritative_row_page_source_proven = true;
  authority.binary_semantic_equivalence_required = true;
  authority.repair_backup_restore_equivalence_required = true;
  authority.durable_mga_inventory_authority_available = true;
  authority.normal_mga_visibility_authority_available = true;
  authority.security_recheck_required = true;
  return authority;
}

dt::DatatypeBinaryValue TextValue(std::string text) {
  dt::DatatypeBinaryValue value;
  value.type_id = dt::CanonicalTypeId::character;
  value.payload.assign(text.begin(), text.end());
  return value;
}

page::RowDataPageBody RowPageBody() {
  page::RowDataPageBody body;
  body.relation_uuid = TypedV7(platform::UuidKind::object, 1724200000000ull, 1);
  body.segment_id = 5;
  body.segment_generation = 6;
  body.page_number = 7;
  body.page_generation = 8;
  body.next_page_number = 0;
  for (platform::u32 i = 0; i < 8; ++i) {
    page::RowDataRecord row;
    row.row_uuid = TypedV7(platform::UuidKind::row,
                           1724200100000ull + i,
                           static_cast<platform::byte>(0x20 + i));
    row.transaction_uuid =
        TypedV7(platform::UuidKind::transaction,
                1724200200000ull + i,
                static_cast<platform::byte>(0x40 + i));
    row.local_transaction_id = 100 + i;
    row.row_version = 1 + i;
    row.deleted = false;
    row.cells.push_back({1, TextValue("tenant-alpha-row-" + std::to_string(i))});
    row.cells.push_back({2, TextValue(std::string(96, static_cast<char>('a' + i)))});
    body.rows.push_back(std::move(row));
  }
  return body;
}

std::vector<platform::byte> EncodedKey(platform::u32 ordinal) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 1;
  component.type_descriptor_uuid =
      TypedV7(platform::UuidKind::object, 1724200300000ull, 9);
  component.payload.assign({'t', 'e', 'n', 'a', 'n', 't', ':', 'a', 'l', 'p',
                            'h', 'a', ':', '0', '0'});
  component.payload.push_back(static_cast<platform::byte>('0' + (ordinal / 10)));
  component.payload.push_back(static_cast<platform::byte>('0' + (ordinal % 10)));
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "index key encoding failed");
  return encoded.encoded;
}

std::vector<idx::ExactIndexPageCompactRecord> ExactRecords(
    platform::u32 count) {
  std::vector<idx::ExactIndexPageCompactRecord> records;
  records.reserve(count);
  for (platform::u32 i = 0; i < count; ++i) {
    idx::ExactIndexPageCompactRecord record;
    record.encoded_key = EncodedKey(i);
    record.row_uuid = TypedV7(platform::UuidKind::row,
                              1724210000000ull + i,
                              static_cast<platform::byte>(i));
    record.version_uuid =
        TypedV7(platform::UuidKind::row,
                1724211000000ull + i,
                static_cast<platform::byte>(0x80 + (i % 64)));
    record.row_ordinal = i + 1;
    record.flags = i % 3;
    const std::string payload = "covering-payload-" + std::to_string(i);
    record.payload_metadata.assign(payload.begin(), payload.end());
    records.push_back(std::move(record));
  }
  std::sort(records.begin(), records.end(), [](const auto& left,
                                               const auto& right) {
    return idx::CompareEncodedIndexKeys(left.encoded_key, right.encoded_key)
               .comparison < 0;
  });
  for (std::size_t i = 0; i < records.size(); ++i) {
    records[i].row_ordinal = static_cast<platform::u64>(i + 1);
  }
  return records;
}

idx::IndexPostingEntry PostingEntry(platform::u32 i) {
  idx::IndexPostingEntry entry;
  entry.locator.table_uuid =
      TypedV7(platform::UuidKind::object, 1724220000000ull, 2);
  entry.locator.row_uuid =
      TypedV7(platform::UuidKind::row,
              1724221000000ull + i,
              static_cast<platform::byte>(i));
  entry.locator.version_uuid =
      TypedV7(platform::UuidKind::row,
              1724222000000ull + i,
              static_cast<platform::byte>(0x50 + (i % 64)));
  entry.locator.local_transaction_id = 1000 + i;
  entry.visible_from_transaction_id = 900 + i;
  entry.visible_until_transaction_id = i % 7 == 0 ? 2000 + i : 0;
  entry.flags = i % 5;
  return entry;
}

idx::IndexPostingList PostingList(platform::u32 count, bool proof) {
  idx::IndexPostingList list;
  list.index_uuid = TypedV7(platform::UuidKind::object, 1724230000000ull, 3);
  list.encoded_key = EncodedKey(42);
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

std::vector<platform::TypedUuid> UuidKeys() {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(1724240000000ull);
  Require(generated.ok(), "uuidv7 seed failed");
  std::vector<platform::TypedUuid> keys;
  for (platform::u32 i = 0; i < 512; ++i) {
    auto value = generated.value;
    value.bytes[14] = static_cast<platform::byte>((i >> 8u) & 0xffu);
    value.bytes[15] = static_cast<platform::byte>(i & 0xffu);
    auto typed = uuid::MakeTypedUuid(platform::UuidKind::row, value);
    Require(typed.ok(), "uuidv7 typed key failed");
    keys.push_back(typed.value);
  }
  return keys;
}

idx::IndexMovementValidationRequest Movement(idx::IndexFamily family,
                                             platform::TypedUuid index_uuid) {
  idx::IndexMovementValidationRequest request;
  request.operation = idx::IndexMovementOperation::restore;
  request.family = family;
  request.resource_available = true;
  request.transaction_finality_proven = true;
  request.destination_supports_family = true;
  request.page_authority.expected_index_uuid = index_uuid;
  request.page_authority.observed_index_uuid = index_uuid;
  request.page_authority.expected_family = family;
  request.page_authority.observed_family = family;
  request.page_authority.expected_resource_epoch = 11;
  request.page_authority.observed_resource_epoch = 11;
  request.page_authority.checksum_valid = true;
  request.page_authority.page_type_supported = true;
  return request;
}

void TestRowPageCompactRepairBackupRestore() {
  idx::RowPageCompactRequest request;
  request.body = RowPageBody();
  request.page_size = 4096;
  request.authority = RowAuthority();
  request.policy = StrongPolicy(idx::CompressionFamily::kRowPage);
  auto built = idx::BuildRowPageCompactEncoding(request);
  Require(built.ok() && built.compressed,
          "row page compact build refused or did not use compact encoding");
  Require(built.exact_round_trip && built.backup_restore_equivalent,
          "row page round trip/backup-restore proof missing");
  Require(HasEvidence(built.evidence,
                      "row_page_compact.transaction_finality_authority=false"),
          "row page finality non-authority evidence missing");

  auto decoded =
      idx::DecodeRowPageCompactEncoding(built.serialized, RowAuthority());
  Require(decoded.ok() && decoded.canonical_row_page == built.canonical_row_page,
          "row page compact decode drifted");

  auto corrupt = built.serialized;
  corrupt[corrupt.size() / 2] ^= 0x33u;
  auto refused =
      idx::DecodeRowPageCompactEncoding(corrupt, RowAuthority());
  Require(!refused.ok() && refused.fail_closed,
          "corrupt row page compact metadata opened");

  idx::RowPageCompactRepairAdmission admission;
  admission.repair_admitted = true;
  admission.authoritative_row_page_source_available = true;
  admission.same_page_identity_proven = true;
  admission.backup_restore_manifest_equivalence_proven = true;
  admission.proof_detail = "authoritative_row_data_page";
  auto repaired = idx::RepairOrValidateRowPageCompactEncoding(
      corrupt, RowAuthority(), &request.body, request.page_size, admission);
  Require(repaired.ok() && repaired.repaired,
          "row page repair from authoritative row page refused");
  Require(HasEvidence(repaired.evidence,
                      "repair.authoritative_row_page_source_used=true"),
          "row page authoritative repair evidence missing");

  idx::RowPageCompactRepairAdmission no_source;
  auto self_repair = idx::RepairOrValidateRowPageCompactEncoding(
      corrupt, RowAuthority(), &request.body, request.page_size, no_source);
  Require(!self_repair.ok() && self_repair.fail_closed,
          "row page self-authoritative repair succeeded");

  auto unsafe = RowAuthority();
  unsafe.parser_client_or_reference_authority = true;
  auto unsafe_result =
      idx::BuildRowPageCompactEncoding({request.body, request.page_size, unsafe});
  Require(!unsafe_result.ok() && unsafe_result.fail_closed,
          "parser/client/reference row-page compact authority accepted");

  auto authority_drift = RowAuthority();
  authority_drift.compact_form_visibility_authority = true;
  authority_drift.compact_form_finality_authority = true;
  authority_drift.compact_form_recovery_authority = true;
  auto authority_drift_result = idx::BuildRowPageCompactEncoding(
      {request.body, request.page_size, authority_drift});
  Require(!authority_drift_result.ok() && authority_drift_result.fail_closed,
          "compact row-page form became visibility/finality/recovery authority");
}

void TestExactIndexPageCompactOrderingRepairAndRouteLimits() {
  idx::ExactIndexPageCompactRequest request;
  request.records = ExactRecords(96);
  request.authority = IndexAuthority();
  request.policy = StrongPolicy(idx::CompressionFamily::kExactIndexPage);
  auto built = idx::BuildExactIndexPageCompactEncoding(request);
  Require(built.ok() && built.compressed,
          "exact index page compact build refused");
  Require(built.exact_round_trip && built.order_preserved,
          "exact index page order proof missing");
  Require(HasEvidence(built.evidence, "exact_page.costed_decision=true"),
          "exact index page cost policy evidence missing");
  for (std::size_t i = 1; i < built.records.size(); ++i) {
    Require(idx::CompareEncodedIndexKeys(built.records[i - 1].encoded_key,
                                         built.records[i].encoded_key)
                .comparison < 0,
            "exact index page logical ordering changed");
  }

  idx::ExactIndexPageCompactRequest legacy;
  legacy.records = request.records;
  legacy.records[0].encoded_key = {'l', 'e', 'g', 'a', 'c', 'y'};
  legacy.authority = IndexAuthority();
  auto legacy_result = idx::BuildExactIndexPageCompactEncoding(legacy);
  Require(!legacy_result.ok() && legacy_result.fail_closed,
          "unsafe legacy/non-order-preserving key accepted");

  auto corrupt = built.serialized;
  corrupt.back() ^= 0x44u;
  idx::IndexCompactRepairAdmission admission;
  admission.repair_admitted = true;
  admission.exact_source_available = true;
  admission.same_page_identity_proven = true;
  admission.order_proof_present = true;
  auto repaired = idx::RepairOrValidateExactIndexPageCompactEncoding(
      corrupt, IndexAuthority(), &request.records, admission);
  Require(repaired.ok() && repaired.repaired,
          "exact index page repair from exact source failed");

  const auto* reference = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::reference_emulated);
  const auto* policy = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::policy_blocked);
  const auto* hash = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::hash);
  Require(reference != nullptr && !reference->route_complete(),
          "reference-emulated compact route treated as authority");
  Require(policy != nullptr && !policy->route_complete(),
          "policy-blocked compact route treated as authority");
  Require(hash != nullptr && hash->supports_equality_lookup &&
              !hash->supports_ordered_range,
          "hash compact route capability lost equality-only limit");
}

void TestPostingListAndCandidateCompactRechecks() {
  idx::CompactPostingListRequest request;
  request.posting_list = PostingList(180, true);
  request.authority = IndexAuthority();
  request.policy = StrongPolicy(idx::CompressionFamily::kPostingList);
  auto built = idx::BuildCompactPostingListEncoding(request);
  Require(built.ok() && built.compressed,
          "posting list compact build refused");
  Require(HasEvidence(built.evidence,
                      "posting_list.varint_integer_encoding=true"),
          "posting list compact evidence missing");
  Require(HasEvidence(built.evidence, "mga_visibility_recheck_required=true"),
          "posting list MGA recheck evidence missing");

  idx::CompactPostingListRequest no_proof;
  no_proof.posting_list = PostingList(16, false);
  no_proof.authority = IndexAuthority();
  auto fallback = idx::BuildCompactPostingListEncoding(no_proof);
  Require(fallback.ok() && fallback.fallback_uncompressed,
          "posting list without equality proof did not fall back exactly");

  auto unsafe = IndexAuthority();
  unsafe.provider_authority = true;
  request.authority = unsafe;
  auto unsafe_result = idx::BuildCompactPostingListEncoding(request);
  Require(!unsafe_result.ok() && unsafe_result.fail_closed,
          "provider-authoritative posting compact proof accepted");

  idx::CompactCandidateSetRequest candidates;
  for (platform::u64 ordinal = 1; ordinal <= 6000; ++ordinal) {
    candidates.row_ordinals.push_back(ordinal);
  }
  candidates.candidate_authority = CandidateAuthority();
  candidates.authority = IndexAuthority();
  candidates.policy = StrongPolicy(idx::CompressionFamily::kCandidateSet);
  auto candidate_result = idx::BuildCompactCandidateSetEncoding(candidates);
  Require(candidate_result.ok() && candidate_result.compressed,
          "candidate row ordinal compact set refused");
  Require(candidate_result.exact_ordinals == candidates.row_ordinals,
          "candidate row ordinal compact set changed identities");
}

void TestUuidV7CompactOrderAndStaleGenerationRefusal() {
  idx::UuidV7CompactKeyBlockRequest request;
  request.keys = UuidKeys();
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 242;
  request.authority = IndexAuthority();
  request.policy = StrongPolicy(idx::CompressionFamily::kExactIndexPage);
  auto built = idx::BuildUuidV7CompactKeyBlock(request);
  Require(built.ok() && built.compressed,
          "uuidv7 compact key block refused");
  Require(built.order_equivalent_to_full_uuid_bytes,
          "uuidv7 compact order equivalence missing");
  Require(HasEvidence(built.evidence,
                      "uuidv7.order_equivalent_to_full_uuid_bytes=true"),
          "uuidv7 compact order evidence missing");

  auto stale = idx::DecodeUuidV7CompactKeyBlock(
      built.serialized, platform::UuidKind::row, 999, IndexAuthority());
  Require(!stale.ok() && stale.fail_closed,
          "stale uuidv7 compact dictionary generation accepted");

  auto unsafe = IndexAuthority();
  unsafe.uuid_order_finality_authority = true;
  auto unsafe_result = idx::BuildUuidV7CompactKeyBlock(
      {request.keys, request.expected_kind, request.dictionary_generation,
       unsafe});
  Require(!unsafe_result.ok() && unsafe_result.fail_closed,
          "uuid compact order finality authority accepted");
}

void TestOptimizedLifecycleAndBackupRestoreMismatchRefusal() {
  const auto index_uuid =
      TypedV7(platform::UuidKind::object, 1724250000000ull, 4);
  auto movement = Movement(idx::IndexFamily::btree, index_uuid);
  auto movement_result = idx::ValidateIndexMovement(movement);
  Require(movement_result.ok(),
          "exact index movement backup/restore validation refused");

  auto stale = movement;
  stale.page_authority.observed_resource_epoch = 12;
  auto stale_result = idx::ValidateIndexMovement(stale);
  Require(!stale_result.ok() && stale_result.refuse_restore,
          "stale/corrupt compact index movement metadata accepted");

  idx::OptimizedStructureLifecycleRequest lifecycle;
  lifecycle.structure =
      idx::OptimizedPersistedStructureKind::secondary_index_delta_ledgers;
  lifecycle.operation = idx::OptimizedStructureLifecycleOperation::restore;
  lifecycle.movement = movement;
  lifecycle.movement_validation_required = true;
  lifecycle.manifest_coverage_verified = true;
  lifecycle.restore_inspection_open = true;
  lifecycle.transaction_finality_proven_by_mga_inventory = true;
  lifecycle.authoritative_base_available = true;
  lifecycle.support_bundle_evidence_sink_available = true;
  lifecycle.published_or_committed_generation = true;
  auto lifecycle_result = idx::EvaluateOptimizedStructureLifecycle(lifecycle);
  Require(lifecycle_result.ok() && lifecycle_result.survived_backup_restore &&
              lifecycle_result.engine_mga_authority_preserved,
          "optimized lifecycle backup/restore equivalence refused");

  auto mismatch = lifecycle;
  mismatch.manifest_coverage_verified = false;
  auto mismatch_result = idx::EvaluateOptimizedStructureLifecycle(mismatch);
  Require(!mismatch_result.ok() && mismatch_result.exact_refusal,
          "backup/restore semantic mismatch accepted");
}

}  // namespace

int main() {
  TestRowPageCompactRepairBackupRestore();
  TestExactIndexPageCompactOrderingRepairAndRouteLimits();
  TestPostingListAndCandidateCompactRechecks();
  TestUuidV7CompactOrderAndStaleGenerationRefusal();
  TestOptimizedLifecycleAndBackupRestoreMismatchRefusal();
  std::cout << "optimizer_runtime_hot_path_orh_242_gate=passed\n";
  return EXIT_SUCCESS;
}
