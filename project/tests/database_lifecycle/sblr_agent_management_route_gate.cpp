// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "sblr_dispatch.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "PFAR-013 UUID generation failed");
  return generated.value;
}

std::string MakeUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(MakeUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string transaction_uuid;
  std::string policy_uuid;
  std::string agent_uuid;
  std::string principal_uuid;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

Fixture MakeFixture(std::string_view name, platform::u64 salt) {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar013_" + std::string(name) + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "pfar013.sbdb";
  fixture.database_uuid = MakeUuidText(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = MakeUuidText(platform::UuidKind::filespace, salt + 2);
  fixture.transaction_uuid = MakeUuidText(platform::UuidKind::transaction, salt + 3);
  fixture.policy_uuid = MakeUuidText(platform::UuidKind::object, salt + 4);
  fixture.agent_uuid = MakeUuidText(platform::UuidKind::object, salt + 5);
  fixture.principal_uuid = MakeUuidText(platform::UuidKind::principal, salt + 6);
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = "session-pfar-013";
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = 9001;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.catalog_generation_id = 11;
  context.security_epoch = 13;
  context.resource_epoch = 17;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  return context;
}

api::EngineObjectReference FilespaceTarget(const Fixture& fixture) {
  api::EngineObjectReference target;
  target.uuid.canonical = fixture.filespace_uuid;
  target.object_kind = "filespace";
  return target;
}

void AddCommonAgentFields(api::EngineAgentActionHookRequest* request,
                          const Fixture& fixture,
                          std::string agent_type,
                          std::string action_class) {
  request->agent_type = std::move(agent_type);
  request->action_class = std::move(action_class);
  request->agent_uuid.canonical = fixture.agent_uuid;
  request->policy_snapshot_uuid.canonical = fixture.policy_uuid;
  request->target_filespace = FilespaceTarget(fixture);
  request->safety_fence_result = "passed";
  request->policy_authorized = true;
  request->evidence_sink_available = true;
  request->metrics_fresh = true;
  request->option_envelopes.push_back("wall_now_us:100");
  request->option_envelopes.push_back("monotonic_now_us:100");
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" + fixture.database_uuid);
  request->option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request->option_envelopes.push_back("agent_metric_snapshot_source_id:sblr-agent-route-source:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_source_sequence:1");
  request->option_envelopes.push_back("agent_metric_snapshot_digest:sha256:sblr-agent-route:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_value_digest:sha256:sblr-agent-route-value:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_schema_digest:sha256:sblr-agent-route-schema:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:sblr-agent-route-key:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:sblr-agent-route-attest:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request->option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_provenance_record:sblr-agent-route-provenance:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_id:sblr-agent-route:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                      fixture.agent_uuid);
}

void AddObservedMetricSnapshotFields(api::EngineApiRequest* request,
                                     const Fixture& fixture,
                                     std::string_view agent_type) {
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" +
                                      fixture.database_uuid);
  request->option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request->option_envelopes.push_back("agent_metric_snapshot_source_id:sblr-agent-route-source:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_source_sequence:1");
  request->option_envelopes.push_back("agent_metric_snapshot_digest:sha256:sblr-agent-route:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_value_digest:sha256:sblr-agent-route-value:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back(
      "agent_metric_snapshot_schema_digest:sha256:sblr-agent-route-schema:" +
      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:sblr-agent-route-key:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:sblr-agent-route-attest:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request->option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_provenance_record:sblr-agent-route-provenance:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_id:sblr-agent-route:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                      fixture.agent_uuid);
}

api::EngineRequestPagePreallocationRequest PageRequest(const Fixture& fixture,
                                                       std::string request_id) {
  api::EngineRequestPagePreallocationRequest request;
  request.context = Context(fixture, std::move(request_id));
  AddCommonAgentFields(&request, fixture, "page_allocation_manager", "page_preallocation_request");
  request.page_family = "data";
  request.page_type = "relation";
  request.requested_pages = 6;
  return request;
}

api::EngineRequestFilespaceGrowthRequest FilespaceRequest(const Fixture& fixture,
                                                          std::string request_id) {
  api::EngineRequestFilespaceGrowthRequest request;
  request.context = Context(fixture, std::move(request_id));
  AddCommonAgentFields(&request, fixture, "filespace_capacity_manager", "filespace_growth_request");
  request.requested_bytes = 12 * 16384;
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  return request;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) {
      continue;
    }
    if (id.empty() || evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result, std::string_view field) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == field) {
        return value.encoded_value;
      }
    }
  }
  return {};
}

void RequireField(const api::EngineApiResult& result,
                  std::string_view field,
                  std::string_view expected) {
  const auto actual = FieldValue(result, field);
  Require(actual == expected,
          std::string(field) + " mismatch: expected " + std::string(expected) +
              " got " + actual);
}

void RequireNonEmptyField(const api::EngineApiResult& result, std::string_view field) {
  Require(!FieldValue(result, field).empty(), std::string(field) + " missing");
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
  }
}

sblr::SblrDispatchResult DispatchWithContext(api::EngineRequestContext context,
                                             std::string operation_id,
                                             std::string opcode,
                                             api::EngineApiRequest api_request,
                                             bool requires_security = true,
                                             bool requires_transaction = true) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "pfar-013");
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_transaction_context = requires_transaction;
  envelope.requires_security_context = requires_security;
  const sblr::SblrDispatchRequest request{std::move(context), envelope, std::move(api_request)};
  auto result = sblr::DispatchSblrOperation(request);
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  return result;
}

sblr::SblrDispatchResult Dispatch(const Fixture& fixture,
                                  std::string operation_id,
                                  std::string opcode,
                                  api::EngineApiRequest api_request,
                                  std::string request_id) {
  return DispatchWithContext(Context(fixture, std::move(request_id)),
                             std::move(operation_id),
                             std::move(opcode),
                             std::move(api_request));
}

api::EngineApiRequest PageSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + fixture.agent_uuid);
  request.option_envelopes.push_back("policy_snapshot_uuid:" + fixture.policy_uuid);
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("page_family:data");
  request.option_envelopes.push_back("page_type:relation");
  request.option_envelopes.push_back("requested_pages:4");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "page_allocation_manager");
  return request;
}

api::EngineApiRequest FilespaceSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + fixture.agent_uuid);
  request.option_envelopes.push_back("policy_snapshot_uuid:" + fixture.policy_uuid);
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("requested_bytes:196608");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "filespace_capacity_manager");
  return request;
}

api::EngineApiRequest FilespaceSblrPagesApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + fixture.agent_uuid);
  request.option_envelopes.push_back("policy_snapshot_uuid:" + fixture.policy_uuid);
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("requested_pages:5");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "filespace_capacity_manager");
  return request;
}

api::EngineApiRequest FilespacePreallocateSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.target_object = FilespaceTarget(fixture);
  request.option_envelopes.push_back("requested_pages:12");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("evidence_sink_available:true");
  return request;
}

void TestApiPagePreallocationStorageMutation() {
  const auto fixture = MakeFixture("api_page", 1000);
  const auto result = api::EngineRequestPagePreallocation(PageRequest(fixture, "api-page-live"));
  if (!result.ok) {
    DumpDiagnostics(result);
  }
  Require(result.ok, "API page preallocation failed");
  Require(HasEvidence(result, "agent_hook", "agents.request_page_preallocation"),
          "API page hook evidence missing");
  Require(HasEvidence(result, "storage_executor", "PreallocatePageFamilyPool"),
          "API page storage executor evidence missing");
  RequireNonEmptyField(result, "page_preallocation_allocation_uuid");
  RequireField(result, "storage_execution", "completed");
  RequireField(result, "page_preallocation_ledger_mutated", "true");
  RequireField(result, "page_preallocation_state", "preallocated");
  RequireField(result, "page_preallocation_diagnostic", "SB-STORAGE-PAGE-PREALLOCATION-PREALLOCATED");
  RequireField(result, "page_preallocation_evidence_action", "preallocate_page_family_pool");
  RequireField(result, "page_preallocation_durable_state_changed", "true");
  RequireField(result, "page_preallocation_capacity_evidence_accepted", "true");
}

void TestSblrPagePreallocationStorageMutation() {
  const auto fixture = MakeFixture("sblr_page", 2000);
  const auto result = Dispatch(fixture,
                               "agents.request_page_preallocation",
                               "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
                               PageSblrApiRequest(fixture),
                               "sblr-page-live");
  Require(result.accepted && result.dispatched_to_api, "SBLR page route was not dispatched");
  Require(result.api_result.ok, "SBLR page preallocation API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "PreallocatePageFamilyPool"),
          "SBLR page storage executor evidence missing");
  RequireNonEmptyField(result.api_result, "page_preallocation_allocation_uuid");
  RequireField(result.api_result, "page_preallocation_ledger_mutated", "true");
  RequireField(result.api_result, "page_preallocation_state", "preallocated");
}

void TestApiFilespaceGrowthStorageMutation() {
  const auto fixture = MakeFixture("api_filespace", 3000);
  const auto result = api::EngineRequestFilespaceGrowth(FilespaceRequest(fixture, "api-filespace-live"));
  if (!result.ok) {
    DumpDiagnostics(result);
  }
  Require(result.ok, "API filespace growth failed");
  Require(HasEvidence(result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "API filespace storage executor evidence missing");
  RequireNonEmptyField(result, "filespace_growth_operation_uuid");
  RequireField(result, "storage_execution", "completed");
  RequireField(result, "filespace_growth_ledger_mutated", "true");
  RequireField(result, "filespace_growth_state", "completed");
  RequireField(result, "filespace_growth_diagnostic", "ok");
  RequireField(result, "filespace_growth_evidence_action", "filespace_physical_growth_commit");
  RequireField(result, "filespace_growth_requested_pages", "12");
  RequireField(result, "filespace_growth_grown_pages", "12");
  RequireField(result, "filespace_growth_durable_state_changed", "true");
  RequireField(result, "filespace_growth_physical_extension_completed", "true");
  RequireField(result, "filespace_growth_physical_extension_synced", "true");
  RequireField(result, "filespace_growth_physical_header_updated", "true");
  RequireField(result, "filespace_growth_metadata_after_physical_extension", "true");
  RequireField(result, "filespace_growth_page_allocation_authority_bypassed", "false");
}

void TestSblrFilespaceGrowthStorageMutation() {
  const auto fixture = MakeFixture("sblr_filespace", 4000);
  const auto result = Dispatch(fixture,
                               "agents.request_filespace_growth",
                               "SBLR_AGENT_REQUEST_FILESPACE_GROWTH",
                               FilespaceSblrApiRequest(fixture),
                               "sblr-filespace-live");
  Require(result.accepted && result.dispatched_to_api, "SBLR filespace route was not dispatched");
  Require(result.api_result.ok, "SBLR filespace growth API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "SBLR filespace storage executor evidence missing");
  RequireNonEmptyField(result.api_result, "filespace_growth_operation_uuid");
  RequireField(result.api_result, "filespace_growth_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_growth_state", "completed");
  RequireField(result.api_result, "filespace_growth_grown_pages", "12");
  RequireField(result.api_result, "filespace_growth_physical_extension_completed", "true");
  RequireField(result.api_result, "filespace_growth_metadata_after_physical_extension", "true");
}

void TestSblrFilespaceGrowthAcceptsRequestedPages() {
  const auto fixture = MakeFixture("sblr_filespace_pages", 4500);
  const auto result = Dispatch(fixture,
                               "agents.request_filespace_growth",
                               "SBLR_AGENT_REQUEST_FILESPACE_GROWTH",
                               FilespaceSblrPagesApiRequest(fixture),
                               "sblr-filespace-pages-live");
  Require(result.accepted && result.dispatched_to_api,
          "SBLR filespace pages route was not dispatched");
  Require(result.api_result.ok, "SBLR filespace pages growth API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "SBLR filespace pages storage executor evidence missing");
  RequireField(result.api_result, "filespace_growth_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_growth_grown_pages", "5");
}

void TestSblrFilespacePreallocateStorageMutation() {
  const auto fixture = MakeFixture("sblr_preallocate", 4600);
  const auto result = Dispatch(fixture,
                               "filespace.preallocate",
                               "SBLR_FILESPACE_PREALLOCATE",
                               FilespacePreallocateSblrApiRequest(fixture),
                               "sblr-filespace-preallocate-live");
  Require(result.accepted && result.dispatched_to_api,
          "SBLR filespace preallocate was not dispatched");
  Require(result.api_result.ok, "SBLR filespace preallocate API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "PreallocateFilespace"),
          "SBLR filespace preallocate storage executor evidence missing");
  RequireNonEmptyField(result.api_result, "filespace_preallocation_operation_uuid");
  RequireField(result.api_result, "storage_execution", "completed");
  RequireField(result.api_result, "filespace_preallocation_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_preallocation_state", "completed");
  RequireField(result.api_result, "filespace_preallocation_diagnostic", "ok");
  RequireField(result.api_result, "filespace_preallocation_evidence_action",
               "filespace_preallocate_commit");
  RequireField(result.api_result, "filespace_preallocation_requested_pages", "12");
  RequireField(result.api_result, "filespace_preallocation_pages", "12");
  RequireField(result.api_result, "filespace_preallocation_durable_state_changed", "true");
}

void TestSblrFilespacePreallocateNegativeCasesDoNotMutate() {
  const auto fixture = MakeFixture("sblr_preallocate_negative", 4700);

  auto missing_security_context = Context(fixture, "preallocate-missing-security");
  missing_security_context.security_context_present = false;
  const auto missing_security = DispatchWithContext(
      missing_security_context,
      "filespace.preallocate",
      "SBLR_FILESPACE_PREALLOCATE",
      FilespacePreallocateSblrApiRequest(fixture),
      false,
      true);
  Require(missing_security.dispatched_to_api, "missing security did not reach API route");
  Require(!missing_security.api_result.ok, "missing security preallocate succeeded");
  Require(HasDiagnostic(missing_security.api_result, "AGENT.SECURITY_CONTEXT_REQUIRED"),
          "missing security diagnostic mismatch");
  RequireField(missing_security.api_result,
               "filespace_preallocation_ledger_mutated",
               "false");

  auto missing_transaction_context = Context(fixture, "preallocate-missing-transaction");
  missing_transaction_context.local_transaction_id = 0;
  const auto missing_transaction = DispatchWithContext(
      missing_transaction_context,
      "filespace.preallocate",
      "SBLR_FILESPACE_PREALLOCATE",
      FilespacePreallocateSblrApiRequest(fixture),
      true,
      false);
  Require(missing_transaction.dispatched_to_api, "missing transaction did not reach API route");
  Require(!missing_transaction.api_result.ok, "missing transaction preallocate succeeded");
  Require(!missing_transaction.api_result.diagnostics.empty() &&
              missing_transaction.api_result.diagnostics.front().detail.find("local_transaction_id_required") !=
                  std::string::npos,
          "missing transaction diagnostic mismatch");

  auto insufficient_capacity = FilespacePreallocateSblrApiRequest(fixture);
  insufficient_capacity.option_envelopes.clear();
  insufficient_capacity.option_envelopes.push_back("requested_pages:12");
  insufficient_capacity.option_envelopes.push_back("filespace.page_size_bytes:16384");
  insufficient_capacity.option_envelopes.push_back("filespace.current_pages:64");
  insufficient_capacity.option_envelopes.push_back("filespace.preallocated_pages:4");
  insufficient_capacity.option_envelopes.push_back("filespace.maximum_pages:70");
  insufficient_capacity.option_envelopes.push_back("evidence_sink_available:true");
  const auto capacity = Dispatch(fixture,
                                 "filespace.preallocate",
                                 "SBLR_FILESPACE_PREALLOCATE",
                                 insufficient_capacity,
                                 "preallocate-insufficient-capacity");
  Require(capacity.dispatched_to_api, "capacity refusal did not dispatch");
  Require(!capacity.api_result.ok, "insufficient capacity preallocate succeeded");
  Require(!capacity.api_result.diagnostics.empty() &&
              capacity.api_result.diagnostics.front().code ==
                  "filespace_preallocate_insufficient_capacity",
          "insufficient capacity diagnostic mismatch");

  const auto live = Dispatch(fixture,
                             "filespace.preallocate",
                             "SBLR_FILESPACE_PREALLOCATE",
                             FilespacePreallocateSblrApiRequest(fixture),
                             "preallocate-live-after-negative");
  Require(live.api_result.ok, "live preallocate after negative cases failed");
  RequireField(live.api_result, "filespace_preallocation_evidence_sequence", "1");
}

void TestDryRunAndValidationFailuresDoNotMutateBeforeLiveRoute() {
  auto fixture = MakeFixture("negative_page", 5000);
  auto dry_run = PageRequest(fixture, "page-dry-run");
  dry_run.dry_run = true;
  const auto dry = api::EngineRequestPagePreallocation(dry_run);
  Require(dry.ok, "dry-run page route failed");
  RequireField(dry, "storage_execution", "dry_run");
  RequireField(dry, "page_preallocation_ledger_mutated", "false");
  Require(!HasEvidence(dry, "storage_executor", "PreallocatePageFamilyPool"),
          "dry-run emitted live storage executor evidence");

  auto missing_security = PageRequest(fixture, "page-missing-security");
  missing_security.context.security_context_present = false;
  const auto refused = api::EngineRequestPagePreallocation(missing_security);
  Require(!refused.ok, "missing security page route succeeded");
  Require(refused.refusal_reason == "security_context_required",
          "missing security refusal mismatch: " + refused.refusal_reason);

  auto missing_transaction = PageRequest(fixture, "page-missing-transaction");
  missing_transaction.context.local_transaction_id = 0;
  const auto no_transaction = api::EngineRequestPagePreallocation(missing_transaction);
  Require(!no_transaction.ok, "missing transaction page route succeeded");
  Require(no_transaction.refusal_reason == "local_transaction_id_required",
          "missing transaction refusal mismatch: " + no_transaction.refusal_reason);

  auto missing_evidence = PageRequest(fixture, "page-missing-evidence");
  missing_evidence.evidence_sink_available = false;
  const auto no_evidence = api::EngineRequestPagePreallocation(missing_evidence);
  Require(!no_evidence.ok, "missing evidence page route succeeded");
  Require(no_evidence.refusal_reason == "evidence_sink_required",
          "missing evidence refusal mismatch: " + no_evidence.refusal_reason);

  auto missing_metrics = PageRequest(fixture, "page-missing-metrics");
  missing_metrics.metrics_fresh = false;
  const auto no_metrics = api::EngineRequestPagePreallocation(missing_metrics);
  Require(!no_metrics.ok, "missing metrics page route succeeded");
  Require(no_metrics.refusal_reason == "metric_freshness_required",
          "missing metrics refusal mismatch: " + no_metrics.refusal_reason);

  const auto live = api::EngineRequestPagePreallocation(PageRequest(fixture, "page-live-after-negative"));
  Require(live.ok, "live page route after negative cases failed");
  RequireField(live, "page_preallocation_evidence_sequence", "1");

  const auto filespace_fixture = MakeFixture("negative_filespace", 6000);
  auto missing_policy = FilespaceRequest(filespace_fixture, "filespace-missing-policy");
  missing_policy.policy_authorized = false;
  const auto denied = api::EngineRequestFilespaceGrowth(missing_policy);
  Require(!denied.ok, "missing policy filespace route succeeded");
  Require(denied.refusal_reason == "policy_authorization_required",
          "missing policy refusal mismatch: " + denied.refusal_reason);

  auto dry_growth = FilespaceRequest(filespace_fixture, "filespace-dry-run");
  dry_growth.dry_run = true;
  const auto dry_growth_result = api::EngineRequestFilespaceGrowth(dry_growth);
  Require(dry_growth_result.ok, "dry-run filespace route failed");
  RequireField(dry_growth_result, "storage_execution", "dry_run");
  RequireField(dry_growth_result, "filespace_growth_ledger_mutated", "false");

  const auto live_growth = api::EngineRequestFilespaceGrowth(
      FilespaceRequest(filespace_fixture, "filespace-live-after-negative"));
  Require(live_growth.ok, "live filespace route after negative cases failed");
  RequireField(live_growth, "filespace_growth_evidence_sequence", "1");
}

}  // namespace

int main() {
  TestApiPagePreallocationStorageMutation();
  TestSblrPagePreallocationStorageMutation();
  TestApiFilespaceGrowthStorageMutation();
  TestSblrFilespaceGrowthStorageMutation();
  TestSblrFilespaceGrowthAcceptsRequestedPages();
  TestSblrFilespacePreallocateStorageMutation();
  TestSblrFilespacePreallocateNegativeCasesDoNotMutate();
  TestDryRunAndValidationFailuresDoNotMutateBeforeLiveRoute();
  return EXIT_SUCCESS;
}
