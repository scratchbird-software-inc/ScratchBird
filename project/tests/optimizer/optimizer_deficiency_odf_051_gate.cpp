// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"
#include "mga_page_finality_evidence.hpp"
#include "mga_page_finality_map.hpp"
#include "page_finality_evidence.hpp"
#include "row_version.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace opt = scratchbird::engine::optimizer;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-051 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::string relation_uuid = NewUuidText(platform::UuidKind::object, 51001);
  platform::u64 page_number = 42;
  platform::u64 page_generation = 7;
  platform::u64 extent_id = 3;
  platform::u64 extent_epoch = 11;
  platform::u64 relation_epoch = 13;
  platform::u64 catalog_epoch = 17;
};

mga::PageFinalityObservedFacts Observed(const Fixture& fixture) {
  mga::PageFinalityObservedFacts observed;
  observed.relation_uuid = fixture.relation_uuid;
  observed.page_number = fixture.page_number;
  observed.page_generation = fixture.page_generation;
  observed.extent_id = fixture.extent_id;
  observed.extent_epoch = fixture.extent_epoch;
  observed.relation_epoch = fixture.relation_epoch;
  observed.catalog_epoch = fixture.catalog_epoch;
  observed.reader_visible_through_local_transaction_id = mga::MakeLocalTransactionId(90);
  observed.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(100);
  observed.transaction_horizon_authoritative = true;
  observed.transaction_inventory_authoritative = true;
  observed.normal_mga_visibility_authority_available = true;
  return observed;
}

mga::PageFinalityMapEntry PageEntry(const Fixture& fixture) {
  mga::PageFinalityMapEntry entry;
  entry.scope = mga::PageFinalityScope::page;
  entry.status = mga::PageFinalityMapStatus::current;
  entry.provenance = mga::PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = fixture.relation_uuid;
  entry.page_number = fixture.page_number;
  entry.page_generation = fixture.page_generation;
  entry.extent_id = fixture.extent_id;
  entry.extent_epoch = fixture.extent_epoch;
  entry.relation_epoch = fixture.relation_epoch;
  entry.catalog_epoch = fixture.catalog_epoch;
  entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(80);
  entry.map_generation = 5;
  entry.persisted_record_present = true;
  entry.checksum_valid = true;
  entry.all_visible = true;
  entry.all_final = false;
  return entry;
}

mga::PageFinalityMapEntry ExtentEntry(const Fixture& fixture) {
  auto entry = PageEntry(fixture);
  entry.scope = mga::PageFinalityScope::extent;
  entry.page_number = 0;
  entry.page_generation = fixture.page_generation;
  entry.all_visible = true;
  entry.all_final = true;
  return entry;
}

bool HasDiagnostic(const opt::OptimizerMgaPageFinalityEvidence& evidence,
                   std::string_view token) {
  for (const auto& diagnostic : evidence.diagnostics) {
    if (diagnostic.find(token) != std::string::npos) { return true; }
  }
  return false;
}

void RequireNoRuntimeDocTokens(
    const opt::OptimizerMgaPageFinalityEvidence& evidence) {
  for (const auto& diagnostic : evidence.diagnostics) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(diagnostic.find(forbidden) == std::string::npos,
              "ODF-051 runtime evidence leaked documentation token");
    }
  }
}

void AcceptedPageEvidenceAcceleratesIndexOnlyAndDmlRecheck() {
  const Fixture fixture;
  page::MgaPageFinalityMap map;
  map.entries.push_back(PageEntry(fixture));

  const auto index_only = page::LookupPageAllVisibleEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::index_only_scan);
  Require(index_only.decision.accepted,
          "ODF-051 index-only page finality evidence was refused");
  Require(index_only.decision.all_visible,
          "ODF-051 accepted page evidence did not report all-visible");
  Require(!index_only.decision.map_is_transaction_finality_authority,
          "ODF-051 map became transaction finality authority");
  Require(index_only.decision.durable_mga_inventory_remains_authority,
          "ODF-051 durable MGA inventory authority flag missing");
  Require(!index_only.decision.normal_mga_recheck_required,
          "ODF-051 accepted page evidence did not accelerate index-only scan");

  const auto optimizer_evidence =
      opt::BuildOptimizerMgaPageFinalityEvidence(index_only);
  Require(optimizer_evidence.accepted,
          "ODF-051 optimizer evidence did not preserve acceptance");
  Require(optimizer_evidence.accepted_count == 1,
          "ODF-051 accepted counter mismatch");
  Require(optimizer_evidence.refused_count == 0,
          "ODF-051 refused counter unexpectedly advanced");
  Require(optimizer_evidence.evidence_name ==
              "mga_page_finality.page_all_visible.accepted",
          "ODF-051 page evidence name mismatch");
  Require(HasDiagnostic(optimizer_evidence,
                        "authority_source=durable_mga_transaction_inventory"),
          "ODF-051 authority-source evidence missing");
  RequireNoRuntimeDocTokens(optimizer_evidence);

  opt::PlanCandidate candidate;
  candidate.candidate_id = "odf051-candidate";
  candidate.mga_page_finality_evidence = optimizer_evidence;
  const auto serialized = opt::SerializePlanCandidateToJson(candidate);
  Require(serialized.find("\"mga_page_finality\"") != std::string::npos,
          "ODF-051 plan candidate serialization omitted finality evidence");
  Require(serialized.find("\"accepted_count\":1") != std::string::npos,
          "ODF-051 plan candidate serialization omitted accepted counter");
  Require(serialized.find("\"finality_map_transaction_authority\":false") !=
              std::string::npos,
          "ODF-051 plan candidate serialization changed authority flag");

  const auto dml = page::LookupPageAllVisibleEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::dml_recheck);
  Require(dml.decision.accepted,
          "ODF-051 DML recheck page finality evidence was refused");
}

void AcceptedExtentEvidenceAcceleratesCleanupAndSummaryPruning() {
  const Fixture fixture;
  page::MgaPageFinalityMap map;
  map.entries.push_back(ExtentEntry(fixture));

  const auto cleanup = page::LookupExtentAllFinalEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::cleanup);
  Require(cleanup.decision.accepted,
          "ODF-051 cleanup extent finality evidence was refused");
  Require(cleanup.decision.all_final,
          "ODF-051 accepted cleanup evidence did not report all-final");

  const auto summary = page::LookupExtentAllFinalEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::summary_pruning);
  Require(summary.decision.accepted,
          "ODF-051 summary pruning extent finality evidence was refused");
  Require(summary.decision.evidence_name ==
              "mga_page_finality.extent_all_final.accepted",
          "ODF-051 extent evidence name mismatch");
}

void StaleAndMissingEvidenceFailClosed() {
  const Fixture fixture;
  page::MgaPageFinalityMap map;
  auto stale = PageEntry(fixture);
  stale.status = mga::PageFinalityMapStatus::stale;
  map.entries.push_back(stale);

  const auto result = page::LookupPageAllVisibleEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::index_only_scan);
  Require(!result.decision.accepted,
          "ODF-051 stale page evidence did not fail closed");
  Require(result.decision.normal_mga_recheck_required,
          "ODF-051 stale evidence did not require normal MGA recheck");
  Require(result.decision.refusal_reason == "finality_map_not_current",
          "ODF-051 stale refusal reason mismatch");
  Require(result.map_counters.stale_refusals == 1,
          "ODF-051 stale refusal counter mismatch");

  page::MgaPageFinalityMap empty;
  const auto missing = page::LookupPageAllVisibleEvidence(
      &empty, Observed(fixture), mga::PageFinalityConsumer::index_only_scan);
  Require(!missing.decision.accepted,
          "ODF-051 missing page evidence did not fail closed");
  Require(missing.decision.refusal_reason == "finality_map_missing",
          "ODF-051 missing refusal reason mismatch");
}

void GenerationAndEpochMismatchFailClosed() {
  const Fixture fixture;
  page::MgaPageFinalityMap map;
  map.entries.push_back(PageEntry(fixture));
  auto observed = Observed(fixture);
  ++observed.page_generation;

  const auto page_generation = page::LookupPageAllVisibleEvidence(
      &map, observed, mga::PageFinalityConsumer::index_only_scan);
  Require(!page_generation.decision.accepted,
          "ODF-051 page generation mismatch was accepted");
  Require(page_generation.decision.refusal_reason == "page_generation_mismatch",
          "ODF-051 page generation refusal reason mismatch");
  Require(page_generation.map_counters.epoch_refusals == 1,
          "ODF-051 page generation epoch counter mismatch");

  page::MgaPageFinalityMap extent_map;
  extent_map.entries.push_back(ExtentEntry(fixture));
  auto extent_observed = Observed(fixture);
  ++extent_observed.extent_epoch;
  const auto extent_epoch = page::LookupExtentAllFinalEvidence(
      &extent_map, extent_observed, mga::PageFinalityConsumer::cleanup);
  Require(!extent_epoch.decision.accepted,
          "ODF-051 extent epoch mismatch was accepted");
  Require(extent_epoch.decision.refusal_reason == "extent_epoch_mismatch",
          "ODF-051 extent epoch refusal reason mismatch");
}

void ActiveTransactionAndProvenanceRefusalsFailClosed() {
  const Fixture fixture;
  {
    page::MgaPageFinalityMap map;
    map.entries.push_back(PageEntry(fixture));
    auto observed = Observed(fixture);
    observed.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(80);
    const auto result = page::LookupPageAllVisibleEvidence(
        &map, observed, mga::PageFinalityConsumer::index_only_scan);
    Require(!result.decision.accepted,
            "ODF-051 active transaction overlap was accepted");
    Require(result.decision.refusal_reason ==
                "active_transaction_overlaps_finality_map",
            "ODF-051 active transaction refusal reason mismatch");
    Require(result.map_counters.horizon_refusals == 1,
            "ODF-051 active transaction counter mismatch");
  }
  {
    page::MgaPageFinalityMap map;
    auto entry = PageEntry(fixture);
    entry.provenance = mga::PageFinalityProvenance::uuid_order_claim;
    map.entries.push_back(entry);
    const auto result = page::LookupPageAllVisibleEvidence(
        &map, Observed(fixture), mga::PageFinalityConsumer::index_only_scan);
    Require(!result.decision.accepted,
            "ODF-051 UUID-order provenance was accepted");
    Require(result.decision.refusal_reason ==
                "finality_map_external_provenance_refused",
            "ODF-051 provenance refusal reason mismatch");
    Require(result.map_counters.provenance_refusals == 1,
            "ODF-051 provenance refusal counter mismatch");
  }
}

mga::TransactionIdentity Tx(platform::u64 local_id, platform::u64 salt) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction, salt),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "ODF-051 transaction identity failed");
  return identity.identity;
}

mga::RowIdentity Row(platform::u64 salt) {
  const auto row = mga::MakeRowIdentity(NewUuid(platform::UuidKind::row, salt));
  Require(row.ok(), "ODF-051 row identity failed");
  return row.identity;
}

void NormalMgaVisibilityRecheckRemainsAuthority() {
  const Fixture fixture;
  page::MgaPageFinalityMap map;
  map.entries.push_back(PageEntry(fixture));
  const auto result = page::LookupPageAllVisibleEvidence(
      &map, Observed(fixture), mga::PageFinalityConsumer::index_only_scan);
  Require(result.decision.accepted,
          "ODF-051 setup page finality evidence was refused");
  Require(!result.decision.map_is_transaction_finality_authority,
          "ODF-051 setup map became authority");

  mga::RowVersionMetadata row_version;
  row_version.identity.row = Row(51100);
  row_version.identity.creator_transaction = Tx(7, 51101);
  row_version.identity.version_sequence = 1;
  row_version.state = mga::RowVersionState::uncommitted;
  row_version.creator_transaction_state = mga::TransactionState::active;
  row_version.payload_present = true;

  mga::VisibilitySnapshot snapshot;
  snapshot.reader_transaction = mga::MakeLocalTransactionId(20);
  snapshot.visible_through_local_transaction_id = 90;
  snapshot.allow_reader_own_uncommitted = false;
  const auto visibility = mga::EvaluateVisibility(row_version, snapshot);
  Require(visibility.decision == mga::VisibilityDecision::wait_for_transaction,
          "ODF-051 finality map bypassed normal MGA row visibility authority");
}

}  // namespace

int main() {
  AcceptedPageEvidenceAcceleratesIndexOnlyAndDmlRecheck();
  AcceptedExtentEvidenceAcceleratesCleanupAndSummaryPruning();
  StaleAndMissingEvidenceFailClosed();
  GenerationAndEpochMismatchFailClosed();
  ActiveTransactionAndProvenanceRefusalsFailClosed();
  NormalMgaVisibilityRecheckRemainsAuthority();
  return 0;
}
