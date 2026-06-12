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
        static_cast<std::uint8_t>((seed + (index * 47u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

std::string ScalarTypeName(engine::CanonicalScalarType type) {
  switch (type) {
    case engine::CanonicalScalarType::boolean:
      return "boolean";
    case engine::CanonicalScalarType::int8:
      return "int8";
    case engine::CanonicalScalarType::int16:
      return "int16";
    case engine::CanonicalScalarType::int32:
      return "int32";
    case engine::CanonicalScalarType::int64:
      return "int64";
    case engine::CanonicalScalarType::int128:
      return "int128";
    case engine::CanonicalScalarType::uint8:
      return "uint8";
    case engine::CanonicalScalarType::uint16:
      return "uint16";
    case engine::CanonicalScalarType::uint32:
      return "uint32";
    case engine::CanonicalScalarType::uint64:
      return "uint64";
    case engine::CanonicalScalarType::uint128:
      return "uint128";
    case engine::CanonicalScalarType::exact_numeric:
      return "exact_numeric";
    case engine::CanonicalScalarType::decimal_floating:
      return "decimal_floating";
    case engine::CanonicalScalarType::float32:
      return "float32";
    case engine::CanonicalScalarType::float64:
      return "float64";
    case engine::CanonicalScalarType::float128:
      return "float128";
    case engine::CanonicalScalarType::money_currency:
      return "money_currency";
  }
  return "unknown";
}

std::vector<engine::CanonicalScalarType> RequiredTypes() {
  using Type = engine::CanonicalScalarType;
  return {Type::boolean,       Type::int8,
          Type::int16,         Type::int32,
          Type::int64,         Type::int128,
          Type::uint8,         Type::uint16,
          Type::uint32,        Type::uint64,
          Type::uint128,       Type::exact_numeric,
          Type::decimal_floating,
          Type::float32,       Type::float64,
          Type::float128,      Type::money_currency};
}

std::vector<std::string> RequiredDiagnostics() {
  return {"SCALAR.DESCRIPTOR.INVALID",
          "SCALAR.INVALID_LITERAL",
          "SCALAR.AMBIGUOUS_LITERAL",
          "SCALAR.AMBIGUOUS_CAST",
          "SCALAR.OUT_OF_RANGE",
          "SCALAR.PRECISION_LOSS",
          "SCALAR.SCALE_LOSS",
          "SCALAR.OVERFLOW",
          "SCALAR.UNDERFLOW",
          "SCALAR.DIVIDE_BY_ZERO",
          "SCALAR.SPECIAL_VALUE_REFUSED",
          "SCALAR.CURRENCY_MISMATCH",
          "SCALAR.BACKEND_UNAVAILABLE",
          "SCALAR.RAW_HOST_ENCODING_REFUSED",
          "SCALAR.REFERENCE.MAPPING_MISSING",
          "SCALAR.TRANSPORT.UNSUPPORTED",
          "SCALAR.MERGE.MANUAL_REVIEW_REQUIRED",
          "float128_backend_unavailable",
          "float128_raw_host_encoding_refused"};
}

std::vector<std::string> RequiredMetrics() {
  return {
      "sys.metrics.datatypes.scalar.descriptor.admissions_total",
      "sys.metrics.datatypes.scalar.literal.invalid_total",
      "sys.metrics.datatypes.scalar.literal.ambiguous_total",
      "sys.metrics.datatypes.scalar.cast.attempts_total",
      "sys.metrics.datatypes.scalar.cast.failures_total",
      "sys.metrics.datatypes.scalar.operation.overflow_total",
      "sys.metrics.datatypes.scalar.operation.underflow_total",
      "sys.metrics.datatypes.scalar.operation.divide_by_zero_total",
      "sys.metrics.datatypes.scalar.operation.precision_loss_refusals_total",
      "sys.metrics.datatypes.scalar.operation.scale_loss_refusals_total",
      "sys.metrics.datatypes.scalar.numeric_context.special_value_refusals_total",
      "sys.metrics.datatypes.scalar.float128.backend_fallbacks_total",
      "sys.metrics.datatypes.scalar.float128.backend_unavailable_total",
      "sys.metrics.datatypes.scalar.float128.raw_host_encoding_refusals_total",
      "sys.metrics.datatypes.scalar.money.currency_mismatches_total",
      "sys.metrics.datatypes.scalar.transport.refusals_total",
      "sys.metrics.datatypes.scalar.reference.mapping_misses_total",
      "sys.metrics.datatypes.scalar.merge.manual_review_required_total"};
}

std::vector<std::string> RequiredGates() {
  return {"CSD-GATE-001",   "CSD-GATE-002",   "CSD-GATE-003",
          "CSD-GATE-004",   "CSD-GATE-005",   "CSD-GATE-006",
          "CSD-GATE-007",   "CSD-GATE-008",   "CSD-GATE-009",
          "CSD-GATE-010",   "CSD-GATE-011",   "SCALAR-CONF-001",
          "SCALAR-CONF-002", "SCALAR-CONF-003", "SCALAR-CONF-004",
          "SCALAR-CONF-005", "SCALAR-CONF-006", "SCALAR-CONF-007",
          "SCALAR-CONF-008", "SCALAR-CONF-009", "SCALAR-CONF-010"};
}

void ApplyScalarShape(engine::ScalarTypeDescriptorRecord& descriptor) {
  using Type = engine::CanonicalScalarType;
  switch (descriptor.canonical_type) {
    case Type::boolean:
      descriptor.bit_width = 1;
      break;
    case Type::int8:
    case Type::uint8:
      descriptor.bit_width = 8;
      descriptor.precision_declared = true;
      descriptor.precision = 3;
      break;
    case Type::int16:
    case Type::uint16:
      descriptor.bit_width = 16;
      descriptor.precision_declared = true;
      descriptor.precision = 5;
      break;
    case Type::int32:
    case Type::uint32:
      descriptor.bit_width = 32;
      descriptor.precision_declared = true;
      descriptor.precision = 10;
      break;
    case Type::int64:
    case Type::uint64:
      descriptor.bit_width = 64;
      descriptor.precision_declared = true;
      descriptor.precision = 20;
      break;
    case Type::int128:
    case Type::uint128:
      descriptor.bit_width = 128;
      descriptor.precision_declared = true;
      descriptor.precision = 39;
      break;
    case Type::exact_numeric:
      descriptor.precision_declared = true;
      descriptor.precision = 38;
      descriptor.scale_declared = true;
      descriptor.scale = 9;
      break;
    case Type::decimal_floating:
      descriptor.precision_declared = true;
      descriptor.precision = 34;
      break;
    case Type::float32:
      descriptor.bit_width = 32;
      descriptor.precision_declared = true;
      descriptor.precision = 24;
      descriptor.canonical_payload_bytes = 4;
      break;
    case Type::float64:
      descriptor.bit_width = 64;
      descriptor.precision_declared = true;
      descriptor.precision = 53;
      descriptor.canonical_payload_bytes = 8;
      break;
    case Type::float128:
      descriptor.bit_width = 128;
      descriptor.precision_declared = true;
      descriptor.precision = 113;
      descriptor.canonical_payload_bytes = 16;
      descriptor.portable_binary128_backend_required = true;
      descriptor.portable_binary128_backend_available = true;
      break;
    case Type::money_currency:
      descriptor.precision_declared = true;
      descriptor.precision = 38;
      descriptor.scale_declared = true;
      descriptor.scale = 4;
      break;
  }
}

engine::Uuid AddNumericContext(
    engine::CanonicalScalarDatatypeRegistry& registry,
    std::string context_key,
    std::uint16_t& uuid_seed) {
  engine::NumericContextRecord context;
  context.numeric_context_uuid = Uuid(uuid_seed++);
  context.context_key = context_key;
  context.rounding_mode = "round_to_nearest_even";
  context.overflow_policy = "fail_closed";
  context.underflow_policy = "diagnostic_or_descriptor_special_value";
  context.divide_by_zero_policy = "stable_diagnostic";
  context.nan_policy = "descriptor_defined_value_not_sql_null";
  context.infinity_policy = "descriptor_defined_admission_and_ordering";
  context.signed_zero_policy = "descriptor_defined_equality_hash_order";
  context.trap_policy = "descriptor_bound_traps";
  context.total_order_profile = "descriptor_total_order_when_indexed";
  context.total_order_required = true;
  context.context_hash = "context." + context_key;
  const auto context_uuid = context.numeric_context_uuid;
  registry.numeric_contexts.push_back(context);
  return context_uuid;
}

engine::Uuid AddDescriptor(
    engine::CanonicalScalarDatatypeRegistry& registry,
    engine::CanonicalScalarType type,
    std::uint16_t& uuid_seed) {
  engine::ScalarTypeDescriptorRecord descriptor;
  descriptor.descriptor_uuid = Uuid(uuid_seed++);
  descriptor.descriptor_key = ScalarTypeName(type);
  descriptor.descriptor_search_key =
      std::string(engine::CanonicalScalarTypeSearchKey(type));
  descriptor.canonical_type = type;
  descriptor.scalar_family = engine::CanonicalScalarTypeFamily(type);
  descriptor.value_state_set_complete = true;
  descriptor.comparison_contract_uuid = Uuid(uuid_seed++);
  descriptor.canonicalization_profile_uuid = Uuid(uuid_seed++);
  descriptor.cast_policy_uuid = Uuid(uuid_seed++);
  descriptor.operation_policy_uuid = Uuid(uuid_seed++);
  descriptor.storage_codec_uuid = Uuid(uuid_seed++);
  descriptor.wire_render_policy_uuid = Uuid(uuid_seed++);
  descriptor.statistics_profile_uuid = Uuid(uuid_seed++);
  descriptor.index_profile_uuid = Uuid(uuid_seed++);
  descriptor.backup_transport_profile_uuid = Uuid(uuid_seed++);
  descriptor.diagnostic_policy_uuid = Uuid(uuid_seed++);
  descriptor.metric_profile_uuid = Uuid(uuid_seed++);
  descriptor.conformance_profile_uuid = Uuid(uuid_seed++);
  descriptor.storage_encoding_canonical = true;
  descriptor.raw_host_encoding_forbidden = true;
  descriptor.descriptor_hash = "descriptor." + descriptor.descriptor_key;
  ApplyScalarShape(descriptor);
  if (engine::CanonicalScalarTypeRequiresNumericContext(type)) {
    descriptor.numeric_context_uuid =
        AddNumericContext(registry, "ctx." + descriptor.descriptor_key,
                          uuid_seed);
  }
  const auto descriptor_uuid = descriptor.descriptor_uuid;
  registry.descriptors.push_back(descriptor);
  return descriptor_uuid;
}

engine::Uuid FindDescriptorUuid(
    const engine::CanonicalScalarDatatypeRegistry& registry,
    engine::CanonicalScalarType type) {
  for (const auto& descriptor : registry.descriptors) {
    if (descriptor.canonical_type == type) {
      return descriptor.descriptor_uuid;
    }
  }
  Fail("missing descriptor in test registry");
}

void AddResolutionRule(engine::CanonicalScalarDatatypeRegistry& registry,
                       engine::CanonicalScalarResolutionKind kind,
                       std::uint16_t& uuid_seed) {
  engine::ScalarLiteralBindResolutionRule rule;
  rule.resolution_rule_uuid = Uuid(uuid_seed++);
  rule.resolution_kind = kind;
  rule.resolution_search_key = "CSD-SCALAR-LITERAL-BIND-RESOLUTION";
  rule.candidate_set_hash = "candidates.literal.bind";
  rule.ranking_rule_hash = "rank.csd.scalar";
  rule.resolved_descriptor_evidence_hash = "resolved.descriptor.uuid";
  rule.ambiguous_diagnostic_code = "SCALAR.AMBIGUOUS_LITERAL";
  rule.no_match_diagnostic_code = "SCALAR.INVALID_LITERAL";
  rule.engine_resolves_final_descriptor = true;
  rule.fail_closed_on_ambiguity = true;
  rule.driver_metadata_is_hint = true;
  rule.parser_spelling_not_authority = true;
  rule.rule_hash = "resolution." + std::to_string(static_cast<int>(kind));
  registry.literal_bind_rules.push_back(rule);
}

void AddOperationRule(engine::CanonicalScalarDatatypeRegistry& registry,
                      engine::CanonicalScalarOperationFamily family,
                      engine::CanonicalScalarType source,
                      engine::CanonicalScalarType target,
                      engine::CanonicalScalarType result,
                      std::uint16_t& uuid_seed) {
  engine::ScalarCastOperationRule rule;
  rule.rule_uuid = Uuid(uuid_seed++);
  rule.operation_family = family;
  rule.cast_rank = engine::CanonicalScalarCastRank::assignment_checked;
  rule.source_descriptor_uuid = FindDescriptorUuid(registry, source);
  rule.target_descriptor_uuid = FindDescriptorUuid(registry, target);
  rule.result_descriptor_uuid = FindDescriptorUuid(registry, result);
  rule.operation_search_key =
      family == engine::CanonicalScalarOperationFamily::cast
          ? "CSD-SCALAR-CAST-RANKING"
          : "CSD-SCALAR-OPERATION-FAMILIES";
  rule.operation_policy_hash = "operation.policy." +
                               std::to_string(static_cast<int>(family));
  rule.range_check_declared = true;
  rule.precision_scale_check_declared = true;
  rule.special_value_policy_checked = true;
  rule.currency_policy_checked = true;
  rule.security_policy_checked = true;
  rule.reference_profile_checked = true;
  rule.result_descriptor_declared = true;
  rule.no_silent_fallback = true;
  rule.failure_diagnostic_code =
      family == engine::CanonicalScalarOperationFamily::cast
          ? "SCALAR.AMBIGUOUS_CAST"
          : "SCALAR.OVERFLOW";
  rule.rule_hash = "rule." + std::to_string(static_cast<int>(family));
  registry.cast_operation_rules.push_back(rule);
}

void AddTransportAndReferenceProfiles(
    engine::CanonicalScalarDatatypeRegistry& registry,
    std::uint16_t& uuid_seed) {
  for (const auto& descriptor : registry.descriptors) {
    engine::ScalarTransportProfileRecord transport;
    transport.transport_profile_uuid = Uuid(uuid_seed++);
    transport.descriptor_uuid = descriptor.descriptor_uuid;
    transport.transport_search_key =
        "CSD-SCALAR-BACKUP-REPLICATION-TRANSPORT";
    transport.backup_manifest_records_descriptor = true;
    transport.restore_incompatible_refuses = true;
    transport.replication_descriptor_version_checked = true;
    transport.cluster_transport_negotiates_descriptor_codec = true;
    transport.logical_delta_derivative_only = true;
    transport.merge_uses_canonical_equality = true;
    transport.driver_metadata_discloses_limitations = true;
    transport.transport_hash = "transport." + descriptor.descriptor_key;
    registry.transport_profiles.push_back(transport);

    engine::ScalarReferenceMappingRecord mapping;
    mapping.reference_mapping_uuid = Uuid(uuid_seed++);
    mapping.descriptor_uuid = descriptor.descriptor_uuid;
    mapping.reference_profile_key = "common.sql.profile";
    mapping.reference_type_name = "reference_" + descriptor.descriptor_key;
    mapping.behavior = engine::CanonicalScalarReferenceBehavior::exact;
    mapping.diagnostic_code = "SCALAR.REFERENCE.MAPPING_MISSING";
    mapping.mapping_hash = "reference.mapping." + descriptor.descriptor_key;
    registry.reference_mappings.push_back(mapping);
  }
}

void AddConformanceGates(engine::CanonicalScalarDatatypeRegistry& registry,
                         std::uint16_t& uuid_seed) {
  for (const auto& gate_id : RequiredGates()) {
    engine::CanonicalScalarConformanceGateRecord gate;
    gate.gate_uuid = Uuid(uuid_seed++);
    gate.gate_id = gate_id;
    gate.status = engine::CanonicalScalarGateStatus::passed;
    gate.evidence_hash = "evidence." + gate_id;
    gate.ctest_name = "database_lifecycle_canonical_scalar_datatypes_"
                      "conformance";
    gate.diagnostic_code = "SCALAR.DESCRIPTOR.INVALID";
    gate.gate_hash = "gate." + gate_id;
    registry.conformance_gates.push_back(gate);
  }
}

engine::CanonicalScalarDatatypeRegistry ValidRegistry() {
  engine::CanonicalScalarDatatypeRegistry registry;
  std::uint16_t uuid_seed = 3000;
  registry.registry_uuid = Uuid(uuid_seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "canonical_scalar_datatypes";
  registry.root_search_key = "CSD-CANONICAL-SCALAR-DATATYPES";
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();

  for (const auto type : RequiredTypes()) {
    AddDescriptor(registry, type, uuid_seed);
  }

  const auto exact_numeric_uuid =
      FindDescriptorUuid(registry, engine::CanonicalScalarType::exact_numeric);
  for (auto& descriptor : registry.descriptors) {
    if (descriptor.canonical_type ==
        engine::CanonicalScalarType::money_currency) {
      descriptor.amount_descriptor_uuid = exact_numeric_uuid;
      descriptor.currency_policy_uuid = Uuid(uuid_seed++);
      descriptor.monetary_rounding_policy_uuid = Uuid(uuid_seed++);
      descriptor.monetary_render_policy_uuid = Uuid(uuid_seed++);
    }
  }

  AddResolutionRule(registry, engine::CanonicalScalarResolutionKind::literal,
                    uuid_seed);
  AddResolutionRule(
      registry, engine::CanonicalScalarResolutionKind::bind_parameter,
      uuid_seed);

  using Family = engine::CanonicalScalarOperationFamily;
  using Type = engine::CanonicalScalarType;
  AddOperationRule(registry, Family::cast, Type::uint64, Type::int128,
                   Type::int128, uuid_seed);
  AddOperationRule(registry, Family::equality_distinctness, Type::boolean,
                   Type::boolean, Type::boolean, uuid_seed);
  AddOperationRule(registry, Family::ordering, Type::int64, Type::int64,
                   Type::boolean, uuid_seed);
  AddOperationRule(registry, Family::hashing, Type::boolean, Type::uint64,
                   Type::uint64, uuid_seed);
  AddOperationRule(registry, Family::arithmetic, Type::exact_numeric,
                   Type::exact_numeric, Type::exact_numeric, uuid_seed);
  AddOperationRule(registry, Family::aggregate, Type::int64,
                   Type::exact_numeric, Type::exact_numeric, uuid_seed);
  AddOperationRule(registry, Family::window, Type::exact_numeric,
                   Type::exact_numeric, Type::exact_numeric, uuid_seed);
  AddOperationRule(registry, Family::serialization, Type::float128,
                   Type::float128, Type::float128, uuid_seed);

  AddTransportAndReferenceProfiles(registry, uuid_seed);
  AddConformanceGates(registry, uuid_seed);
  return registry;
}

void RequireStatus(const engine::CanonicalScalarDatatypeRegistry& registry,
                   engine::CanonicalScalarDatatypeStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateCanonicalScalarDatatypeRegistry(registry);
  Require(!result.ok(), message);
  if (result.status != expected) {
    std::cerr << "expected="
              << engine::CanonicalScalarDatatypeStatusName(expected)
              << " actual="
              << engine::CanonicalScalarDatatypeStatusName(result.status)
              << '\n';
    Fail("canonical scalar datatype status mismatch");
  }
}

void TestValidRegistryCoversAllScalarFamilies() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateCanonicalScalarDatatypeRegistry(registry).ok(),
          "CSD rejected valid canonical scalar datatype registry");
  Require(registry.descriptors.size() == RequiredTypes().size(),
          "CSD did not register every required scalar descriptor");
  Require(engine::CanonicalScalarDatatypeStatusName(
              engine::CanonicalScalarDatatypeStatus::ok) == "ok",
          "CSD status names are not stable");
}

void TestDescriptorAndNumericContextFailures() {
  auto registry = ValidRegistry();
  registry.descriptors.erase(registry.descriptors.begin());
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::descriptor_record_missing,
                "CSD accepted missing boolean descriptor");

  registry = ValidRegistry();
  registry.descriptors[0].comparison_contract_uuid = {};
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::descriptor_policy_missing,
                "CSD accepted descriptor without comparison contract");

  registry = ValidRegistry();
  registry.descriptors[11].scale = 40;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::scale_invalid,
                "CSD accepted exact numeric scale exceeding precision");

  registry = ValidRegistry();
  registry.descriptors[12].numeric_context_uuid = {};
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::numeric_context_missing,
                "CSD accepted decimal floating without numeric context");
}

void TestFloat128AndLiteralBindFailures() {
  auto registry = ValidRegistry();
  registry.descriptors[15].portable_binary128_backend_available = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::float128_backend_missing,
                "CSD accepted float128 without portable backend");

  registry = ValidRegistry();
  registry.descriptors[15].silent_downgrade_forbidden = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    float128_contract_incomplete,
                "CSD accepted float128 silent downgrade");

  registry = ValidRegistry();
  registry.literal_bind_rules[0].fail_closed_on_ambiguity = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    literal_bind_ambiguity_not_fail_closed,
                "CSD accepted ambiguous scalar literal guessing");
}

void TestOperationTransportReferenceGateFailures() {
  auto registry = ValidRegistry();
  registry.cast_operation_rules[0].range_check_declared = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    cast_operation_checks_incomplete,
                "CSD accepted cast without range checks");

  registry = ValidRegistry();
  registry.transport_profiles[0].logical_delta_derivative_only = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    transport_recovery_authority_violation,
                "CSD accepted scalar delta stream as recovery authority");

  registry = ValidRegistry();
  registry.reference_mappings.erase(registry.reference_mappings.begin());
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::reference_mapping_missing,
                "CSD accepted missing reference scalar mapping");

  registry = ValidRegistry();
  registry.conformance_gates[0].status =
      engine::CanonicalScalarGateStatus::failed;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::conformance_gate_failed,
                "CSD accepted failed scalar conformance gate");
}

void TestDiagnosticMetricAndAuthorityFailures() {
  auto registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    diagnostic_vector_missing,
                "CSD accepted missing scalar diagnostic vector");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::local_metric_missing,
                "CSD accepted missing scalar metric");

  registry = ValidRegistry();
  registry.reference_names_not_authority = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    authority_invariant_violation,
                "CSD accepted reference scalar names as authority");

  registry = ValidRegistry();
  registry.cluster_metrics_guarded_by_cluster_governance = false;
  RequireStatus(registry,
                engine::CanonicalScalarDatatypeStatus::
                    cluster_metric_guard_required,
                "CSD accepted cluster scalar metrics without cluster guard");
}

}  // namespace

int main() {
  TestValidRegistryCoversAllScalarFamilies();
  TestDescriptorAndNumericContextFailures();
  TestFloat128AndLiteralBindFailures();
  TestOperationTransportReferenceGateFailures();
  TestDiagnosticMetricAndAuthorityFailures();
  std::cout << "canonical_scalar_datatypes_conformance=passed\n";
  return EXIT_SUCCESS;
}
