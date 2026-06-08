// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_physical_provider_contract.hpp"
#include "optimizer_contract.hpp"
#include "specialized_planner.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view token) {
  for (const auto& value : values) {
    if (value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireDiagnostic(const api::EngineNoSqlPhysicalProviderSelection& selection,
                       std::string_view diagnostic) {
  Require(api::EngineNoSqlSelectionHasDiagnostic(selection, diagnostic),
          "ODF-070 expected provider diagnostic was missing");
}

void RequireEvidence(const std::vector<std::string>& evidence,
                     std::string_view token) {
  Require(Contains(evidence, token), "ODF-070 expected provider evidence was missing");
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "parser_executes_sql=true",
          "behavior_store_scan_selected=true", "descriptor_scan_selected=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "parser_transaction_finality_authority=true",
          "write_ahead_log_transaction_finality_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-070 evidence leaked forbidden authority or document token");
    }
  }
}

api::EngineNoSqlPhysicalProviderContract BaseContract() {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "nosql.local.document_path_provider";
  contract.fallback_provider_id = "nosql.local.row_uuid_exact_recheck";
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.estimated_rows = 23;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.descriptor_visibility.descriptor_generation = 7;
  contract.descriptor_visibility.proof_id = "descriptor-visible:7";
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.security_redaction.redaction_profile = "tenant-redacted";
  contract.security_redaction.proof_id = "security-redaction-bound";
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = 5;
  contract.index_generation.available_generation = 5;
  contract.index_generation.index_uuid = "idx-document-path";
  contract.index_generation.proof_id = "index-generation:5";
  contract.delta_overlay.required = true;
  contract.delta_overlay.proof_present = true;
  contract.delta_overlay.covers_snapshot = true;
  contract.delta_overlay.overlay_generation = 3;
  contract.delta_overlay.proof_id = "delta-overlay:3";
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.policy.policy_snapshot_uuid = "policy-snapshot-local";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

std::vector<std::string> CompleteDescriptors() {
  return {
      "nosql.provider.family=document",
      "nosql.provider.local",
      "nosql.provider.id=nosql.local.document_path_provider",
      "nosql.provider.fallback_id=nosql.local.row_uuid_exact_recheck",
      "nosql.exact_fallback.available",
      "nosql.descriptor.compatible",
      "nosql.descriptor_visibility.proof",
      "nosql.security.proof",
      "nosql.index.available",
      "nosql.index_generation.proof",
      "nosql.index_generation.required=5",
      "nosql.index_generation.available=5",
      "nosql.index.covers_predicate",
      "nosql.delta_overlay.required",
      "nosql.delta_overlay.proof",
      "nosql.policy.proof",
      "nosql.policy.allowed",
      "nosql.mga_recheck.proof",
  };
}

plan::LogicalPlan NoSqlPlan(std::vector<std::string> descriptors) {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "ODF-070-nosql-provider";
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kNoSqlOperation,
                                        plan::PhysicalAccessKind::kDocumentPathProbe,
                                        "nosql.document_find",
                                        "odf_070_document_find");
  node.required_object_uuids.push_back("document.collection.local");
  node.required_descriptors = std::move(descriptors);
  logical.nodes.push_back(std::move(node));
  return logical;
}

void SuccessfulLocalProviderSelectionWithAllProofs() {
  const auto contract = BaseContract();
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(selection.selected, "ODF-070 complete provider proof was not selected");
  Require(!selection.fail_closed, "ODF-070 complete provider proof failed closed");
  Require(selection.missing_diagnostics.empty(), "ODF-070 complete proof reported missing diagnostics");
  Require(selection.refusal_diagnostics.empty(), "ODF-070 complete proof reported refusals");
  Require(selection.row_mga_recheck_required, "ODF-070 MGA recheck was not required");
  Require(selection.row_security_recheck_required, "ODF-070 security recheck was not required");
  RequireEvidence(selection.evidence, "selected_access=local_physical_provider");
  RequireEvidence(selection.evidence, "selected_provider_id=nosql.local.document_path_provider");
  RequireEvidence(selection.evidence, "fallback_provider_id=nosql.local.row_uuid_exact_recheck");
  RequireEvidence(selection.evidence, "row_mga_recheck_required=true");
  RequireEvidence(selection.evidence, "row_security_recheck_required=true");
  RequireEvidence(selection.evidence, "transaction_authority_source=engine_transaction_inventory");
  RequireEvidence(selection.evidence, "provider_transaction_finality_authority=false");
  RequireEvidence(selection.evidence, "provider_visibility_authority=false");
  RequireEvidence(selection.evidence, "parser_transaction_finality_authority=false");
  RequireEvidence(selection.evidence, "write_ahead_log_transaction_finality_authority=false");
  RequireEvidenceHygiene(selection.evidence);

  const auto candidate = opt::PlanNoSqlPhysicalProviderCandidate(contract);
  Require(candidate.cost.selectable, "ODF-070 optimizer candidate refused complete provider proof");
  Require(candidate.access_kind == plan::PhysicalAccessKind::kDocumentPathProbe,
          "ODF-070 optimizer selected the wrong physical provider kind");
  Require(Contains(candidate.runtime_evidence, "selected_access=local_physical_provider"),
          "ODF-070 optimizer candidate did not carry provider evidence");
}

void OptimizerUsesProviderContractAndDoesNotSelectScanFallbacks() {
  const auto optimized = opt::OptimizeLogicalPlan(NoSqlPlan(CompleteDescriptors()));
  Require(optimized.ok, "ODF-070 complete NoSQL logical plan did not optimize");
  Require(optimized.has_physical_plan, "ODF-070 complete NoSQL logical plan had no physical plan");
  Require(optimized.physical_root.access_kind == plan::PhysicalAccessKind::kDocumentPathProbe,
          "ODF-070 optimizer did not select the local document provider");
  RequireEvidence(optimized.physical_root.runtime_evidence,
                  "selected_access=local_physical_provider");
  RequireEvidence(optimized.physical_root.runtime_evidence,
                  "row_mga_recheck_required=true");
  RequireEvidence(optimized.physical_root.runtime_evidence,
                  "row_security_recheck_required=true");
  RequireEvidenceHygiene(optimized.physical_root.runtime_evidence);

  auto descriptor_scan_descriptors = CompleteDescriptors();
  descriptor_scan_descriptors.push_back("nosql.descriptor_scan.selected");
  const auto descriptor_scan = opt::OptimizeLogicalPlan(
      NoSqlPlan(descriptor_scan_descriptors));
  Require(!descriptor_scan.ok, "ODF-070 descriptor scan was selected as a physical provider");
  Require(!descriptor_scan.has_physical_plan,
          "ODF-070 descriptor scan produced a successful physical plan");
  Require(!descriptor_scan.candidates.empty(),
          "ODF-070 descriptor scan did not produce a refused candidate");
  Require(!descriptor_scan.candidates.front().cost.selectable,
          "ODF-070 descriptor scan candidate remained selectable");
  Require(descriptor_scan.candidates.front().rejection_reason ==
              api::kNoSqlProviderDescriptorScanNotPhysicalProvider,
          "ODF-070 descriptor scan refusal diagnostic changed");
}

void MissingDescriptorVisibilityFailsClosed() {
  auto contract = BaseContract();
  contract.descriptor_visibility.proof_present = false;
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 missing descriptor visibility did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderDescriptorVisibilityProofMissing);

  auto descriptors = CompleteDescriptors();
  descriptors.erase(descriptors.begin() + 6);
  const auto optimized = opt::OptimizeLogicalPlan(NoSqlPlan(descriptors));
  Require(!optimized.ok && !optimized.has_physical_plan,
          "ODF-070 optimizer selected a NoSQL provider without descriptor visibility");
  Require(!optimized.candidates.empty() &&
              optimized.candidates.front().rejection_reason ==
                  api::kNoSqlProviderDescriptorVisibilityProofMissing,
          "ODF-070 optimizer descriptor visibility diagnostic changed");
}

void MissingSecurityProofFailsClosed() {
  auto contract = BaseContract();
  contract.security_redaction.proof_present = false;
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 missing security proof did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderSecurityProofMissing);
}

void MissingOrStaleIndexGenerationFailsClosed() {
  auto missing = BaseContract();
  missing.index_generation.proof_present = false;
  auto selection = api::SelectLocalNoSqlPhysicalProvider(missing);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 missing index generation did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderIndexGenerationProofMissing);

  auto stale = BaseContract();
  stale.index_generation.required_generation = 6;
  stale.index_generation.available_generation = 5;
  selection = api::SelectLocalNoSqlPhysicalProvider(stale);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 stale index generation did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderIndexGenerationStale);
}

void MissingDeltaOverlayProofFailsClosedWhenRequired() {
  auto contract = BaseContract();
  contract.delta_overlay.required = true;
  contract.delta_overlay.proof_present = false;
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 missing delta overlay proof did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderDeltaOverlayProofMissing);
}

void RecheckAndAuthorityRequirementsRemainExplicit() {
  auto missing_mga_recheck = BaseContract();
  missing_mga_recheck.mga_recheck.proof_present = false;
  auto selection = api::SelectLocalNoSqlPhysicalProvider(missing_mga_recheck);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 missing MGA recheck proof did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderMgaRecheckProofMissing);

  auto no_row_recheck = BaseContract();
  no_row_recheck.mga_recheck.row_mga_recheck_required = false;
  selection = api::SelectLocalNoSqlPhysicalProvider(no_row_recheck);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 provider was allowed to suppress row MGA recheck");
  RequireDiagnostic(selection, api::kNoSqlProviderRowMgaRecheckRequired);

  auto no_security_recheck = BaseContract();
  no_security_recheck.mga_recheck.row_security_recheck_required = false;
  selection = api::SelectLocalNoSqlPhysicalProvider(no_security_recheck);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 provider was allowed to suppress security recheck");
  RequireDiagnostic(selection, api::kNoSqlProviderSecurityRecheckRequired);
}

void ParserWriteAheadAndBehaviorScanAuthorityAreRefused() {
  auto provider_finality = BaseContract();
  provider_finality.mga_recheck.provider_claims_transaction_finality_authority = true;
  auto selection = api::SelectLocalNoSqlPhysicalProvider(provider_finality);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 provider finality authority claim did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderFinalityAuthorityRefused);

  auto parser_finality = BaseContract();
  parser_finality.mga_recheck.parser_claims_transaction_finality_authority = true;
  selection = api::SelectLocalNoSqlPhysicalProvider(parser_finality);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 parser finality authority claim did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderParserFinalityAuthorityRefused);

  auto write_ahead_finality = BaseContract();
  write_ahead_finality.mga_recheck.write_ahead_log_claims_transaction_finality_authority = true;
  selection = api::SelectLocalNoSqlPhysicalProvider(write_ahead_finality);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 write-ahead finality authority claim did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);

  auto behavior_scan = BaseContract();
  behavior_scan.descriptor_visibility.behavior_store_scan_selected = true;
  selection = api::SelectLocalNoSqlPhysicalProvider(behavior_scan);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 behavior-store scan was accepted as a physical provider");
  RequireDiagnostic(selection, api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
}

void ClusterOrDistributedRequirementsFailClosedLocally() {
  auto cluster = BaseContract();
  cluster.scope = api::EngineNoSqlProviderScope::kClusterOnly;
  auto selection = api::SelectLocalNoSqlPhysicalProvider(cluster);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 cluster-only provider did not fail closed locally");
  RequireDiagnostic(selection, api::kNoSqlProviderClusterScopeRefusedLocalOnly);

  auto distributed = BaseContract();
  distributed.scope = api::EngineNoSqlProviderScope::kDistributed;
  selection = api::SelectLocalNoSqlPhysicalProvider(distributed);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 distributed provider did not fail closed locally");
  RequireDiagnostic(selection, api::kNoSqlProviderDistributedScopeRefusedLocalOnly);
}

void UnknownProviderFamilyFailsClosed() {
  auto contract = BaseContract();
  contract.family = api::EngineNoSqlProviderFamily::kUnknown;
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(!selection.selected && selection.fail_closed,
          "ODF-070 unknown provider family did not fail closed");
  RequireDiagnostic(selection, api::kNoSqlProviderFamilyUnsupported);

  const auto candidate = opt::PlanNoSqlPhysicalProviderCandidate(contract);
  Require(!candidate.cost.selectable,
          "ODF-070 optimizer candidate selected unknown provider family");
  Require(candidate.access_kind == plan::PhysicalAccessKind::kNone,
          "ODF-070 unknown provider family should not map to a physical access");
  Require(candidate.cost.rejection_reason == api::kNoSqlProviderFamilyUnsupported,
          "ODF-070 unknown provider family diagnostic changed");
}

}  // namespace

int main() {
  SuccessfulLocalProviderSelectionWithAllProofs();
  OptimizerUsesProviderContractAndDoesNotSelectScanFallbacks();
  MissingDescriptorVisibilityFailsClosed();
  MissingSecurityProofFailsClosed();
  MissingOrStaleIndexGenerationFailsClosed();
  MissingDeltaOverlayProofFailsClosedWhenRequired();
  RecheckAndAuthorityRequirementsRemainExplicit();
  ParserWriteAheadAndBehaviorScanAuthorityAreRefused();
  ClusterOrDistributedRequirementsFailClosedLocally();
  UnknownProviderFamilyFailsClosed();
  return 0;
}
