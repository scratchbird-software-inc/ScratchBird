// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "uuid.hpp"

// SEARCH_KEY: AEIC_AGENT_SECURITY_NEGATIVE_REDACTION_TESTS

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string MakeUuidText(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "PFAR-016B UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::string database_uuid = MakeUuidText(platform::UuidKind::database, 10);
  std::string principal_uuid = MakeUuidText(platform::UuidKind::principal, 11);
  std::string transaction_uuid = MakeUuidText(platform::UuidKind::transaction, 12);
  std::string agent_uuid = MakeUuidText(platform::UuidKind::object, 13);
  std::string policy_uuid = MakeUuidText(platform::UuidKind::object, 14);
  std::string scope_uuid = MakeUuidText(platform::UuidKind::object, 15);
  std::string request_uuid = MakeUuidText(platform::UuidKind::object, 16);
};

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "pfar-016b-third-party-management";
  context.database_path = "/tmp/pfar-016b.sbdb";
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = 16016;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.catalog_generation_id = 22;
  context.security_epoch = 23;
  context.resource_epoch = 24;
  context.trace_tags.push_back("security.fixture_trace_authority");
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  return context;
}

api::EngineAgentCatalogIdentitySource Identity(const Fixture& fixture) {
  api::EngineAgentCatalogIdentitySource source;
  source.agent_type_id = "page_allocation_manager";
  source.agent_uuid = fixture.agent_uuid;
  source.scope_uuid = fixture.scope_uuid;
  source.policy_uuid = fixture.policy_uuid;
  source.policy_name = "page allocation baseline";
  source.component = "storage.pages";
  source.scope_kind = "database";
  return source;
}

api::EngineThirdPartyAgentManagementRequest Request(
    const Fixture& fixture,
    std::string operation,
    std::initializer_list<std::string_view> rights) {
  api::EngineThirdPartyAgentManagementRequest request;
  request.context = Context(fixture, rights);
  request.operation_id = "agents.third_party.request";
  request.agent_catalog_identity_sources.push_back(Identity(fixture));
  request.management_request.request_uuid = fixture.request_uuid;
  request.management_request.requester_principal_uuid = fixture.principal_uuid;
  request.management_request.external_system_id = "ticketing-system";
  request.management_request.agent_ref = fixture.agent_uuid;
  request.management_request.operation = std::move(operation);
  request.management_request.requested_action = request.management_request.operation;
  request.management_request.policy_ref = fixture.policy_uuid;
  request.management_request.reason_code = "scheduled_maintenance";
  request.management_request.requested_expiry = "2026-06-01T00:00:00Z";
  request.management_request.redaction_context = "support_safe";
  request.management_request.idempotency_key = "pfar-016b-key";
  return request;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

std::string Field(const api::EngineApiResult& result, std::string_view name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name) { return field.second.encoded_value; }
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

void RequireUuidField(const api::EngineApiResult& result,
                      std::string_view field,
                      platform::UuidKind kind) {
  const auto value = Field(result, field);
  Require(!value.empty(), std::string(field) + " missing");
  Require(value.rfind("agent.", 0) != 0 &&
              value.rfind("policy.", 0) != 0 &&
              value.rfind("scope.", 0) != 0,
          std::string(field) + " used label-prefixed identity");
  Require(uuid::ParseDurableEngineIdentityUuid(kind, value).ok(),
          std::string(field) + " is not a durable typed UUID: " + value);
}

void TestAcceptedMutatingRequest(const Fixture& fixture) {
  auto request = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  request.management_request.protected_payload = "operator note";

  const auto result = api::EngineSubmitThirdPartyAgentManagementRequest(request);
  Require(result.ok, "accepted mutating third-party request failed");
  Require(HasDiagnostic(result, "AGENT.THIRD_PARTY.REQUEST_ACCEPTED"),
          "accepted request diagnostic missing");
  Require(Field(result, "result_state") == "request_accepted",
          "mutating request reported action success instead of request acceptance");
  Require(Field(result, "queued") == "true", "mutating request was not queued");
  Require(Field(result, "third_party_authority") == "false",
          "third party was treated as authority");
  Require(Field(result, "parser_finality_authority") == "false",
          "parser/client finality authority leaked into request route");
  Require(Field(result, "agent_uuid") == fixture.agent_uuid,
          "resolved agent UUID was not fixture/catalog sourced");
  Require(Field(result, "policy_uuid") == fixture.policy_uuid,
          "policy UUID was not fixture/catalog sourced");
  RequireUuidField(result, "request_uuid", platform::UuidKind::object);
  RequireUuidField(result, "requester_principal_uuid", platform::UuidKind::principal);
  RequireUuidField(result, "agent_uuid", platform::UuidKind::object);
  RequireUuidField(result, "policy_uuid", platform::UuidKind::object);
  RequireUuidField(result, "request_evidence_uuid", platform::UuidKind::object);
  Require(HasEvidence(result, "agent_third_party_request_evidence"),
          "accepted request did not expose request evidence");
}

void TestReadOnlyRequest(const Fixture& fixture) {
  auto request = Request(fixture, "agents.policy.get", {"OBS_POLICY_READ"});
  const auto result = api::EngineSubmitThirdPartyAgentManagementRequest(request);
  Require(result.ok, "read-only third-party request failed");
  Require(Field(result, "result_state") == "read_result",
          "read-only request did not return read result state");
  Require(Field(result, "queued") == "false", "read-only request was queued");
  Require(HasDiagnostic(result, "AGENT.THIRD_PARTY.READ_RESULT"),
          "read-only request diagnostic missing");
}

void TestCatalogUuidAuthority(const Fixture& fixture) {
  auto type_ref = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  type_ref.management_request.agent_ref = "page_allocation_manager";
  const auto type_ref_result = api::EngineSubmitThirdPartyAgentManagementRequest(type_ref);
  Require(type_ref_result.ok, "catalog-backed agent type reference was not resolved");
  Require(Field(type_ref_result, "agent_uuid") == fixture.agent_uuid,
          "agent type reference did not resolve to catalog agent UUID");
  RequireUuidField(type_ref_result, "agent_uuid", platform::UuidKind::object);

  auto missing_catalog = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  missing_catalog.management_request.agent_ref = "page_allocation_manager";
  missing_catalog.agent_catalog_identity_sources.clear();
  const auto missing_catalog_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(missing_catalog);
  Require(!missing_catalog_result.ok, "static agent label fallback was accepted");
  Require(HasDiagnostic(missing_catalog_result, "AGENT.THIRD_PARTY.AGENT_NOT_FOUND"),
          "missing catalog-backed agent UUID diagnostic mismatch");
  Require(Field(missing_catalog_result, "agent_uuid").empty(),
          "missing catalog-backed agent UUID emitted a UUID field");
  Require(HasEvidence(missing_catalog_result, "agent_third_party_request_evidence"),
          "missing catalog-backed agent UUID did not write request evidence");

  auto policy_mismatch = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  policy_mismatch.agent_catalog_identity_sources.front().policy_uuid =
      MakeUuidText(platform::UuidKind::object, 300);
  const auto policy_mismatch_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(policy_mismatch);
  Require(!policy_mismatch_result.ok, "mismatched catalog policy UUID was accepted");
  Require(HasDiagnostic(policy_mismatch_result, "AGENT.THIRD_PARTY.POLICY_MISMATCH"),
          "policy mismatch diagnostic mismatch");
  Require(Field(policy_mismatch_result, "policy_uuid").empty(),
          "policy mismatch emitted an admitted request row");
  Require(HasEvidence(policy_mismatch_result, "agent_third_party_request_evidence"),
          "policy mismatch did not write request evidence");
}

void TestUnauthenticatedDoesNotLeakEvidence(const Fixture& fixture) {
  auto request = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  request.context.security_context_present = false;
  const auto result = api::EngineSubmitThirdPartyAgentManagementRequest(request);
  Require(!result.ok, "missing security context was accepted");
  Require(HasDiagnostic(result, "AGENT.SECURITY_CONTEXT_REQUIRED"),
          "missing security context diagnostic mismatch");
  Require(result.result_shape.rows.empty(), "pre-auth denial leaked request rows");
  Require(!HasEvidence(result, "agent_third_party_request_evidence"),
          "pre-auth denial wrote request evidence");
}

void TestDeniedAfterAuthenticationWritesEvidence(const Fixture& fixture) {
  auto request = Request(fixture, "agents.restart", {});
  const auto result = api::EngineSubmitThirdPartyAgentManagementRequest(request);
  Require(!result.ok, "insufficient-right request was accepted");
  Require(HasDiagnostic(result, "AGENT.THIRD_PARTY.PERMISSION_DENIED"),
          "insufficient-right diagnostic mismatch");
  Require(Field(result, "result_state") == "denied",
          "authenticated denial row did not report denied");
  Require(HasEvidence(result, "agent_third_party_request_evidence"),
          "authenticated denial did not write request evidence");
}

void TestResidencyDeniedAndBackpressure(const Fixture& fixture) {
  auto residency = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  residency.management_request.residency_allowed = false;
  const auto residency_result = api::EngineSubmitThirdPartyAgentManagementRequest(residency);
  Require(!residency_result.ok, "residency-denied request was accepted");
  Require(HasDiagnostic(residency_result, "AGENT.THIRD_PARTY.RESIDENCY_DENIED"),
          "residency diagnostic mismatch");
  Require(Field(residency_result, "queued") == "false",
          "residency denial queued the request");

  auto backpressure = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  backpressure.management_request.backpressure = true;
  backpressure.management_request.retry_after = "PT30S";
  const auto backpressure_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(backpressure);
  Require(!backpressure_result.ok, "backpressure request was accepted");
  Require(HasDiagnostic(backpressure_result, "AGENT.REQUEST_BACKPRESSURE"),
          "backpressure diagnostic mismatch");
  Require(Field(backpressure_result, "retry_after") == "PT30S",
          "backpressure retry_after missing");
}

void TestDirectActuatorAndRedaction(const Fixture& fixture) {
  auto actuator = Request(fixture, "actuator.preallocate_page_family", {"OBS_AGENT_CONTROL"});
  actuator.management_request.requested_action = "preallocate_page_family";
  const auto actuator_result = api::EngineSubmitThirdPartyAgentManagementRequest(actuator);
  Require(!actuator_result.ok, "direct actuator bypass was accepted");
  Require(HasDiagnostic(actuator_result, "AGENT.THIRD_PARTY.ACTUATOR_BYPASS_DENIED"),
          "direct actuator diagnostic mismatch");

  auto redaction = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  redaction.management_request.protected_payload = "password=secret /var/db/file";
  const auto redaction_result = api::EngineSubmitThirdPartyAgentManagementRequest(redaction);
  Require(redaction_result.ok, "redaction request failed");
  Require(Field(redaction_result, "payload_redacted") == "true",
          "protected payload was not redacted");
  Require(Field(redaction_result, "protected_payload") == "<redacted>",
          "unsafe protected payload leaked");
}

void TestRequiredFieldsAndTypedUuid(const Fixture& fixture) {
  auto missing_idempotency = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  missing_idempotency.management_request.idempotency_key.clear();
  const auto missing_idempotency_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(missing_idempotency);
  Require(!missing_idempotency_result.ok, "missing idempotency key was accepted");
  Require(HasDiagnostic(missing_idempotency_result, "AGENT.THIRD_PARTY.IDEMPOTENCY_REQUIRED"),
          "missing idempotency diagnostic mismatch");
  Require(HasEvidence(missing_idempotency_result, "agent_third_party_request_evidence"),
          "authenticated missing-idempotency denial did not write evidence");

  auto missing_principal = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  missing_principal.management_request.requester_principal_uuid.clear();
  const auto missing_principal_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(missing_principal);
  Require(!missing_principal_result.ok, "missing requester principal was accepted");
  Require(HasDiagnostic(missing_principal_result,
                        "AGENT.THIRD_PARTY.REQUESTER_PRINCIPAL_REQUIRED"),
          "missing principal diagnostic mismatch");
  Require(HasEvidence(missing_principal_result, "agent_third_party_request_evidence"),
          "authenticated missing-principal denial did not write evidence");

  auto malformed_policy = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  malformed_policy.management_request.policy_ref = "policy.page_allocation.default";
  const auto malformed_policy_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(malformed_policy);
  Require(!malformed_policy_result.ok, "malformed policy UUID was accepted");
  Require(HasDiagnostic(malformed_policy_result, "AGENT.THIRD_PARTY.INVALID_UUID"),
          "malformed UUID diagnostic mismatch");

  auto malformed_request = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  malformed_request.management_request.request_uuid = "not-a-uuid";
  const auto malformed_request_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(malformed_request);
  Require(!malformed_request_result.ok, "malformed request UUID was accepted");
  Require(HasDiagnostic(malformed_request_result, "AGENT.THIRD_PARTY.INVALID_UUID"),
          "malformed request UUID diagnostic mismatch");

  auto no_redaction = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  no_redaction.management_request.redaction_context.clear();
  const auto no_redaction_result = api::EngineSubmitThirdPartyAgentManagementRequest(no_redaction);
  Require(!no_redaction_result.ok, "missing redaction context was accepted");
  Require(HasDiagnostic(no_redaction_result, "AGENT.THIRD_PARTY.REDACTION_CONTEXT_REQUIRED"),
          "missing redaction diagnostic mismatch");

  auto no_residency = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  no_residency.management_request.residency_context_present = false;
  const auto no_residency_result = api::EngineSubmitThirdPartyAgentManagementRequest(no_residency);
  Require(!no_residency_result.ok, "missing residency context was accepted");
  Require(HasDiagnostic(no_residency_result, "AGENT.THIRD_PARTY.RESIDENCY_CONTEXT_REQUIRED"),
          "missing residency diagnostic mismatch");

  auto missing_action = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  missing_action.management_request.requested_action.clear();
  const auto missing_action_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(missing_action);
  Require(!missing_action_result.ok, "missing requested action was accepted");
  Require(HasDiagnostic(missing_action_result, "AGENT.THIRD_PARTY.REQUIRED_FIELD_MISSING"),
          "missing action diagnostic mismatch");

  auto mismatched_action = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  mismatched_action.management_request.requested_action = "disable";
  const auto mismatched_action_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(mismatched_action);
  Require(!mismatched_action_result.ok, "mismatched requested action was accepted");
  Require(HasDiagnostic(mismatched_action_result, "AGENT.THIRD_PARTY.ACTION_NOT_ALLOWED"),
          "mismatched action diagnostic mismatch");
}

void TestIdempotencyAndEvidenceBeforeSuccess(const Fixture& fixture) {
  auto first = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  auto second = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  second.management_request.request_uuid = first.management_request.request_uuid;
  second.management_request.idempotency_key = first.management_request.idempotency_key;

  const auto first_result = api::EngineSubmitThirdPartyAgentManagementRequest(first);
  const auto second_result = api::EngineSubmitThirdPartyAgentManagementRequest(second);
  Require(first_result.ok && second_result.ok, "idempotent replay was not stable");
  Require(Field(first_result, "request_uuid") == Field(second_result, "request_uuid"),
          "idempotent replay changed request UUID");
  Require(Field(first_result, "idempotency_key_present") == "true" &&
              Field(second_result, "idempotency_key_present") == "true",
          "idempotency evidence was not returned");

  auto evidence_missing = Request(fixture, "agents.restart", {"OBS_AGENT_CONTROL"});
  evidence_missing.management_request.evidence_store_available = false;
  const auto evidence_missing_result =
      api::EngineSubmitThirdPartyAgentManagementRequest(evidence_missing);
  Require(!evidence_missing_result.ok, "request succeeded without required evidence");
  Require(Field(evidence_missing_result, "result_state") == "pending_evidence",
          "missing evidence did not report pending_evidence");
  Require(HasDiagnostic(evidence_missing_result, "AGENT.THIRD_PARTY.EVIDENCE_REQUIRED"),
          "missing evidence diagnostic mismatch");
}

}  // namespace

int main() {
  const Fixture fixture;
  TestAcceptedMutatingRequest(fixture);
  TestReadOnlyRequest(fixture);
  TestCatalogUuidAuthority(fixture);
  TestUnauthenticatedDoesNotLeakEvidence(fixture);
  TestDeniedAfterAuthenticationWritesEvidence(fixture);
  TestResidencyDeniedAndBackpressure(fixture);
  TestDirectActuatorAndRedaction(fixture);
  TestRequiredFieldsAndTypedUuid(fixture);
  TestIdempotencyAndEvidenceBeforeSuccess(fixture);
  return EXIT_SUCCESS;
}
