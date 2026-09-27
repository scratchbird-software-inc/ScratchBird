// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "management/memory_management_api.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = scratchbird::engine::internal_api;
namespace disk = scratchbird::storage::disk;
namespace mem = scratchbird::core::memory;

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void RequireOk(const engine::EngineApiResult& result,
               std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

bool HasEvidence(const engine::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const engine::EngineApiResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "failed to open expected memory catalog");
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

struct MemoryCatalogRecord {
  std::vector<std::string> key;
  std::vector<std::pair<std::string, std::string>> identities;
  std::string attributes;
};

std::vector<MemoryCatalogRecord> ReadMemoryCatalog(
    const std::filesystem::path& path, std::size_t expected_count) {
  const auto bytes = ReadBytes(path);
  std::vector<std::string> frames;
  Require(engine::DecodeMgaMetadataStream(
              {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
              &frames),
          "memory catalog binary stream framing/checksum invalid");
  Require(frames.size() == expected_count, "memory catalog record count mismatch");
  std::vector<MemoryCatalogRecord> records;
  for (const auto& frame : frames) {
    std::vector<std::string> fields;
    Require(engine::DecodeMgaMetadataFields(frame, &fields) &&
                fields.size() == 4 && fields[0] == "memory.catalog.v2",
            "memory catalog binary record format mismatch");
    MemoryCatalogRecord record;
    Require(engine::DecodeMgaMetadataFields(fields[1], &record.key),
            "memory catalog binary key invalid");
    Require(engine::DecodeMetadataPairs(fields[2], &record.identities),
            "memory catalog binary identity fields invalid");
    record.attributes = std::move(fields[3]);
    Require(record.attributes.find("_uuid=") == std::string::npos,
            "memory catalog retained a textual UUID shadow");
    records.push_back(std::move(record));
  }
  return records;
}

void RequireUuidBytes(std::string_view bytes, const engine::EngineUuid& expected) {
  Require(bytes.size() == 16 &&
              std::equal(expected.bytes.begin(), expected.bytes.end(),
                         reinterpret_cast<const std::uint8_t*>(bytes.data())),
          "memory catalog UUID must be exact binary16 with no text shadow");
}

void RequireIdentity(const MemoryCatalogRecord& record, std::string_view name,
                     const engine::EngineUuid& expected) {
  Require(record.identities.size() == 1 && record.identities[0].first == name,
          "memory catalog identity field mismatch");
  RequireUuidBytes(record.identities[0].second, expected);
}

void RequireAttribute(const MemoryCatalogRecord& record, std::string_view name,
                      std::string_view expected) {
  // Match complete fields, not a substring in an unrelated value or record.
  std::string_view remaining = record.attributes;
  std::size_t matches = 0;
  while (!remaining.empty()) {
    const auto delimiter = remaining.find('|');
    const auto field = remaining.substr(0, delimiter);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && field.substr(0, equals) == name) {
      ++matches;
      Require(field.substr(equals + 1) == expected,
              "memory catalog attribute value mismatch");
    }
    if (delimiter == std::string_view::npos) break;
    remaining.remove_prefix(delimiter + 1);
  }
  Require(matches == 1, "memory catalog attribute missing or duplicated");
}

void RequireCatalogReplayAndCorruptionRefusal(
    const std::filesystem::path& path,
    const engine::EngineMemoryManagementRequest& request,
    std::size_t expected_count) {
  const auto original = ReadBytes(path);
  const auto replay = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(replay, "memory catalog binary replay/upsert failed");
  Require(replay.durable_catalog_record_count == expected_count &&
              ReadBytes(path) == original,
          "memory catalog upsert duplicated or changed existing records");

  auto write = [&](const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Require(static_cast<bool>(out), "memory catalog corruption setup failed");
  };
  auto corrupt_checksum = original;
  Require(!corrupt_checksum.empty(), "memory catalog cannot be empty");
  corrupt_checksum.back() ^= 1;
  std::vector<std::string> corruptions{
           "format=ScratchBirdMemoryPolicyCatalog|version=1\n",
           original.substr(0, original.size() - 1), original + '\0',
           corrupt_checksum};
  std::vector<std::string> frames, fields, key;
  Require(engine::DecodeMgaMetadataStream(
              {reinterpret_cast<const std::uint8_t*>(original.data()), original.size()},
              &frames) && !frames.empty() &&
              engine::DecodeMgaMetadataFields(frames.front(), &fields) &&
              fields.size() == 4 && engine::DecodeMgaMetadataFields(fields[1], &key) &&
              key.size() >= 3,
          "memory catalog semantic corruption setup failed");
  auto non_v7 = key[1];
  Require(non_v7.size() == 16, "memory catalog original node UUID width invalid");
  non_v7[6] = static_cast<char>((static_cast<unsigned char>(non_v7[6]) & 15) | 0x40);
  for (const auto& invalid : std::vector<std::string>{
           "019e4000-0000-7000-8000-000000000001", std::string(16, '\0'), non_v7}) {
    key[1] = invalid;
    fields[1] = engine::EncodeMgaMetadataFields(key);
    auto corrupt = engine::EncodeMgaMetadataFields(fields);
    for (std::size_t n = 1; n < frames.size(); ++n) corrupt += frames[n];
    corruptions.push_back(std::move(corrupt));
  }
  for (const auto& corrupt : corruptions) {
    write(corrupt);
    const auto refused = engine::EnginePlanMemoryManagementOperation(request);
    Require(!refused.ok &&
                HasDiagnostic(refused, "MEMORY.CATALOG_READ_FAILED") &&
                !refused.durable_state_changed && ReadBytes(path) == corrupt,
            "memory catalog accepted or overwrote corrupt/legacy storage");
  }
  write(original);
  RequireOk(engine::EnginePlanMemoryManagementOperation(request),
            "memory catalog replay failed after restoring original bytes");
}

engine::EngineRequestContext Context(bool control = false,
                                     bool cluster_authority = false) {
  engine::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000001");
  context.transaction_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000002");
  context.local_transaction_id = control ? 8101 : 0;
  context.resource_epoch = 12;
  context.security_epoch = 34;
  context.cluster_authority_available = cluster_authority;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  if (control) {
    context.trace_tags.push_back("right:OBS_CONFIG_CONTROL");
  }
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "memory_management_descriptor_conformance",
      control ? std::initializer_list<std::string_view>{"OBS_CONFIG_INSPECT",
                                                        "OBS_CONFIG_CONTROL"}
              : std::initializer_list<std::string_view>{"OBS_CONFIG_INSPECT"});
  return context;
}

mem::MemoryPolicyConfig PolicyConfig() {
  mem::MemoryPolicyConfig config;
  config.policy_name = "memory_descriptor_gate_policy";
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = 192ull * kMiB;
  config.per_context_limit_bytes = 64ull * kMiB;
  config.page_buffer_pool_limit_bytes = 64ull * kMiB;
  config.enable_platform_memory_probe = false;
  config.policy_generation = 7;
  return config;
}

mem::MemoryPressureObservation PressureObservation() {
  mem::MemoryPressureObservation observation;
  observation.route_label = "memory.management.descriptor_gate";
  observation.operation_id = "MEMORY-MANAGEMENT-DESCRIPTOR-PLANNER";
  observation.current_bytes = 900;
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.unified_budget_bytes = 900;
  observation.unified_budget_limit_bytes = 1000;
  observation.spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.background_cleanup_supported = true;
  observation.cancellation_supported = true;
  observation.engine_mga_authoritative = true;
  return observation;
}

void FillGovernance(engine::EngineMemoryManagementRequest* request) {
  request->governance.profile_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000010");
  request->governance.policy_config = PolicyConfig();
  request->governance.expected_policy_generation = 7;
  request->governance.observed_policy_generation = 7;
  request->governance.profile_resolved = true;
  request->governance.memory_tree_snapshot_present = true;
  request->governance.cache_governor_registered = true;
  request->governance.pressure_observation_present = true;
  request->governance.pressure_observation = PressureObservation();
  request->governance.grant_feedback_surface_present = true;
  request->governance.parser_front_door_limit_surface_present = true;
  request->governance.udr_limit_surface_present = true;
  request->governance.streaming_window_surface_present = true;
  request->governance.maintenance_budget_surface_present = true;
  request->governance.dump_swap_policy_present = true;
  request->governance.allocator_scavenging_surface_present = true;
  request->governance.platform_capability_matrix_present = true;
  request->governance.protected_material_redaction_validated = true;
  request->governance.activation_timing_declared = true;
  request->governance.current_snapshot.current_bytes = 128ull * kMiB;
  request->governance.support_bundle_request.bundle_profile =
      "memory.management.descriptor_gate.v1";
  request->governance.support_bundle_request.snapshot =
      request->governance.current_snapshot;
  request->governance.support_bundle_request.allow_protected_material = false;
  request->governance.support_bundle_request.exclude_protected_material = true;
}

engine::EngineMemoryManagementRequest GovernanceRequest(
    engine::EngineMemoryManagementOperation operation,
    bool control = false) {
  engine::EngineMemoryManagementRequest request;
  request.context = Context(control);
  request.memory_operation = operation;
  FillGovernance(&request);
  if (operation == engine::EngineMemoryManagementOperation::plan_cache_control) {
    request.governance.cache_flush_or_invalidation_requested = true;
  }
  return request;
}

void FillAutomation(engine::EngineMemoryManagementRequest* request) {
  request->automation.recommendation_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000020");
  request->automation.report_generation = 3;
  request->automation.recommendation_generation = 4;
  request->automation.report_bounded = true;
  request->automation.report_redaction_validated = true;
  request->automation.metrics_contract_present = true;
  request->automation.recommendation_explainable = true;
  request->automation.recommend_only_default = true;
  request->automation.safe_apply_requested = true;
  request->automation.maintenance_window_bound = true;
  request->automation.audit_enabled = true;
  request->automation.guardrail_policy_resolved = true;
}

engine::EngineMemoryManagementRequest AutomationRequest(
    engine::EngineMemoryManagementOperation operation,
    bool control = false) {
  engine::EngineMemoryManagementRequest request;
  request.context = Context(control);
  request.memory_operation = operation;
  FillAutomation(&request);
  return request;
}

void FillResidency(engine::EngineMemoryManagementRequest* request) {
  request->object_residency.object_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000030");
  request->object_residency.filespace_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000031");
  request->object_residency.object_kind = "table";
  request->object_residency.residency_class =
      engine::EngineMemoryObjectResidencyClass::warm_on_open;
  request->object_residency.page_types = {disk::PageType::row_data,
                                          disk::PageType::index_btree_leaf};
  request->object_residency.expected_policy_generation = 7;
  request->object_residency.observed_policy_generation = 7;
  request->object_residency.warmup_budget_bytes = 16ull * kMiB;
  request->object_residency.profile_resolved = true;
  request->object_residency.object_resolved = true;
  request->object_residency.filespace_placement_validated = true;
  request->object_residency.security_scope_validated = true;
  request->object_residency.cluster_placement_validated = true;
  request->object_residency.heat_history_derivative_only = true;
}

engine::EngineMemoryManagementRequest ResidencyRequest(bool control = false,
                                                       bool cluster = false) {
  engine::EngineMemoryManagementRequest request;
  request.context = Context(control, cluster);
  request.memory_operation = control
                                 ? engine::EngineMemoryManagementOperation::set_object_residency
                                 : engine::EngineMemoryManagementOperation::inspect_object_residency;
  request.cluster_scoped = cluster;
  FillResidency(&request);
  return request;
}

void FillRateLimit(engine::EngineMemoryManagementRequest* request) {
  request->rate_limit.limit_class =
      engine::EngineMemoryRateLimitClass::cache_flush_abuse;
  request->rate_limit.action = engine::EngineMemoryRateLimitAction::throttle;
  request->rate_limit.limit_per_window = 4;
  request->rate_limit.window_seconds = 60;
  request->rate_limit.policy_generation = 7;
  request->rate_limit.policy_resolved = true;
  request->rate_limit.audit_enabled = true;
}

engine::EngineMemoryManagementRequest RateLimitRequest(bool control = false) {
  engine::EngineMemoryManagementRequest request;
  request.context = Context(control);
  request.memory_operation = control
                                 ? engine::EngineMemoryManagementOperation::set_rate_limit
                                 : engine::EngineMemoryManagementOperation::inspect_rate_limit;
  FillRateLimit(&request);
  return request;
}

void FillMigration(engine::EngineMemoryManagementRequest* request) {
  request->migration.profile_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000040");
  request->migration.policy_uuid = scratchbird::tests::FixtureUuidLiteral("019e4000-0000-7000-8000-000000000041");
  request->migration.source_policy_version = 2;
  request->migration.target_policy_version = 3;
  request->migration.source_schema_version = 2;
  request->migration.target_schema_version = 3;
  request->migration.policy_schema_validated = true;
  request->migration.grant_feedback_migration_declared = true;
  request->migration.heat_history_migration_declared = true;
  request->migration.derivative_state_audit_enabled = true;
  request->migration.discard_incompatible_derivative_state_allowed = true;
}

engine::EngineMemoryManagementRequest MigrationRequest(
    engine::EngineMemoryManagementOperation operation,
    bool control = false) {
  engine::EngineMemoryManagementRequest request;
  request.context = Context(control);
  request.memory_operation = operation;
  FillMigration(&request);
  return request;
}

void RequireDescriptorSuccess(const engine::EngineMemoryManagementResult& result,
                              std::string_view family) {
  if (!result.ok) {
    std::string detail = "memory descriptor planner returned failure";
    if (!result.diagnostics.empty()) {
      detail += ":";
      detail += result.diagnostics.front().code;
      detail += ":";
      detail += result.diagnostics.front().detail;
    }
    Fail(detail);
  }
  Require(result.result_shape.result_kind ==
              "rs.memory.management.descriptor_plan.v1",
          "memory descriptor result shape mismatch");
  Require(!result.durable_state_changed,
          "memory descriptor plan mutated durable state");
  Require(!result.parser_memory_authority,
          "memory descriptor plan gave parser memory authority");
  Require(!result.transaction_finality_authority,
          "memory descriptor plan claimed transaction finality authority");
  Require(!result.recovery_authority,
          "memory descriptor plan claimed recovery authority");
  Require(!result.reference_or_wal_recovery_authority,
          "memory descriptor plan claimed reference/WAL recovery authority");
  Require(!result.private_provider_dispatch,
          "memory descriptor plan dispatched private provider");
  Require(!result.physical_action_dispatched,
          "memory descriptor plan dispatched physical action");
  Require(HasEvidence(result, "memory_management_family", family),
          "memory descriptor family evidence missing");
  Require(HasEvidence(result, "mga_visibility_authority",
                      "durable_transaction_inventory"),
          "memory descriptor omitted MGA authority boundary evidence");
}

void TestGovernanceDescriptorSurfaces() {
  auto inspect = engine::EnginePlanMemoryManagementOperation(
      GovernanceRequest(engine::EngineMemoryManagementOperation::inspect_governance));
  RequireDescriptorSuccess(inspect, "governance");

  auto pressure = engine::EnginePlanMemoryManagementOperation(
      GovernanceRequest(engine::EngineMemoryManagementOperation::plan_pressure_response));
  RequireDescriptorSuccess(pressure, "governance");

  auto cache = engine::EnginePlanMemoryManagementOperation(
      GovernanceRequest(engine::EngineMemoryManagementOperation::plan_cache_control,
                        true));
  RequireDescriptorSuccess(cache, "governance");
  Require(cache.cache_invalidation_planned,
          "cache control plan did not mark invalidation planned");

  auto physical = GovernanceRequest(
      engine::EngineMemoryManagementOperation::validate_governance);
  physical.governance.physical_allocator_action_requested = true;
  const auto physical_result =
      engine::EnginePlanMemoryManagementOperation(physical);
  Require(!physical_result.ok, "memory governance accepted physical allocator action");
  Require(HasDiagnostic(
              physical_result,
              "MEMORY.PHYSICAL_ALLOCATOR_ACTION_NOT_AVAILABLE_IN_DESCRIPTOR_PLAN"),
          "memory governance physical action diagnostic mismatch");
}

void TestGovernanceDirectExecutorSurfaces() {
  auto unauthorized = GovernanceRequest(
      engine::EngineMemoryManagementOperation::plan_cache_control,
      true);
  unauthorized.governance.cache_control_execution_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory governance cache executor accepted missing authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.CACHE_CONTROL_EXECUTION_AUTHORITY_REQUIRED"),
          "memory governance cache executor authority diagnostic mismatch");

  auto missing_scavenge = GovernanceRequest(
      engine::EngineMemoryManagementOperation::plan_cache_control,
      true);
  missing_scavenge.governance.allocator_scavenging_surface_present = false;
  missing_scavenge.governance.allocator_scavenging_execution_requested = true;
  missing_scavenge.governance.allocator_scavenging_execution_authorized = true;
  const auto missing_scavenge_result =
      engine::EnginePlanMemoryManagementOperation(missing_scavenge);
  Require(!missing_scavenge_result.ok,
          "memory governance accepted allocator scavenging without surface");
  Require(HasDiagnostic(missing_scavenge_result, "SB_ENGINE_API_INVALID_REQUEST"),
          "memory governance allocator surface diagnostic mismatch");

  auto request = GovernanceRequest(
      engine::EngineMemoryManagementOperation::plan_cache_control,
      true);
  request.governance.cache_control_execution_requested = true;
  request.governance.cache_control_execution_authorized = true;
  request.governance.allocator_scavenging_execution_requested = true;
  request.governance.allocator_scavenging_execution_authorized = true;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory governance direct executor failed");
  Require(!result.durable_state_changed,
          "memory governance direct executor mutated durable state");
  Require(result.cache_control_executed,
          "memory governance cache executor flag missing");
  Require(result.cache_flush_executed,
          "memory governance cache flush flag missing");
  Require(result.cache_invalidation_executed,
          "memory governance cache invalidation flag missing");
  Require(result.allocator_scavenging_executed,
          "memory governance allocator scavenging flag missing");
  Require(result.allocator_scavenging_reclaimed_bytes == 12288,
          "memory governance allocator scavenging bytes mismatch");
  Require(!result.physical_action_dispatched,
          "memory governance direct executor dispatched physical action");
  Require(HasEvidence(result,
                      "memory_cache_control_executor",
                      "flush_invalidate_executed"),
          "memory cache control executor evidence missing");
  Require(HasEvidence(result,
                      "memory_allocator_scavenging_executor",
                      "background_reclamation_executed"),
          "memory allocator scavenging executor evidence missing");
  Require(HasEvidence(result,
                      "memory_allocator_scavenging_reclaimed_bytes",
                      "12288"),
          "memory allocator scavenging byte evidence missing");
}

void TestAutomationDescriptorSurfaces() {
  auto report = engine::EnginePlanMemoryManagementOperation(
      AutomationRequest(engine::EngineMemoryManagementOperation::create_report));
  RequireDescriptorSuccess(report, "automation");
  Require(report.report_materialized,
          "memory report descriptor did not mark report materialization");

  auto apply = engine::EnginePlanMemoryManagementOperation(
      AutomationRequest(engine::EngineMemoryManagementOperation::apply_safe_recommendation,
                        true));
  RequireDescriptorSuccess(apply, "automation");

  auto unsafe = AutomationRequest(
      engine::EngineMemoryManagementOperation::review_recommendation);
  unsafe.automation.unsafe_action_requested = true;
  const auto unsafe_result = engine::EnginePlanMemoryManagementOperation(unsafe);
  Require(!unsafe_result.ok, "memory automation accepted unsafe recommendation");
  Require(HasDiagnostic(unsafe_result,
                        "MEMORY.AUTOMATION_UNSAFE_ACTION_REFUSED"),
          "memory automation unsafe diagnostic mismatch");
}

void TestAutomationDurableReportCatalogPersistence() {
  const auto database_path =
      std::filesystem::temp_directory_path() /
      "scratchbird_memory_report_catalog_gate.sbdb";
  const auto catalog_path =
      std::filesystem::path(database_path.string() +
                            ".sb.sys.memory_report_catalog");
  std::error_code ignored;
  std::filesystem::remove(catalog_path, ignored);

  auto unauthorized =
      AutomationRequest(engine::EngineMemoryManagementOperation::create_report,
                        true);
  unauthorized.context.database_path = database_path.string();
  unauthorized.automation.durable_report_catalog_persistence_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory report catalog persistence accepted missing authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.REPORT_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED"),
          "memory report catalog authority diagnostic mismatch");

  auto request =
      AutomationRequest(engine::EngineMemoryManagementOperation::create_report,
                        true);
  request.context.database_path = database_path.string();
  request.automation.durable_report_catalog_persistence_requested = true;
  request.automation.durable_report_catalog_persistence_authorized = true;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory report catalog persistence failed");
  Require(result.durable_state_changed,
          "memory report catalog did not mark durable state changed");
  Require(result.memory_report_catalog_persisted,
          "memory report catalog persisted flag missing");
  Require(result.report_materialized,
          "memory report catalog lost report materialization flag");
  Require(!result.physical_action_dispatched,
          "memory report catalog persistence dispatched physical action");
  Require(HasEvidence(result,
                      "memory_report_catalog_persisted",
                      "sys.memory_report_catalog"),
          "memory report catalog evidence missing");

  const auto catalog = ReadMemoryCatalog(catalog_path, 1).front();
  Require(catalog.key.size() == 3 && catalog.key[0] == "sys.memory_report_catalog" &&
              catalog.key[2] == "3", "memory report catalog key mismatch");
  RequireUuidBytes(catalog.key[1], request.context.database_uuid);
  RequireIdentity(catalog, "recommendation_uuid", request.automation.recommendation_uuid);
  RequireAttribute(catalog, "report_bounded", "true");
  RequireAttribute(catalog, "report_redaction_validated", "true");
  RequireAttribute(catalog, "metrics_contract_present", "true");
  RequireCatalogReplayAndCorruptionRefusal(catalog_path, request, 1);
}

void TestAutomationSafeExecutorSurfaces() {
  auto unauthorized =
      AutomationRequest(engine::EngineMemoryManagementOperation::apply_safe_recommendation,
                        true);
  unauthorized.automation.safe_automation_execution_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory safe automation accepted missing execution authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.SAFE_AUTOMATION_EXECUTION_AUTHORITY_REQUIRED"),
          "memory safe automation authority diagnostic mismatch");

  auto request =
      AutomationRequest(engine::EngineMemoryManagementOperation::apply_safe_recommendation,
                        true);
  request.automation.safe_automation_execution_requested = true;
  request.automation.safe_automation_execution_authorized = true;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory safe automation executor failed");
  Require(!result.durable_state_changed,
          "memory safe automation executor mutated durable state");
  Require(result.memory_safe_automation_executed,
          "memory safe automation executed flag missing");
  Require(result.memory_safe_automation_audit_emitted,
          "memory safe automation audit flag missing");
  Require(result.cache_invalidation_planned,
          "memory safe automation did not plan cache invalidation");
  Require(!result.physical_action_dispatched,
          "memory safe automation dispatched physical action");
  Require(HasEvidence(result,
                      "memory_safe_automation_executor",
                      "safe_recommendation_applied"),
          "memory safe automation executor evidence missing");
  Require(HasEvidence(result,
                      "memory_safe_automation_audit",
                      "emitted"),
          "memory safe automation audit evidence missing");
}

void TestObjectResidencyDescriptorSurfaces() {
  auto inspect =
      engine::EnginePlanMemoryManagementOperation(ResidencyRequest(false));
  RequireDescriptorSuccess(inspect, "object_residency");

  auto set = engine::EnginePlanMemoryManagementOperation(ResidencyRequest(true));
  RequireDescriptorSuccess(set, "object_residency");

  auto cluster = ResidencyRequest(false, true);
  cluster.context.cluster_authority_available = false;
  const auto cluster_result =
      engine::EnginePlanMemoryManagementOperation(cluster);
  Require(!cluster_result.ok,
          "memory residency accepted missing cluster authority");
  Require(HasDiagnostic(cluster_result, "MEMORY.CLUSTER_AUTHORITY_REQUIRED"),
          "memory residency cluster authority diagnostic mismatch");

  auto physical = ResidencyRequest(false);
  physical.object_residency.physical_prefetch_requested = true;
  const auto physical_result =
      engine::EnginePlanMemoryManagementOperation(physical);
  Require(!physical_result.ok, "memory residency accepted physical prefetch");
  Require(HasDiagnostic(
              physical_result,
              "MEMORY.RESIDENCY_PHYSICAL_PREFETCH_NOT_AVAILABLE_IN_DESCRIPTOR_PLAN"),
          "memory residency physical prefetch diagnostic mismatch");
}

void TestObjectResidencyDurableCatalogPersistence() {
  const auto database_path =
      std::filesystem::temp_directory_path() /
      "scratchbird_memory_object_residency_catalog_gate.sbdb";
  const auto catalog_path =
      std::filesystem::path(database_path.string() +
                            ".sb.sys.memory_object_residency_policy");
  std::error_code ignored;
  std::filesystem::remove(catalog_path, ignored);

  auto unauthorized = ResidencyRequest(true);
  unauthorized.context.database_path = database_path.string();
  unauthorized.object_residency.durable_catalog_persistence_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory residency catalog persistence accepted missing authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.RESIDENCY_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED"),
          "memory residency catalog authority diagnostic mismatch");

  auto request = ResidencyRequest(true);
  request.context.database_path = database_path.string();
  request.object_residency.durable_catalog_persistence_requested = true;
  request.object_residency.durable_catalog_persistence_authorized = true;
  request.object_residency.restart_warmup_manifest_persistence_requested = true;
  request.object_residency.restart_warmup_manifest_persistence_authorized = true;
  request.object_residency.heat_history_generation = 44;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory residency catalog persistence failed");
  Require(result.durable_state_changed,
          "memory residency catalog did not mark durable state changed");
  Require(result.memory_object_residency_catalog_persisted,
          "memory residency catalog persisted flag missing");
  Require(result.memory_object_residency_restart_warmup_persisted,
          "memory residency restart warmup manifest flag missing");
  Require(!result.physical_action_dispatched,
          "memory residency catalog persistence dispatched physical action");
  Require(HasEvidence(result,
                      "memory_object_residency_catalog_persisted",
                      "sys.memory_object_residency_policy"),
          "memory residency catalog evidence missing");
  Require(HasEvidence(result,
                      "memory_object_residency_restart_warmup_manifest",
                      "persisted"),
          "memory residency restart warmup evidence missing");

  const auto catalog = ReadMemoryCatalog(catalog_path, 1).front();
  Require(catalog.key.size() == 3 && catalog.key[0] == "sys.memory_object_residency_policy",
          "memory residency catalog key mismatch");
  RequireUuidBytes(catalog.key[1], request.context.database_uuid);
  RequireUuidBytes(catalog.key[2], request.object_residency.object_uuid);
  RequireIdentity(catalog, "filespace_uuid", request.object_residency.filespace_uuid);
  RequireAttribute(catalog, "residency_class", "warm_on_open");
  RequireAttribute(catalog, "page_types", "row_data,index_btree_leaf");
  RequireAttribute(catalog, "restart_warmup_manifest_persisted", "true");
  RequireAttribute(catalog, "heat_history_generation", "44");
  RequireCatalogReplayAndCorruptionRefusal(catalog_path, request, 1);
}

void TestRateLimitDescriptorSurfaces() {
  auto set = engine::EnginePlanMemoryManagementOperation(RateLimitRequest(true));
  RequireDescriptorSuccess(set, "rate_limit");

  auto integrity = RateLimitRequest(true);
  integrity.rate_limit.limit_class =
      engine::EngineMemoryRateLimitClass::integrity_or_corruption_signal;
  integrity.rate_limit.integrity_event = true;
  integrity.rate_limit.action = engine::EngineMemoryRateLimitAction::throttle;
  const auto integrity_result =
      engine::EnginePlanMemoryManagementOperation(integrity);
  Require(!integrity_result.ok,
          "memory rate limit hid integrity/corruption event");
  Require(HasDiagnostic(
              integrity_result,
              "MEMORY.RATE_LIMIT_INTEGRITY_SIGNAL_MUST_NOT_HIDE_EVENT"),
          "memory rate limit integrity diagnostic mismatch");
}

void TestRateLimitDurableCatalogPersistence() {
  const auto database_path =
      std::filesystem::temp_directory_path() /
      "scratchbird_memory_rate_limit_catalog_gate.sbdb";
  const auto catalog_path =
      std::filesystem::path(database_path.string() +
                            ".sb.sys.memory_rate_limit_policy");
  std::error_code ignored;
  std::filesystem::remove(catalog_path, ignored);

  auto unauthorized = RateLimitRequest(true);
  unauthorized.context.database_path = database_path.string();
  unauthorized.rate_limit.durable_catalog_persistence_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory rate-limit catalog persistence accepted missing authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.RATE_LIMIT_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED"),
          "memory rate-limit catalog authority diagnostic mismatch");

  auto request = RateLimitRequest(true);
  request.context.database_path = database_path.string();
  request.rate_limit.durable_catalog_persistence_requested = true;
  request.rate_limit.durable_catalog_persistence_authorized = true;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory rate-limit catalog persistence failed");
  Require(result.durable_state_changed,
          "memory rate-limit catalog did not mark durable state changed");
  Require(result.memory_rate_limit_catalog_persisted,
          "memory rate-limit catalog persisted flag missing");
  Require(!result.physical_action_dispatched,
          "memory rate-limit catalog persistence dispatched physical action");
  Require(HasEvidence(result,
                      "memory_rate_limit_catalog_persisted",
                      "sys.memory_rate_limit_policy"),
          "memory rate-limit catalog evidence missing");

  const auto catalog = ReadMemoryCatalog(catalog_path, 1).front();
  Require(catalog.key.size() == 3 && catalog.key[0] == "sys.memory_rate_limit_policy" &&
              catalog.key[2] == "cache_flush_abuse", "memory rate-limit catalog key mismatch");
  RequireUuidBytes(catalog.key[1], request.context.database_uuid);
  Require(catalog.identities.empty(), "memory rate-limit has unexpected identity fields");
  RequireAttribute(catalog, "action", "throttle");
  RequireAttribute(catalog, "limit_per_window", "4");
  RequireAttribute(catalog, "window_seconds", "60");
  RequireCatalogReplayAndCorruptionRefusal(catalog_path, request, 1);
}

void TestRateLimitLiveExecutorSurfaces() {
  auto unauthorized = RateLimitRequest(true);
  unauthorized.rate_limit.live_executor_evaluation_requested = true;
  unauthorized.rate_limit.observed_count_in_window = 8;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory rate-limit live executor accepted missing authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.RATE_LIMIT_LIVE_EXECUTOR_AUTHORITY_REQUIRED"),
          "memory rate-limit live executor authority diagnostic mismatch");

  auto throttle = RateLimitRequest(true);
  throttle.rate_limit.live_executor_evaluation_requested = true;
  throttle.rate_limit.live_executor_evaluation_authorized = true;
  throttle.rate_limit.observed_count_in_window = 8;
  const auto throttle_result =
      engine::EnginePlanMemoryManagementOperation(throttle);
  RequireOk(throttle_result, "memory rate-limit throttle executor failed");
  Require(throttle_result.memory_rate_limit_live_executor_evaluated,
          "memory rate-limit live executor flag missing");
  Require(throttle_result.memory_rate_limit_throttle_executed,
          "memory rate-limit throttle flag missing");
  Require(!throttle_result.memory_rate_limit_refuse_executed,
          "memory rate-limit throttle also refused");
  Require(throttle_result.memory_rate_limit_audit_emitted,
          "memory rate-limit throttle did not emit audit");
  Require(!throttle_result.physical_action_dispatched,
          "memory rate-limit live executor dispatched physical action");
  Require(HasEvidence(throttle_result,
                      "memory_rate_limit_live_executor",
                      "throttle_executed"),
          "memory rate-limit throttle evidence missing");
  Require(HasEvidence(throttle_result,
                      "memory_rate_limit_observed_count",
                      "8"),
          "memory rate-limit observed-count evidence missing");

  auto refuse = RateLimitRequest(true);
  refuse.rate_limit.action = engine::EngineMemoryRateLimitAction::refuse;
  refuse.rate_limit.live_executor_evaluation_requested = true;
  refuse.rate_limit.live_executor_evaluation_authorized = true;
  refuse.rate_limit.observed_count_in_window = 9;
  const auto refuse_result =
      engine::EnginePlanMemoryManagementOperation(refuse);
  RequireOk(refuse_result, "memory rate-limit refuse executor failed");
  Require(refuse_result.memory_rate_limit_refuse_executed,
          "memory rate-limit refuse flag missing");
  Require(!refuse_result.memory_rate_limit_throttle_executed,
          "memory rate-limit refuse also throttled");
  Require(refuse_result.memory_rate_limit_audit_emitted,
          "memory rate-limit refuse did not emit audit");
  Require(HasEvidence(refuse_result,
                      "memory_rate_limit_live_executor",
                      "refuse_executed"),
          "memory rate-limit refuse evidence missing");

  auto integrity = RateLimitRequest(true);
  integrity.rate_limit.limit_class =
      engine::EngineMemoryRateLimitClass::integrity_or_corruption_signal;
  integrity.rate_limit.action = engine::EngineMemoryRateLimitAction::audit_only;
  integrity.rate_limit.integrity_event = true;
  integrity.rate_limit.live_executor_evaluation_requested = true;
  integrity.rate_limit.live_executor_evaluation_authorized = true;
  integrity.rate_limit.observed_count_in_window = 1;
  const auto integrity_result =
      engine::EnginePlanMemoryManagementOperation(integrity);
  RequireOk(integrity_result,
            "memory rate-limit integrity audit executor failed");
  Require(integrity_result.memory_rate_limit_audit_emitted,
          "memory rate-limit integrity signal did not emit audit");
  Require(!integrity_result.memory_rate_limit_throttle_executed &&
              !integrity_result.memory_rate_limit_refuse_executed,
          "memory rate-limit integrity signal was hidden by throttle/refuse");
  Require(HasEvidence(integrity_result,
                      "memory_rate_limit_integrity_signal",
                      "audit_preserved"),
          "memory rate-limit integrity audit evidence missing");
}

void TestPolicyMigrationDescriptorSurfaces() {
  auto upgrade = engine::EnginePlanMemoryManagementOperation(
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_upgrade));
  RequireDescriptorSuccess(upgrade, "policy_upgrade");
  Require(upgrade.policy_migration_planned,
          "memory policy upgrade did not mark migration planned");

  auto migration = engine::EnginePlanMemoryManagementOperation(
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_migration,
                       true));
  RequireDescriptorSuccess(migration, "policy_upgrade");

  auto downgrade = MigrationRequest(
      engine::EngineMemoryManagementOperation::plan_policy_upgrade);
  downgrade.migration.downgrade_requested = true;
  downgrade.migration.source_policy_version = 4;
  downgrade.migration.target_policy_version = 3;
  downgrade.migration.downgrade_compatibility_validated = false;
  const auto downgrade_result =
      engine::EnginePlanMemoryManagementOperation(downgrade);
  Require(!downgrade_result.ok, "memory policy downgrade was accepted");
  Require(HasDiagnostic(downgrade_result, "MEMORY.POLICY_DOWNGRADE_REFUSED"),
          "memory policy downgrade diagnostic mismatch");

  auto derivative = MigrationRequest(
      engine::EngineMemoryManagementOperation::plan_policy_upgrade);
  derivative.migration.derivative_state_audit_enabled = false;
  const auto derivative_result =
      engine::EnginePlanMemoryManagementOperation(derivative);
  Require(!derivative_result.ok,
          "memory policy migration accepted unaudited derivative state");
  Require(HasDiagnostic(derivative_result,
                        "MEMORY.DERIVATIVE_STATE_MIGRATION_AUDIT_REQUIRED"),
          "memory derivative migration diagnostic mismatch");
}

void TestPolicyMigrationDurableCatalogPersistence() {
  const auto database_path =
      std::filesystem::temp_directory_path() /
      "scratchbird_memory_policy_migration_catalog_gate.sbdb";
  const auto catalog_path =
      std::filesystem::path(database_path.string() +
                            ".sb.sys.memory_policy_migration_catalog");
  std::error_code ignored;
  std::filesystem::remove(catalog_path, ignored);

  auto unauthorized =
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_migration,
                       true);
  unauthorized.context.database_path = database_path.string();
  unauthorized.migration.persistent_format_mutation_requested = true;
  const auto unauthorized_result =
      engine::EnginePlanMemoryManagementOperation(unauthorized);
  Require(!unauthorized_result.ok,
          "memory policy migration accepted missing persistent mutation authority");
  Require(HasDiagnostic(
              unauthorized_result,
              "MEMORY.PERSISTENT_POLICY_MUTATION_AUTHORITY_REQUIRED"),
          "memory policy migration authority diagnostic mismatch");

  auto missing_derivative =
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_migration,
                       true);
  missing_derivative.context.database_path = database_path.string();
  missing_derivative.migration.persistent_format_mutation_requested = true;
  missing_derivative.migration.durable_policy_schema_migration_authorized = true;
  missing_derivative.migration.derivative_state_migration_execution_requested = true;
  const auto missing_derivative_result =
      engine::EnginePlanMemoryManagementOperation(missing_derivative);
  Require(!missing_derivative_result.ok,
          "memory policy migration accepted missing derivative execution authority");
  Require(HasDiagnostic(
              missing_derivative_result,
              "MEMORY.DERIVATIVE_STATE_MIGRATION_AUTHORITY_REQUIRED"),
          "memory derivative execution authority diagnostic mismatch");

  auto request =
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_migration,
                       true);
  request.context.database_path = database_path.string();
  request.migration.persistent_format_mutation_requested = true;
  request.migration.durable_policy_schema_migration_authorized = true;
  request.migration.derivative_state_migration_execution_requested = true;
  request.migration.derivative_state_migration_execution_authorized = true;
  request.migration.recovery_checkpoint_persistence_requested = true;
  request.migration.recovery_checkpoint_persistence_authorized = true;
  const auto result = engine::EnginePlanMemoryManagementOperation(request);
  RequireOk(result, "memory policy migration catalog persistence failed");
  Require(result.durable_state_changed,
          "memory policy migration did not mark durable state changed");
  Require(result.memory_policy_schema_migration_persisted,
          "memory policy schema migration persisted flag missing");
  Require(result.memory_derivative_state_migration_persisted,
          "memory derivative migration persisted flag missing");
  Require(result.memory_policy_migration_recovery_checkpoint_persisted,
          "memory migration recovery checkpoint flag missing");
  Require(!result.physical_action_dispatched,
          "memory policy migration dispatched physical action");
  Require(HasEvidence(result,
                      "memory_policy_schema_migration_catalog_persisted",
                      "sys.memory_policy_migration_catalog"),
          "memory policy migration catalog evidence missing");
  Require(HasEvidence(result,
                      "memory_derivative_state_migration_checkpoint",
                      "persisted"),
          "memory derivative migration checkpoint evidence missing");
  Require(HasEvidence(result,
                      "memory_policy_migration_recovery_checkpoint",
                      "persisted"),
          "memory migration recovery checkpoint evidence missing");

  auto downgrade =
      MigrationRequest(engine::EngineMemoryManagementOperation::plan_policy_migration,
                       true);
  downgrade.context.database_path = database_path.string();
  downgrade.migration.source_policy_version = 4;
  downgrade.migration.target_policy_version = 2;
  downgrade.migration.source_schema_version = 4;
  downgrade.migration.target_schema_version = 2;
  downgrade.migration.downgrade_requested = true;
  downgrade.migration.downgrade_compatibility_validated = true;
  downgrade.migration.persistent_format_mutation_requested = true;
  downgrade.migration.durable_policy_schema_migration_authorized = true;
  downgrade.migration.recovery_checkpoint_persistence_requested = true;
  downgrade.migration.recovery_checkpoint_persistence_authorized = true;
  const auto downgrade_result =
      engine::EnginePlanMemoryManagementOperation(downgrade);
  RequireOk(downgrade_result,
            "memory policy downgrade compatibility catalog persistence failed");
  Require(downgrade_result.memory_policy_schema_migration_persisted,
          "memory downgrade compatibility record was not persisted");

  const auto catalog = ReadMemoryCatalog(catalog_path, 2);
  std::size_t upgrades = 0, downgrades = 0;
  for (const auto& record : catalog) {
    Require(record.key.size() == 5 && record.key[0] == "sys.memory_policy_migration_catalog",
            "memory policy migration catalog key mismatch");
    RequireUuidBytes(record.key[1], request.context.database_uuid);
    RequireUuidBytes(record.key[2], request.migration.policy_uuid);
    RequireIdentity(record, "profile_uuid", request.migration.profile_uuid);
    RequireAttribute(record, "recovery_checkpoint_persisted", "true");
    if (record.key[3] == "3") {
      ++upgrades;
      Require(record.key[4] == "3", "memory migration target schema version mismatch");
      RequireAttribute(record, "derivative_state_migration_persisted", "true");
      RequireAttribute(record, "downgrade_compatibility_recorded", "false");
    } else {
      ++downgrades;
      Require(record.key[3] == "2" && record.key[4] == "2",
              "memory downgrade target versions mismatch");
      RequireAttribute(record, "downgrade_compatibility_recorded", "true");
    }
  }
  Require(upgrades == 1 && downgrades == 1, "memory migration records missing or duplicated");
  RequireCatalogReplayAndCorruptionRefusal(catalog_path, request, 2);
}

void TestCommonAuthorityAndTransactionRefusals() {
  auto unsafe = GovernanceRequest(
      engine::EngineMemoryManagementOperation::inspect_governance);
  unsafe.parser_memory_authority = true;
  const auto unsafe_result = engine::EnginePlanMemoryManagementOperation(unsafe);
  Require(!unsafe_result.ok, "memory planner accepted parser authority");
  Require(HasDiagnostic(unsafe_result, "MEMORY.UNSAFE_AUTHORITY_BOUNDARY"),
          "memory unsafe authority diagnostic mismatch");

  auto missing_tx = GovernanceRequest(
      engine::EngineMemoryManagementOperation::plan_cache_control,
      true);
  missing_tx.context.local_transaction_id = 0;
  const auto missing_tx_result =
      engine::EnginePlanMemoryManagementOperation(missing_tx);
  Require(!missing_tx_result.ok,
          "memory planner accepted mutation-shaped operation without transaction");
  Require(HasDiagnostic(missing_tx_result, "SB_ENGINE_API_INVALID_REQUEST"),
          "memory missing transaction diagnostic mismatch");

  auto missing_security = GovernanceRequest(
      engine::EngineMemoryManagementOperation::inspect_governance);
  missing_security.context.security_context_present = false;
  const auto missing_security_result =
      engine::EnginePlanMemoryManagementOperation(missing_security);
  Require(!missing_security_result.ok,
          "memory planner accepted missing security context");
  Require(HasDiagnostic(missing_security_result,
                        "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "memory missing security diagnostic mismatch");
}

}  // namespace

int main() {
  TestGovernanceDescriptorSurfaces();
  TestGovernanceDirectExecutorSurfaces();
  TestAutomationDescriptorSurfaces();
  TestAutomationDurableReportCatalogPersistence();
  TestAutomationSafeExecutorSurfaces();
  TestObjectResidencyDescriptorSurfaces();
  TestObjectResidencyDurableCatalogPersistence();
  TestRateLimitDescriptorSurfaces();
  TestRateLimitDurableCatalogPersistence();
  TestRateLimitLiveExecutorSurfaces();
  TestPolicyMigrationDescriptorSurfaces();
  TestPolicyMigrationDurableCatalogPersistence();
  TestCommonAuthorityAndTransactionRefusals();
  return EXIT_SUCCESS;
}
