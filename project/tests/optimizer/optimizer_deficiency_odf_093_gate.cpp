// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "late_materialization_executor.hpp"
#include "late_payload_fetch.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
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
  Require(generated.ok(), "ODF-093 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x93;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-093 typed UUIDv7 creation failed");
  return typed.value;
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

idx::CandidateSetRow Row(platform::byte suffix,
                         double score,
                         bool exact = true,
                         bool visible = true,
                         bool authorized = true) {
  idx::CandidateSetRow row;
  row.row_uuid = V7(platform::UuidKind::row, 1710000093000ull, suffix);
  row.score = score;
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = true;
  row.source = "odf093";
  return row;
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

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true",
          "payload_bytes_exposed=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-093 evidence leaked forbidden documentation or authority token");
    }
  }
}

std::vector<platform::byte> Bytes(std::string_view value) {
  return std::vector<platform::byte>(value.begin(), value.end());
}

struct Ids {
  platform::TypedUuid database_uuid =
      V7(platform::UuidKind::database, 1710000093000ull, 0x01);
  platform::TypedUuid filespace_uuid =
      V7(platform::UuidKind::filespace, 1710000093000ull, 0x02);
  platform::TypedUuid owner_uuid =
      V7(platform::UuidKind::object, 1710000093000ull, 0x03);
  platform::TypedUuid transaction_uuid =
      V7(platform::UuidKind::transaction, 1710000093000ull, 0x04);
  platform::TypedUuid chunk_policy_uuid =
      V7(platform::UuidKind::object, 1710000093000ull, 0x05);
};

page::LargePayloadDescriptor StorePayload(page::LargePayloadStore* store,
                                          const Ids& ids,
                                          const platform::TypedUuid& row_uuid,
                                          std::string_view payload) {
  page::LargePayloadStoreRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = ids.owner_uuid;
  request.generation_scope_uuid = row_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.chunk_policy_uuid = ids.chunk_policy_uuid;
  request.local_transaction_id = 93;
  request.family = page::LargePayloadFamily::document;
  request.payload_bytes = Bytes(payload);
  request.inline_threshold_bytes = 0;
  request.allow_inline_payload = false;
  request.retire_previous_generations = false;
  request.reason =
      "diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
  request.mga_write_admitted_by_transaction_inventory = true;
  auto stored = page::StoreLargePayloadGeneration(store, request);
  Require(stored.ok(), "ODF-093 payload fixture store failed");
  return stored.descriptor;
}

page::LatePayloadReference Reference(
    const platform::TypedUuid& row_uuid,
    const page::LargePayloadDescriptor& descriptor,
    bool redacted = false) {
  page::LatePayloadReference reference;
  reference.row_uuid = row_uuid;
  reference.descriptor = descriptor;
  reference.observer_snapshot_visible_through_local_transaction_id = 93;
  reference.descriptor_evidence_present = true;
  reference.descriptor_fresh = true;
  reference.exact_predicate_rechecked_by_engine = true;
  reference.mga_visibility_rechecked_by_engine = true;
  reference.security_authorized_by_engine = true;
  reference.security_snapshot_bound = true;
  reference.redaction_policy_bound = true;
  reference.protected_payload = redacted;
  reference.redaction_required = redacted;
  reference.unredacted_payload_authorized_by_security = !redacted;
  reference.redaction_reason = redacted ? "column_policy_redacted" : "";
  return reference;
}

exec::LateMaterializationPlan PlanFromRows(
    const std::vector<idx::CandidateSetRow>& left_rows,
    const std::vector<idx::CandidateSetRow>& right_rows,
    std::vector<page::LatePayloadReference> references,
    platform::u64 top_k) {
  const auto authority = Authority();
  auto left = idx::MakeExactRowUuidOrderedCandidateSet(left_rows, authority);
  Require(left.ok(), "ODF-093 left candidate set failed");
  auto right = idx::MakeExactRowUuidOrderedCandidateSet(right_rows, authority);
  Require(right.ok(), "ODF-093 right candidate set failed");
  auto intersection =
      idx::IntersectCandidateSets(left.output, right.output, authority);
  Require(intersection.ok(), "ODF-093 candidate intersection failed");
  Require(EvidenceHas(intersection.evidence, "operation=intersect"),
          "ODF-093 intersection evidence missing");

  exec::LateMaterializationPlan plan;
  plan.plan_id = "odf093";
  plan.candidate_intersection = intersection.output;
  plan.authority = authority;
  plan.payload_references = std::move(references);
  plan.top_k_limit = top_k;
  plan.candidate_intersection_proven = true;
  return plan;
}

std::string PayloadText(const exec::LateMaterializedRow& row) {
  return std::string(row.payload_bytes.begin(), row.payload_bytes.end());
}

void FetchesOnlyFinalAuthorizedPrunedRowsAndRedactsProtectedPayloads() {
  const Ids ids;
  page::LargePayloadStore store;
  const auto r1 = Row(0x11, 100.0);
  const auto r2 = Row(0x12, 95.0, true, true, false);
  const auto r3 = Row(0x13, 90.0, true, false, true);
  const auto r4 = Row(0x14, 80.0);
  const auto r5 = Row(0x15, 70.0);

  std::vector<page::LatePayloadReference> references = {
      Reference(r1.row_uuid, StorePayload(&store, ids, r1.row_uuid, "alpha")),
      Reference(r2.row_uuid, StorePayload(&store, ids, r2.row_uuid, "unauthorized")),
      Reference(r3.row_uuid, StorePayload(&store, ids, r3.row_uuid, "invisible")),
      Reference(r4.row_uuid, StorePayload(&store, ids, r4.row_uuid, "protected"), true),
      Reference(r5.row_uuid, StorePayload(&store, ids, r5.row_uuid, "pruned"))};

  auto plan = PlanFromRows({r1, r2, r3, r4, r5},
                           {r1, r2, r3, r4, r5},
                           std::move(references),
                           2);

  std::vector<platform::TypedUuid> requested_rows;
  const auto result = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        requested_rows.push_back(request.reference.row_uuid);
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });

  if (!result.ok()) {
    std::cerr << "ODF-093 diagnostic: "
              << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.ok(), "ODF-093 late materialization failed");
  Require(result.rows.size() == 2,
          "ODF-093 materialized row count changed");
  Require(result.counters.candidate_input_count == 5,
          "ODF-093 candidate input count missing");
  Require(result.counters.rows_after_exact_mga_security_recheck == 3,
          "ODF-093 exact/MGA/security recheck count changed");
  Require(result.counters.top_k_limit == 2,
          "ODF-093 top-K limit counter changed");
  Require(result.counters.rows_after_top_k_pruning == 2,
          "ODF-093 top-K pruning count changed");
  Require(result.counters.payload_fetcher_invocation_count == 2,
          "ODF-093 fetcher invocation count changed");
  Require(result.counters.payload_fetch_count == 1,
          "ODF-093 full payload fetch count changed");
  Require(result.counters.redacted_payload_count == 1,
          "ODF-093 redacted payload count changed");
  Require(result.counters.skipped_row_count == 3,
          "ODF-093 skipped row count changed");
  Require(requested_rows.size() == 2,
          "ODF-093 fetcher saw non-final rows");
  Require(SameUuid(requested_rows[0], r1.row_uuid) &&
              SameUuid(requested_rows[1], r4.row_uuid),
          "ODF-093 fetcher order was not final row UUID order");
  Require(PayloadText(result.rows[0]) == "alpha",
          "ODF-093 unredacted payload bytes changed");
  Require(result.rows[1].redacted && result.rows[1].payload_bytes.empty(),
          "ODF-093 redacted row exposed payload bytes");
  Require(EvidenceHas(result.evidence,
                      "late_materialization.fetch_after_candidate_intersection=true"),
          "ODF-093 candidate intersection order evidence missing");
  Require(EvidenceHas(result.evidence,
                      "late_materialization.fetch_after_security_redaction_gate=true"),
          "ODF-093 security/redaction order evidence missing");
  Require(EvidenceHas(result.evidence,
                      "late_materialization.payload_fetch_count=1"),
          "ODF-093 payload fetch counter evidence missing");
  Require(EvidenceHas(result.evidence,
                      "late_materialization.redacted_payload_count=1"),
          "ODF-093 redaction counter evidence missing");
  Require(EvidenceHas(result.evidence,
                      "late_materialization.materialization_order="),
          "ODF-093 materialization order evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void MissingPlanAndAuthorityContractsFailBeforeFetcher() {
  const Ids ids;
  page::LargePayloadStore store;
  const auto row = Row(0x21, 10.0);
  auto reference =
      Reference(row.row_uuid, StorePayload(&store, ids, row.row_uuid, "payload"));
  auto plan = PlanFromRows({row}, {row}, {reference}, 1);

  platform::u64 fetch_calls = 0;
  plan.candidate_intersection_proven = false;
  auto refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        ++fetch_calls;
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 missing candidate intersection proof was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.CANDIDATE_INTERSECTION_REQUIRED",
          "ODF-093 candidate intersection diagnostic changed");
  Require(fetch_calls == 0,
          "ODF-093 fetched payload before candidate intersection proof");

  plan.candidate_intersection_proven = true;
  plan.redaction_gate_required = false;
  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        ++fetch_calls;
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 missing redaction gate was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.REDACTION_GATE_REQUIRED",
          "ODF-093 redaction gate diagnostic changed");

  plan.redaction_gate_required = true;
  plan.authority.security_context_bound = false;
  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        ++fetch_calls;
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 missing security recheck was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.SECURITY_RECHECK_REQUIRED",
          "ODF-093 candidate-set security diagnostic changed");
  Require(fetch_calls == 0,
          "ODF-093 fetched payload after a failed recheck contract");

  plan = PlanFromRows({row}, {row}, {reference}, 1);
  plan.provider_finality_or_visibility_authority = true;
  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        ++fetch_calls;
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 provider authority drift was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.UNSAFE_AUTHORITY",
          "ODF-093 provider authority diagnostic changed");
  Require(fetch_calls == 0,
          "ODF-093 fetched payload after unsafe provider authority");
}

void StorageRedactionAndDescriptorContractsFailClosed() {
  const Ids ids;
  page::LargePayloadStore store;
  const auto row = Row(0x31, 10.0);
  const auto descriptor = StorePayload(&store, ids, row.row_uuid, "secret");

  auto redacted = Reference(row.row_uuid, descriptor, true);
  page::LatePayloadFetchRequest request;
  request.large_payload_store = &store;
  request.reference = redacted;
  request.requester_final_authorized_and_pruned = true;
  request.allow_full_payload_bytes = false;
  const auto result = page::FetchLateMaterializationPayload(request);
  Require(result.ok() && result.redacted && result.payload_bytes.empty(),
          "ODF-093 storage redaction did not return metadata only");
  Require(store.cache.miss_count == 0,
          "ODF-093 redacted storage fetch touched payload cache");

  auto unredacted_protected = Reference(row.row_uuid, descriptor, false);
  unredacted_protected.protected_payload = true;
  unredacted_protected.unredacted_payload_authorized_by_security = false;
  request.reference = unredacted_protected;
  request.allow_full_payload_bytes = true;
  const auto refused = page::FetchLateMaterializationPayload(request);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 storage allowed unredacted protected payload");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD",
          "ODF-093 unredacted protected diagnostic changed");

  auto unsafe_reference = Reference(row.row_uuid, descriptor, false);
  unsafe_reference.provider_finality_or_visibility_authority = true;
  request.reference = unsafe_reference;
  request.allow_full_payload_bytes = true;
  const auto unsafe = page::FetchLateMaterializationPayload(request);
  Require(!unsafe.ok() && unsafe.fail_closed,
          "ODF-093 storage accepted unsafe provider authority");
  Require(unsafe.diagnostic.diagnostic_code ==
              "SB_LATE_PAYLOAD_FETCH.UNSAFE_AUTHORITY",
          "ODF-093 storage unsafe authority diagnostic changed");

  request.reference = Reference(row.row_uuid, descriptor, false);
  request.requester_final_authorized_and_pruned = false;
  request.allow_full_payload_bytes = true;
  const auto non_final = page::FetchLateMaterializationPayload(request);
  Require(!non_final.ok() && non_final.fail_closed,
          "ODF-093 storage fetched before final authorized pruning");
  Require(non_final.diagnostic.diagnostic_code ==
              "SB_LATE_PAYLOAD_FETCH.FINAL_ROWS_REQUIRED",
          "ODF-093 storage final-row diagnostic changed");

  request.requester_final_authorized_and_pruned = true;
  request.reference.observer_snapshot_visible_through_local_transaction_id = 0;
  const auto missing_snapshot = page::FetchLateMaterializationPayload(request);
  Require(!missing_snapshot.ok() && missing_snapshot.fail_closed,
          "ODF-093 storage fetched without MGA snapshot boundary");
  Require(missing_snapshot.diagnostic.diagnostic_code ==
              "SB_LATE_PAYLOAD_FETCH.MGA_SNAPSHOT_REQUIRED",
          "ODF-093 storage MGA snapshot diagnostic changed");
}

void StaleDescriptorAndUnsafeFetcherRowsFailClosed() {
  const Ids ids;
  page::LargePayloadStore store;
  const auto row = Row(0x41, 10.0);
  auto reference =
      Reference(row.row_uuid, StorePayload(&store, ids, row.row_uuid, "payload"));
  auto plan = PlanFromRows({row}, {row}, {reference}, 1);

  plan.payload_references[0].descriptor.generation += 1;
  platform::u64 fetch_calls = 0;
  auto refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        ++fetch_calls;
        auto storage_request = request;
        storage_request.large_payload_store = &store;
        return page::FetchLateMaterializationPayload(storage_request);
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 stale descriptor was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_PAYLOAD_FETCH.DESCRIPTOR_STALE",
          "ODF-093 stale descriptor diagnostic changed");
  Require(fetch_calls == 1 && refused.rows.empty(),
          "ODF-093 stale descriptor did not fail closed at fetch");

  plan.payload_references[0] = reference;
  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::storage_page};
        result.fetched = true;
        result.row_uuid = V7(platform::UuidKind::row, 1710000093000ull, 0x77);
        result.descriptor = request.reference.descriptor;
        result.payload_bytes = Bytes("wrong-row");
        result.evidence.push_back("late_payload_fetch.full_payload=true");
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 mismatched fetcher row was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.FETCHER_ROW_MISMATCH",
          "ODF-093 fetcher mismatch diagnostic changed");

  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::storage_page};
        result.fetched = true;
        result.row_uuid = request.reference.row_uuid;
        result.descriptor = request.reference.descriptor;
        result.descriptor.generation += 1;
        result.payload_bytes = Bytes("wrong-descriptor");
        result.evidence.push_back("late_payload_fetch.full_payload=true");
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 mismatched fetcher descriptor was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.FETCHER_DESCRIPTOR_MISMATCH",
          "ODF-093 fetcher descriptor diagnostic changed");

  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::storage_page};
        result.redacted = true;
        result.row_uuid = request.reference.row_uuid;
        result.descriptor = request.reference.descriptor;
        result.evidence.push_back("late_payload_fetch.redacted=true");
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 unexpected unredacted-row redaction was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.UNEXPECTED_REDACTION",
          "ODF-093 unexpected redaction diagnostic changed");

  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::storage_page};
        result.fetched = true;
        result.row_uuid = request.reference.row_uuid;
        result.descriptor = request.reference.descriptor;
        result.payload_bytes = Bytes("short");
        result.evidence.push_back("late_payload_fetch.full_payload=true");
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 mismatched fetcher payload byte count was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.FETCHER_PAYLOAD_SIZE_MISMATCH",
          "ODF-093 fetcher payload size diagnostic changed");

  plan.payload_references[0] = Reference(row.row_uuid, reference.descriptor, true);
  refused = exec::ExecuteLateMaterialization(
      plan,
      [&](const page::LatePayloadFetchRequest& request) {
        page::LatePayloadFetchResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::storage_page};
        result.redacted = true;
        result.fetched = true;
        result.row_uuid = request.reference.row_uuid;
        result.descriptor = request.reference.descriptor;
        result.payload_bytes = Bytes("leaked");
        result.evidence.push_back("late_payload_fetch.redacted=true");
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-093 redacted fetcher bytes were accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_LATE_MATERIALIZATION.UNREDACTED_PROTECTED_PAYLOAD",
          "ODF-093 redacted byte leak diagnostic changed");
}

}  // namespace

int main() {
  FetchesOnlyFinalAuthorizedPrunedRowsAndRedactsProtectedPayloads();
  MissingPlanAndAuthorityContractsFailBeforeFetcher();
  StorageRedactionAndDescriptorContractsFailClosed();
  StaleDescriptorAndUnsafeFetcherRowsFailClosed();
  return EXIT_SUCCESS;
}
