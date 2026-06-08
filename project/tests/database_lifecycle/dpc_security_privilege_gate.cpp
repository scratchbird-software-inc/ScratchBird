// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/index_management_api.hpp"
#include "observability/cleanup_diagnostics_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace engine_sblr = scratchbird::engine::sblr;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey = "DPC_SECURITY_PRIVILEGE_GATE";

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
          std::chrono::system_clock::now().time_since_epoch()).count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    if (uuid::UuidKindAllowsDurableIdentity(kind)) {
      const auto generated =
          uuid::GenerateDurableEngineIdentityV7(kind, base_millis + salt);
      Require(generated.ok(), "DPC-066 generated UUID creation failed");
      return generated.value;
    }
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(base_millis + salt);
    Require(raw.ok(), "DPC-066 generated UUID creation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "DPC-066 generated UUID creation failed");
    return typed.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

struct TestIds {
  std::string database;
  std::string table;
  std::string index;
  std::string generation;
  std::string principal;
  std::string session;
  std::string transaction;
};

TestIds MakeIds(const UuidFactory& uuids, platform::u64 salt) {
  return {uuids.Text(platform::UuidKind::database, salt + 1),
          uuids.Text(platform::UuidKind::object, salt + 2),
          uuids.Text(platform::UuidKind::object, salt + 3),
          uuids.Text(platform::UuidKind::object, salt + 4),
          uuids.Text(platform::UuidKind::principal, salt + 5),
          uuids.Text(platform::UuidKind::session, salt + 6),
          uuids.Text(platform::UuidKind::transaction, salt + 7)};
}

api::EngineRequestContext Context(const TestIds& ids,
                                  std::vector<std::string> tags,
                                  bool security_context_present = true) {
  api::EngineRequestContext context;
  context.security_context_present = security_context_present;
  context.request_id = "dpc066-security-privilege";
  context.database_uuid.canonical = ids.database;
  context.principal_uuid.canonical = ids.principal;
  context.session_uuid.canonical = ids.session;
  context.transaction_uuid.canonical = ids.transaction;
  context.local_transaction_id = 66;
  context.snapshot_visible_through_local_transaction_id = 66;
  context.catalog_generation_id = 1066;
  context.security_epoch = 2066;
  context.resource_epoch = 3066;
  context.name_resolution_epoch = 4066;
  context.trace_tags = std::move(tags);
  context.trace_tags.push_back(std::string(kGateSearchKey));
  return context;
}

void AddOption(api::EngineApiRequest* request, std::string key, std::string value) {
  request->option_envelopes.push_back(std::move(key) + ":" + std::move(value));
}

api::EngineIndexManagementRequest IndexRequest(const TestIds& ids,
                                               std::string operation_id,
                                               std::vector<std::string> tags,
                                               bool security_context_present = true) {
  api::EngineIndexManagementRequest request;
  request.context = Context(ids, std::move(tags), security_context_present);
  request.operation_id = std::move(operation_id);
  request.target_database.uuid.canonical = ids.database;
  request.target_object.uuid.canonical = ids.index;
  request.target_object.object_kind = "index";
  AddOption(&request, "database_uuid", ids.database);
  AddOption(&request, "table_uuid", ids.table);
  AddOption(&request, "index_uuid", ids.index);
  AddOption(&request, "target_object_uuid", ids.index);
  AddOption(&request, "generation_uuid", ids.generation);
  AddOption(&request, "index_family", "btree");
  AddOption(&request, "catalog_resolution_proven", "true");
  AddOption(&request, "names_resolved_to_uuids", "true");
  AddOption(&request, "policy_allows_mutation", "true");
  if (request.operation_id == "index.repair") {
    AddOption(&request, "validation_family", "secondary_delta_ledger");
  } else {
    AddOption(&request, "validation_family", "ordered_table_candidate_set");
  }
  return request;
}

void AddOperand(engine_sblr::SblrOperationEnvelope* envelope,
                std::string name,
                std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

std::string OpcodeFor(std::string_view operation_id) {
  const auto* entry = engine_sblr::LookupSblrOperation(operation_id);
  Require(entry != nullptr, "DPC-066 SBLR registry operation missing");
  return entry->opcode;
}

engine_sblr::SblrOperationEnvelope IndexEnvelope(const TestIds& ids,
                                                 std::string operation_id) {
  auto envelope = engine_sblr::MakeSblrEnvelope(operation_id,
                                                OpcodeFor(operation_id),
                                                "dpc066.security.privilege");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  AddOperand(&envelope, "database_uuid", ids.database);
  AddOperand(&envelope, "table_uuid", ids.table);
  AddOperand(&envelope, "index_uuid", ids.index);
  AddOperand(&envelope, "target_object_uuid", ids.index);
  AddOperand(&envelope, "generation_uuid", ids.generation);
  AddOperand(&envelope, "index_family", "btree");
  AddOperand(&envelope, "catalog_resolution_proven", "true");
  AddOperand(&envelope, "names_resolved_to_uuids", "true");
  AddOperand(&envelope, "policy_allows_mutation", "true");
  AddOperand(&envelope,
             "validation_family",
             operation_id == "index.repair" ? "secondary_delta_ledger"
                                             : "ordered_table_candidate_set");
  return envelope;
}

api::PerformanceOptimizationSurfaceSnapshot SurfaceSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc066_security_privilege";
  snapshot.index_delta_backlog_count = 4;
  snapshot.index_garbage_backlog_count = 3;
  snapshot.page_summary_backlog_count = 2;
  snapshot.exact_refusal_diagnostic_code =
      "SECURITY.AUTHORIZATION.DENIED";
  snapshot.exact_refusal_message_vector =
      "SECURITY.AUTHORIZATION.DENIED|DPC-066|ENGINE_AUTHORIZATION";
  snapshot.exact_refusal_source = "engine.internal_api.authorization";
  snapshot.message_vector_ready = true;
  return snapshot;
}

api::EngineInspectPerformanceOptimizationSurfaceRequest SurfaceRequest(
    const TestIds& ids,
    std::vector<std::string> tags,
    bool security_context_present = true) {
  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = Context(ids, std::move(tags), security_context_present);
  request.snapshot = SurfaceSnapshot();
  request.snapshot_present = true;
  return request;
}

api::EngineCleanupDiagnosticsRequest CleanupRequest(
    const TestIds& ids,
    std::vector<std::string> tags,
    bool security_context_present = true) {
  api::EngineCleanupDiagnosticsRequest request;
  request.context = Context(ids, std::move(tags), security_context_present);
  request.cleanup_horizon_present = true;
  request.cleanup_horizon.status = {platform::StatusCode::ok,
                                    platform::Severity::info,
                                    platform::Subsystem::transaction_mga};
  request.cleanup_horizon.cleanup_horizon = mga::MakeLocalTransactionId(44);
  request.cleanup_horizon.cleanup_horizon_authoritative = true;
  request.context_kinds = {"backup", "repair"};
  return request;
}

engine_sblr::SblrDispatchResult DispatchIndexSblr(
    const TestIds& ids,
    std::string operation_id,
    std::vector<std::string> tags,
    bool security_context_present = true) {
  engine_sblr::SblrDispatchRequest request;
  request.context = Context(ids, std::move(tags), security_context_present);
  request.envelope = IndexEnvelope(ids, std::move(operation_id));
  return engine_sblr::DispatchSblrOperation(request);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string RowField(const api::EngineRowValue& row,
                     std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) {
      return field.second.encoded_value;
    }
  }
  return {};
}

bool HasAuthorizationRow(const api::EngineApiResult& result,
                         std::string_view decision,
                         std::string_view right_fragment) {
  for (const auto& row : result.result_shape.rows) {
    if (RowField(row, "record_kind") == "engine_authorization_decision" &&
        RowField(row, "decision") == decision &&
        Contains(RowField(row, "required_rights"), right_fragment)) {
      return true;
    }
  }
  return false;
}

void RequireAllowed(const api::EngineApiResult& result,
                    std::string_view right_fragment,
                    std::string_view message) {
  Require(result.ok, message);
  Require(HasEvidence(result, "engine_authorization_authority",
                      "EngineAuthorize"),
          "DPC-066 allowed result missing engine authorization authority");
  Require(HasAuthorizationRow(result, "allow", right_fragment),
          "DPC-066 allowed result missing authorization audit row");
  Require(HasEvidence(result, "parser_executes_sql", "false") ||
              HasEvidence(result, "parser_finality_authority", "false") ||
              HasEvidence(result, "mga_authority_boundary", "engine_owned") ||
              HasEvidence(result, "authority_source",
                          "durable_mga_transaction_inventory"),
          "DPC-066 allowed result missing engine authority boundary evidence");
}

void RequireDenied(const api::EngineApiResult& result,
                   std::string_view code,
                   std::string_view right_fragment,
                   std::string_view message) {
  Require(!result.ok, message);
  Require(HasDiagnostic(result, code), "DPC-066 denial diagnostic mismatch");
  Require(HasEvidence(result, "engine_authorization_authority",
                      "EngineAuthorize"),
          "DPC-066 denied result missing engine authorization authority");
  Require(HasAuthorizationRow(result, "deny", right_fragment),
          "DPC-066 denied result missing exact authorization audit row");
}

void RequireSblrAllowed(const engine_sblr::SblrDispatchResult& result,
                        std::string_view right_fragment,
                        std::string_view message) {
  Require(result.envelope_validated, "DPC-066 SBLR envelope did not validate");
  Require(result.accepted && result.dispatched_to_api,
          "DPC-066 SBLR route did not dispatch to engine API");
  RequireAllowed(result.api_result, right_fragment, message);
}

void RequireSblrDenied(const engine_sblr::SblrDispatchResult& result,
                       std::string_view code,
                       std::string_view right_fragment,
                       std::string_view message) {
  Require(result.envelope_validated, "DPC-066 SBLR denial envelope did not validate");
  Require(result.accepted && result.dispatched_to_api,
          "DPC-066 SBLR denial did not reach engine API");
  RequireDenied(result.api_result, code, right_fragment, message);
}

void TestIndexPrivilegeMatrix(const TestIds& ids) {
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.validate", {"right:OBS_INDEX_PROFILE_READ"})),
                 "OBS_INDEX_PROFILE_READ",
                 "DPC-066 validate with profile-read right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.analyze", {"right:OBS_MANAGEMENT_INSPECT"})),
                 "OBS_MANAGEMENT_INSPECT",
                 "DPC-066 analyze with management-inspect right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.backlog", {"right:MGA_CLEANUP_INSPECT"})),
                 "MGA_CLEANUP_INSPECT",
                 "DPC-066 backlog visibility with cleanup-inspect right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.rebuild", {"right:OBS_MANAGEMENT_CONTROL"})),
                 "OBS_MANAGEMENT_CONTROL",
                 "DPC-066 rebuild with management-control right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.repair", {"right:OBS_MANAGEMENT_CONTROL"})),
                 "OBS_MANAGEMENT_CONTROL",
                 "DPC-066 repair with management-control right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.cleanup_mga_versions",
                     {"right:MGA_CLEANUP_CONTROL"})),
                 "MGA_CLEANUP_CONTROL",
                 "DPC-066 cleanup with cleanup-control right was denied");
  RequireAllowed(api::EngineIndexManagementOperation(IndexRequest(
                     ids, "index.optimization_control",
                     {"right:OBS_CONFIG_CONTROL"})),
                 "OBS_CONFIG_CONTROL",
                 "DPC-066 optimization control with config-control right was denied");

  RequireDenied(api::EngineIndexManagementOperation(IndexRequest(
                    ids, "index.validate", {"right:CONNECT"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "OBS_INDEX_PROFILE_READ",
                "DPC-066 validate without inspect right was admitted");
  RequireDenied(api::EngineIndexManagementOperation(IndexRequest(
                    ids, "index.rebuild", {"right:OBS_MANAGEMENT_INSPECT"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "OBS_MANAGEMENT_CONTROL",
                "DPC-066 rebuild with inspect-only right was admitted");
  RequireDenied(api::EngineIndexManagementOperation(IndexRequest(
                    ids, "index.cleanup_mga_versions",
                    {"right:MGA_CLEANUP_INSPECT"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "MGA_CLEANUP_CONTROL",
                "DPC-066 cleanup with inspect-only right was admitted");
  RequireDenied(api::EngineIndexManagementOperation(IndexRequest(
                    ids, "index.optimization_control",
                    {"right:OBS_CONFIG_INSPECT"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "OBS_CONFIG_CONTROL",
                "DPC-066 optimization control with inspect-only right was admitted");
}

void TestMissingAndExpiredContexts(const TestIds& ids) {
  const auto missing = api::EngineIndexManagementOperation(IndexRequest(
      ids, "index.validate", {}, false));
  Require(!missing.ok &&
              HasDiagnostic(missing, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "DPC-066 missing security context diagnostic mismatch");

  RequireDenied(api::EngineIndexManagementOperation(IndexRequest(
                    ids,
                    "index.repair",
                    {"right:OBS_MANAGEMENT_CONTROL",
                     "security_context:expired"})),
                "SECURITY.CONTEXT.EXPIRED",
                "OBS_MANAGEMENT_CONTROL",
                "DPC-066 expired index management context was admitted");

  RequireDenied(api::EngineInspectPerformanceOptimizationSurface(SurfaceRequest(
                    ids,
                    {"right:OBS_MANAGEMENT_INSPECT",
                     "security_context:expired"})),
                "SECURITY.CONTEXT.EXPIRED",
                "OBS_MANAGEMENT_INSPECT",
                "DPC-066 expired performance surface context was admitted");
}

void TestBacklogAndCleanupSurfacePrivileges(const TestIds& ids) {
  RequireAllowed(api::EngineInspectPerformanceOptimizationSurface(SurfaceRequest(
                     ids, {"right:OBS_MANAGEMENT_INSPECT"})),
                 "OBS_MANAGEMENT_INSPECT",
                 "DPC-066 performance surface with management-inspect right failed");
  RequireAllowed(api::EngineInspectPerformanceOptimizationSurface(SurfaceRequest(
                     ids, {"right:MGA_CLEANUP_INSPECT"})),
                 "MGA_CLEANUP_INSPECT",
                 "DPC-066 backlog surface with cleanup-inspect right failed");
  RequireDenied(api::EngineInspectPerformanceOptimizationSurface(SurfaceRequest(
                    ids, {"right:OBS_CONFIG_INSPECT"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "OBS_MANAGEMENT_INSPECT",
                "DPC-066 performance surface with config-only right was admitted");

  RequireAllowed(api::EngineInspectCleanupDiagnostics(CleanupRequest(
                     ids, {"right:MGA_CLEANUP_INSPECT"})),
                 "MGA_CLEANUP_INSPECT",
                 "DPC-066 cleanup diagnostics with cleanup-inspect right failed");
  RequireDenied(api::EngineInspectCleanupDiagnostics(CleanupRequest(
                    ids, {"right:OBS_INDEX_PROFILE_READ"})),
                "SECURITY.AUTHORIZATION.DENIED",
                "MGA_CLEANUP_INSPECT",
                "DPC-066 cleanup diagnostics with profile-only right was admitted");
}

void TestSblrRoutePrivileges(const TestIds& ids) {
  RequireSblrAllowed(DispatchIndexSblr(ids,
                                       "index.validate",
                                       {"right:OBS_INDEX_PROFILE_READ"}),
                     "OBS_INDEX_PROFILE_READ",
                     "DPC-066 SBLR validate with profile-read right was denied");
  RequireSblrAllowed(DispatchIndexSblr(ids,
                                       "index.repair",
                                       {"right:OBS_MANAGEMENT_CONTROL"}),
                     "OBS_MANAGEMENT_CONTROL",
                     "DPC-066 SBLR repair with management-control right was denied");
  RequireSblrDenied(DispatchIndexSblr(ids,
                                      "index.repair",
                                      {"right:OBS_MANAGEMENT_INSPECT"}),
                    "SECURITY.AUTHORIZATION.DENIED",
                    "OBS_MANAGEMENT_CONTROL",
                    "DPC-066 SBLR repair with inspect-only right was admitted");

  const auto missing_context = DispatchIndexSblr(ids, "index.validate", {}, false);
  Require(missing_context.envelope_validated,
          "DPC-066 SBLR missing-context envelope did not validate");
  Require(!missing_context.accepted && !missing_context.dispatched_to_api,
          "DPC-066 SBLR missing-context request reached engine API");
  Require(HasDiagnostic(missing_context.api_result,
                        "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED"),
          "DPC-066 SBLR missing-context diagnostic mismatch");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  const UuidFactory uuids;
  const auto ids = MakeIds(uuids, 6600);
  TestIndexPrivilegeMatrix(ids);
  TestMissingAndExpiredContexts(ids);
  TestBacklogAndCleanupSurfacePrivileges(ids);
  TestSblrRoutePrivileges(ids);
  return EXIT_SUCCESS;
}
