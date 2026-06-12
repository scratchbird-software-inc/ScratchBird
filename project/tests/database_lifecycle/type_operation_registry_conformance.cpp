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
  descriptor.descriptor_epoch = 71;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 32;
  return descriptor;
}

engine::ExecutionTypeDescriptor DomainTypeDescriptor(
    std::uint8_t seed,
    std::string_view name,
    const engine::Uuid& domain_uuid) {
  auto descriptor = TypeDescriptor(seed, name);
  descriptor.domain_uuid = domain_uuid;
  descriptor.domain_stack.push_back(domain_uuid);
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  return descriptor;
}

engine::DomainCastRuleDescriptor ValidCastRule(
    const engine::Uuid& source_domain_uuid,
    const engine::Uuid& target_domain_uuid) {
  engine::DomainCastRuleDescriptor descriptor;
  descriptor.cast_rule_uuid = Uuid(0x20);
  descriptor.cast_policy_uuid = Uuid(0x21);
  descriptor.descriptor_epoch = 71;
  descriptor.cast_policy_epoch = 71;
  descriptor.stable_name = "tor.domain.cast";
  descriptor.cast_kind = engine::DomainCastRuleKind::explicit_only;
  descriptor.source_domain_uuid = source_domain_uuid;
  descriptor.target_domain_uuid = target_domain_uuid;
  descriptor.source_descriptor =
      DomainTypeDescriptor(0x22, "tor.cast.source", source_domain_uuid);
  descriptor.target_descriptor =
      DomainTypeDescriptor(0x23, "tor.cast.target", target_domain_uuid);
  descriptor.source_descriptor_uuid =
      descriptor.source_descriptor.descriptor_uuid;
  descriptor.target_descriptor_uuid =
      descriptor.target_descriptor.descriptor_uuid;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::native_sblr;
  return descriptor;
}

engine::DomainOperationDescriptor ValidDomainOperation(
    const engine::Uuid& domain_uuid) {
  engine::DomainOperationDescriptor descriptor;
  descriptor.operation_uuid = Uuid(0x30);
  descriptor.operation_policy_uuid = Uuid(0x31);
  descriptor.domain_uuid = domain_uuid;
  descriptor.descriptor_epoch = 71;
  descriptor.operation_policy_epoch = 71;
  descriptor.stable_name = "tor.domain.equals";
  descriptor.operation_kind = engine::DomainOperationKind::comparison;
  descriptor.min_arity = 1;
  descriptor.max_arity = 1;

  engine::DomainOperationOperandDescriptor operand;
  operand.domain_uuid = domain_uuid;
  operand.descriptor =
      DomainTypeDescriptor(0x32, "tor.domain.operand", domain_uuid);
  operand.operand_descriptor_uuid = operand.descriptor.descriptor_uuid;
  descriptor.operands.push_back(operand);

  descriptor.result_domain_uuid = domain_uuid;
  descriptor.result_descriptor =
      DomainTypeDescriptor(0x33, "tor.domain.result", domain_uuid);
  descriptor.result_descriptor_uuid =
      descriptor.result_descriptor.descriptor_uuid;
  descriptor.index_eligibility = engine::DomainIndexEligibility::equality;
  descriptor.implementation_kind =
      engine::DomainDescriptorImplementationKind::native_sblr;
  return descriptor;
}

std::vector<std::string> RequiredMetricNames() {
  return {
      "sys.metrics.type_operation.registration_count",
      "sys.metrics.type_operation.registration_failure_count",
      "sys.metrics.type_operation.active_count",
      "sys.metrics.type_operation.deferred_count",
      "sys.metrics.type_operation.unsupported_count",
      "sys.metrics.type_operation.execution_count",
      "sys.metrics.type_operation.execution_refusal_count",
      "sys.metrics.type_operation.overload_resolution_count",
      "sys.metrics.type_operation.overload_ambiguous_count",
      "sys.metrics.type_operation.cast_execution_count",
      "sys.metrics.type_operation.cast_refusal_count",
      "sys.metrics.type_operation.sblr_stale_binding_count",
      "sys.metrics.type_operation.udr_bridge_invocation_count",
      "sys.metrics.type_operation.non_cpp_udr_refusal_count",
      "sys.metrics.type_operation.llvm_fallback_count",
      "sys.metrics.type_operation.cache_invalidation_count"};
}

engine::TypeOperationDiagnosticVector Diagnostic(
    const engine::TypeOperationRegistryEntry& entry,
    std::string_view code) {
  engine::TypeOperationDiagnosticVector diagnostic;
  diagnostic.diagnostic_code = std::string(code);
  diagnostic.operation_uuid = entry.operation_uuid;
  diagnostic.operation_family_uuid = entry.operation_family_uuid;
  diagnostic.row_status = entry.row_status;
  diagnostic.schema_epoch = entry.schema_epoch;
  diagnostic.security_epoch = entry.security_epoch;
  diagnostic.resource_epoch = entry.resource_epoch;
  diagnostic.implementation_version = entry.implementation_version;
  diagnostic.reference_profile_uuid = entry.reference_profile_uuid;
  return diagnostic;
}

engine::TypeOperationRegistryEntry ValidEntry(
    std::uint8_t seed,
    engine::TypeOperationKind kind,
    std::string_view stable_name,
    std::string_view overload_key) {
  engine::TypeOperationRegistryEntry entry;
  entry.operation_uuid = Uuid(seed);
  entry.operation_family_uuid = Uuid(seed + 1);
  entry.schema_uuid = Uuid(seed + 2);
  entry.owning_package_uuid = Uuid(seed + 3);
  entry.name_ref_uuid = Uuid(seed + 4);
  entry.operation_epoch = 71;
  entry.schema_epoch = 72;
  entry.security_epoch = 73;
  entry.resource_epoch = 74;
  entry.implementation_version = 75;
  entry.stable_name = std::string(stable_name);
  entry.definition_hash = std::string("definition.") + entry.stable_name;
  entry.diagnostic_search_key = "TYPE_OPERATION.REGISTRY_ROW_INVALID";
  entry.conformance_key = "TOR-GATE-001";
  entry.operation_kind = kind;

  auto argument = TypeDescriptor(seed + 5, "tor.argument");
  auto result = TypeDescriptor(seed + 6, "tor.result");
  entry.overload.overload_set_uuid = Uuid(seed + 7);
  entry.overload.overload_epoch = 71;
  entry.overload.overload_key = std::string(overload_key);
  entry.overload.argument_descriptors.push_back(argument);
  entry.overload.argument_descriptor_uuids.push_back(argument.descriptor_uuid);
  entry.overload.result_descriptor = result;
  entry.overload.result_descriptor_uuid = result.descriptor_uuid;

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

void AttachCppUdr(engine::TypeOperationRegistryEntry& entry) {
  entry.implementation_target = engine::TypeOperationImplementationTarget::cpp_udr;
  entry.row_status = engine::TypeOperationRegistryRowStatus::bridge_only;
  entry.cpp_udr_hook.present = true;
  entry.cpp_udr_hook.library_uuid = Uuid(0x80);
  entry.cpp_udr_hook.mapping_descriptor_uuid = Uuid(0x81);
  entry.cpp_udr_hook.mapping_descriptor_epoch = 71;
  entry.cpp_udr_hook.entrypoint_symbol = "sb_tor_udr_entry";
  entry.cpp_udr_hook.preserves_descriptors = true;
  entry.cpp_udr_hook.parser_independent = true;
  entry.cpp_udr_package_uuid = Uuid(0x82);
  entry.cpp_udr_package_version = 71;
  entry.cache_key.cpp_udr_package_uuid = entry.cpp_udr_package_uuid;
  entry.cache_key.cpp_udr_package_version = entry.cpp_udr_package_version;
}

void AttachLlvm(engine::TypeOperationRegistryEntry& entry) {
  AttachCppUdr(entry);
  entry.row_status = engine::TypeOperationRegistryRowStatus::active;
  entry.implementation_target =
      engine::TypeOperationImplementationTarget::llvm_native;
  entry.llvm_artifact_uuid = Uuid(0x83);
  entry.llvm_artifact_epoch = 71;
  entry.llvm_artifact_version = 72;
  entry.llvm_fallback_reference_uuid = Uuid(0x84);
  entry.cache_key.llvm_artifact_uuid = entry.llvm_artifact_uuid;
  entry.cache_key.llvm_artifact_version = entry.llvm_artifact_version;
}

void AttachAggregateState(engine::TypeOperationRegistryEntry& entry,
                          std::uint8_t seed) {
  entry.aggregate_state_present = true;
  entry.aggregate_state.state_descriptor =
      TypeDescriptor(seed, "tor.aggregate.state");
  entry.aggregate_state.state_descriptor_uuid =
      entry.aggregate_state.state_descriptor.descriptor_uuid;
  entry.aggregate_state.state_version = 71;
  entry.aggregate_state.transition_function_uuid = Uuid(seed + 1);
  entry.aggregate_state.final_function_uuid = Uuid(seed + 2);
  entry.aggregate_state.combine_function_uuid = Uuid(seed + 3);
  entry.aggregate_state.memory_class_uuid = Uuid(seed + 4);
  entry.aggregate_state.spill_policy_uuid = Uuid(seed + 5);
  entry.aggregate_state.cleanup_policy_uuid = Uuid(seed + 6);
  entry.aggregate_state.serialization_function_uuid = Uuid(seed + 7);
  entry.aggregate_state.deserialization_function_uuid = Uuid(seed + 8);
  entry.aggregate_state.serialization_version = 71;
  entry.aggregate_state.parallel_combine_allowed = true;
  entry.aggregate_state.combine_associativity_proven = true;
}

void AttachWindowFrame(engine::TypeOperationRegistryEntry& entry,
                       std::uint8_t seed) {
  entry.window_frame_present = true;
  entry.window_frame.frame_policy_uuid = Uuid(seed);
  entry.window_frame.frame_policy_epoch = 71;
  entry.window_frame.frame_removal_allowed = true;
  entry.window_frame.inverse_function_uuid = Uuid(seed + 1);
}

engine::TypeOperationRegistry ValidRegistry() {
  engine::TypeOperationRegistry registry;
  registry.registry_uuid = Uuid(0x01);
  registry.registry_epoch = 71;
  registry.stable_name = "tor.registry";
  registry.catalog_snapshot_uuid = Uuid(0x02);
  registry.visible_transaction_uuid = Uuid(0x03);
  registry.local_metric_names = RequiredMetricNames();

  registry.entries.push_back(ValidEntry(
      0x10, engine::TypeOperationKind::type_operator, "tor.add", "i32.i32"));

  auto domain_cast = ValidEntry(
      0x40, engine::TypeOperationKind::domain_cast, "tor.cast",
      "domain.source.domain.target");
  domain_cast.cast_class = engine::TypeOperationCastClass::explicit_cast;
  domain_cast.domain_cast_rule_present = true;
  domain_cast.domain_cast_rule = ValidCastRule(Uuid(0x90), Uuid(0x91));
  domain_cast.domain_cast_rule_uuid =
      domain_cast.domain_cast_rule.cast_rule_uuid;
  registry.entries.push_back(domain_cast);

  auto aggregate = ValidEntry(
      0x50, engine::TypeOperationKind::type_aggregate, "tor.sum", "i32");
  AttachAggregateState(aggregate, 0xa0);
  registry.entries.push_back(aggregate);

  auto window = ValidEntry(
      0x60, engine::TypeOperationKind::type_window, "tor.row_window", "i32");
  AttachAggregateState(window, 0xb0);
  AttachWindowFrame(window, 0xba);
  registry.entries.push_back(window);

  auto udr = ValidEntry(
      0x70, engine::TypeOperationKind::type_function, "tor.udr_scale",
      "i32.scale");
  AttachCppUdr(udr);
  registry.entries.push_back(udr);

  auto llvm = ValidEntry(
      0xc0, engine::TypeOperationKind::type_function, "tor.llvm_abs",
      "i32.abs");
  AttachLlvm(llvm);
  registry.entries.push_back(llvm);

  auto reference = ValidEntry(
      0xd0, engine::TypeOperationKind::reference_method,
      "tor.sqlserver.geometry.starea", "geometry");
  reference.implementation_target = engine::TypeOperationImplementationTarget::cpp_udr;
  reference.domain_operation_present = true;
  reference.domain_operation = ValidDomainOperation(Uuid(0x92));
  reference.domain_operation_uuid = reference.domain_operation.operation_uuid;
  reference.reference_method_binding_present = true;
  reference.reference_profile_uuid = Uuid(0x93);
  reference.reference_family = "sqlserver";
  reference.reference_version_profile = "2022";
  reference.reference_method_name = "STArea";
  reference.inverse_rendering_policy = "render_as_reference_method";
  reference.cache_key.reference_profile_uuid = reference.reference_profile_uuid;
  AttachCppUdr(reference);
  registry.entries.push_back(reference);

  return registry;
}

void RequireRegistryStatus(const engine::TypeOperationRegistry& registry,
                           engine::TypeOperationRegistryStatus expected,
                           std::string_view message) {
  const auto result = engine::ValidateTypeOperationRegistry(registry);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "TOR type operation registry status mismatch");
}

void RequireRegistryDescriptorStatus(
    const engine::TypeOperationRegistry& registry,
    engine::TypeOperationRegistryStatus expected,
    engine::ExecutionDataPacketStatus descriptor_status,
    std::string_view message) {
  const auto result = engine::ValidateTypeOperationRegistry(registry);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "TOR type operation registry status mismatch");
  Require(result.descriptor_status == descriptor_status,
          "TOR type operation registry descriptor status mismatch");
}

void TestValidRegistryCoversTorGates() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateTypeOperationRegistry(registry).ok(),
          "TOR rejected valid type operation registry");
  Require(engine::TypeOperationRegistryStatusName(
              engine::TypeOperationRegistryStatus::ok) == "ok",
          "TOR status names are not stable");
}

void TestRegistryRowAndRegistrationFailures() {
  auto registry = ValidRegistry();
  registry.registry_uuid = {};
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::registry_uuid_required,
      "TOR accepted registry without UUID");

  registry = ValidRegistry();
  registry.entries[1].operation_uuid = registry.entries[0].operation_uuid;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::duplicate_operation_uuid,
      "TOR accepted duplicate operation UUID");

  registry = ValidRegistry();
  registry.entries[1].stable_name = registry.entries[0].stable_name;
  registry.entries[1].operation_kind = registry.entries[0].operation_kind;
  registry.entries[1].overload.overload_key =
      registry.entries[0].overload.overload_key;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::duplicate_overload_signature,
      "TOR accepted duplicate overload signature");

  registry = ValidRegistry();
  registry.entries[0].implementation_target_present = false;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::implementation_target_missing,
      "TOR accepted active operation without implementation target");

  registry = ValidRegistry();
  registry.entries[0].non_cpp_udr_runtime_requested = true;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::non_cpp_udr_forbidden,
      "TOR accepted non-C++ UDR operation target");

  registry = ValidRegistry();
  registry.entries[0].transactional_registration = false;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::transactional_registration_required,
      "TOR accepted non-transactional operation registration");

  registry = ValidRegistry();
  registry.entries[0].row_status =
      engine::TypeOperationRegistryRowStatus::proposed;
  registry.entries[0].owner_transaction_uuid = Uuid(0xf1);
  registry.entries[0].sblr_binding.executable_outside_owner_transaction = true;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::
          proposed_row_executable_outside_owner,
      "TOR accepted proposed row executable outside owner transaction");
}

void TestOverloadAndDescriptorFailures() {
  auto registry = ValidRegistry();
  registry.entries[0].overload.ambiguity_fails_closed = false;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::
          overload_ambiguity_fail_closed_required,
      "TOR accepted overload resolution without ambiguity refusal");

  registry = ValidRegistry();
  registry.entries[0].overload.argument_descriptor_uuids.pop_back();
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::operand_descriptor_count_mismatch,
      "TOR accepted overload descriptor/UUID count mismatch");

  registry = ValidRegistry();
  registry.entries[0].overload.argument_descriptors[0].descriptor_epoch = 0;
  RequireRegistryDescriptorStatus(
      registry,
      engine::TypeOperationRegistryStatus::operand_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "TOR accepted invalid argument descriptor");

  registry = ValidRegistry();
  registry.entries[0].overload.result_descriptor_uuid = Uuid(0xf2);
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::result_descriptor_uuid_mismatch,
      "TOR accepted mismatched result descriptor UUID");
}

void TestCastAggregateWindowAndReferenceFailures() {
  auto registry = ValidRegistry();
  registry.entries[1].cast_class = engine::TypeOperationCastClass::none;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::cast_class_required,
      "TOR accepted cast operation without cast class");

  registry = ValidRegistry();
  registry.entries[1].domain_cast_rule.cast_rule_uuid = {};
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::domain_cast_descriptor_invalid,
      "TOR accepted invalid nested domain cast rule");

  registry = ValidRegistry();
  registry.entries[2].aggregate_state_present = false;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::aggregate_state_required,
      "TOR accepted aggregate without state descriptor");

  registry = ValidRegistry();
  registry.entries[2].aggregate_state.state_descriptor.descriptor_epoch = 0;
  RequireRegistryDescriptorStatus(
      registry,
      engine::TypeOperationRegistryStatus::aggregate_state_descriptor_invalid,
      engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
      "TOR accepted invalid aggregate state descriptor");

  registry = ValidRegistry();
  registry.entries[2].aggregate_state.combine_associativity_proven = false;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::aggregate_parallel_proof_required,
      "TOR accepted parallel aggregate without associativity proof");

  registry = ValidRegistry();
  registry.entries[3].window_frame.inverse_function_uuid = {};
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::window_inverse_required,
      "TOR accepted removable window frame without inverse function");

  registry = ValidRegistry();
  registry.entries[6].reference_method_name.clear();
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::reference_method_binding_invalid,
      "TOR accepted incomplete reference method binding");
}

void TestUdrLlvmSblrAndCacheFailures() {
  auto registry = ValidRegistry();
  registry.entries[4].cpp_udr_hook.present = false;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::cpp_udr_hook_required,
      "TOR accepted C++ UDR operation without hook");

  registry = ValidRegistry();
  registry.entries[4].cpp_udr_bridge_admitted = false;
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::cpp_udr_bridge_admission_required,
      "TOR accepted C++ UDR operation without bridge admission");

  registry = ValidRegistry();
  registry.entries[5].llvm_fallback_matches = false;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::llvm_fallback_mismatch,
      "TOR accepted LLVM operation with mismatched fallback");

  registry = ValidRegistry();
  registry.entries[0].sblr_binding.definition_hash = "stale";
  RequireRegistryStatus(
      registry,
      engine::TypeOperationRegistryStatus::
          sblr_binding_definition_hash_mismatch,
      "TOR accepted stale SBLR operation binding");

  registry = ValidRegistry();
  registry.entries[0].sblr_binding.runtime_security_recheck = false;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::sblr_binding_recheck_required,
      "TOR accepted SBLR binding without runtime admission recheck");

  registry = ValidRegistry();
  registry.entries[5].cache_key.llvm_artifact_uuid = {};
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::cache_key_llvm_missing,
      "TOR accepted LLVM operation cache key without artifact identity");
}

void TestDiagnosticsAndMetricsFailures() {
  auto registry = ValidRegistry();
  registry.entries[0].row_status =
      engine::TypeOperationRegistryRowStatus::deferred;
  registry.entries[0].implementation_target_present = false;
  registry.entries[0].implementation_target =
      engine::TypeOperationImplementationTarget::none;
  registry.entries[0].status_execution_refusal_diagnostic = true;
  registry.entries[0].diagnostics.clear();
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::diagnostic_vector_required,
      "TOR accepted deferred row without diagnostic vector");

  registry.entries[0].diagnostics.push_back(
      Diagnostic(registry.entries[0], "TYPE_OPERATION.STATUS_NOT_EXECUTABLE"));
  Require(engine::ValidateTypeOperationRegistry(registry).ok(),
          "TOR rejected deferred row with deterministic diagnostic");

  registry = ValidRegistry();
  registry.entries[0].diagnostics.push_back(Diagnostic(registry.entries[0], ""));
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::diagnostic_code_required,
      "TOR accepted diagnostic vector without code");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::local_metric_missing,
      "TOR accepted registry missing required local metric");

  registry = ValidRegistry();
  registry.cluster_metrics_guarded_by_cluster_governance = false;
  RequireRegistryStatus(
      registry, engine::TypeOperationRegistryStatus::cluster_metrics_guard_required,
      "TOR accepted cluster metrics without cluster governance guard");
}

}  // namespace

int main() {
  TestValidRegistryCoversTorGates();
  TestRegistryRowAndRegistrationFailures();
  TestOverloadAndDescriptorFailures();
  TestCastAggregateWindowAndReferenceFailures();
  TestUdrLlvmSblrAndCacheFailures();
  TestDiagnosticsAndMetricsFailures();
  std::cout << "type_operation_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
