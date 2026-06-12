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
        static_cast<std::uint8_t>((seed + (index * 29u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

struct ControlDefinition {
  engine::RemainingDetailClosureControlKind kind;
  std::string rdc_id;
  std::string spec_path;
  std::string owner;
  std::vector<std::string> gate_keys;
};

std::vector<std::string> RequiredDiagnostics() {
  return {"RDC.CONTROL_RECORD_MISSING",
          "RDC.CONTROL_IDENTITY_INCOMPLETE",
          "RDC.SURFACE_RECORD_MISSING",
          "RDC.SURFACE_INCOMPLETE",
          "RDC.GATE_RECORD_MISSING",
          "RDC.GATE_EVIDENCE_INCOMPLETE",
          "RDC.PARSER_AUTHORITY_VIOLATION",
          "RDC.SECURITY_POLICY_MISSING",
          "RDC.RESOURCE_INVALIDATION_MISSING",
          "RDC.SILENT_FALLBACK_ALLOWED",
          "RDC.EXAMPLE_AUTHORITY_VIOLATION"};
}

std::vector<std::string> RequiredMetrics() {
  return {"sys.metrics.remaining_detail_closure.control_count",
          "sys.metrics.remaining_detail_closure.surface_count",
          "sys.metrics.remaining_detail_closure.gate_count",
          "sys.metrics.remaining_detail_closure.control_missing_total",
          "sys.metrics.remaining_detail_closure.surface_missing_total",
          "sys.metrics.remaining_detail_closure.surface_incomplete_total",
          "sys.metrics.remaining_detail_closure.gate_missing_total",
          "sys.metrics.remaining_detail_closure.gate_evidence_incomplete_total",
          "sys.metrics.remaining_detail_closure.parser_authority_violation_total",
          "sys.metrics.remaining_detail_closure.security_policy_missing_total",
          "sys.metrics.remaining_detail_closure.resource_invalidation_missing_total",
          "sys.metrics.remaining_detail_closure.silent_fallback_allowed_total",
          "sys.metrics.remaining_detail_closure.example_authority_violation_total"};
}

std::vector<engine::RemainingDetailClosureSurfaceKind> AllSurfaceKinds() {
  using Kind = engine::RemainingDetailClosureSurfaceKind;
  return {Kind::catalog_schema,
          Kind::privilege_policy,
          Kind::diagnostic_registry,
          Kind::resource_versioning,
          Kind::parser_sblr_lowering,
          Kind::reference_matrix_file,
          Kind::canonical_encoding_example,
          Kind::compatibility_mode,
          Kind::conformance_manifest,
          Kind::documentation_policy,
          Kind::system_catalog_table,
          Kind::formal_grammar,
          Kind::normative_encoding_example,
          Kind::manifest_inventory,
          Kind::reference_version_profile,
          Kind::commercial_gate,
          Kind::security,
          Kind::diagnostic,
          Kind::metrics,
          Kind::implementation_trace};
}

std::vector<ControlDefinition> ControlDefinitions() {
  using Kind = engine::RemainingDetailClosureControlKind;
  return {
      {Kind::system_catalog_schema,
       "RDC-001",
       "docs/specifications/chapters/catalog-schema/"
       "appendix-system-catalog-schema.md",
       "catalog",
       {"SCS-GATE-001", "SCS-GATE-002", "SCS-GATE-003",
        "SCS-GATE-004", "SCS-GATE-005"}},
      {Kind::type_security_privilege,
       "RDC-002",
       "docs/specifications/chapters/data-representation/datatypes/"
       "appendix-type-security-privilege-matrix.md",
       "security",
       {"TSP-GATE-001", "TSP-GATE-002", "TSP-GATE-003",
        "TSP-GATE-004"}},
      {Kind::diagnostic_registry,
       "RDC-003",
       "docs/specifications/chapters/implementation-guidance/"
       "appendix-diagnostic-error-code-registry.md",
       "diagnostics",
       {"DER-GATE-001", "DER-GATE-002", "DER-GATE-003",
        "DER-GATE-004", "DER-GATE-005", "DER-GATE-006",
        "DER-GATE-007", "DER-GATE-008", "DER-GATE-009",
        "DER-GATE-010", "DER-GATE-011", "DER-GATE-012",
        "DER-GATE-013"}},
      {Kind::resource_registry_versioning,
       "RDC-004",
       "docs/specifications/chapters/core/"
       "appendix-resource-registry-versioning.md",
       "resource_registry",
       {"RRV-GATE-001", "RRV-GATE-002", "RRV-GATE-003",
        "RRV-GATE-004", "RRV-CONF-001", "RRV-CONF-002",
        "RRV-CONF-003", "RRV-CONF-004", "RRV-CONF-005",
        "RRV-CONF-006", "RRV-CONF-007", "RRV-CONF-008",
        "RRV-CONF-009", "RRV-CONF-010", "RRV-CONF-011",
        "RRV-CONF-012"}},
      {Kind::parser_sblr_grammar_expansion,
       "RDC-005",
       "docs/specifications/chapters/parser-v3/sblr-lowering/"
       "appendix-parser-sblr-grammar-expansion-tracker.md",
       "parser_sblr",
       {"PSG-GATE-001", "PSG-GATE-002", "PSG-GATE-003"}},
      {Kind::reference_specific_type_matrices,
       "RDC-006",
       "docs/specifications/chapters/references/common/"
       "appendix-reference-specific-type-matrices.md",
       "reference_compatibility",
       {"DSM-GATE-001", "DSM-GATE-002", "DSM-GATE-003",
        "DSM-GATE-004", "DSM-GATE-005", "DSM-GATE-006",
        "DSM-GATE-007", "DSM-GATE-008", "DSM-GATE-009",
        "DSM-GATE-010", "DSM-GATE-011", "DSM-GATE-012",
        "DSM-GATE-013"}},
      {Kind::canonical_encoding_examples,
       "RDC-007",
       "docs/specifications/chapters/storage/physical-encoding/"
       "appendix-canonical-encoding-examples.md",
       "storage_encoding",
       {"CEE-GATE-001", "CEE-CONF-001", "CEE-CONF-002",
        "CEE-CONF-003", "CEE-CONF-004", "CEE-CONF-005",
        "CEE-CONF-006", "CEE-CONF-007"}},
      {Kind::compatibility_mode_matrix,
       "RDC-008",
       "docs/specifications/chapters/core/"
       "appendix-compatibility-mode-matrix.md",
       "compatibility",
       {"CMM-GATE-001", "CMM-GATE-002", "CMM-GATE-003",
        "CMM-GATE-004", "CMM-GATE-005", "CMM-GATE-006",
        "CMM-GATE-007", "CMM-GATE-008", "CMM-GATE-009",
        "CMM-GATE-010", "CMM-GATE-011", "CMM-GATE-012",
        "CMM-GATE-013"}},
      {Kind::conformance_test_manifest_format,
       "RDC-009",
       "docs/specifications/chapters/implementation-guidance/"
       "appendix-conformance-test-manifest-format.md",
       "conformance",
       {"CTM-GATE-001", "CTM-GATE-002", "CTM-GATE-003",
        "CTM-GATE-004", "CTM-GATE-005", "CTM-GATE-006",
        "CTM-GATE-007", "CTM-GATE-008", "CTM-GATE-009",
        "CTM-GATE-010", "CTM-GATE-011", "CTM-GATE-012",
        "CTM-GATE-013"}},
      {Kind::documentation_examples_policy,
       "RDC-010",
       "docs/specifications/chapters/implementation-guidance/"
       "appendix-documentation-examples-policy.md",
       "documentation",
       {"DEP-GATE-001", "DEP-GATE-002", "DEP-GATE-003",
        "DEP-GATE-004", "DEP-GATE-005", "DEP-GATE-006",
        "DEP-GATE-007", "DEP-GATE-008"}},
      {Kind::system_catalog_table_definitions,
       "RDC-011",
       "docs/specifications/chapters/catalog-schema/"
       "appendix-system-catalog-table-definitions.md",
       "catalog",
       {"SCT-GATE-001", "SCT-GATE-002", "SCT-GATE-003",
        "SCT-GATE-004"}},
      {Kind::parser_sblr_formal_grammar,
       "RDC-012",
       "docs/specifications/chapters/parser-v3/sblr-lowering/"
       "appendix-parser-sblr-formal-grammar.md",
       "parser_sblr",
       {"PSF-GATE-001", "PSF-GATE-002", "PSF-GATE-003",
        "PSF-GATE-004", "PSF-CONF-001", "PSF-CONF-002",
        "PSF-CONF-003", "PSF-CONF-004", "PSF-CONF-005",
        "PSF-CONF-006", "PSF-CONF-007", "PSF-CONF-008",
        "PSF-CONF-009", "PSF-CONF-010", "PSF-CONF-011",
        "PSF-CONF-012"}},
      {Kind::normative_encoding_examples,
       "RDC-013",
       "docs/specifications/chapters/storage/physical-encoding/"
       "appendix-normative-encoding-examples.md",
       "storage_encoding",
       {"NEE-GATE-001", "NEE-GATE-002", "NEE-GATE-003",
        "NEE-GATE-004", "NEE-GATE-005"}},
      {Kind::conformance_manifest_inventory,
       "RDC-014",
       "docs/specifications/chapters/implementation-guidance/"
       "appendix-conformance-manifest-inventory.md",
       "conformance",
       {"CMI-GATE-001", "CMI-GATE-002", "CMI-GATE-003",
        "CMI-GATE-004", "CMI-GATE-005", "CMI-GATE-006",
        "CMI-GATE-007", "CMI-GATE-008", "CMI-GATE-009",
        "CMI-GATE-010", "CMI-GATE-011", "CMI-GATE-012"}},
      {Kind::reference_version_profile_closure,
       "RDC-015",
       "docs/specifications/chapters/references/common/"
       "appendix-reference-version-profile-closure.md",
       "reference_compatibility",
       {"DVP-GATE-001", "DVP-GATE-002", "DVP-GATE-003",
        "DVP-GATE-004", "DVP-GATE-005", "DVP-GATE-006",
        "DVP-GATE-007", "DVP-GATE-008", "DVP-GATE-009",
        "DVP-GATE-010"}},
      {Kind::commercial_grade_completion_gates,
       "RDC-016",
       "docs/specifications/chapters/implementation-guidance/"
       "appendix-commercial-grade-completion-gates.md",
       "commercial_readiness",
       {"CGC-GATE-001", "CGC-GATE-002", "CGC-GATE-003",
        "CGC-GATE-004", "CGC-GATE-005", "CGC-GATE-006",
        "CGC-GATE-007", "CGC-GATE-008", "CGC-GATE-009",
        "CGC-GATE-010", "CGC-GATE-011", "CGC-GATE-012",
        "CGC-GATE-013", "CGC-GATE-014", "CGC-GATE-015",
        "CGC-GATE-016", "CGC-GATE-017", "CGC-GATE-018",
        "CGC-GATE-019", "CGC-GATE-020", "CGC-GATE-021",
        "CGC-GATE-022", "CGC-GATE-023", "CGC-GATE-024",
        "CGC-GATE-025"}}};
}

void AddControl(engine::RemainingDetailClosureRegistry& registry,
                const ControlDefinition& definition,
                std::uint16_t& seed) {
  engine::RemainingDetailClosureControlRecord control;
  control.control_uuid = Uuid(seed++);
  control.rdc_id = definition.rdc_id;
  control.control_kind = definition.kind;
  control.control_search_key = std::string(
      engine::RemainingDetailClosureControlSearchKey(definition.kind));
  control.controlling_spec_path = definition.spec_path;
  control.controlling_spec_hash = "spec." + definition.rdc_id;
  control.owner_subsystem = definition.owner;
  control.status = engine::RemainingDetailClosureControlStatus::complete;
  control.required_gate_keys = definition.gate_keys;
  control.implementation_trace_uuid = Uuid(seed++);
  control.conformance_manifest_hash = "manifest." + definition.rdc_id;
  control.diagnostic_code = "RDC.CONTROL.OK";
  control.control_hash = "control." + definition.rdc_id;
  const auto control_uuid = control.control_uuid;
  registry.controls.push_back(control);

  for (const auto surface_kind : AllSurfaceKinds()) {
    if (!engine::RemainingDetailClosureSurfaceRequiredForControl(
            definition.kind, surface_kind)) {
      continue;
    }
    engine::RemainingDetailClosureSurfaceRecord surface;
    surface.surface_uuid = Uuid(seed++);
    surface.control_uuid = control_uuid;
    surface.surface_kind = surface_kind;
    surface.surface_status =
        engine::RemainingDetailClosureSurfaceStatus::complete;
    surface.owning_search_key =
        std::string(engine::RemainingDetailClosureControlSearchKey(
            definition.kind));
    surface.evidence_hash = "surface.evidence." + definition.rdc_id;
    surface.failure_diagnostic_code = "RDC.SURFACE_RECORD_MISSING";
    surface.surface_hash = "surface." + definition.rdc_id;
    registry.surfaces.push_back(surface);
  }

  for (const auto& gate_key : definition.gate_keys) {
    engine::RemainingDetailClosureGateRecord gate;
    gate.gate_uuid = Uuid(seed++);
    gate.control_uuid = control_uuid;
    gate.gate_key = gate_key;
    gate.gate_status = engine::RemainingDetailClosureGateStatus::go;
    gate.required_evidence_hash = "gate.evidence." + gate_key;
    gate.provided_evidence_hash = gate.required_evidence_hash;
    gate.implementation_trace_uuid = Uuid(seed++);
    gate.diagnostic_code = "RDC.GATE.OK";
    gate.gate_hash = "gate." + gate_key;
    registry.gates.push_back(gate);
  }
}

engine::RemainingDetailClosureRegistry ValidRegistry() {
  engine::RemainingDetailClosureRegistry registry;
  std::uint16_t seed = 1000;
  registry.registry_uuid = Uuid(seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "remaining_detail_closure_registry";
  registry.root_search_key = "RDC-REMAINING-DETAIL-CLOSURE-CONTROLS";
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();
  for (const auto& definition : ControlDefinitions()) {
    AddControl(registry, definition, seed);
  }
  return registry;
}

void RequireStatus(const engine::RemainingDetailClosureRegistry& registry,
                   engine::RemainingDetailClosureStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateRemainingDetailClosureRegistry(registry);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::RemainingDetailClosureStatusName(expected)
              << " actual="
              << engine::RemainingDetailClosureStatusName(result.status)
              << '\n';
    Fail("RDC closure status mismatch");
  }
}

void TestValidRegistryCoversAllRdcControls() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateRemainingDetailClosureRegistry(registry).ok(),
          "RDC rejected valid remaining detail closure registry");
  Require(registry.controls.size() == 16,
          "RDC did not register all remaining detail controls");
  Require(registry.gates.size() > 150,
          "RDC did not register expanded appendix gates");
  Require(engine::RemainingDetailClosureStatusName(
              engine::RemainingDetailClosureStatus::ok) == "ok",
          "RDC status names are not stable");
}

void TestControlSurfaceAndGateFailures() {
  auto registry = ValidRegistry();
  registry.controls.pop_back();
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::control_record_missing,
                "RDC accepted missing control record");

  registry = ValidRegistry();
  registry.controls[0].control_search_key = "RDC-WRONG";
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::
                    control_identity_incomplete,
                "RDC accepted wrong control search key");

  registry = ValidRegistry();
  registry.surfaces.erase(registry.surfaces.begin());
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::surface_record_missing,
                "RDC accepted missing control surface");

  registry = ValidRegistry();
  registry.surfaces[0].surface_status =
      engine::RemainingDetailClosureSurfaceStatus::partial;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::surface_incomplete,
                "RDC accepted incomplete control surface");

  registry = ValidRegistry();
  registry.gates.erase(registry.gates.begin());
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::gate_record_missing,
                "RDC accepted missing required gate");

  registry = ValidRegistry();
  registry.gates[0].provided_evidence_hash = "mismatch";
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::gate_evidence_incomplete,
                "RDC accepted gate with incomplete evidence");
}

void TestAuthorityPolicyDiagnosticAndMetricFailures() {
  auto registry = ValidRegistry();
  registry.controls[4].reference_sql_not_engine_authority = false;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::
                    parser_authority_violation,
                "RDC accepted reference SQL as engine authority");

  registry = ValidRegistry();
  registry.controls[1].security_policy_enforced = false;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::security_policy_missing,
                "RDC accepted missing security policy enforcement");

  registry = ValidRegistry();
  registry.controls[3].resource_invalidation_declared = false;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::
                    resource_invalidation_missing,
                "RDC accepted missing resource invalidation declaration");

  registry = ValidRegistry();
  registry.controls[7].silent_fallback_forbidden = false;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::silent_fallback_allowed,
                "RDC accepted silent compatibility fallback");

  registry = ValidRegistry();
  registry.controls[9].examples_non_authoritative = false;
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::
                    example_authority_violation,
                "RDC accepted documentation example authority override");

  registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::
                    diagnostic_vector_missing,
                "RDC accepted missing diagnostic code");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireStatus(registry,
                engine::RemainingDetailClosureStatus::local_metric_missing,
                "RDC accepted missing metric");
}

}  // namespace

int main() {
  TestValidRegistryCoversAllRdcControls();
  TestControlSurfaceAndGateFailures();
  TestAuthorityPolicyDiagnosticAndMetricFailures();
  std::cout << "remaining_detail_closure_controls_conformance=passed\n";
  return EXIT_SUCCESS;
}
