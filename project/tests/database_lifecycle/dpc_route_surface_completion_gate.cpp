// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"

#include <array>
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
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    if (!uuid::UuidKindAllowsDurableIdentity(kind)) {
      const auto raw = uuid::GenerateCompatibilityUnixTimeV7(base_millis + salt);
      Require(raw.ok(), "DPC-060 generated UUID creation failed");
      const auto typed = uuid::MakeTypedUuid(kind, raw.value);
      Require(typed.ok(), "DPC-060 generated UUID creation failed");
      return typed.value;
    }
    const auto generated = uuid::GenerateDurableEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-060 generated UUID creation failed");
    return generated.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

sb_engine_uuid_t EngineUuid(const platform::TypedUuid& typed) {
  sb_engine_uuid_t out{};
  std::memcpy(out.bytes, typed.value.bytes.data(), typed.value.bytes.size());
  return out;
}

struct RouteIds {
  platform::TypedUuid database;
  platform::TypedUuid table;
  platform::TypedUuid index;
  platform::TypedUuid generation;
  platform::TypedUuid principal;
  platform::TypedUuid session;
};

RouteIds MakeRouteIds(const UuidFactory& uuids) {
  RouteIds ids;
  ids.database = uuids.Typed(platform::UuidKind::database, 1);
  ids.table = uuids.Typed(platform::UuidKind::object, 2);
  ids.index = uuids.Typed(platform::UuidKind::object, 3);
  ids.generation = uuids.Typed(platform::UuidKind::object, 4);
  ids.principal = uuids.Typed(platform::UuidKind::principal, 5);
  ids.session = uuids.Typed(platform::UuidKind::session, 6);
  return ids;
}

std::string Text(const platform::TypedUuid& typed) {
  return uuid::UuidToString(typed.value);
}

api::EngineUuid ApiUuid(const platform::TypedUuid& typed) {
  return {Text(typed)};
}

api::EngineMaterializedAuthorizationGrant GrantForIndex(const RouteIds& ids,
                                                        std::string right,
                                                        std::uint64_t salt) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = Text(ids.generation) + ":dpc060-grant-" +
                               std::to_string(salt);
  grant.subject_uuid = ApiUuid(ids.principal);
  grant.subject_kind = "principal";
  grant.target_uuid = ApiUuid(ids.index);
  grant.right = std::move(right);
  grant.security_epoch = 1;
  return grant;
}

api::EngineMaterializedAuthorizationContext AuthorizationContextFor(
    const RouteIds& ids) {
  api::EngineMaterializedAuthorizationContext authorization;
  authorization.present = true;
  authorization.authority_uuid.canonical = Text(ids.database) + ":dpc060-authority";
  authorization.principal_uuid = ApiUuid(ids.principal);
  authorization.security_epoch = 1;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = 1;
  authorization.effective_subjects.push_back(
      {ApiUuid(ids.principal), "principal"});
  authorization.grants.push_back(
      GrantForIndex(ids, "OBS_INDEX_PROFILE_READ", 1));
  authorization.grants.push_back(
      GrantForIndex(ids, "OBS_MANAGEMENT_CONTROL", 2));
  authorization.evidence_tags.push_back("dpc060_materialized_authorization");
  return authorization;
}

void AddOperand(engine_sblr::SblrOperationEnvelope* envelope,
                std::string name,
                std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

const char* OpcodeFor(std::string_view operation_id) {
  const auto* entry = engine_sblr::LookupSblrOperation(operation_id);
  Require(entry != nullptr, "DPC-060 SBLR opcode registry entry missing");
  return entry->opcode.c_str();
}

engine_sblr::SblrOperationEnvelope MakeIndexEnvelope(std::string operation_id,
                                                     const RouteIds& ids,
                                                     std::string validation_family,
                                                     bool policy_allows_mutation) {
  auto envelope = engine_sblr::MakeSblrEnvelope(operation_id,
                                                OpcodeFor(operation_id),
                                                "dpc060.route_surface");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  AddOperand(&envelope, "database_uuid", Text(ids.database));
  AddOperand(&envelope, "table_uuid", Text(ids.table));
  AddOperand(&envelope, "index_uuid", Text(ids.index));
  AddOperand(&envelope, "target_object_uuid", Text(ids.index));
  AddOperand(&envelope, "generation_uuid", Text(ids.generation));
  AddOperand(&envelope, "index_family", "btree");
  AddOperand(&envelope, "validation_family", std::move(validation_family));
  AddOperand(&envelope, "policy_allows_mutation",
             policy_allows_mutation ? "true" : "false");
  AddOperand(&envelope, "catalog_resolution_proven", "true");
  AddOperand(&envelope, "names_resolved_to_uuids", "true");
  return envelope;
}

api::EngineRequestContext MakeContext(const RouteIds& ids) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.request_id = "dpc060-route-surface";
  context.database_uuid.canonical = Text(ids.database);
  context.principal_uuid.canonical = Text(ids.principal);
  context.session_uuid.canonical = Text(ids.session);
  context.transaction_uuid.canonical = "transaction:dpc060-local";
  context.local_transaction_id = 60;
  context.snapshot_visible_through_local_transaction_id = 60;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("group:OPS");
  context.authorization_context = AuthorizationContextFor(ids);
  return context;
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

void AssertRegistryEntry(std::string_view operation_id,
                         std::string_view opcode,
                         bool mutating) {
  const auto* entry = engine_sblr::LookupSblrOperation(operation_id);
  Require(entry != nullptr, "DPC-060 public/admin index registry entry missing");
  Require(entry->opcode == opcode, "DPC-060 index registry opcode mismatch");
  Require(entry->category == engine_sblr::SblrOpcodeCategory::management,
          "DPC-060 index registry entry is not management category");
  Require(entry->support == engine_sblr::SblrOpcodeSupport::implemented,
          "DPC-060 index registry entry is not implemented");
  Require(entry->security_class == engine_sblr::SblrOpcodeSecurityClass::admin_authorized,
          "DPC-060 index registry entry is not admin authorized");
  Require(entry->requires_security_context,
          "DPC-060 index registry entry does not require security context");
  Require(entry->requires_transaction_context,
          "DPC-060 index registry entry does not require transaction context");
  if (mutating) {
    Require(entry->transaction_effect == engine_sblr::SblrOpcodeTransactionEffect::management,
            "DPC-060 mutating index route does not use management transaction effect");
  } else {
    Require(entry->transaction_effect == engine_sblr::SblrOpcodeTransactionEffect::read,
            "DPC-060 read-only index route does not use read transaction effect");
  }
}

void AssertServerAdmission(const std::string& encoded,
                           std::string_view expected_operation) {
  server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_envelope = encoded;
  const auto admission = server::AdmitServerSblrEnvelope(request);
  Require(admission.admitted, "DPC-060 server admission refused index SBLR route");
  Require(admission.operation_id == expected_operation,
          "DPC-060 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.index.maintenance.v3",
          "DPC-060 server admission did not classify index maintenance family");
  Require(admission.requires_public_abi_dispatch,
          "DPC-060 server admission did not require public ABI dispatch");
}

void AssertSblrDispatch(const engine_sblr::SblrOperationEnvelope& envelope,
                        const api::EngineRequestContext& context,
                        bool expect_mutating) {
  engine_sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = envelope;
  const auto result = engine_sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "DPC-060 SBLR envelope did not validate");
  Require(result.accepted, "DPC-060 SBLR dispatch did not accept index route");
  Require(result.dispatched_to_api,
          "DPC-060 SBLR dispatch did not reach engine API");
  Require(result.api_result.ok, "DPC-060 index management API refused valid route");
  Require(HasEvidence(result.api_result, "engine_api_function",
                      "EngineIndexManagementOperation"),
          "DPC-060 dispatch missing EngineIndexManagementOperation evidence");
  Require(HasEvidence(result.api_result, "route_surface", "sblr"),
          "DPC-060 dispatch missing SBLR route evidence");
  Require(HasEvidence(result.api_result, "transaction_authority", "engine_mga"),
          "DPC-060 dispatch missing engine MGA authority evidence");
  Require(HasEvidence(result.api_result,
                      "driver_visible_classification",
                      "capability_or_admin_route_not_driver_speed_benchmark"),
          "DPC-060 dispatch missing driver-visible classification evidence");
  if (expect_mutating) {
    Require(HasEvidence(result.api_result, "index_support", "operation=repair") ||
                HasEvidence(result.api_result, "index_support",
                            "operation=discard_unpublished"),
            "DPC-060 mutating route did not report index operation evidence");
  }
}

std::string PayloadText(sb_engine_result_t result) {
  sb_engine_string_view_t payload{};
  Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK,
          "DPC-060 public ABI payload unavailable");
  return payload.data == nullptr ? std::string{} : std::string(payload.data, payload.size_bytes);
}

void AssertPublicAbiDispatch(const engine_sblr::SblrOperationEnvelope& envelope,
                             const RouteIds& ids) {
  const std::string text = engine_sblr::EncodeSblrEnvelope(envelope);
  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_control, 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                        text.size())
                          .encode();

  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK &&
              engine != nullptr,
          "DPC-060 public ABI engine open failed");

  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid = EngineUuid(ids.principal);
  session_params.session_uuid = EngineUuid(ids.session);
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  sb_engine_session_t session = nullptr;
  Require(sb_engine_session_begin(engine, &session_params, &session, nullptr) ==
              SB_ENGINE_STATUS_OK &&
              session != nullptr,
          "DPC-060 public ABI session begin failed");

  sb_engine_request_context_v1_t context{};
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.effective_user_uuid = EngineUuid(ids.principal);
  context.session_uuid = EngineUuid(ids.session);
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;
  context.transaction_ref = 60;

  sb_engine_sblr_dispatch_params_v1_t dispatch{};
  dispatch.struct_size = sizeof(dispatch);
  dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  dispatch.envelope_bytes = binary.data();
  dispatch.envelope_size_bytes = static_cast<std::uint64_t>(binary.size());
  sb_engine_result_t result = nullptr;
  Require(sb_engine_dispatch_sblr(session, nullptr, &context, &dispatch, &result) ==
              SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "DPC-060 public ABI SBLR dispatch failed");
  const std::string payload = PayloadText(result);
  Require(payload.find("EngineIndexManagementOperation") != std::string::npos,
          "DPC-060 public ABI payload missing index management API evidence");
  Require(payload.find("driver_visible_classification") != std::string::npos,
          "DPC-060 public ABI payload missing driver classification evidence");
  (void)sb_engine_result_release(result);

  sb_engine_session_end_params_v1_t end{};
  end.struct_size = sizeof(end);
  end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions = 1;
  end.cancel_open_results = 1;
  (void)sb_engine_session_end(session, &end, nullptr);
  (void)sb_engine_close(engine, nullptr);
}

void AssertCapabilityReportHasNoBenchmarkClaim() {
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK &&
              engine != nullptr,
          "DPC-060 capability engine open failed");
  sb_engine_capability_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  sb_engine_result_t result = nullptr;
  Require(sb_engine_describe_capabilities(engine, &request, &result) ==
              SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "DPC-060 capability report failed");
  const std::string payload = PayloadText(result);
  Require(payload.find("sblr_dispatch=") != std::string::npos,
          "DPC-060 capability report did not expose route capability metadata");
  Require(payload.find("driver_speed_benchmark") == std::string::npos,
          "DPC-060 capability report falsely claims driver speed benchmark evidence");
  Require(payload.find("executor_batch_benchmark") == std::string::npos,
          "DPC-060 capability report falsely exposes executor batch benchmark evidence");
  (void)sb_engine_result_release(result);
  (void)sb_engine_close(engine, nullptr);
}

void AssertExecutorBatchInternalOnly() {
  Require(engine_sblr::LookupSblrOperation("executor.batch") == nullptr,
          "DPC-060 executor batching leaked as direct SBLR driver operation");
  Require(engine_sblr::LookupSblrOperation("executor.batch.benchmark") == nullptr,
          "DPC-060 executor benchmark leaked as direct SBLR driver operation");
  Require(engine_sblr::LookupSblrOpcode("SBLR_EXECUTOR_BATCH") == nullptr,
          "DPC-060 executor batching leaked as direct SBLR opcode");
}

}  // namespace

int main() {
  const UuidFactory uuids;
  const RouteIds ids = MakeRouteIds(uuids);
  const auto context = MakeContext(ids);

  AssertRegistryEntry("index.validate", "SBLR_INDEX_VALIDATE", false);
  AssertRegistryEntry("index.repair", "SBLR_INDEX_REPAIR", true);
  AssertRegistryEntry("index.discard_unpublished",
                      "SBLR_INDEX_DISCARD_UNPUBLISHED",
                      true);

  const auto validate = MakeIndexEnvelope("index.validate",
                                          ids,
                                          "ordered_table_candidate_set",
                                          false);
  const auto repair = MakeIndexEnvelope("index.repair",
                                        ids,
                                        "secondary_delta_ledger",
                                        true);
  const auto discard = MakeIndexEnvelope("index.discard_unpublished",
                                         ids,
                                         "ordered_table_candidate_set",
                                         true);

  AssertServerAdmission(engine_sblr::EncodeSblrEnvelope(validate), "index.validate");
  AssertServerAdmission("sblr.index.validate", "index.validate");
  server::ServerSblrAdmissionRequest raw_sql;
  raw_sql.encoded_sblr_envelope = "select * from dpc060_route_surface";
  Require(!server::AdmitServerSblrEnvelope(raw_sql).admitted,
          "DPC-060 raw SQL bypass was admitted");

  AssertSblrDispatch(validate, context, false);
  AssertSblrDispatch(repair, context, true);
  AssertSblrDispatch(discard, context, true);
  AssertPublicAbiDispatch(validate, ids);
  AssertCapabilityReportHasNoBenchmarkClaim();
  AssertExecutorBatchInternalOnly();

  std::cout << "dpc_route_surface_completion_gate=passed\n";
  return EXIT_SUCCESS;
}
