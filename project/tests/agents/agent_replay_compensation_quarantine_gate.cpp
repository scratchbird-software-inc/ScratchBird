// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_replay_quarantine.hpp"
#include "agent_action_dispatch.hpp"
#include "agent_commercial_evidence.hpp"
#include "agent_package_provenance_test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentActionAuthorityProvenance Authority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid = "019f0810-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0810-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid =
      "019f0810-0000-7000-8000-000000000022";
  authority.rights = {"OBS_AGENT_CONTROL"};
  authority.sealed_bootstrap_authority = true;
  return authority;
}

agents::AgentActionRequest Action() {
  agents::AgentActionRequest action;
  action.action_uuid = "019f0810-0000-7000-8000-000000000040";
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0810-0000-7000-8000-000000000011";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "ceic081-page-preallocate";
  action.dry_run = false;
  action.inputs["evidence_uuid"] = "019f0810-0000-7000-8000-000000000030";
  action.inputs["metric_digest"] = "sha256:ceic081-observed-metric-digest";
  action.inputs["scope_uuid"] = "019f0810-0000-7000-8000-000000000021";
  return action;
}

agents::DurableAgentCatalogImage CatalogImage() {
  const auto authority = Authority();
  const auto action = Action();

  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      "019f0810-0000-7000-8000-0000000000aa";
  image.authority.transaction_generation = 1;
  image.authority.evidence_uuid =
      "019f0810-0000-7000-8000-0000000000ab";
  image.authority.database_uuid =
      "019f0810-0000-7000-8000-0000000000ac";
  image.authority.catalog_storage_uuid =
      "019f0810-0000-7000-8000-0000000000ad";
  image.authority.storage_commit_evidence_uuid =
      "019f0810-0000-7000-8000-0000000000ae";
  image.authority.local_transaction_id = 81;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = action.instance_uuid;
  instance.agent_type_id = action.agent_type_id;
  instance.policy_uuid = "019f0810-0000-7000-8000-000000000012";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.run_generation = 1;
  instance.policy_generation = 7;
  instance.instance_generation = 1;
  image.instances.push_back(instance);

  const auto descriptor = agents::FindAgentType(action.agent_type_id);
  Require(descriptor.has_value(), "CEIC-081 agent descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = instance.policy_uuid;
  policy.policy_name = "CEIC-081 replay policy";
  policy.scope = instance.scope;
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.enabled = true;
  policy.allow_live_action = true;
  policy.require_manual_approval = true;
  policy.require_dry_run_before_live = false;
  policy.policy_generation = instance.policy_generation;
  image.policies.push_back(policy);

  agents::AgentPolicyAttachmentRecord attachment;
  attachment.attachment_uuid = "019f0810-0000-7000-8000-000000000071";
  attachment.agent_type_id = action.agent_type_id;
  attachment.policy_family = policy.policy_family;
  attachment.policy_uuid = policy.policy_uuid;
  attachment.scope = policy.scope;
  attachment.policy_generation = policy.policy_generation;
  attachment.attachment_generation = policy.policy_generation;
  attachment.baseline = false;
  attachment.active = true;
  attachment.valid = true;
  attachment.diagnostic_code = "SB_AGENT_POLICY_ATTACHMENT.CEIC081";
  attachment.evidence_uuid = "019f0810-0000-7000-8000-000000000072";
  image.attachments.push_back(attachment);

  const std::string action_digest = agents::AgentActionInputEvidenceDigest(action);
  agents::CommercialAgentEvidenceBuildRequest evidence_request;
  evidence_request.action = action;
  evidence_request.authority = authority;
  evidence_request.provider_id = "page_manager:preallocate_page_family";
  evidence_request.input_evidence_digest = action_digest;
  evidence_request.input_metric_digest = action.inputs.at("metric_digest");
  evidence_request.policy_generation = instance.policy_generation;
  evidence_request.scope_uuids = {authority.scope_uuid};
  evidence_request.decision_payload =
      "ceic081|page_manager:preallocate_page_family|completed";
  evidence_request.result_state = "completed";
  evidence_request.diagnostic_code = "SB_AGENT_ACTION.OUTCOME_VERIFIED";
  evidence_request.outcome_verification_evidence_uuid =
      "019f0810-0000-7000-8000-000000000031";
  evidence_request.created_at_microseconds = 1810000000000000ull;
  evidence_request.tamper_key_id = "agent-evidence-ledger-key-v1";
  evidence_request.tamper_key_provenance = "engine_local_protected_hmac_key";
  evidence_request.evidence_key_policy_id = "agent-evidence-key-policy-v1";
  evidence_request.tamper_key_generation = 1;
  evidence_request.tamper_key_rotation_epoch = 1;
  evidence_request.tamper_key_not_before_microseconds = 1;
  evidence_request.tamper_key_not_after_microseconds =
      1810000000000000ull + 365ull * 86400000000ull;
  evidence_request.key_residency_class = "engine_local_protected";
  evidence_request.data_residency_class = "engine_local";
  evidence_request.production_key_material = true;
  evidence_request.storage_linkage_digest =
      "sha256:ceic081-action-evidence-storage-link";
  auto evidence = agents::BuildCommercialAgentEvidence(evidence_request);
  Require(agents::ValidateCommercialAgentEvidence(evidence).status.ok,
          "CEIC-081 evidence was not valid");
  image.evidence.push_back(evidence);

  agents::DurableAgentActionRecord record;
  record.action_uuid = action.action_uuid;
  record.instance_uuid = action.instance_uuid;
  record.owner_uuid = authority.principal_uuid;
  record.operation_id = action.operation_id;
  record.actuator_provider_id = action.actuator_id + ":" + action.operation_id;
  record.state = agents::DurableAgentActionState::completed;
  record.idempotency_key = action.idempotency_key;
  record.input_evidence_digest = action_digest;
  record.evidence_uuid = evidence.evidence_uuid;
  record.verification_evidence_uuid =
      evidence.outcome_verification_evidence_uuid;
  record.diagnostic_code = "SB_AGENT_ACTION.OUTCOME_VERIFIED";
  record.generation = 1;
  record.outcome_verified = true;
  record.compensation_required = false;
  image.actions.push_back(record);

  agents::DurableAgentResourceReservationRecord reservation;
  reservation.reservation_uuid =
      "019f0810-0000-7000-8000-000000000081";
  reservation.reservation_key =
      "action_dispatch:page_allocation_manager:" + action.idempotency_key;
  reservation.owner_scope = authority.principal_uuid;
  reservation.agent_type_id = action.agent_type_id;
  reservation.operation_id = action.operation_id;
  reservation.state =
      agents::DurableAgentResourceReservationState::released;
  reservation.acquired_at_microseconds = 1810000000000000ull;
  reservation.released_at_microseconds = 1810000000000100ull;
  reservation.memory_bytes = 4096;
  reservation.worker_slots = 1;
  reservation.overhead_microseconds = 1000;
  reservation.evidence_uuid =
      "019f0810-0000-7000-8000-000000000082";
  reservation.release_evidence_uuid = evidence.evidence_uuid;
  reservation.release_reason = "completed";
  image.resource_reservations.push_back(reservation);

  const auto refreshed = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &image, "019f0810-0000-7000-8000-0000000000af");
  Require(refreshed.ok, "CEIC-081 catalog digest refresh failed: " +
                            refreshed.diagnostic_code);
  Require(agents::ValidateDurableAgentCatalogForProduction(image).ok,
          "CEIC-081 catalog was not production-valid");
  return image;
}

agents::AgentReplayDigestCapture Capture(
    const agents::DurableAgentCatalogImage& image,
    const agents::AgentPackageProvenanceBundle& package) {
  agents::AgentReplayDigestCaptureRequest request;
  request.catalog = &image;
  request.action_uuid = Action().action_uuid;
  request.security_epoch = 81;
  request.package_provenance = package;
  const auto captured = agents::CaptureAgentReplayDigests(request);
  Require(captured.status.ok, "CEIC-081 digest capture failed: " +
                                  captured.status.diagnostic_code);
  return captured.capture;
}

agents::AgentReplayControlRequest ReplayRequest(
    agents::DurableAgentCatalogImage* image,
    agents::AgentReplayOperationKind operation,
    agents::AgentReplayDigestCapture capture,
    const agents::AgentPackageProvenanceBundle& package,
    const std::string& suffix) {
  agents::AgentReplayControlRequest request;
  request.catalog = image;
  request.action_uuid = Action().action_uuid;
  request.operation = operation;
  request.capture = std::move(capture);
  request.package_provenance = package;
  request.evidence_uuid =
      "019f0810-0000-7000-8000-0000000001" + suffix;
  request.now_microseconds = 1810000000001000ull + suffix.size();
  request.max_retry_count = 3;
  request.retry_after_microseconds = 5000;
  return request;
}

void TestReplayRetryCompensationAndQuarantineAreDurableAndIdempotent() {
  auto image = CatalogImage();
  const auto package = agent_test_support::PageProviderPackageProvenance(
      "019f0810-0000-7000-8000-00000000009");

  auto replay = ReplayRequest(&image,
                              agents::AgentReplayOperationKind::mark_replay_pending,
                              Capture(image, package),
                              package,
                              "01");
  auto result = agents::ApplyAgentReplayControl(replay);
  Require(result.status.ok && result.replay_record_written &&
              result.action_state_updated,
          "CEIC-081 replay-pending application failed: " +
              result.status.diagnostic_code);
  Require(image.actions.front().state ==
              agents::DurableAgentActionState::replay_pending,
          "CEIC-081 replay did not mark action replay_pending");
  Require(image.replay_records.size() == 1,
          "CEIC-081 replay record was not durable-catalog-backed");
  const auto duplicate = agents::ApplyAgentReplayControl(replay);
  Require(duplicate.status.ok && duplicate.idempotent,
          "CEIC-081 duplicate replay was not idempotent");
  Require(image.replay_records.size() == 1,
          "CEIC-081 duplicate replay wrote a second record");

  auto retry = ReplayRequest(&image,
                             agents::AgentReplayOperationKind::schedule_retry,
                             Capture(image, package),
                             package,
                             "02");
  result = agents::ApplyAgentReplayControl(retry);
  Require(result.status.ok && result.retry_scheduled,
          "CEIC-081 retry scheduling failed");
  Require(image.actions.front().retry_scheduled &&
              image.actions.front().retry_count == 1,
          "CEIC-081 retry state not written to action record");

  auto compensation =
      ReplayRequest(&image,
                    agents::AgentReplayOperationKind::record_compensation,
                    Capture(image, package),
                    package,
                    "03");
  compensation.compensation_evidence_uuid =
      "019f0810-0000-7000-8000-0000000002c3";
  result = agents::ApplyAgentReplayControl(compensation);
  Require(result.status.ok && result.compensation_recorded,
          "CEIC-081 compensation record failed");
  Require(image.actions.front().compensation_attempted &&
              image.actions.front().state ==
                  agents::DurableAgentActionState::quarantined,
          "CEIC-081 compensation did not quarantine failed action");

  auto release_without_review =
      ReplayRequest(&image,
                    agents::AgentReplayOperationKind::release_quarantine,
                    Capture(image, package),
                    package,
                    "04");
  result = agents::ApplyAgentReplayControl(release_without_review);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.QUARANTINE_REVIEW_REQUIRED",
          "CEIC-081 quarantine release succeeded without review evidence");

  auto release = release_without_review;
  release.review_evidence_uuid =
      "019f0810-0000-7000-8000-0000000002d4";
  result = agents::ApplyAgentReplayControl(release);
  Require(result.status.ok && result.quarantine_released,
          "CEIC-081 quarantine release with review failed");
  Require(image.actions.front().state ==
              agents::DurableAgentActionState::replay_pending,
          "CEIC-081 reviewed quarantine release did not return to replay_pending");
  Require(!image.retained_history.empty(),
          "CEIC-081 replay history was not retained");

  const auto encoded = agents::SerializeDurableAgentCatalogImage(image);
  const auto decoded = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok,
          "CEIC-081 serialized replay catalog was not production-valid: " +
              decoded.status.diagnostic_code);
  Require(decoded.image.replay_records.size() == image.replay_records.size(),
          "CEIC-081 replay records did not survive durable serialization");
}

void TestDigestAndAuthorityFailuresFailClosed() {
  auto image = CatalogImage();
  const auto package = agent_test_support::PageProviderPackageProvenance(
      "019f0810-0000-7000-8000-0000000000a");
  const auto capture = Capture(image, package);

  auto missing = ReplayRequest(&image,
                               agents::AgentReplayOperationKind::mark_replay_pending,
                               capture,
                               package,
                               "11");
  missing.capture.metric_digest.clear();
  auto result = agents::ApplyAgentReplayControl(missing);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.DIGEST_CAPTURE_REQUIRED",
          "CEIC-081 missing metric digest did not fail closed");

  auto stale_policy = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "12");
  stale_policy.capture.policy_digest = "sha256:stale-policy";
  result = agents::ApplyAgentReplayControl(stale_policy);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.POLICY_DIGEST_STALE",
          "CEIC-081 stale policy digest did not fail closed");

  auto stale_security = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "13");
  stale_security.capture.security_digest = "sha256:stale-security";
  result = agents::ApplyAgentReplayControl(stale_security);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.SECURITY_DIGEST_STALE",
          "CEIC-081 stale security digest did not fail closed");

  auto stale_resource = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "14");
  stale_resource.capture.resource_reservation_digest =
      "sha256:stale-resource";
  result = agents::ApplyAgentReplayControl(stale_resource);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.RESOURCE_DIGEST_STALE",
          "CEIC-081 stale resource digest did not fail closed");

  auto stale_binary = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "15");
  stale_binary.capture.binary_package_digest = "sha256:stale-binary";
  result = agents::ApplyAgentReplayControl(stale_binary);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.BINARY_PACKAGE_DIGEST_STALE",
          "CEIC-081 stale binary/package digest did not fail closed");

  auto stale_action = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "16");
  stale_action.capture.action_record_digest = "sha256:stale-action";
  result = agents::ApplyAgentReplayControl(stale_action);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.ACTION_EVIDENCE_DIGEST_STALE",
          "CEIC-081 stale action/evidence digest did not fail closed");

  auto stale_catalog_image = image;
  stale_catalog_image.health.push_back(
      {"019f0810-0000-7000-8000-000000000011",
       "observed",
       "SB_AGENT_HEALTH.CEIC081",
       "019f0810-0000-7000-8000-000000000099",
       1810000000000900ull});
  Require(agents::RefreshDurableAgentCatalogAuthorityDigest(
              &stale_catalog_image,
              "019f0810-0000-7000-8000-00000000009a")
              .ok,
          "CEIC-081 stale catalog setup failed");
  auto stale_catalog = ReplayRequest(
      &stale_catalog_image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "17");
  result = agents::ApplyAgentReplayControl(stale_catalog);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.CATALOG_DIGEST_STALE",
          "CEIC-081 stale catalog digest did not fail closed");

  auto cluster = ReplayRequest(&image,
                               agents::AgentReplayOperationKind::mark_replay_pending,
                               capture,
                               package,
                               "18");
  cluster.cluster_route_requested = true;
  result = agents::ApplyAgentReplayControl(cluster);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          "CEIC-081 local cluster replay did not fail closed");

  auto forbidden = ReplayRequest(
      &image,
      agents::AgentReplayOperationKind::mark_replay_pending,
      capture,
      package,
      "19");
  forbidden.memory_authority = true;
  result = agents::ApplyAgentReplayControl(forbidden);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_REPLAY.AUTHORITY_FLAG_FORBIDDEN",
          "CEIC-081 forbidden replay authority did not fail closed");

  Require(image.replay_records.empty(),
          "CEIC-081 failed-closed replay attempts wrote durable records");
}

}  // namespace

int main() {
  TestReplayRetryCompensationAndQuarantineAreDurableAndIdempotent();
  TestDigestAndAuthorityFailuresFailClosed();
  return EXIT_SUCCESS;
}
