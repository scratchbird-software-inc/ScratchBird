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

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor TypeDescriptor(std::uint8_t seed,
                                               std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 91;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 32;
  return descriptor;
}

engine::TypeOperationRegistryEntry ValidOperationEntry(
    const engine::Uuid& operation_uuid,
    const engine::ExecutionTypeDescriptor& descriptor) {
  engine::TypeOperationRegistryEntry entry;
  entry.operation_uuid = operation_uuid;
  entry.operation_family_uuid = Uuid(0x20);
  entry.schema_uuid = Uuid(0x21);
  entry.owning_package_uuid = Uuid(0x22);
  entry.name_ref_uuid = Uuid(0x23);
  entry.operation_epoch = 91;
  entry.schema_epoch = 91;
  entry.security_epoch = 94;
  entry.resource_epoch = 93;
  entry.implementation_version = 95;
  entry.stable_name = "tio.eq";
  entry.definition_hash = "tio.eq.definition";
  entry.diagnostic_search_key = "OPTIMIZER.TYPE_OPERATION_ADMISSION_REFUSED";
  entry.conformance_key = "TIO-GATE-007";
  entry.operation_kind = engine::TypeOperationKind::type_operator;

  entry.overload.overload_set_uuid = Uuid(0x24);
  entry.overload.overload_epoch = 91;
  entry.overload.overload_key = "i32.eq.i32";
  entry.overload.argument_descriptors.push_back(descriptor);
  entry.overload.argument_descriptor_uuids.push_back(
      descriptor.descriptor_uuid);
  entry.overload.result_descriptor = descriptor;
  entry.overload.result_descriptor_uuid = descriptor.descriptor_uuid;

  entry.sblr_binding.operation_uuid = entry.operation_uuid;
  entry.sblr_binding.operation_family_uuid = entry.operation_family_uuid;
  entry.sblr_binding.implementation_version = entry.implementation_version;
  entry.sblr_binding.argument_descriptor_uuids =
      entry.overload.argument_descriptor_uuids;
  entry.sblr_binding.argument_domain_uuids =
      entry.overload.argument_domain_uuids;
  entry.sblr_binding.result_descriptor_uuid =
      entry.overload.result_descriptor_uuid;
  entry.sblr_binding.result_domain_uuid = entry.overload.result_domain_uuid;
  entry.sblr_binding.schema_epoch = entry.schema_epoch;
  entry.sblr_binding.security_epoch = entry.security_epoch;
  entry.sblr_binding.resource_epoch = entry.resource_epoch;
  entry.sblr_binding.definition_hash = entry.definition_hash;
  entry.sblr_binding.diagnostic_search_key = entry.diagnostic_search_key;

  entry.cache_key.operation_uuid = entry.operation_uuid;
  entry.cache_key.operation_family_uuid = entry.operation_family_uuid;
  entry.cache_key.argument_descriptor_uuids =
      entry.overload.argument_descriptor_uuids;
  entry.cache_key.argument_domain_uuids = entry.overload.argument_domain_uuids;
  entry.cache_key.result_descriptor_uuid =
      entry.overload.result_descriptor_uuid;
  entry.cache_key.result_domain_uuid = entry.overload.result_domain_uuid;
  entry.cache_key.schema_epoch = entry.schema_epoch;
  entry.cache_key.security_epoch = entry.security_epoch;
  entry.cache_key.resource_epoch = entry.resource_epoch;
  entry.cache_key.implementation_version = entry.implementation_version;
  entry.cache_key.definition_hash = entry.definition_hash;

  return entry;
}

std::vector<std::string> RequiredMetricNames() {
  return {
      "sys.metrics.optimizer.type_index_stats.admission_count",
      "sys.metrics.optimizer.type_index_stats.refusal_count",
      "sys.metrics.optimizer.type_index_stats.conservative_admission_count",
      "sys.metrics.optimizer.type_index_stats.comparison_contract_missing_count",
      "sys.metrics.optimizer.type_index_stats.index_compatibility_missing_count",
      "sys.metrics.optimizer.type_index_stats.statistics_epoch_mismatch_count",
      "sys.metrics.optimizer.type_index_stats.statistics_privacy_denied_count",
      "sys.metrics.optimizer.type_index_stats.unknown_selectivity_count",
      "sys.metrics.optimizer.type_index_stats.plan_cache_invalidations",
      "sys.metrics.optimizer.type_index_stats.udr_metadata_refusal_count",
      "sys.metrics.optimizer.type_index_stats.reference_semantics_refusal_count",
      "sys.metrics.optimizer.type_index_stats.recheck_required_plan_count"};
}

engine::TypeIndexFamilyMatrixClassification Classification(
    engine::TypeIndexFamily family,
    engine::TypeIndexFamilySupport support) {
  engine::TypeIndexFamilyMatrixClassification classification;
  classification.index_family = family;
  classification.support = support;
  classification.diagnostic_declared = true;
  return classification;
}

std::vector<engine::TypeIndexFamilyMatrixClassification>
AllIndexFamilyClassifications() {
  return {Classification(engine::TypeIndexFamily::btree,
                         engine::TypeIndexFamilySupport::supported),
          Classification(engine::TypeIndexFamily::hash,
                         engine::TypeIndexFamilySupport::supported),
          Classification(engine::TypeIndexFamily::bitmap,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::inverted,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::full_text,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::spatial,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::vector,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::columnar,
                         engine::TypeIndexFamilySupport::conservative),
          Classification(engine::TypeIndexFamily::bloom,
                         engine::TypeIndexFamilySupport::supported_with_recheck),
          Classification(engine::TypeIndexFamily::sketch,
                         engine::TypeIndexFamilySupport::supported_with_recheck),
          Classification(engine::TypeIndexFamily::graph,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::time_series,
                         engine::TypeIndexFamilySupport::unsupported),
          Classification(engine::TypeIndexFamily::extension,
                         engine::TypeIndexFamilySupport::deferred)};
}

engine::TypeIndexFamilyMatrixRow MatrixRow(engine::ExecutionTypeFamily family) {
  engine::TypeIndexFamilyMatrixRow row;
  row.type_family = family;
  row.classifications = AllIndexFamilyClassifications();
  return row;
}

std::vector<engine::TypeIndexFamilyMatrixRow> ValidMatrix() {
  return {MatrixRow(engine::ExecutionTypeFamily::boolean),
          MatrixRow(engine::ExecutionTypeFamily::signed_integer),
          MatrixRow(engine::ExecutionTypeFamily::unsigned_integer),
          MatrixRow(engine::ExecutionTypeFamily::real),
          MatrixRow(engine::ExecutionTypeFamily::decimal),
          MatrixRow(engine::ExecutionTypeFamily::uuid),
          MatrixRow(engine::ExecutionTypeFamily::character),
          MatrixRow(engine::ExecutionTypeFamily::binary),
          MatrixRow(engine::ExecutionTypeFamily::bit_string),
          MatrixRow(engine::ExecutionTypeFamily::temporal),
          MatrixRow(engine::ExecutionTypeFamily::blob),
          MatrixRow(engine::ExecutionTypeFamily::network),
          MatrixRow(engine::ExecutionTypeFamily::document),
          MatrixRow(engine::ExecutionTypeFamily::search),
          MatrixRow(engine::ExecutionTypeFamily::structured),
          MatrixRow(engine::ExecutionTypeFamily::range),
          MatrixRow(engine::ExecutionTypeFamily::spatial),
          MatrixRow(engine::ExecutionTypeFamily::vector),
          MatrixRow(engine::ExecutionTypeFamily::graph),
          MatrixRow(engine::ExecutionTypeFamily::time_series),
          MatrixRow(engine::ExecutionTypeFamily::columnar),
          MatrixRow(engine::ExecutionTypeFamily::aggregate_state),
          MatrixRow(engine::ExecutionTypeFamily::sketch),
          MatrixRow(engine::ExecutionTypeFamily::locator),
          MatrixRow(engine::ExecutionTypeFamily::opaque)};
}

engine::TypeOptimizerDecisionInput ValidDecision() {
  engine::TypeOptimizerDecisionInput decision;
  decision.decision_uuid = Uuid(0x01);
  decision.query_scope_uuid = Uuid(0x02);
  decision.descriptor_uuid = Uuid(0x10);
  decision.operation_uuid = Uuid(0x11);
  decision.comparison_contract_uuid = Uuid(0x12);
  decision.canonicalization_profile_uuid = Uuid(0x13);
  decision.index_compatibility_uuid = Uuid(0x14);
  decision.statistics_uuid = Uuid(0x15);
  decision.selectivity_model_uuid = Uuid(0x16);
  decision.schema_epoch = 91;
  decision.security_epoch = 94;
  decision.resource_epoch = 93;
  decision.descriptor = TypeDescriptor(0x10, "tio.i32");
  decision.operation_entry =
      ValidOperationEntry(decision.operation_uuid, decision.descriptor);

  decision.comparison_contract.comparison_contract_uuid =
      decision.comparison_contract_uuid;
  decision.comparison_contract.descriptor_uuid = decision.descriptor_uuid;
  decision.comparison_contract.descriptor_epoch =
      decision.descriptor.descriptor_epoch;
  decision.comparison_contract.descriptor = decision.descriptor;
  decision.comparison_contract.equality_operation_uuid = decision.operation_uuid;
  decision.comparison_contract.ordering_operation_uuid = Uuid(0x17);
  decision.comparison_contract.hash_operation_uuid = Uuid(0x18);
  decision.comparison_contract.canonicalization_profile_uuid =
      decision.canonicalization_profile_uuid;
  decision.comparison_contract.resource_epoch = decision.resource_epoch;
  decision.comparison_contract.supports_ordering = true;
  decision.comparison_contract.supports_hashing = true;
  decision.comparison_contract.supports_index = true;
  decision.comparison_contract.index_equivalence_class.push_back(
      engine::TypeIndexFamily::btree);
  decision.comparison_contract.index_equivalence_class.push_back(
      engine::TypeIndexFamily::hash);

  decision.canonicalization_profile.canonicalization_profile_uuid =
      decision.canonicalization_profile_uuid;
  decision.canonicalization_profile.descriptor_uuid = decision.descriptor_uuid;
  decision.canonicalization_profile.descriptor_epoch =
      decision.descriptor.descriptor_epoch;
  decision.canonicalization_profile.descriptor = decision.descriptor;
  decision.canonicalization_profile.profile_family =
      engine::TypeCanonicalizationProfileFamily::numeric;
  decision.canonicalization_profile.resource_profile_uuid = Uuid(0x19);
  decision.canonicalization_profile.resource_epoch = decision.resource_epoch;
  decision.canonicalization_profile.profile_version = 1;
  decision.canonicalization_profile.numeric_scale_decimal_context_declared =
      true;
  decision.canonicalization_profile.numeric_special_value_posture_declared =
      true;
  decision.canonicalization_profile.numeric_overflow_declared = true;

  decision.index_compatibility.index_compatibility_uuid =
      decision.index_compatibility_uuid;
  decision.index_compatibility.descriptor_uuid = decision.descriptor_uuid;
  decision.index_compatibility.descriptor_epoch =
      decision.descriptor.descriptor_epoch;
  decision.index_compatibility.descriptor = decision.descriptor;
  decision.index_compatibility.comparison_contract_uuid =
      decision.comparison_contract_uuid;
  decision.index_compatibility.index_family = engine::TypeIndexFamily::btree;
  decision.index_compatibility.key_encoding_profile_uuid = Uuid(0x1a);
  decision.index_compatibility.predicate_operation_refs.push_back(
      decision.operation_uuid);

  decision.statistics.statistics_uuid = decision.statistics_uuid;
  decision.statistics.descriptor_uuid = decision.descriptor_uuid;
  decision.statistics.descriptor_epoch = decision.descriptor.descriptor_epoch;
  decision.statistics.descriptor = decision.descriptor;
  decision.statistics.source_object_uuid = Uuid(0x1b);
  decision.statistics.schema_epoch = decision.schema_epoch;
  decision.statistics.sample_epoch = 92;
  decision.statistics.security_epoch = decision.security_epoch;
  decision.statistics.resource_epoch = decision.resource_epoch;
  decision.statistics.row_count = 100;
  decision.statistics.null_count = 1;
  decision.statistics.distinct_count = 50;
  decision.statistics.selectivity_model_uuid = decision.selectivity_model_uuid;
  decision.statistics.privacy_policy_uuid = Uuid(0x1c);
  engine::TypeMostCommonValueSummary value;
  value.value_summary_uuid = Uuid(0x1d);
  value.frequency = 10;
  decision.statistics.most_common_values.push_back(value);
  decision.statistics.histogram.histogram_uuid = Uuid(0x1e);
  decision.statistics.histogram.histogram_kind = "equi_depth";
  decision.statistics.histogram.bucket_count = 8;

  decision.selectivity_model.selectivity_model_uuid =
      decision.selectivity_model_uuid;
  decision.selectivity_model.descriptor_uuid = decision.descriptor_uuid;
  decision.selectivity_model.operation_uuid = decision.operation_uuid;
  decision.selectivity_model.statistics_uuid = decision.statistics_uuid;
  decision.selectivity_model.schema_epoch = decision.schema_epoch;
  decision.selectivity_model.security_epoch = decision.security_epoch;
  decision.selectivity_model.resource_epoch = decision.resource_epoch;

  decision.cache_key.database_uuid = Uuid(0x1f);
  decision.cache_key.query_scope_uuid = decision.query_scope_uuid;
  decision.cache_key.normalized_sblr_expression_uuid = Uuid(0x30);
  decision.cache_key.descriptor_uuid = decision.descriptor_uuid;
  decision.cache_key.operation_uuid = decision.operation_uuid;
  decision.cache_key.comparison_contract_uuid = decision.comparison_contract_uuid;
  decision.cache_key.canonicalization_profile_uuid =
      decision.canonicalization_profile_uuid;
  decision.cache_key.index_compatibility_uuid = decision.index_compatibility_uuid;
  decision.cache_key.index_uuid = Uuid(0x31);
  decision.cache_key.index_generation = 7;
  decision.cache_key.statistics_uuid = decision.statistics_uuid;
  decision.cache_key.sample_epoch = decision.statistics.sample_epoch;
  decision.cache_key.schema_epoch = decision.schema_epoch;
  decision.cache_key.security_epoch = decision.security_epoch;
  decision.cache_key.resource_epoch = decision.resource_epoch;
  decision.cache_key.plan_cache_key_hash = "tio.cache.key";

  decision.diagnostic.diagnostic_code = "OPTIMIZER.ADMISSION_EVIDENCE";
  decision.diagnostic.descriptor_uuid = decision.descriptor_uuid;
  decision.diagnostic.operation_uuid = decision.operation_uuid;
  decision.diagnostic.comparison_contract_uuid =
      decision.comparison_contract_uuid;
  decision.diagnostic.index_compatibility_uuid =
      decision.index_compatibility_uuid;
  decision.diagnostic.statistics_uuid = decision.statistics_uuid;
  decision.diagnostic.resource_epoch = decision.resource_epoch;
  decision.diagnostic.security_epoch = decision.security_epoch;

  return decision;
}

engine::TypeIndexStatsOptimizerContract ValidContract() {
  engine::TypeIndexStatsOptimizerContract contract;
  contract.contract_uuid = Uuid(0xf0);
  contract.contract_epoch = 91;
  contract.stable_name = "tio.contract";
  contract.decisions.push_back(ValidDecision());
  contract.index_family_matrix = ValidMatrix();
  contract.local_metric_names = RequiredMetricNames();
  return contract;
}

void RequireStatus(const engine::TypeIndexStatsOptimizerContract& contract,
                   engine::TypeIndexStatsOptimizerStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateTypeIndexStatsOptimizerContract(contract);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "TIO type/index/statistics optimizer status mismatch");
}

void TestValidContractCoversTioGates() {
  const auto contract = ValidContract();
  Require(engine::ValidateTypeIndexStatsOptimizerContract(contract).ok(),
          "TIO rejected valid optimizer admission contract");
  Require(engine::TypeIndexStatsOptimizerStatusName(
              engine::TypeIndexStatsOptimizerStatus::ok) == "ok",
          "TIO status names are not stable");
}

void TestComparisonAndCanonicalizationFailures() {
  auto contract = ValidContract();
  contract.decisions[0].comparison_contract_present = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    comparison_contract_required,
                "TIO accepted decision without comparison contract");

  contract = ValidContract();
  contract.decisions[0].comparison_contract.resource_epoch = 999;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    comparison_resource_epoch_mismatch,
                "TIO accepted stale comparison resource epoch");

  contract = ValidContract();
  contract.decisions[0]
      .canonicalization_profile.numeric_overflow_declared = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    canonicalization_family_rules_missing,
                "TIO accepted incomplete canonicalization profile");
}

void TestIndexStatisticsAndSelectivityFailures() {
  auto contract = ValidContract();
  contract.decisions[0].index_compatibility_present = false;
  RequireStatus(
      contract,
      engine::TypeIndexStatsOptimizerStatus::index_compatibility_required,
      "TIO accepted indexed decision without compatibility descriptor");

  contract = ValidContract();
  contract.decisions[0].index_compatibility.lossiness_policy =
      engine::TypeIndexLossinessPolicy::lossy_requires_recheck;
  contract.decisions[0].index_compatibility.exact_recheck_required = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::index_recheck_required,
                "TIO accepted lossy index without exact recheck obligation");

  contract = ValidContract();
  contract.decisions[0].uses_protected_statistics = true;
  contract.decisions[0].statistics.protected_rare_values_redacted = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::statistics_privacy_denied,
                "TIO accepted protected statistics leakage");

  contract = ValidContract();
  contract.decisions[0].selectivity_model.unknown_selectivity_explicit = false;
  RequireStatus(
      contract,
      engine::TypeIndexStatsOptimizerStatus::selectivity_unknown_not_explicit,
      "TIO accepted implicit unknown selectivity");
}

void TestOptimizerAdmissionAndCacheFailures() {
  auto contract = ValidContract();
  contract.decisions[0].operation_executable = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::operation_not_executable,
                "TIO accepted non-executable operation");

  contract = ValidContract();
  contract.decisions[0].udr_metadata_requested = true;
  contract.decisions[0].udr_metadata_cpp = false;
  RequireStatus(
      contract,
      engine::TypeIndexStatsOptimizerStatus::non_cpp_udr_metadata_forbidden,
      "TIO accepted non-C++ UDR optimizer metadata");

  contract = ValidContract();
  contract.decisions[0].cache_key.includes_resource_epoch = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    cache_key_dependency_missing,
                "TIO accepted incomplete plan-cache key");

  contract = ValidContract();
  contract.decisions[0].cache_key.parser_family_untrusted_context_only = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::cache_key_parser_authority,
                "TIO accepted parser family as optimizer authority");

  contract = ValidContract();
  contract.decisions[0].diagnostic.diagnostic_code.clear();
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::diagnostic_code_required,
                "TIO accepted diagnostic without code");

  contract = ValidContract();
  contract.decisions[0].admission_result =
      engine::TypeOptimizerAdmissionState::refused_unknown_semantics;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::unsafe_admission_result,
                "TIO accepted unsafe admission result as usable optimization");
}

void TestMatrixAndMetricFailures() {
  auto contract = ValidContract();
  contract.index_family_matrix.pop_back();
  RequireStatus(
      contract,
      engine::TypeIndexStatsOptimizerStatus::index_family_matrix_family_missing,
      "TIO accepted index family matrix missing a type family");

  contract = ValidContract();
  contract.index_family_matrix[1].classifications.pop_back();
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    index_family_matrix_classification_missing,
                "TIO accepted incomplete index family classifications");

  contract = ValidContract();
  contract.index_family_matrix[1].classifications[2].diagnostic_declared =
      false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::
                    index_family_matrix_diagnostic_required,
                "TIO accepted unsupported index family without diagnostic");

  contract = ValidContract();
  contract.local_metric_names.pop_back();
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::local_metric_missing,
                "TIO accepted missing optimizer metric");

  contract = ValidContract();
  contract.cluster_metrics_guarded_by_cluster_governance = false;
  RequireStatus(contract,
                engine::TypeIndexStatsOptimizerStatus::cluster_metrics_guard_required,
                "TIO accepted cluster metrics without governance guard");
}

}  // namespace

int main() {
  TestValidContractCoversTioGates();
  TestComparisonAndCanonicalizationFailures();
  TestIndexStatisticsAndSelectivityFailures();
  TestOptimizerAdmissionAndCacheFailures();
  TestMatrixAndMetricFailures();
  std::cout << "type_index_stats_optimizer_conformance=passed\n";
  return EXIT_SUCCESS;
}
