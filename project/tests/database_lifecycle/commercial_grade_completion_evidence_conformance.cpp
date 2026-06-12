// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint16_t seed) {
  engine::Uuid uuid;
  uuid.bytes[0] = static_cast<std::uint8_t>(seed & 0xffu);
  uuid.bytes[1] = static_cast<std::uint8_t>((seed >> 8) & 0xffu);
  for (std::size_t index = 2; index < 16; ++index) {
    uuid.bytes[index] =
        static_cast<std::uint8_t>((seed + (index * 41u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

struct RequirementSeed {
  std::string key;
  std::string path;
  std::string search_key;
  std::string subsystem;
  engine::CommercialGradeRequirementKind kind;
};

std::vector<RequirementSeed> RequirementSeeds() {
  using Kind = engine::CommercialGradeRequirementKind;
  return {{"CGC-REQ-SCT",
           "docs/specifications/chapters/catalog-schema/"
           "appendix-system-catalog-table-definitions.md",
           "SCT-SYSTEM-CATALOG-TABLE-DEFINITIONS",
           "catalog",
           Kind::catalog_object},
          {"CGC-REQ-PSF",
           "docs/specifications/chapters/parser-v3/sblr-lowering/"
           "appendix-parser-sblr-formal-grammar.md",
           "PSF-PARSER-SBLR-FORMAL-GRAMMAR",
           "parser_sblr",
           Kind::algorithm},
          {"CGC-REQ-NEE",
           "docs/specifications/chapters/storage/physical-encoding/"
           "appendix-normative-encoding-examples.md",
           "NEE-NORMATIVE-ENCODING-EXAMPLES",
           "storage_encoding",
           Kind::example},
          {"CGC-REQ-CMI",
           "docs/specifications/chapters/implementation-guidance/"
           "appendix-conformance-manifest-inventory.md",
           "CMI-CONFORMANCE-MANIFEST-INVENTORY",
           "conformance",
           Kind::conformance_gate},
          {"CGC-REQ-DVP",
           "docs/specifications/chapters/references/common/"
           "appendix-reference-version-profile-closure.md",
           "DVP-REFERENCE-VERSION-PROFILE-CLOSURE",
           "reference_compatibility",
           Kind::record},
          {"CGC-REQ-CGC",
           "docs/specifications/chapters/implementation-guidance/"
           "appendix-commercial-grade-completion-gates.md",
           "CGC-COMMERCIAL-GRADE-COMPLETION-GATES",
           "commercial_readiness",
           Kind::security_rule}};
}

std::vector<std::string> RequiredDiagnostics() {
  return {"CGC.REQUIREMENT_RECORD_MISSING",
          "CGC.SEARCH_KEY_MISSING",
          "CGC.FIELD_COMPLETENESS_MISSING",
          "CGC.ALGORITHM_INCOMPLETE",
          "CGC.DECISION_BRANCH_MISSING",
          "CGC.DIAGNOSTIC_MISSING",
          "CGC.CATALOG_OR_METRIC_UNDEFINED",
          "CGC.CACHE_INVALIDATION_UNDEFINED",
          "CGC.INTERACTION_UNDEFINED",
          "CGC.CONTRADICTION_OPEN",
          "CGC.EXTERNAL_REFERENCE_UNCOPIED",
          "CGC.UNSUPPORTED_BEHAVIOR_UNDECLARED",
          "CGC.DEFERRED_RECORD_INCOMPLETE",
          "CGC.IMPLEMENTATION_MAPPING_MISSING",
          "CGC.CONFORMANCE_GATE_MISSING",
          "CGC.RELEASE_EVIDENCE_MISSING"};
}

std::vector<std::string> RequiredMetrics() {
  return {"sys.metrics.commercial_grade.requirement_count",
          "sys.metrics.commercial_grade.field_incomplete_count",
          "sys.metrics.commercial_grade.algorithm_incomplete_count",
          "sys.metrics.commercial_grade.decision_branch_missing_count",
          "sys.metrics.commercial_grade.diagnostic_missing_count",
          "sys.metrics.commercial_grade.catalog_metric_undefined_count",
          "sys.metrics.commercial_grade.cache_invalidation_missing_count",
          "sys.metrics.commercial_grade.open_contradiction_count",
          "sys.metrics.commercial_grade.external_reference_uncopied_count",
          "sys.metrics.commercial_grade.unsupported_undeclared_count",
          "sys.metrics.commercial_grade.implementation_mapping_missing_count",
          "sys.metrics.commercial_grade.conformance_gate_missing_count",
          "sys.metrics.commercial_grade.gate_decision_go_count",
          "sys.metrics.commercial_grade.gate_decision_no_go_count"};
}

void AddRequirement(
    engine::CommercialGradeCompletionEvidenceRegistry& registry,
    const RequirementSeed& seed,
    std::uint16_t& uuid_seed) {
  engine::CommercialGradeRequirementRecord requirement;
  requirement.requirement_uuid = Uuid(uuid_seed++);
  requirement.requirement_key = seed.key;
  requirement.source_spec_path = seed.path;
  requirement.source_search_key = seed.search_key;
  requirement.subsystem = seed.subsystem;
  requirement.requirement_kind = seed.kind;
  requirement.normative_text_hash = "normative." + seed.key;
  requirement.dependency_set_hash = "dependencies." + seed.key;
  requirement.status = engine::CommercialGradeCompletionLevel::release_candidate;
  requirement.diagnostic_profile_uuid = Uuid(uuid_seed++);
  requirement.conformance_gate_uuid = Uuid(uuid_seed++);
  requirement.record_hash = "requirement." + seed.key;
  const auto requirement_uuid = requirement.requirement_uuid;
  const auto gate_uuid = requirement.conformance_gate_uuid;
  registry.requirements.push_back(requirement);

  engine::CommercialGradeFieldCompletenessRecord field;
  field.field_record_uuid = Uuid(uuid_seed++);
  field.requirement_uuid = requirement_uuid;
  field.field_name = "status";
  field.field_type = "STRING";
  field.units = "none";
  field.valid_range = "stable identifier";
  field.default_value = "none";
  field.authority = seed.search_key;
  field.invalid_state_behavior = "reject";
  field.field_hash = "field." + seed.key;
  registry.fields.push_back(field);

  engine::CommercialGradeAlgorithmRecord algorithm;
  algorithm.algorithm_uuid = Uuid(uuid_seed++);
  algorithm.requirement_uuid = requirement_uuid;
  algorithm.algorithm_name = "validate_" + seed.key;
  algorithm.input_contract_hash = "input." + seed.key;
  algorithm.output_contract_hash = "output." + seed.key;
  algorithm.step_count = 4;
  algorithm.decision_branch_set_hash = "branches." + seed.key;
  algorithm.failure_diagnostic_set_hash = "diagnostics." + seed.key;
  algorithm.side_effect_set_hash = "empty-set";
  algorithm.algorithm_hash = "algorithm." + seed.key;
  registry.algorithms.push_back(algorithm);

  engine::CommercialGradeImplementationMappingRecord mapping;
  mapping.implementation_mapping_uuid = Uuid(uuid_seed++);
  mapping.requirement_uuid = requirement_uuid;
  mapping.implementation_path = "project/include/scratchbird/engine/value.hpp";
  mapping.implementation_search_key =
      "CGC-COMMERCIAL-GRADE-COMPLETION-GATES";
  mapping.implementation_status =
      engine::CommercialGradeImplementationStatus::verified;
  mapping.conformance_gate_uuid = gate_uuid;
  mapping.evidence_path =
      "docs/migration/commercial-grade-completion-audit-matrix.md";
  mapping.mapping_hash = "mapping." + seed.key;
  registry.mappings.push_back(mapping);
}

engine::CommercialGradeCompletionEvidenceRegistry ValidRegistry() {
  engine::CommercialGradeCompletionEvidenceRegistry registry;
  std::uint16_t uuid_seed = 2000;
  registry.registry_uuid = Uuid(uuid_seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "commercial_grade_completion_evidence";
  registry.root_search_key = "CGC-COMMERCIAL-GRADE-COMPLETION-GATES";
  registry.requested_level =
      engine::CommercialGradeCompletionLevel::release_candidate;
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();

  for (const auto& seed : RequirementSeeds()) {
    AddRequirement(registry, seed, uuid_seed);
  }

  for (std::size_t index = 1; index < registry.requirements.size(); ++index) {
    engine::CommercialGradeCrossSpecDependencyRecord dependency;
    dependency.dependency_uuid = Uuid(uuid_seed++);
    dependency.source_requirement_uuid =
        registry.requirements[index].requirement_uuid;
    dependency.target_requirement_uuid =
        registry.requirements[index - 1].requirement_uuid;
    dependency.dependency_kind =
        engine::CommercialGradeDependencyKind::must_not_conflict;
    dependency.resolution_state =
        engine::CommercialGradeDependencyResolution::resolved;
    dependency.resolution_search_key = "CGC-FINAL-CONTRADICTION-CHECKLIST";
    dependency.dependency_hash = "dependency." +
                                 registry.requirements[index].requirement_key;
    registry.dependencies.push_back(dependency);
  }

  engine::CommercialGradeContradictionRecord contradiction;
  contradiction.contradiction_uuid = Uuid(uuid_seed++);
  contradiction.first_requirement_uuid =
      registry.requirements[0].requirement_uuid;
  contradiction.second_requirement_uuid =
      registry.requirements[1].requirement_uuid;
  contradiction.contradiction_kind =
      engine::CommercialGradeContradictionKind::authority_conflict;
  contradiction.severity = engine::CommercialGradeContradictionSeverity::minor;
  contradiction.resolution_state =
      engine::CommercialGradeContradictionResolution::resolved;
  contradiction.resolution_search_key = "CGC-FINAL-CONTRADICTION-CHECKLIST";
  contradiction.record_hash = "contradiction.resolved";
  registry.contradictions.push_back(contradiction);

  engine::CommercialGradeGateDecisionRecord decision;
  decision.gate_decision_uuid = Uuid(uuid_seed++);
  decision.scope_uuid = Uuid(uuid_seed++);
  decision.scope_path =
      "docs/specifications/chapters/implementation-guidance/"
      "appendix-commercial-grade-completion-gates.md";
  decision.requested_level =
      engine::CommercialGradeCompletionLevel::release_candidate;
  decision.evaluated_requirement_set_hash = "requirements.cgc";
  decision.passed_gate_set_hash = "CGC-GATE-001..CGC-GATE-025";
  decision.failed_gate_set_hash = "empty-set";
  decision.blocking_contradiction_set_hash = "empty-set";
  decision.decision = engine::CommercialGradeGateDecision::go;
  decision.decision_rationale_search_key = "CGC-AUDIT-MATRIX";
  decision.decision_hash = "decision.cgc";
  registry.gate_decisions.push_back(decision);

  return registry;
}

void RequireStatus(
    const engine::CommercialGradeCompletionEvidenceRegistry& registry,
    engine::CommercialGradeCompletionStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateCommercialGradeCompletionEvidenceRegistry(registry);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::CommercialGradeCompletionStatusName(expected)
              << " actual="
              << engine::CommercialGradeCompletionStatusName(result.status)
              << '\n';
    Fail("CGC completion evidence status mismatch");
  }
}

void TestValidRegistryCoversCgcGates() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateCommercialGradeCompletionEvidenceRegistry(registry)
              .ok(),
          "CGC rejected valid completion evidence registry");
  Require(registry.requirements.size() == RequirementSeeds().size(),
          "CGC did not register all representative requirement families");
  Require(engine::CommercialGradeCompletionStatusName(
              engine::CommercialGradeCompletionStatus::ok) == "ok",
          "CGC status names are not stable");
}

void TestRequirementFieldAlgorithmFailures() {
  auto registry = ValidRegistry();
  registry.requirements[0].source_spec_path = "https://example.invalid/spec";
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    source_path_outside_private_docs,
                "CGC accepted external implementation authority");

  registry = ValidRegistry();
  registry.fields[0].units.clear();
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    field_identity_incomplete,
                "CGC accepted incomplete field record");

  registry = ValidRegistry();
  registry.fields.erase(registry.fields.begin());
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    field_completeness_missing,
                "CGC accepted requirement without field completeness");

  registry = ValidRegistry();
  registry.algorithms[0].step_count = 0;
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::algorithm_incomplete,
                "CGC accepted incomplete algorithm record");
}

void TestDependencyMappingEvidenceFailures() {
  auto registry = ValidRegistry();
  registry.dependencies[0].resolution_state =
      engine::CommercialGradeDependencyResolution::blocked;
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::dependency_unresolved,
                "CGC accepted unresolved cross-spec dependency");

  registry = ValidRegistry();
  registry.mappings.erase(registry.mappings.begin());
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    implementation_mapping_missing,
                "CGC accepted missing implementation mapping");

  registry = ValidRegistry();
  registry.requirements[0].conformance_gate_uuid = {};
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    conformance_gate_missing,
                "CGC accepted missing conformance gate UUID");

  registry = ValidRegistry();
  registry.mappings[0].evidence_path.clear();
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    release_evidence_missing,
                "CGC accepted release candidate without evidence path");
}

void TestContradictionDecisionInvariantDiagnosticMetricFailures() {
  auto registry = ValidRegistry();
  registry.contradictions[0].severity =
      engine::CommercialGradeContradictionSeverity::blocking;
  registry.contradictions[0].resolution_state =
      engine::CommercialGradeContradictionResolution::open;
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::contradiction_open,
                "CGC accepted open blocking contradiction");

  registry = ValidRegistry();
  registry.gate_decisions[0].failed_gate_set_hash = "CGC-GATE-024";
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::gate_decision_missing,
                "CGC accepted no-go gate decision for release candidate");

  registry = ValidRegistry();
  registry.mga_authority_preserved = false;
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    authority_invariant_violation,
                "CGC accepted broken MGA authority invariant");

  registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::
                    diagnostic_vector_missing,
                "CGC accepted missing diagnostic vector");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireStatus(registry,
                engine::CommercialGradeCompletionStatus::local_metric_missing,
                "CGC accepted missing commercial-grade metric");
}

}  // namespace

int main() {
  TestValidRegistryCoversCgcGates();
  TestRequirementFieldAlgorithmFailures();
  TestDependencyMappingEvidenceFailures();
  TestContradictionDecisionInvariantDiagnosticMetricFailures();
  std::cout << "commercial_grade_completion_evidence_conformance=passed\n";
  return EXIT_SUCCESS;
}
