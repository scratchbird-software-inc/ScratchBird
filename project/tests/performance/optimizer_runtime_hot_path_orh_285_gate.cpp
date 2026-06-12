// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_index_constraint_publication.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-285 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(std::string(message));
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[6] = static_cast<platform::byte>(0x70u | (suffix & 0x0fu));
  value.bytes[7] = suffix;
  value.bytes[8] = static_cast<platform::byte>(0x80u | (suffix & 0x3fu));
  value.bytes[9] = static_cast<platform::byte>(0x20u + suffix);
  value.bytes[10] = static_cast<platform::byte>(0x30u + suffix);
  value.bytes[11] = static_cast<platform::byte>(0x40u + suffix);
  value.bytes[12] = static_cast<platform::byte>(0x50u + suffix);
  value.bytes[13] = static_cast<platform::byte>(0x60u + suffix);
  value.bytes[14] = static_cast<platform::byte>(0x70u + suffix);
  value.bytes[15] = static_cast<platform::byte>(0x80u + suffix);
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

bool HasEvidence(const idx::ParallelIndexConstraintPublicationResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  return std::any_of(result.evidence.begin(), result.evidence.end(),
                     [&](const auto& evidence) {
                       return evidence.evidence_kind == kind &&
                              (id.empty() ||
                               evidence.evidence_id.find(id) !=
                                   std::string::npos);
                     });
}

idx::ParallelIndexConstraintPublicationRequest BaseRequest() {
  idx::ParallelIndexConstraintPublicationRequest request;
  request.route_label = "orh285.dml_deferred_parallel_publish";
  request.route = idx::IndexRouteKind::dml_insert;
  request.family = idx::IndexFamily::btree;
  request.unique = true;
  request.worker_count = 3;
  request.index_uuid = GeneratedUuid(platform::UuidKind::object, 285000, 1);
  request.table_uuid = GeneratedUuid(platform::UuidKind::object, 285001, 2);
  request.parent_keys = {"parent:10", "parent:20", "parent:30"};
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_context_bound = true;
  for (int i = 0; i < 6; ++i) {
    idx::ParallelIndexConstraintRow row;
    row.encoded_key = "K:" + std::to_string(100 + i);
    row.row_uuid = UuidText(platform::UuidKind::row, 285100 + i, i + 10);
    row.version_uuid =
        UuidText(platform::UuidKind::row, 285200 + i, i + 20);
    row.parent_key = request.parent_keys[static_cast<std::size_t>(i % 3)];
    row.payload_value = "payload:" + std::to_string(i);
    row.check_value = i + 1;
    request.rows.push_back(std::move(row));
  }
  return request;
}

void RequireAccepted(
    const idx::ParallelIndexConstraintPublicationResult& result) {
  if (!result.ok()) {
    Fail("parallel index/constraint publication did not pass: " +
         result.diagnostic.diagnostic_code);
  }
  Require(result.runtime_consumed, "runtime route was not consumed");
  Require(result.route_capability_consumed,
          "route capability evidence was not consumed");
  Require(result.parallel_sorted_build_consumed,
          "sorted bulk build was not consumed");
  Require(result.worker_evidence_consumed,
          "parallel worker evidence was not consumed");
  Require(result.worker_count >= 2, "parallel worker count was not proven");
  Require(result.parallel_uniqueness_validated,
          "parallel uniqueness validation missing");
  Require(result.parallel_foreign_key_validated,
          "parallel FK validation missing");
  Require(result.parallel_check_validated,
          "parallel check validation missing");
  Require(result.publish_fence_validated, "publish fence was not validated");
  Require(result.exact_publication, "exact publication evidence missing");
  Require(result.validated_generation_published,
          "validated generation was not published");
  Require(result.recovery_reopen_proven,
          "publish recovery/reopen evidence missing");
  Require(!result.half_root_exposed, "half-root exposure was allowed");
  Require(result.mga_visibility_recheck_proven,
          "MGA visibility recheck evidence missing");
  Require(result.security_recheck_proven,
          "security recheck evidence missing");
  Require(!result.parser_client_reference_authority,
          "parser/client/reference authority was accepted");
  Require(!result.index_metadata_finality_authority,
          "index metadata became finality/visibility authority");
  Require(!result.index_metadata_recovery_authority,
          "index metadata became recovery authority");
  Require(!result.state_hash.empty(), "state hash missing");
  Require(result.sorted_build.ok(), "physical sorted build did not pass");
  Require(result.sorted_build.candidate_root_generation.validated_tree,
          "candidate root tree was not validated");
  Require(result.recovery.ok(), "root publish recovery did not pass");
  Require(result.recovery.active_root ==
              idx::IndexBulkPublishActiveRoot::new_root,
          "validated root was not activated after durable publish");
  Require(result.recovery.root_publish_authorized,
          "root publish authorization evidence missing");
  Require(HasEvidence(result,
                      "corrected_route_capability_limits",
                      "reference_policy_hash_vector_text_refused"),
          "route capability limit evidence missing");
  Require(HasEvidence(result, "mga_visibility_authority",
                      "durable_transaction_inventory"),
          "MGA authority evidence missing");
  Require(HasEvidence(result, "index_build_finality_authority", "false"),
          "index-build finality non-authority evidence missing");
  Require(HasEvidence(result, "index_metadata_recovery_authority", "false"),
          "index metadata recovery non-authority evidence missing");
}

void RequireRejected(
    idx::ParallelIndexConstraintPublicationRequest request,
    std::string_view expected_code) {
  const auto result =
      idx::PublishParallelValidatedDeferredIndexGeneration(request);
  Require(!result.benchmark_clean,
          "negative parallel index/constraint case was benchmark-clean");
  Require(result.fail_closed,
          "negative parallel index/constraint case did not fail closed");
  if (result.diagnostic.diagnostic_code != expected_code) {
    Fail("diagnostic mismatch: expected " + std::string(expected_code) +
         " got " + result.diagnostic.diagnostic_code);
  }
  Require(HasEvidence(result, "benchmark_clean", "false"),
          "negative benchmark-clean refusal evidence missing");
}

void VerifyNegativeCases() {
  auto parser_authority = BaseRequest();
  parser_authority.route_label = "orh285.parser_authority";
  parser_authority.authority.parser_client_or_reference_index_authority = true;
  RequireRejected(
      parser_authority,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.PARSER_CLIENT_REFERENCE_AUTHORITY");

  auto metadata_finality = BaseRequest();
  metadata_finality.route_label = "orh285.metadata_finality";
  metadata_finality.authority.index_metadata_visibility_or_finality_authority =
      true;
  RequireRejected(
      metadata_finality,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INDEX_METADATA_FINALITY_AUTHORITY");

  auto metadata_recovery = BaseRequest();
  metadata_recovery.route_label = "orh285.metadata_recovery";
  metadata_recovery.authority.index_metadata_recovery_authority = true;
  RequireRejected(
      metadata_recovery,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INDEX_METADATA_RECOVERY_AUTHORITY");

  auto metadata_only_recovery = BaseRequest();
  metadata_only_recovery.route_label = "orh285.metadata_only_recovery";
  metadata_only_recovery.authority.recovery_from_index_metadata_alone = true;
  RequireRejected(
      metadata_only_recovery,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INDEX_METADATA_RECOVERY_AUTHORITY");

  auto missing_mga = BaseRequest();
  missing_mga.route_label = "orh285.missing_mga";
  missing_mga.authority.transaction_inventory_authoritative = false;
  RequireRejected(
      missing_mga,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_MGA_SECURITY_PROOF");

  auto missing_security = BaseRequest();
  missing_security.route_label = "orh285.missing_security";
  missing_security.authority.security_context_bound = false;
  RequireRejected(
      missing_security,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_MGA_SECURITY_PROOF");

  auto stale_capability = BaseRequest();
  stale_capability.route_label = "orh285.stale_capability";
  stale_capability.observed_route_capability_generation = 6;
  RequireRejected(
      stale_capability,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.STALE_ROUTE_CAPABILITY");

  auto reference_emulated = BaseRequest();
  reference_emulated.route_label = "orh285.reference_emulated";
  reference_emulated.family = idx::IndexFamily::reference_emulated;
  RequireRejected(
      reference_emulated,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED");

  auto policy_blocked = BaseRequest();
  policy_blocked.route_label = "orh285.policy_blocked";
  policy_blocked.family = idx::IndexFamily::policy_blocked;
  RequireRejected(
      policy_blocked,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED");

  auto hash_route = BaseRequest();
  hash_route.route_label = "orh285.hash_not_ordered_write";
  hash_route.family = idx::IndexFamily::hash;
  RequireRejected(
      hash_route,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED");

  auto vector_route = BaseRequest();
  vector_route.route_label = "orh285.vector_not_dml_ordered_write";
  vector_route.family = idx::IndexFamily::vector_hnsw;
  RequireRejected(
      vector_route,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED");

  auto text_route = BaseRequest();
  text_route.route_label = "orh285.text_not_dml_ordered_write";
  text_route.family = idx::IndexFamily::full_text;
  RequireRejected(
      text_route,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED");

  auto unvalidated_generation = BaseRequest();
  unvalidated_generation.route_label = "orh285.unvalidated_generation";
  unvalidated_generation.generation_validation_proof = false;
  RequireRejected(
      unvalidated_generation,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.UNVALIDATED_GENERATION_PUBLISH");

  auto uniqueness_conflict = BaseRequest();
  uniqueness_conflict.route_label = "orh285.uniqueness_conflict";
  uniqueness_conflict.rows[4].encoded_key =
      uniqueness_conflict.rows[1].encoded_key;
  RequireRejected(
      uniqueness_conflict,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.UNIQUENESS_CONFLICT");

  auto fk_missing = BaseRequest();
  fk_missing.route_label = "orh285.fk_missing";
  fk_missing.rows[2].parent_key = "parent:missing";
  RequireRejected(
      fk_missing,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.FK_MISSING_PARENT");

  auto check_failed = BaseRequest();
  check_failed.route_label = "orh285.check_failed";
  check_failed.rows[3].check_value = -1;
  RequireRejected(check_failed,
                  "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.CHECK_FAILED");

  auto worker_mismatch = BaseRequest();
  worker_mismatch.route_label = "orh285.worker_mismatch";
  worker_mismatch.worker_result_match = false;
  RequireRejected(
      worker_mismatch,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.WORKER_RESULT_MISMATCH");

  auto missing_fence = BaseRequest();
  missing_fence.route_label = "orh285.missing_fence";
  missing_fence.publish_fence_available = false;
  RequireRejected(
      missing_fence,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_PUBLISH_FENCE");

  auto no_fallback = BaseRequest();
  no_fallback.route_label = "orh285.no_fallback";
  no_fallback.exact_fallback_available = false;
  RequireRejected(no_fallback,
                  "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.NO_EXACT_FALLBACK");

  auto contract_only = BaseRequest();
  contract_only.route_label = "orh285.contract_only";
  contract_only.contract_only_evidence = true;
  RequireRejected(
      contract_only,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.NO_RUNTIME_CONSUMPTION");
}

}  // namespace

int main() {
  const auto result =
      idx::PublishParallelValidatedDeferredIndexGeneration(BaseRequest());
  RequireAccepted(result);
  VerifyNegativeCases();
  std::cout << "ORH-285 parallel index constraint gate passed: "
            << result.state_hash << '\n';
  return EXIT_SUCCESS;
}
