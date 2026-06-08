// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_commercial_evidence.hpp"
#include "agent_durable_catalog.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agents = scratchbird::core::agents;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

agents::AgentActionRequest Action(const std::string& uuid) {
  agents::AgentActionRequest action;
  action.action_uuid = uuid;
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0770-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "ceic-077:" + uuid;
  action.inputs["metric_digest"] = "sha256:metric-digest-ceic-077";
  action.inputs["scope_uuid"] = "019f0770-0000-7000-8000-000000000011";
  return action;
}

agents::AgentActionAuthorityProvenance Authority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::operator_request;
  authority.principal_uuid = "019f0770-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0770-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0770-0000-7000-8000-000000000022";
  authority.operator_authority = true;
  authority.rights = {"OBS_AGENT_CONTROL"};
  return authority;
}

agents::CommercialAgentEvidenceBuildRequest BuildRequest(
    const std::string& action_uuid,
    const std::string& previous_tamper_digest =
        "scratchbird-agent-evidence-ledger-genesis",
    const std::string& redaction_class = "standard") {
  agents::CommercialAgentEvidenceBuildRequest request;
  request.action = Action(action_uuid);
  request.authority = Authority();
  request.provider_id = "page_manager:preallocate_page_family";
  request.input_evidence_digest = "sha256:input-evidence:" + action_uuid;
  request.input_metric_digest = "sha256:metric-digest-ceic-077";
  request.policy_generation = 77;
  request.scope_uuids = {"019f0770-0000-7000-8000-000000000030"};
  request.decision_payload = "ceic-077-decision-payload:" + action_uuid;
  request.result_state = "completed";
  request.diagnostic_code = "SB_AGENT_ACTION.OUTCOME_VERIFIED";
  request.redaction_class = redaction_class;
  request.retention_class = "audit";
  request.outcome_verification_evidence_uuid =
      "019f0770-0000-7000-8000-000000000040";
  request.previous_tamper_digest = previous_tamper_digest;
  request.tamper_key_id = "agent-evidence-ledger-key-v1";
  request.tamper_key_provenance = "engine_local_protected_hmac_key";
  request.tamper_key_generation = 1;
  request.evidence_key_policy_id = "agent-evidence-key-policy-v1";
  request.tamper_key_rotation_epoch = 1;
  request.tamper_key_not_before_microseconds = 1;
  request.tamper_key_not_after_microseconds = 2000000000000000ull;
  request.key_residency_class = "engine_local_protected";
  request.data_residency_class = "engine_local";
  request.storage_linkage_digest = "sha256:durable-catalog-link:" + action_uuid;
  request.created_at_microseconds = 1700000000000077ull;
  request.production_key_material = true;
  request.test_key_material = false;
  request.key_material_exported = false;
  request.protected_material_present =
      redaction_class == "protected_material";
  return request;
}

agents::AgentRuntimeContext SupportContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f0770-0000-7000-8000-000000000050";
  context.principal_uuid = "019f0770-0000-7000-8000-000000000051";
  context.rights = {"OBS_SUPPORT_BUNDLE_READ"};
  context.wall_now_microseconds = 1700000000001077ull;
  return context;
}

void TestValidPolicyAndChainContinuity() {
  auto first = agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000060"));
  auto second = agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000061",
                   first.tamper_chain_digest));

  const auto first_validation =
      agents::ValidateCommercialAgentEvidence(first);
  Require(first_validation.status.ok, "valid evidence failed validation");
  Require(first_validation.tamper_valid, "tamper validation missing");
  Require(first_validation.key_policy_valid, "key policy was not valid");
  Require(first_validation.redaction_before_buffering,
          "redaction-before-buffering flag missing");
  Require(first_validation.storage_linked, "storage linkage missing");

  agents::CommercialAgentEvidenceChainValidationRequest chain;
  chain.evidence = {first, second};
  chain.key_policy = agents::DefaultCommercialAgentEvidenceKeyPolicy();
  chain.key_policy.key_not_after_microseconds =
      first.tamper_key_not_after_microseconds;
  chain.now_microseconds = 1700000000001077ull;
  const auto chain_result =
      agents::ValidateCommercialAgentEvidenceChain(chain);
  Require(chain_result.status.ok, "valid tamper chain rejected: " +
                                      chain_result.status.diagnostic_code);
  Require(chain_result.validated_records == 2,
          "chain did not validate both records");
}

void TestDurableCatalogRoundTripPreservesKeyPolicyFields() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      "019f0770-0000-7000-8000-000000000070";
  image.authority.transaction_generation = 77;
  image.authority.evidence_uuid =
      "019f0770-0000-7000-8000-000000000071";
  image.authority.database_uuid =
      "019f0770-0000-7000-8000-000000000072";
  image.authority.catalog_storage_uuid =
      "019f0770-0000-7000-8000-000000000073";
  image.authority.storage_commit_evidence_uuid =
      image.authority.evidence_uuid;
  image.authority.catalog_generation = 77;
  image.authority.local_transaction_id = 77;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  image.evidence.push_back(agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000074")));
  const auto refresh = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &image, image.authority.evidence_uuid);
  Require(refresh.ok, "catalog digest refresh failed");
  const auto encoded = agents::SerializeDurableAgentCatalogImage(image);
  const auto decoded =
      agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok,
          "durable catalog image rejected CEIC-077 evidence fields");
  Require(decoded.image.evidence.size() == 1,
          "durable catalog evidence did not round-trip");
  const auto& evidence = decoded.image.evidence.front();
  Require(evidence.evidence_key_policy_id == "agent-evidence-key-policy-v1",
          "evidence key policy id did not round-trip");
  Require(evidence.tamper_key_rotation_epoch == 1,
          "key rotation epoch did not round-trip");
  Require(evidence.redaction_applied_before_buffering,
          "redaction prebuffer flag did not round-trip");
  Require(evidence.production_key_material,
          "production key material flag did not round-trip");
  Require(agents::ValidateCommercialAgentEvidence(evidence).status.ok,
          "round-tripped evidence failed validation");
}

void TestTestKeyAndExportedKeyRefusal() {
  auto request = BuildRequest("019f0770-0000-7000-8000-000000000062");
  request.tamper_key_id = "agent-evidence-ledger-test-key-v1";
  request.tamper_key_provenance = "test_fixture_hmac_key";
  request.test_key_material = true;
  request.production_key_material = false;
  auto evidence = agents::BuildCommercialAgentEvidence(request);
  auto validation = agents::ValidateCommercialAgentEvidence(evidence);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.PRODUCTION_KEY_REFUSED",
          "test key material was accepted");

  request = BuildRequest("019f0770-0000-7000-8000-000000000063");
  request.key_material_exported = true;
  evidence = agents::BuildCommercialAgentEvidence(request);
  validation = agents::ValidateCommercialAgentEvidence(evidence);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.PRODUCTION_KEY_REFUSED",
          "exported key material was accepted");
}

void TestRotationWindowAndResidencyRefusal() {
  auto request = BuildRequest("019f0770-0000-7000-8000-000000000064");
  request.created_at_microseconds = 1000;
  request.tamper_key_not_before_microseconds = 2000;
  auto evidence = agents::BuildCommercialAgentEvidence(request);
  auto validation = agents::ValidateCommercialAgentEvidence(evidence);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.KEY_NOT_YET_VALID",
          "pre-rotation evidence was accepted");

  request = BuildRequest("019f0770-0000-7000-8000-000000000065");
  request.key_residency_class = "external_unattested";
  evidence = agents::BuildCommercialAgentEvidence(request);
  auto policy = agents::DefaultCommercialAgentEvidenceKeyPolicy();
  policy.key_not_after_microseconds =
      evidence.tamper_key_not_after_microseconds;
  const auto key_status =
      agents::ValidateCommercialAgentEvidenceKeyPolicy(evidence, policy);
  Require(!key_status.ok &&
              key_status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.RESIDENCY_MISMATCH",
          "residency mismatch was accepted");
}

void TestLegalHoldAndRetention() {
  auto request = BuildRequest("019f0770-0000-7000-8000-000000000066");
  request.retention_class = "legal_hold";
  request.legal_hold_active = true;
  auto evidence = agents::BuildCommercialAgentEvidence(request);
  auto validation = agents::ValidateCommercialAgentEvidence(
      evidence, 1900000000000000ull);
  Require(validation.status.ok, "legal hold evidence expired or failed");
  Require(evidence.expires_at_microseconds == 0,
          "legal hold evidence had an expiry");

  evidence.expires_at_microseconds = 10;
  agents::FinalizeCommercialAgentEvidenceDigests(&evidence);
  validation = agents::ValidateCommercialAgentEvidence(evidence);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.LEGAL_HOLD_INVALID",
          "invalid legal hold retention was accepted");
}

void TestProtectedViewSuppressesBeforeAndAfterBuffering() {
  auto evidence = agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000067",
                   "scratchbird-agent-evidence-ledger-genesis",
                   "protected_material"));
  Require(evidence.redaction_applied_before_buffering,
          "protected evidence did not run pre-buffer redaction");
  Require(evidence.protected_material_suppressed,
          "protected evidence was not suppressed before buffering");
  Require(evidence.detail == "redacted:protected-material-suppressed",
          "protected detail was stored before redaction");

  agents::CommercialAgentEvidenceViewRequest request;
  request.evidence = evidence;
  request.context = SupportContext();
  request.support_bundle_view = true;
  request.now_microseconds = 1700000000001077ull;
  const auto view = agents::ProjectCommercialAgentEvidenceView(request);
  Require(view.visible, "support-bundle protected evidence hidden");
  Require(view.redacted, "support-bundle protected evidence not redacted");
  Require(view.protected_material_suppressed,
          "support-bundle suppression flag missing");
  Require(view.evidence.principal_uuid.empty(),
          "support-bundle leaked principal");
  Require(view.evidence.input_metric_digest.empty(),
          "support-bundle leaked metric digest");
  Require(view.evidence.tamper_signature == "redacted",
          "support-bundle leaked HMAC signature");
  Require(view.evidence.tamper_key_provenance == "redacted",
          "support-bundle leaked key provenance");
}

void TestBrokenChainRefusal() {
  auto first = agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000068"));
  auto second = agents::BuildCommercialAgentEvidence(
      BuildRequest("019f0770-0000-7000-8000-000000000069",
                   "wrong-previous-digest"));

  agents::CommercialAgentEvidenceChainValidationRequest chain;
  chain.evidence = {first, second};
  chain.key_policy = agents::DefaultCommercialAgentEvidenceKeyPolicy();
  chain.key_policy.key_not_after_microseconds =
      first.tamper_key_not_after_microseconds;
  const auto result = agents::ValidateCommercialAgentEvidenceChain(chain);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE_CHAIN.BROKEN",
          "broken tamper chain was accepted");
}

int main() {
  try {
    TestValidPolicyAndChainContinuity();
    TestDurableCatalogRoundTripPreservesKeyPolicyFields();
    TestTestKeyAndExportedKeyRefusal();
    TestRotationWindowAndResidencyRefusal();
    TestLegalHoldAndRetention();
    TestProtectedViewSuppressesBeforeAndAfterBuffering();
    TestBrokenChainRefusal();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
