// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/executable_object_lifecycle.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
std::string DiagnosticCode(const TResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().code;
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) { std::cerr << DiagnosticCode(result) << '\n'; }
  Require(result.ok, message);
}

template <typename TResult>
void RequireDiagnostic(const TResult& result,
                       std::string_view expected,
                       std::string_view message) {
  Require(!result.ok, message);
  if (DiagnosticCode(result) != expected) {
    std::cerr << "expected=" << expected << " actual=" << DiagnosticCode(result) << '\n';
  }
  Require(DiagnosticCode(result) == expected, message);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view key) {
  for (const auto& [name, value] : row.fields) {
    if (name == key) { return value.encoded_value; }
  }
  return {};
}

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, key) == value) { return true; }
  }
  return false;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_013ag_executable_object_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

api::EngineRequestContext Context(const std::filesystem::path& path,
                                  std::uint64_t tx,
                                  std::uint64_t visible_through = 0) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "db-dblc-013ag";
  context.principal_uuid.canonical = "principal-owner";
  context.session_uuid.canonical = "session-dblc-013ag";
  context.transaction_uuid.canonical = "txn-" + std::to_string(tx);
  context.local_transaction_id = tx;
  context.snapshot_visible_through_local_transaction_id = visible_through;
  context.security_context_present = true;
  context.catalog_generation_id = tx;
  context.security_epoch = tx;
  context.resource_epoch = tx;
  return context;
}

template <typename TRequest>
TRequest BaseRequest(const std::filesystem::path& path,
                     std::uint64_t tx,
                     std::string uuid,
                     std::string kind) {
  TRequest request;
  request.context = Context(path, tx, tx);
  request.target_database.uuid.canonical = "db-dblc-013ag";
  request.target_database.object_kind = "database";
  request.target_schema.uuid.canonical = "schema-app";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::move(uuid);
  request.target_object.object_kind = std::move(kind);
  return request;
}

void AddManage(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("permission:manage_executable");
}

void AddEventTriggerManage(api::EngineApiRequest* request) {
  AddManage(request);
  request->option_envelopes.push_back("permission:manage_event_trigger");
}

void AddInvoke(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("permission:invoke_executable");
}

void AddStoredSblr(api::EngineApiRequest* request, std::string hash_seed) {
  request->option_envelopes.push_back("executor:sblr");
  request->option_envelopes.push_back("sblr_hash:sha256:" + std::move(hash_seed));
  request->option_envelopes.push_back("sblr_provenance:parser_lowered_uuid_bound_sblr_v3");
}

api::EngineCreateExecutableObjectRequest CreateSblrRequest(const std::filesystem::path& path,
                                                           std::uint64_t tx,
                                                           std::string uuid,
                                                           std::string kind,
                                                           std::string hash_seed) {
  auto request = BaseRequest<api::EngineCreateExecutableObjectRequest>(
      path, tx, std::move(uuid), std::move(kind));
  AddManage(&request);
  AddStoredSblr(&request, std::move(hash_seed));
  return request;
}

api::EngineInvokeExecutableObjectRequest InvokeRequest(const std::filesystem::path& path,
                                                       std::uint64_t tx,
                                                       std::string uuid,
                                                       std::string kind) {
  auto request = BaseRequest<api::EngineInvokeExecutableObjectRequest>(
      path, tx, std::move(uuid), std::move(kind));
  AddInvoke(&request);
  request.option_envelopes.push_back("sblr_authorized_invocation:true");
  return request;
}

void TestCreateAlterDropDependencyAndGeneration(const std::filesystem::path& path) {
  auto package = CreateSblrRequest(path, 1, "pkg-billing", "package", "pkg");
  RequireOk(api::EngineCreateExecutableObject(package), "package create failed");

  auto stored_sblr = CreateSblrRequest(path, 2, "sblr-billing-tax", "stored_sblr", "stored");
  const auto stored_created = api::EngineCreateExecutableObject(stored_sblr);
  RequireOk(stored_created, "stored SBLR create failed");
  Require(HasEvidence(stored_created, "stored_sblr", "hash_and_provenance_recorded"),
          "stored SBLR provenance evidence was not emitted");

  auto function = CreateSblrRequest(path, 3, "fn-tax-rate", "function", "fn-v1");
  function.related_objects.push_back({{"pkg-billing"}, "package"});
  function.related_objects.push_back({{"sblr-billing-tax"}, "stored_sblr"});
  const auto fn_created = api::EngineCreateExecutableObject(function);
  RequireOk(fn_created, "function create failed");
  Require(fn_created.executable_generation == 1, "function create generation mismatch");

  auto procedure = CreateSblrRequest(path, 4, "proc-invoice", "procedure", "proc-v1");
  procedure.related_objects.push_back({{"fn-tax-rate"}, "function"});
  RequireOk(api::EngineCreateExecutableObject(procedure), "procedure create failed");

  auto trigger = CreateSblrRequest(path, 5, "trg-invoice-audit", "trigger", "trigger-v1");
  trigger.related_objects.push_back({{"proc-invoice"}, "procedure"});
  RequireOk(api::EngineCreateExecutableObject(trigger), "trigger create failed");

  auto routine = BaseRequest<api::EngineCreateExecutableObjectRequest>(
      path, 6, "routine-internal-seed", "routine");
  AddManage(&routine);
  routine.option_envelopes.push_back("executor:internal_procedure");
  routine.option_envelopes.push_back("internal_procedure_id:sys.exec.seed_builtin");
  RequireOk(api::EngineCreateExecutableObject(routine), "internal routine create failed");

  auto alter_function = BaseRequest<api::EngineAlterExecutableObjectRequest>(
      path, 7, "fn-tax-rate", "function");
  AddManage(&alter_function);
  AddStoredSblr(&alter_function, "fn-v2");
  const auto altered = api::EngineAlterExecutableObject(alter_function);
  RequireOk(altered, "function alter failed");
  Require(altered.executable_generation == 2, "function alter generation mismatch");
  Require(HasEvidence(altered, "dependency_invalidation", "fn-tax-rate"),
          "function alter did not publish dependency invalidation evidence");

  const auto invalidated_proc = api::EngineInvokeExecutableObject(
      InvokeRequest(path, 8, "proc-invoice", "procedure"));
  RequireDiagnostic(invalidated_proc,
                    api::kExecutableObjectDiagnosticDependencyInvalidated,
                    "invalidated dependent procedure invocation was accepted");

  auto inspect = BaseRequest<api::EngineInspectExecutableObjectRequest>(
      path, 9, "proc-invoice", "procedure");
  inspect.option_envelopes.push_back("permission:inspect_executable");
  const auto inspected = api::EngineInspectExecutableObjects(inspect);
  RequireOk(inspected, "inspect after dependency invalidation failed");
  Require(HasRowField(inspected, "invalidated", "true"),
          "inspect did not expose invalidated dependency state");

  auto hidden_invoke = InvokeRequest(path, 10, "fn-tax-rate", "function");
  hidden_invoke.context.snapshot_visible_through_local_transaction_id = 2;
  RequireDiagnostic(api::EngineInvokeExecutableObject(hidden_invoke),
                    api::kExecutableObjectDiagnosticMgaVisibilityRefused,
                    "snapshot-invisible executable generation was accepted");

  auto drop_trigger = BaseRequest<api::EngineDropExecutableObjectRequest>(
      path, 11, "trg-invoice-audit", "trigger");
  AddManage(&drop_trigger);
  const auto dropped = api::EngineDropExecutableObject(drop_trigger);
  RequireOk(dropped, "trigger drop failed");
  Require(dropped.executable_generation == 2, "drop did not advance executable generation");

  auto dropped_invoke = InvokeRequest(path, 12, "trg-invoice-audit", "trigger");
  RequireDiagnostic(api::EngineInvokeExecutableObject(dropped_invoke),
                    api::kExecutableObjectDiagnosticNotFound,
                    "dropped trigger remained invokable");
}

void TestPermissionExecutionBoundaryAndStoredSblr(const std::filesystem::path& path) {
  auto no_permission = CreateSblrRequest(path, 20, "fn-denied", "function", "denied");
  no_permission.option_envelopes.clear();
  AddStoredSblr(&no_permission, "denied");
  RequireDiagnostic(api::EngineCreateExecutableObject(no_permission),
                    api::kExecutableObjectDiagnosticPermissionDenied,
                    "create without executable management permission was admitted");

  auto missing_hash = BaseRequest<api::EngineCreateExecutableObjectRequest>(
      path, 21, "fn-missing-hash", "function");
  AddManage(&missing_hash);
  missing_hash.option_envelopes.push_back("executor:sblr");
  missing_hash.option_envelopes.push_back("sblr_provenance:parser_lowered_uuid_bound_sblr_v3");
  RequireDiagnostic(api::EngineCreateExecutableObject(missing_hash),
                    api::kExecutableObjectDiagnosticStoredSblrRequired,
                    "SBLR executable without hash was admitted");

  auto missing_provenance = BaseRequest<api::EngineCreateExecutableObjectRequest>(
      path, 22, "fn-missing-provenance", "function");
  AddManage(&missing_provenance);
  missing_provenance.option_envelopes.push_back("executor:sblr");
  missing_provenance.option_envelopes.push_back("sblr_hash:sha256:missing-provenance");
  RequireDiagnostic(api::EngineCreateExecutableObject(missing_provenance),
                    api::kExecutableObjectDiagnosticStoredSblrProvenanceRequired,
                    "SBLR executable without provenance was admitted");

  auto boundary = CreateSblrRequest(path, 23, "fn-parser-owned", "function", "parser-owned");
  boundary.option_envelopes.push_back("parser_execute:true");
  RequireDiagnostic(api::EngineCreateExecutableObject(boundary),
                    api::kExecutableObjectDiagnosticExecutionBoundaryRefused,
                    "parser-owned executable request was admitted");
}

void TestSideEffectPolicy(const std::filesystem::path& path) {
  auto sidefx = CreateSblrRequest(path, 30, "proc-sidefx", "procedure", "sidefx");
  sidefx.option_envelopes.push_back("side_effect_class:external_non_idempotent");
  RequireOk(api::EngineCreateExecutableObject(sidefx), "side-effecting procedure create failed");

  auto denied = InvokeRequest(path, 31, "proc-sidefx", "procedure");
  RequireDiagnostic(api::EngineInvokeExecutableObject(denied),
                    api::kExecutableObjectDiagnosticSideEffectPolicyDenied,
                    "side-effecting invocation was admitted without policy allowance");

  auto allowed = InvokeRequest(path, 32, "proc-sidefx", "procedure");
  allowed.option_envelopes.push_back("policy:executable.side_effect:allow");
  const auto allowed_result = api::EngineInvokeExecutableObject(allowed);
  RequireOk(allowed_result, "side-effecting invocation with policy allowance failed");
  Require(HasEvidence(allowed_result, "side_effect_policy", "allowed_by_engine_policy"),
          "side-effect allowance evidence was not emitted");
}

void TestUnloadQuiesceAndActiveInvocation(const std::filesystem::path& path) {
  auto active_proc = CreateSblrRequest(path, 40, "proc-active", "procedure", "active");
  RequireOk(api::EngineCreateExecutableObject(active_proc), "active procedure create failed");

  auto begin = BaseRequest<api::EngineBeginExecutableObjectInvocationRequest>(
      path, 41, "proc-active", "procedure");
  AddInvoke(&begin);
  const auto begun = api::EngineBeginExecutableObjectInvocation(begin);
  RequireOk(begun, "begin invocation failed");
  Require(!begun.invocation_lease_uuid.empty(), "begin invocation did not return a lease UUID");

  auto unload_blocked = BaseRequest<api::EngineUnloadExecutableObjectRequest>(
      path, 42, "proc-active", "procedure");
  AddManage(&unload_blocked);
  RequireDiagnostic(api::EngineUnloadExecutableObject(unload_blocked),
                    api::kExecutableObjectDiagnosticUnloadBlockedActiveInvocation,
                    "unload with active invocation was admitted");

  auto finish = BaseRequest<api::EngineFinishExecutableObjectInvocationRequest>(
      path, 43, "proc-active", "procedure");
  AddInvoke(&finish);
  finish.option_envelopes.push_back("invocation_lease_uuid:" + begun.invocation_lease_uuid);
  RequireOk(api::EngineFinishExecutableObjectInvocation(finish), "finish invocation failed");

  auto quiesce = BaseRequest<api::EngineQuiesceExecutableObjectRequest>(
      path, 44, "proc-active", "procedure");
  AddManage(&quiesce);
  const auto quiesced = api::EngineQuiesceExecutableObject(quiesce);
  RequireOk(quiesced, "quiesce failed");
  Require(HasEvidence(quiesced, "unload_behavior", "quiescing_new_invocations_blocked"),
          "quiesce did not emit new-invocation blocking evidence");

  auto quiesced_invoke = InvokeRequest(path, 45, "proc-active", "procedure");
  RequireDiagnostic(api::EngineInvokeExecutableObject(quiesced_invoke),
                    api::kExecutableObjectDiagnosticQuiescing,
                    "quiesced executable accepted a new invocation");

  auto unload = BaseRequest<api::EngineUnloadExecutableObjectRequest>(
      path, 46, "proc-active", "procedure");
  AddManage(&unload);
  const auto unloaded = api::EngineUnloadExecutableObject(unload);
  RequireOk(unloaded, "unload after invocation release failed");
  Require(HasEvidence(unloaded, "unload_behavior", "dispatch_table_removed"),
          "unload did not emit dispatch-table removal evidence");

  auto unloaded_invoke = InvokeRequest(path, 47, "proc-active", "procedure");
  RequireDiagnostic(api::EngineInvokeExecutableObject(unloaded_invoke),
                    api::kExecutableObjectDiagnosticUnloaded,
                    "unloaded executable accepted an invocation");
}

void TestEventTriggerBoundary(const std::filesystem::path& path) {
  auto event_trigger = BaseRequest<api::EngineCreateExecutableObjectRequest>(
      path, 50, "evt-ddl-end", "event_trigger");
  event_trigger.target_schema.uuid.canonical.clear();
  AddEventTriggerManage(&event_trigger);
  AddStoredSblr(&event_trigger, "evt");
  event_trigger.option_envelopes.push_back("event:DDL_COMMAND_END");
  const auto created = api::EngineCreateExecutableObject(event_trigger);
  RequireOk(created, "event trigger create failed");
  Require(HasEvidence(created, "event_trigger_boundary", "ddl_event_filter_registered"),
          "event trigger create did not publish boundary evidence");

  auto fire = BaseRequest<api::EngineFireExecutableEventTriggerRequest>(
      path, 51, "", "event_trigger");
  fire.target_object.uuid.canonical.clear();
  fire.target_schema.uuid.canonical.clear();
  fire.option_envelopes.push_back("engine_event_trigger_dispatch:true");
  fire.option_envelopes.push_back("event:DDL_COMMAND_END");
  fire.option_envelopes.push_back("ddl_command_tag:CREATE_TABLE");
  fire.option_envelopes.push_back("ddl_object_kind:table");
  const auto fired = api::EngineFireExecutableEventTrigger(fire);
  RequireOk(fired, "event trigger fire failed");
  Require(HasEvidence(fired, "event_trigger_boundary", "ddl_event_dispatched_by_engine"),
          "event trigger fire did not emit engine boundary evidence");
  Require(HasRowField(fired, "fired_count", "1"),
          "event trigger fire count mismatch");

  auto self_fire = fire;
  self_fire.context = Context(path, 52, 52);
  self_fire.option_envelopes.back() = "ddl_object_kind:event_trigger";
  const auto suppressed = api::EngineFireExecutableEventTrigger(self_fire);
  RequireOk(suppressed, "event trigger self-boundary dispatch failed");
  Require(HasEvidence(suppressed, "event_trigger_boundary", "event_trigger_self_event_suppressed"),
          "event trigger self-boundary was not suppressed");
  Require(HasRowField(suppressed, "fired_count", "0"),
          "event trigger self-boundary fired unexpectedly");

  auto unavailable = fire;
  unavailable.context = Context(path, 53, 53);
  unavailable.option_envelopes.push_back("event_trigger_authority_unavailable:true");
  RequireDiagnostic(api::EngineFireExecutableEventTrigger(unavailable),
                    api::kExecutableObjectDiagnosticEventTriggerAuthorityUnavailable,
                    "event trigger authority-unavailable boundary did not fail closed");
}

}  // namespace

int main() {
  const auto path = TestPath();
  TestCreateAlterDropDependencyAndGeneration(path);
  TestPermissionExecutionBoundaryAndStoredSblr(path);
  TestSideEffectPolicy(path);
  TestUnloadQuiesceAndActiveInvocation(path);
  TestEventTriggerBoundary(path);
  std::filesystem::remove(path.string() + ".sb.executable_object_events");
  return EXIT_SUCCESS;
}
