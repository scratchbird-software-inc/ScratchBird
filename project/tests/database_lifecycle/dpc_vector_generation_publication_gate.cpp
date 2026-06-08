// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "uuid.hpp"
#include "vector_index_generation_publication.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_VECTOR_GENERATION_PUBLICATION_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_VECTOR_GENERATION_PUBLICATION";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "DPC-044 generated UUID creation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

idx::VectorGenerationResourceEnvelope ResourceEnvelope(platform::u64 seed) {
  idx::VectorGenerationResourceEnvelope envelope;
  envelope.memory_limit_bytes = 64 * 1024 * 1024;
  envelope.memory_observed_bytes = 12 * 1024 * 1024 + seed;
  envelope.temp_space_limit_bytes = 256 * 1024 * 1024;
  envelope.temp_space_observed_bytes = 24 * 1024 * 1024 + seed;
  envelope.worker_limit = 4;
  envelope.workers_used = 2;
  envelope.resource_governor_evidence_present = true;
  envelope.resource_governor_evidence_ref =
      "resource_governor:vector_generation:" + std::to_string(seed);
  return envelope;
}

idx::VectorGenerationRecallContract RecallContract(platform::u64 seed) {
  idx::VectorGenerationRecallContract contract;
  contract.top_k = 10;
  contract.exact_sample_rows = 128;
  contract.required_recall = 0.95;
  contract.observed_recall = 0.98;
  contract.deterministic_sample = true;
  contract.evidence_present = true;
  contract.evidence_ref =
      "recall_contract:vector_generation:" + std::to_string(seed);
  return contract;
}

idx::VectorGenerationRequest BuildRequest(platform::u64 seed) {
  idx::VectorGenerationRequest request;
  request.index_uuid = NewUuid(platform::UuidKind::object, seed + 1);
  request.table_uuid = NewUuid(platform::UuidKind::object, seed + 2);
  request.generation = seed;
  request.algorithm = idx::IndexVectorAlgorithm::hnsw;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:vector_generation:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:vector_generation:" + std::to_string(seed);
  request.resource_envelope = ResourceEnvelope(seed);
  return request;
}

idx::VectorGenerationDescriptor PublishedGeneration(
    idx::VectorGenerationLedger* ledger,
    idx::VectorGenerationRequest request,
    platform::u64 seed) {
  auto requested = idx::RequestVectorGeneration(ledger, request);
  Require(requested.ok(), "DPC-044 vector generation request failed");
  auto generation = requested.generation;
  Require(generation.generation_uuid.valid(),
          "DPC-044 request did not generate generation UUID");
  Require(!generation.visible, "DPC-044 requested generation was visible");

  auto building = idx::StartVectorGenerationBuild(ledger, &generation);
  Require(building.ok(), "DPC-044 vector generation build start failed");
  Require(!generation.visible, "DPC-044 building generation was visible");

  idx::VectorGenerationTrainingRequest training;
  training.training_succeeded = true;
  training.complete_training_set = true;
  training.training_evidence_ref =
      "training:vector_generation:" + std::to_string(seed);
  auto trained =
      idx::MarkVectorGenerationTrained(ledger, &generation, training);
  Require(trained.ok(), "DPC-044 vector generation training failed");
  Require(!generation.visible, "DPC-044 trained generation was visible");

  idx::VectorGenerationValidationRequest validation;
  validation.validation_succeeded = true;
  validation.complete_generation = true;
  validation.validation_evidence_ref =
      "validation:vector_generation:" + std::to_string(seed);
  auto validated =
      idx::ValidateVectorGeneration(ledger, &generation, validation);
  Require(validated.ok(), "DPC-044 vector generation validation failed");
  Require(!generation.visible, "DPC-044 validated generation was visible");

  idx::VectorGenerationSealRequest seal;
  seal.sealed_bytes_complete = true;
  seal.sealed_generation_evidence_ref =
      "sealed_generation:vector_generation:" + std::to_string(seed);
  seal.recall_contract = RecallContract(seed);
  auto sealed = idx::SealVectorGeneration(ledger, &generation, seal);
  Require(sealed.ok(), "DPC-044 vector generation seal failed");
  Require(!generation.visible, "DPC-044 sealed generation was visible");

  const auto generated_uuid = generation.generation_uuid;
  const auto index_uuid = generation.index_uuid;
  const auto table_uuid = generation.table_uuid;
  idx::VectorGenerationPublishRequest publish;
  publish.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:vector_generation:" +
      std::to_string(seed);
  publish.engine_owned_mga_publish_barrier = true;
  auto published =
      idx::PublishVectorGeneration(ledger, &generation, publish);
  Require(published.ok(), "DPC-044 vector generation publish failed");
  Require(generation.visible,
          "DPC-044 published vector generation was not visible");
  Require(SameUuid(generation.generation_uuid, generated_uuid),
          "DPC-044 publish did not preserve generated generation UUID");
  Require(SameUuid(generation.index_uuid, index_uuid) &&
              SameUuid(generation.table_uuid, table_uuid),
          "DPC-044 publish did not preserve index/table UUID identity");
  Require(generation.authority_source == idx::kVectorGenerationAuthoritySource,
          "DPC-044 publish did not record engine MGA authority source");
  return generation;
}

idx::VectorGenerationDescriptor PublishedGeneration(
    idx::VectorGenerationLedger* ledger,
    platform::u64 seed) {
  return PublishedGeneration(ledger, BuildRequest(seed), seed);
}

void RequireDiagnostic(const idx::VectorGenerationAccessPlan& plan,
                       std::string_view diagnostic_code,
                       std::string_view fallback_reason,
                       std::string_view message) {
  if (plan.diagnostic.diagnostic_code != diagnostic_code ||
      plan.fallback_reason != fallback_reason) {
    std::cerr << "diagnostic=" << plan.diagnostic.diagnostic_code
              << " fallback=" << plan.fallback_reason
              << " access=" << plan.selected_access << '\n';
  }
  Require(plan.diagnostic.diagnostic_code == diagnostic_code, message);
  Require(plan.fallback_reason == fallback_reason, message);
}

void ProveInvisibleUntilSealRecallAndMgaPublish() {
  idx::VectorGenerationLedger ledger;
  const auto generation = PublishedGeneration(&ledger, 4401);

  bool saw_requested = false;
  bool saw_building = false;
  bool saw_trained = false;
  bool saw_validated = false;
  bool saw_sealed = false;
  bool saw_published = false;
  for (const auto& row : ledger.evidence) {
    Require(!row.diagnostic_code.empty(),
            "DPC-044 evidence diagnostic was empty");
    Require(row.resource_governor_evidence_present,
            "DPC-044 evidence missing resource governor proof");
    Require(row.memory_observed_bytes <= row.memory_limit_bytes &&
                row.temp_space_observed_bytes <=
                    row.temp_space_limit_bytes &&
                row.workers_used <= row.worker_limit,
            "DPC-044 resource evidence exceeded its envelope");
    Require(!row.parser_finality_authority &&
                !row.client_state_authority &&
                !row.timestamp_ordering_authority &&
                !row.uuid_ordering_authority &&
                !row.event_stream_authority &&
                !row.donor_authority &&
                !row.write_ahead_authority,
            "DPC-044 evidence accepted external finality authority");
    if (row.state == idx::VectorGenerationState::requested) {
      saw_requested = true;
      Require(!row.visible, "DPC-044 requested evidence was visible");
    }
    if (row.state == idx::VectorGenerationState::building) {
      saw_building = true;
      Require(!row.visible, "DPC-044 building evidence was visible");
    }
    if (row.state == idx::VectorGenerationState::trained) {
      saw_trained = true;
      Require(!row.visible, "DPC-044 trained evidence was visible");
    }
    if (row.state == idx::VectorGenerationState::validated) {
      saw_validated = true;
      Require(row.validation_evidence_present,
              "DPC-044 validated evidence missing validation proof");
      Require(!row.visible, "DPC-044 validated evidence was visible");
    }
    if (row.state == idx::VectorGenerationState::sealed) {
      saw_sealed = true;
      Require(row.validation_evidence_present &&
                  row.sealed_generation_evidence_present &&
                  row.recall_contract_evidence_present,
              "DPC-044 sealed evidence missing recall or seal proof");
      Require(!row.visible, "DPC-044 sealed evidence was visible");
    }
    if (row.state == idx::VectorGenerationState::published) {
      saw_published = true;
      Require(row.visible, "DPC-044 published evidence was hidden");
      Require(row.publish_barrier_evidence_present &&
                  row.publish_barrier_engine_owned_mga,
              "DPC-044 published evidence missing MGA barrier");
      Require(row.authority_source == idx::kVectorGenerationAuthoritySource,
              "DPC-044 published evidence authority source changed");
    }
  }
  Require(saw_requested && saw_building && saw_trained && saw_validated &&
              saw_sealed && saw_published,
          "DPC-044 lifecycle evidence did not cover vector states");

  idx::VectorGenerationAccessRequest access;
  access.generations = {generation};
  const auto plan = idx::PlanVectorGenerationAccess(access);
  Require(plan.ok(), "DPC-044 published generation was not selected");
  Require(plan.selected_access == "sealed_ann_vector_scan",
          "DPC-044 selected vector access changed");
  Require(plan.published_generation_uuids.size() == 1 &&
              SameUuid(plan.published_generation_uuids.front(),
                       generation.generation_uuid),
          "DPC-044 selected generation UUID did not match published state");
  Require(!plan.generation_metadata_visibility_authority &&
              !plan.generation_metadata_finality_authority,
          "DPC-044 generation metadata became visibility/finality authority");
}

void ProveExactFallbackForUnsafeGenerationStates() {
  idx::VectorGenerationLedger ledger;
  const auto generation = PublishedGeneration(&ledger, 4501);

  idx::VectorGenerationAccessRequest request;
  request.generations = {generation};

  auto disabled = request;
  disabled.vector_generation_enabled = false;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(disabled),
      "INDEX.VECTOR_GENERATION.DISABLED_EXACT_FALLBACK",
      "disabled_generation_exact_fallback",
      "DPC-044 disabled generation did not select exact fallback");

  auto missing = request;
  missing.generations.clear();
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(missing),
      "INDEX.VECTOR_GENERATION.MISSING_EXACT_FALLBACK",
      "missing_generation_exact_fallback",
      "DPC-044 missing generation did not select exact fallback");

  auto stale = request;
  stale.generations.front().stale = true;
  RequireDiagnostic(idx::PlanVectorGenerationAccess(stale),
                    "INDEX.VECTOR_GENERATION.STALE_EXACT_FALLBACK",
                    "stale_generation_exact_fallback",
                    "DPC-044 stale generation did not select exact fallback");

  auto corrupt = request;
  corrupt.generations.front().checksum_valid = false;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(corrupt),
      "INDEX.VECTOR_GENERATION.CORRUPT_EXACT_FALLBACK",
      "corrupt_generation_exact_fallback",
      "DPC-044 corrupt generation did not select exact fallback");

  auto incomplete = request;
  incomplete.generations.front().state = idx::VectorGenerationState::building;
  incomplete.generations.front().visible = false;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(incomplete),
      "INDEX.VECTOR_GENERATION.INCOMPLETE_EXACT_FALLBACK",
      "incomplete_generation_exact_fallback",
      "DPC-044 incomplete generation did not select exact fallback");

  auto unsealed = request;
  unsealed.generations.front().state = idx::VectorGenerationState::sealed;
  unsealed.generations.front().visible = false;
  unsealed.generations.front().persisted_record_present = false;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(unsealed),
      "INDEX.VECTOR_GENERATION.UNSEALED_EXACT_FALLBACK",
      "unsealed_generation_exact_fallback",
      "DPC-044 unsealed generation did not select exact fallback");

  auto non_authoritative = request;
  non_authoritative.generations.front().authority_source = "parser_timestamp";
  non_authoritative.generations.front().parser_finality_authority_claimed =
      true;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(non_authoritative),
      "INDEX.VECTOR_GENERATION.NON_AUTHORITATIVE_EXACT_FALLBACK",
      "non_authoritative_generation_exact_fallback",
      "DPC-044 non-authoritative generation did not select exact fallback");

  auto invalid_identity = request;
  invalid_identity.generations.front().generation_uuid = platform::TypedUuid{};
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(invalid_identity),
      "INDEX.VECTOR_GENERATION.INVALID_IDENTITY_EXACT_FALLBACK",
      "invalid_identity_exact_fallback",
      "DPC-044 invalid identity did not select exact fallback");

  auto recall_failure = request;
  recall_failure.generations.front().recall_contract.observed_recall = 0.80;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(recall_failure),
      "INDEX.VECTOR_GENERATION.RECALL_EXACT_FALLBACK",
      "recall_contract_exact_fallback",
      "DPC-044 recall failure did not select exact fallback");

  auto unsafe = request;
  unsafe.generations.front().publish_barrier_engine_owned_mga = false;
  RequireDiagnostic(
      idx::PlanVectorGenerationAccess(unsafe),
      "INDEX.VECTOR_GENERATION.UNSAFE_STATE_EXACT_FALLBACK",
      "unsafe_state_exact_fallback",
      "DPC-044 unsafe state did not select exact fallback");

  auto no_fallback = corrupt;
  no_fallback.exact_vector_scan_fallback_available = false;
  const auto refused = idx::PlanVectorGenerationAccess(no_fallback);
  Require(!refused.status.ok(),
          "DPC-044 unsafe generation without exact fallback did not fail closed");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.VECTOR_GENERATION.UNSAFE_NO_EXACT_FALLBACK",
          "DPC-044 unsafe no-fallback diagnostic changed");
  Require(refused.selected_access == "refused",
          "DPC-044 unsafe no-fallback access was not refused");
}

void ProveCrashReopenClassification() {
  idx::VectorGenerationLedger clean_ledger;
  (void)PublishedGeneration(&clean_ledger, 4601);
  const auto clean =
      idx::ClassifyVectorGenerationReopen(clean_ledger, true);
  Require(clean.ok(), "DPC-044 clean reopen failed");
  Require(clean.recovery_class ==
              idx::VectorGenerationRecoveryClass::
                  clean_sealed_published_generation,
          "DPC-044 clean reopen class changed");

  idx::VectorGenerationLedger pending_ledger = clean_ledger;
  auto pending =
      idx::RequestVectorGeneration(&pending_ledger, BuildRequest(4611));
  Require(pending.ok(), "DPC-044 pending reopen fixture failed");
  const auto fallback =
      idx::ClassifyVectorGenerationReopen(pending_ledger, true);
  Require(fallback.ok(),
          "DPC-044 reopen with pending generation and fallback failed closed");
  Require(fallback.recovery_class ==
              idx::VectorGenerationRecoveryClass::
                  incomplete_pending_exact_fallback,
          "DPC-044 pending reopen fallback class changed");
  Require(fallback.action ==
              idx::VectorGenerationRecoveryAction::
                  use_exact_vector_scan_fallback,
          "DPC-044 pending reopen fallback action changed");

  auto unsafe_ledger = clean_ledger;
  Require(!unsafe_ledger.generations.empty(),
          "DPC-044 unsafe reopen fixture missing generation");
  unsafe_ledger.generations.front().recall_contract.observed_recall = 0.10;
  unsafe_ledger.generations.front().visible = true;
  const auto refused =
      idx::ClassifyVectorGenerationReopen(unsafe_ledger, true);
  Require(!refused.ok(),
          "DPC-044 unsafe visible generation was not refused");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.VECTOR_GENERATION.RECOVERY_UNSAFE_REFUSED",
          "DPC-044 unsafe reopen diagnostic changed");
}

void ProvePublishEvidenceRefusalDoesNotMutateGeneration() {
  idx::VectorGenerationLedger ledger;
  auto requested = idx::RequestVectorGeneration(&ledger, BuildRequest(4651));
  Require(requested.ok(), "DPC-044 corrupt publish setup request failed");
  auto generation = requested.generation;
  Require(idx::StartVectorGenerationBuild(&ledger, &generation).ok(),
          "DPC-044 corrupt publish setup build failed");

  idx::VectorGenerationTrainingRequest training;
  training.training_succeeded = true;
  training.complete_training_set = true;
  training.training_evidence_ref = "training:vector_generation:4651";
  Require(idx::MarkVectorGenerationTrained(&ledger, &generation, training).ok(),
          "DPC-044 corrupt publish setup training failed");

  idx::VectorGenerationValidationRequest validation;
  validation.validation_succeeded = true;
  validation.complete_generation = true;
  validation.validation_evidence_ref = "validation:vector_generation:4651";
  Require(idx::ValidateVectorGeneration(&ledger, &generation, validation).ok(),
          "DPC-044 corrupt publish setup validation failed");

  idx::VectorGenerationSealRequest seal;
  seal.sealed_bytes_complete = true;
  seal.sealed_generation_evidence_ref =
      "sealed_generation:vector_generation:4651";
  seal.recall_contract = RecallContract(4651);
  Require(idx::SealVectorGeneration(&ledger, &generation, seal).ok(),
          "DPC-044 corrupt publish setup seal failed");

  generation.recall_contract.observed_recall = 0.10;
  idx::VectorGenerationPublishRequest publish;
  publish.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:vector_generation:4651";
  publish.engine_owned_mga_publish_barrier = true;
  const auto refused =
      idx::PublishVectorGeneration(&ledger, &generation, publish);
  Require(!refused.ok(),
          "DPC-044 corrupt sealed generation publish was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.VECTOR_GENERATION.PUBLISH_EVIDENCE_MISSING",
          "DPC-044 corrupt sealed publish diagnostic changed");
  Require(generation.state == idx::VectorGenerationState::sealed &&
              !generation.persisted_record_present &&
              !generation.visible,
          "DPC-044 failed publish mutated generation into visible/persisted state");
}

void ProveResourceEnvelopeAndAuthorityClaimsRejected() {
  auto unbounded = BuildRequest(4701);
  unbounded.resource_envelope.worker_limit = 0;
  idx::VectorGenerationLedger ledger;
  const auto resource_refused =
      idx::RequestVectorGeneration(&ledger, unbounded);
  Require(!resource_refused.ok(),
          "DPC-044 unbounded resource envelope was accepted");
  Require(resource_refused.diagnostic.diagnostic_code ==
              "INDEX.VECTOR_GENERATION.RESOURCE_ENVELOPE_REFUSED",
          "DPC-044 resource refusal diagnostic changed");

  auto request = BuildRequest(4711);
  request.parser_finality_authority = true;
  request.client_state_authority = true;
  request.timestamp_ordering_authority = true;
  request.uuid_ordering_authority = true;
  request.event_stream_authority = true;
  request.donor_authority = true;
  request.write_ahead_authority = true;
  const auto authority_refused =
      idx::RequestVectorGeneration(&ledger, request);
  Require(!authority_refused.ok(),
          "DPC-044 parser/client/timestamp/UUID/event/donor/write-ahead authority claim was accepted");
  Require(authority_refused.diagnostic.diagnostic_code ==
              "INDEX.VECTOR_GENERATION.EXTERNAL_AUTHORITY_REJECTED",
          "DPC-044 external authority rejection diagnostic changed");
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "DPC-044 gate search key missing");
  Require(!kImplementationSearchKey.empty(),
          "DPC-044 implementation search key missing");

  ProveInvisibleUntilSealRecallAndMgaPublish();
  ProveExactFallbackForUnsafeGenerationStates();
  ProveCrashReopenClassification();
  ProvePublishEvidenceRefusalDoesNotMutateGeneration();
  ProveResourceEnvelopeAndAuthorityClaimsRejected();

  std::cout << "DPC_VECTOR_GENERATION_PUBLICATION_GATE passed\n";
  return EXIT_SUCCESS;
}
