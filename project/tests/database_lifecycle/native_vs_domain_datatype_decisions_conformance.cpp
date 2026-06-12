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
        static_cast<std::uint8_t>((seed + (index * 53u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

std::vector<std::string> RequiredNativeFamilies() {
  return {"boolean",
          "signed_integer",
          "unsigned_integer",
          "exact_numeric_integer_foundation",
          "decimal_floating",
          "approximate_numeric",
          "text",
          "binary_string",
          "bit_string",
          "temporal",
          "interval_duration",
          "uuid_guid",
          "network_address",
          "lob_oversized_value",
          "array_list",
          "map_dictionary",
          "row_composite_struct",
          "variant_tagged_union",
          "range_multirange",
          "document",
          "full_text_search",
          "spatial",
          "vector_similarity",
          "graph",
          "time_series",
          "columnar_olap_wrapper",
          "aggregate_state",
          "approximate_sketch",
          "locator_envelope",
          "opaque_extension_envelope"};
}

std::vector<std::string> RequiredDiagnostics() {
  return {"SB_DIAG_DATATYPE_DECISION_MISSING",
          "SB_DIAG_DATATYPE_DECISION_INVALID",
          "SB_DIAG_DATATYPE_NATIVE_REQUIRED",
          "SB_DIAG_DATATYPE_DOMAIN_FORBIDDEN",
          "SB_DIAG_DATATYPE_DESCRIPTOR_MISSING",
          "SB_DIAG_DATATYPE_UNSUPPORTED_BY_VERSION",
          "SB_DIAG_DATATYPE_UNSUPPORTED_BY_POLICY",
          "SB_DIAG_DATATYPE_UNSUPPORTED_BY_PROFILE",
          "SB_DIAG_DATATYPE_CXX_UDR_REQUIRED",
          "SB_DIAG_DATATYPE_NON_CPP_UDR_FORBIDDEN",
          "SB_DIAG_DATATYPE_COMPOUND_ADDRESS_INVALID",
          "SB_DIAG_DATATYPE_FULL_SUPPORT_OVERCLAIM"};
}

std::vector<std::string> RequiredMetrics() {
  return {"sys.metrics.datatype.decisions.total",
          "sys.metrics.datatype.decisions.native_total",
          "sys.metrics.datatype.decisions.domain_total",
          "sys.metrics.datatype.decisions.deferred_total",
          "sys.metrics.datatype.decisions.unsupported_total",
          "sys.metrics.datatype.donor_mappings.total",
          "sys.metrics.datatype.donor_mappings.incomplete_total",
          "sys.metrics.datatype.diagnostics.unsupported_by_version_total",
          "sys.metrics.datatype.diagnostics.unsupported_by_policy_total",
          "sys.metrics.datatype.diagnostics.requires_cxx_udr_total",
          "sys.metrics.datatype.conformance.full_support_overclaim_total"};
}

std::vector<std::string> RequiredGates() {
  return {"NVD-GATE-001", "NVD-GATE-002", "NVD-GATE-003",
          "NVD-GATE-004", "NVD-CONF-001", "NVD-CONF-002",
          "NVD-CONF-003", "NVD-CONF-004", "NVD-CONF-005",
          "NVD-CONF-006", "NVD-CONF-007", "NVD-CONF-008",
          "NVD-CONF-009", "NVD-CONF-010", "NVD-CONF-011",
          "NVD-CONF-012", "NVD-CONF-013", "NVD-CONF-014",
          "NVD-CONF-015"};
}

void FillImplementationDescriptors(engine::DatatypeDecisionRecord& decision,
                                   std::uint16_t& uuid_seed) {
  decision.base_physical_encoding_uuid = Uuid(uuid_seed++);
  decision.in_page_representation_uuid = Uuid(uuid_seed++);
  decision.out_of_page_representation_uuid = Uuid(uuid_seed++);
  decision.out_of_page_possible = true;
  decision.comparison_profile_uuid = Uuid(uuid_seed++);
  decision.casting_profile_uuid = Uuid(uuid_seed++);
  decision.operation_profile_uuid = Uuid(uuid_seed++);
  decision.statistics_profile_uuid = Uuid(uuid_seed++);
  decision.index_profile_uuid = Uuid(uuid_seed++);
  decision.driver_metadata_profile_uuid = Uuid(uuid_seed++);
  decision.backup_restore_profile_uuid = Uuid(uuid_seed++);
  decision.transport_profile_uuid = Uuid(uuid_seed++);
  decision.diagnostic_profile_uuid = Uuid(uuid_seed++);
}

engine::Uuid AddDecision(
    engine::NativeDomainDatatypeDecisionRegistry& registry,
    std::string family_name,
    engine::NativeDomainDecisionClass decision_class,
    std::uint16_t& uuid_seed) {
  engine::DatatypeDecisionRecord decision;
  decision.decision_uuid = Uuid(uuid_seed++);
  decision.scratchbird_family_name = family_name;
  decision.decision_class = decision_class;
  decision.native_substrate_family =
      engine::NativeDomainDecisionClassRequiresNativeSubstrate(decision_class)
          ? family_name
          : "";
  decision.donor_coverage_state =
      engine::NativeDomainDonorCoverageState::complete_for_claimed_donors;
  decision.conformance_status =
      engine::NativeDomainDecisionClassIsUnsupportedOrDeferred(decision_class)
          ? engine::NativeDomainConformanceStatus::deferred
          : engine::NativeDomainConformanceStatus::conformance_passed;
  decision.controlling_search_key =
      decision_class == engine::NativeDomainDecisionClass::domain_over_native
          ? "NVD-DOMAIN-ONLY-CRITERIA"
          : "NVD-NATIVE-PROMOTION-CRITERIA";
  decision.native_promotion_criteria_satisfied =
      decision_class == engine::NativeDomainDecisionClass::native_substrate;
  decision.domain_only_justification_present =
      decision_class == engine::NativeDomainDecisionClass::domain_over_native;
  decision.compound_element_addressing_present =
      decision_class == engine::NativeDomainDecisionClass::compound_domain;
  decision.opaque_lifecycle_policy_present =
      decision_class == engine::NativeDomainDecisionClass::opaque_domain;
  decision.locator_authority_policy_present =
      decision_class == engine::NativeDomainDecisionClass::locator_domain;
  decision.render_only_execution_forbidden =
      decision_class ==
      engine::NativeDomainDecisionClass::render_only_metadata;
  decision.non_cpp_udr_forbidden = true;
  decision.parser_name_not_identity = true;
  decision.implementation_trace_anchor_present = true;
  decision.unsupported_reason =
      engine::NativeDomainDecisionClassIsUnsupportedOrDeferred(decision_class)
          ? "unsupported or deferred by release profile"
          : "";
  decision.decision_hash = "decision." + family_name;
  if (!engine::NativeDomainDecisionClassIsUnsupportedOrDeferred(
          decision_class)) {
    FillImplementationDescriptors(decision, uuid_seed);
  } else {
    decision.diagnostic_profile_uuid = Uuid(uuid_seed++);
  }
  if (decision_class == engine::NativeDomainDecisionClass::cxx_udr_bridge) {
    decision.cxx_udr_package_uuid = Uuid(uuid_seed++);
  }
  const auto decision_uuid = decision.decision_uuid;
  registry.decisions.push_back(decision);
  return decision_uuid;
}

void AddMapping(engine::NativeDomainDatatypeDecisionRegistry& registry,
                engine::Uuid decision_uuid,
                engine::NativeDomainDonorFamily donor_family,
                std::string donor_type_name,
                engine::NativeDomainDonorTypeCategory category,
                std::uint16_t& uuid_seed,
                bool full_support_claimed = false,
                engine::NativeDomainUnsupportedBehavior unsupported_behavior =
                    engine::NativeDomainUnsupportedBehavior::none) {
  engine::DonorDatatypeMappingRecord mapping;
  mapping.mapping_uuid = Uuid(uuid_seed++);
  mapping.donor_family = donor_family;
  mapping.donor_version_range = "all-targeted";
  mapping.donor_type_name = donor_type_name;
  mapping.donor_type_category = category;
  mapping.scratchbird_decision_uuid = decision_uuid;
  mapping.literal_policy_uuid = Uuid(uuid_seed++);
  mapping.bind_policy_uuid = Uuid(uuid_seed++);
  mapping.cast_policy_uuid = Uuid(uuid_seed++);
  mapping.operation_policy_uuid = Uuid(uuid_seed++);
  mapping.index_statistics_policy_uuid = Uuid(uuid_seed++);
  mapping.storage_policy_uuid = Uuid(uuid_seed++);
  mapping.transport_policy_uuid = Uuid(uuid_seed++);
  mapping.diagnostic_policy_uuid = Uuid(uuid_seed++);
  mapping.unsupported_behavior = unsupported_behavior;
  mapping.conformance_test_id = "NVD-CONF-" + donor_type_name;
  mapping.parser_accepts_type = true;
  mapping.full_support_claimed = full_support_claimed;
  mapping.mapping_hash = "mapping." + donor_type_name;
  registry.donor_mappings.push_back(mapping);
}

void AddUnsupportedMapping(
    engine::NativeDomainDatatypeDecisionRegistry& registry,
    engine::Uuid decision_uuid,
    std::string donor_type_name,
    engine::NativeDomainUnsupportedBehavior unsupported_behavior,
    std::uint16_t& uuid_seed) {
  engine::DonorDatatypeMappingRecord mapping;
  mapping.mapping_uuid = Uuid(uuid_seed++);
  mapping.donor_family = engine::NativeDomainDonorFamily::oracle;
  mapping.donor_version_range = "current-release";
  mapping.donor_type_name = donor_type_name;
  mapping.donor_type_category =
      engine::NativeDomainDonorTypeCategory::unsupported;
  mapping.scratchbird_decision_uuid = decision_uuid;
  mapping.diagnostic_policy_uuid = Uuid(uuid_seed++);
  mapping.unsupported_behavior = unsupported_behavior;
  mapping.conformance_test_id = "NVD-CONF-" + donor_type_name;
  mapping.parser_accepts_type = true;
  mapping.mapping_hash = "mapping." + donor_type_name;
  registry.donor_mappings.push_back(mapping);
}

void AddCompoundElement(
    engine::NativeDomainDatatypeDecisionRegistry& registry,
    engine::Uuid decision_uuid,
    std::uint16_t& uuid_seed) {
  engine::CompoundDomainElementRecord element;
  element.element_uuid = Uuid(uuid_seed++);
  element.decision_uuid = decision_uuid;
  element.element_name = "field_a";
  element.element_ordinal = 1;
  element.path_expression = "$.field_a";
  element.nullability_policy = "nullable-by-domain-field";
  element.cast_rule_uuid = Uuid(uuid_seed++);
  element.operation_rule_uuid = Uuid(uuid_seed++);
  element.versioning_rule = "shape change creates new domain version";
  element.parser_name_not_identity = true;
  element.element_hash = "compound.field_a";
  registry.compound_elements.push_back(element);
}

void AddFullSupportClaim(
    engine::NativeDomainDatatypeDecisionRegistry& registry,
    std::uint16_t& uuid_seed) {
  engine::DonorFamilyFullSupportClaimRecord claim;
  claim.claim_uuid = Uuid(uuid_seed++);
  claim.donor_family = engine::NativeDomainDonorFamily::postgresql;
  claim.expected_type_row_count = 1;
  claim.mapped_type_row_count = 1;
  claim.conformance_manifest_hash = "postgresql.full.support.rows";
  claim.full_support_claimed = true;
  claim.claim_hash = "claim.postgresql";
  registry.full_support_claims.push_back(claim);
}

void AddConformanceGates(
    engine::NativeDomainDatatypeDecisionRegistry& registry,
    std::uint16_t& uuid_seed) {
  for (const auto& gate_id : RequiredGates()) {
    engine::NativeDomainConformanceGateRecord gate;
    gate.gate_uuid = Uuid(uuid_seed++);
    gate.gate_id = gate_id;
    gate.status = engine::NativeDomainGateStatus::passed;
    gate.evidence_hash = "evidence." + gate_id;
    gate.ctest_name = "database_lifecycle_native_vs_domain_datatype_"
                      "decisions_conformance";
    gate.diagnostic_code = "SB_DIAG_DATATYPE_DECISION_INVALID";
    gate.gate_hash = "gate." + gate_id;
    registry.conformance_gates.push_back(gate);
  }
}

engine::NativeDomainDatatypeDecisionRegistry ValidRegistry() {
  engine::NativeDomainDatatypeDecisionRegistry registry;
  std::uint16_t uuid_seed = 4000;
  registry.registry_uuid = Uuid(uuid_seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "native_vs_domain_datatype_decisions";
  registry.root_search_key = "NVD-NATIVE-VS-DOMAIN-DATATYPE-DECISIONS";
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();

  for (const auto& family_name : RequiredNativeFamilies()) {
    const auto decision_uuid =
        AddDecision(registry, family_name,
                    engine::NativeDomainDecisionClass::native_substrate,
                    uuid_seed);
    AddMapping(registry, decision_uuid,
               engine::NativeDomainDonorFamily::postgresql,
               "pg_" + family_name,
               engine::NativeDomainDonorTypeCategory::scalar, uuid_seed,
               family_name == "boolean");
  }

  const auto domain_uuid =
      AddDecision(registry, "money_currency_domain",
                  engine::NativeDomainDecisionClass::domain_over_native,
                  uuid_seed);
  AddMapping(registry, domain_uuid,
             engine::NativeDomainDonorFamily::postgresql, "money",
             engine::NativeDomainDonorTypeCategory::scalar, uuid_seed);

  const auto compound_uuid =
      AddDecision(registry, "postgresql_composite_domain",
                  engine::NativeDomainDecisionClass::compound_domain,
                  uuid_seed);
  AddCompoundElement(registry, compound_uuid, uuid_seed);
  AddMapping(registry, compound_uuid,
             engine::NativeDomainDonorFamily::postgresql, "composite",
             engine::NativeDomainDonorTypeCategory::compound, uuid_seed);

  const auto opaque_uuid =
      AddDecision(registry, "opaque_extension_domain",
                  engine::NativeDomainDecisionClass::opaque_domain,
                  uuid_seed);
  AddMapping(registry, opaque_uuid, engine::NativeDomainDonorFamily::redis,
             "module_value", engine::NativeDomainDonorTypeCategory::opaque,
             uuid_seed);

  const auto locator_uuid =
      AddDecision(registry, "oracle_bfile_locator",
                  engine::NativeDomainDecisionClass::locator_domain,
                  uuid_seed);
  AddMapping(registry, locator_uuid, engine::NativeDomainDonorFamily::oracle,
             "BFILE", engine::NativeDomainDonorTypeCategory::locator,
             uuid_seed);

  const auto bridge_uuid =
      AddDecision(registry, "redis_module_cxx_bridge",
                  engine::NativeDomainDecisionClass::cxx_udr_bridge,
                  uuid_seed);
  AddMapping(registry, bridge_uuid, engine::NativeDomainDonorFamily::redis,
             "trusted_module_value",
             engine::NativeDomainDonorTypeCategory::extension, uuid_seed);

  const auto render_uuid =
      AddDecision(registry, "rowid_render_only_metadata",
                  engine::NativeDomainDecisionClass::render_only_metadata,
                  uuid_seed);
  AddMapping(registry, render_uuid,
             engine::NativeDomainDonorFamily::firebird, "DB_KEY",
             engine::NativeDomainDonorTypeCategory::pseudo, uuid_seed);

  const auto deferred_uuid =
      AddDecision(registry, "future_donor_type",
                  engine::NativeDomainDecisionClass::version_deferred,
                  uuid_seed);
  AddUnsupportedMapping(
      registry, deferred_uuid, "future_type",
      engine::NativeDomainUnsupportedBehavior::unsupported_by_version,
      uuid_seed);

  const auto unsupported_uuid =
      AddDecision(registry, "unsafe_javascript_type",
                  engine::NativeDomainDecisionClass::policy_unsupported,
                  uuid_seed);
  registry.decisions.back().conformance_status =
      engine::NativeDomainConformanceStatus::unsupported;
  AddUnsupportedMapping(
      registry, unsupported_uuid, "javascript",
      engine::NativeDomainUnsupportedBehavior::unsupported_by_policy,
      uuid_seed);

  AddFullSupportClaim(registry, uuid_seed);
  AddConformanceGates(registry, uuid_seed);
  return registry;
}

void RequireStatus(
    const engine::NativeDomainDatatypeDecisionRegistry& registry,
    engine::NativeDomainDatatypeDecisionStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateNativeDomainDatatypeDecisionRegistry(registry);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::NativeDomainDatatypeDecisionStatusName(expected)
              << " actual="
              << engine::NativeDomainDatatypeDecisionStatusName(result.status)
              << '\n';
    Fail("native/domain datatype decision status mismatch");
  }
}

void TestValidRegistryCoversAllNvdGates() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateNativeDomainDatatypeDecisionRegistry(registry).ok(),
          "NVD rejected valid native/domain decision registry");
  Require(registry.decisions.size() >= RequiredNativeFamilies().size(),
          "NVD did not register required native substrate families");
  Require(engine::NativeDomainDatatypeDecisionStatusName(
              engine::NativeDomainDatatypeDecisionStatus::ok) == "ok",
          "NVD status names are not stable");
}

void TestDecisionFailures() {
  auto registry = ValidRegistry();
  registry.decisions.erase(registry.decisions.begin());
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    decision_record_missing,
                "NVD accepted missing required native decision");

  registry = ValidRegistry();
  registry.decisions[0].native_promotion_criteria_satisfied = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    native_promotion_missing,
                "NVD accepted native substrate without promotion criteria");

  registry = ValidRegistry();
  registry.decisions[30].domain_only_justification_present = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    domain_justification_missing,
                "NVD accepted domain-only decision without justification");

  registry = ValidRegistry();
  registry.decisions[0].base_physical_encoding_uuid = {};
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::descriptor_missing,
                "NVD accepted implementation-ready decision without descriptor");
}

void TestCompoundOpaqueBridgeFailures() {
  auto registry = ValidRegistry();
  registry.compound_elements.clear();
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    compound_element_missing,
                "NVD accepted compound domain without element identity");

  registry = ValidRegistry();
  registry.decisions[32].opaque_lifecycle_policy_present = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    opaque_lifecycle_missing,
                "NVD accepted opaque domain without lifecycle policy");

  registry = ValidRegistry();
  registry.decisions[34].cxx_udr_package_uuid = {};
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    cxx_udr_package_missing,
                "NVD accepted bridge decision without C++ UDR package");

  registry = ValidRegistry();
  registry.decisions[34].non_cpp_udr_forbidden = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::non_cpp_udr_allowed,
                "NVD accepted non-C++ UDR bridge execution");
}

void TestMappingAndFullSupportFailures() {
  auto registry = ValidRegistry();
  registry.donor_mappings.erase(registry.donor_mappings.begin());
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    donor_mapping_missing,
                "NVD accepted decision without donor mapping");

  registry = ValidRegistry();
  registry.donor_mappings[0].literal_policy_uuid = {};
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    donor_mapping_policy_missing,
                "NVD accepted parser-visible donor mapping without policies");

  registry = ValidRegistry();
  registry.donor_mappings.back().unsupported_behavior =
      engine::NativeDomainUnsupportedBehavior::none;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    unsupported_behavior_missing,
                "NVD accepted unsupported mapping without exact behavior");

  registry = ValidRegistry();
  registry.full_support_claims[0].mapped_type_row_count = 0;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    full_support_overclaim,
                "NVD accepted donor full-support overclaim");
}

void TestGateDiagnosticMetricAuthorityFailures() {
  auto registry = ValidRegistry();
  registry.conformance_gates[0].status = engine::NativeDomainGateStatus::failed;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    conformance_gate_failed,
                "NVD accepted failed conformance gate");

  registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    diagnostic_vector_missing,
                "NVD accepted missing diagnostic vector");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    local_metric_missing,
                "NVD accepted missing local metric");

  registry = ValidRegistry();
  registry.donor_compatibility_not_authority = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    authority_invariant_violation,
                "NVD accepted donor compatibility as datatype authority");

  registry = ValidRegistry();
  registry.cluster_metrics_guarded_by_cluster_governance = false;
  RequireStatus(registry,
                engine::NativeDomainDatatypeDecisionStatus::
                    cluster_metric_guard_required,
                "NVD accepted cluster metrics without governance guard");
}

}  // namespace

int main() {
  TestValidRegistryCoversAllNvdGates();
  TestDecisionFailures();
  TestCompoundOpaqueBridgeFailures();
  TestMappingAndFullSupportFailures();
  TestGateDiagnosticMetricAuthorityFailures();
  std::cout << "native_vs_domain_datatype_decisions_conformance=passed\n";
  return EXIT_SUCCESS;
}
