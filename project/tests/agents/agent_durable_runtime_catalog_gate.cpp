// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_durable_catalog.hpp"
#include "agent_runtime_service.hpp"

#include <algorithm>
#include <openssl/sha.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agents = scratchbird::core::agents;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

bool LayoutContains(const std::vector<std::string>& layouts,
                    const std::string& token) {
  return std::any_of(layouts.begin(), layouts.end(), [&](const auto& layout) {
    return layout.find(token) != std::string::npos;
  });
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x01", 16);
  image.authority.transaction_generation = 7;
  image.authority.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x02", 16);
  image.authority.database_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x03", 16);
  image.authority.catalog_storage_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x04", 16);
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 7007;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x10", 16);
  instance.agent_type_id = "storage_health_manager";
  instance.policy_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x11", 16);
  instance.scope = "node/database/filespace";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = 7;
  instance.instance_generation = 7;
  instance.retired_generation = 1;
  instance.last_run_start_microseconds = 101;
  instance.last_run_end_microseconds = 202;
  instance.crash_loop_count = 2;
  instance.supervision_failure_count = 3;
  instance.restart_attempts = 1;
  instance.restart_not_before_microseconds = 303;
  instance.cooldown_until_microseconds = 404;
  instance.disabled_by_operator = true;
  instance.safe_mode = true;
  instance.quarantined = true;
  instance.cancellation_requested = true;
  instance.retirement_evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x19", 16);
  instance.last_failure_diagnostic_code = "SB_AGENT_TEST.FAILURE";
  instance.last_supervision_detail = "detail with separators |=;\tand newline\nkept";
  image.instances.push_back(instance);

  agents::AgentPolicy policy;
  policy.policy_uuid = instance.policy_uuid;
  policy.policy_name = "storage health policy";
  policy.policy_family = "storage_health_policy";
  policy.scope = instance.scope;
  policy.policy_generation = 7;
  policy.action_mode = "recommend_only";
  policy.invalid_policy_behavior = "fail_closed";
  policy.activation = agents::AgentActivationProfile::recommend_only;
  policy.allow_live_action = true;
  policy.require_manual_approval = false;
  policy.require_dry_run_before_live = false;
  policy.evidence_required = true;
  policy.explainability_required = true;
  policy.run_interval_microseconds = 11;
  policy.jitter_microseconds = 12;
  policy.lease_microseconds = 13;
  policy.cooldown_microseconds = 14;
  policy.max_runtime_microseconds = 15;
  policy.max_restart_attempts = 16;
  policy.initial_backoff_microseconds = 17;
  policy.max_backoff_microseconds = 18;
  policy.max_history_query_rows = 19;
  policy.max_evidence_fanout = 20;
  policy.max_label_cardinality = 21;
  policy.action_budget_per_window = 22;
  policy.required_metric_families = {"sb_storage_health", "sb_page|capacity"};
  policy.policy_dependencies = {"policy=dependency;one"};
  policy.config_fields["threshold"] = "75";
  policy.config_fields["escaped"] = "value|with=separators;and%percent";
  image.policies.push_back(policy);

  agents::AgentPolicyAttachmentRecord attachment;
  attachment.attachment_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x12", 16);
  attachment.agent_type_id = instance.agent_type_id;
  attachment.policy_family = policy.policy_family;
  attachment.policy_uuid = instance.policy_uuid;
  attachment.scope = instance.scope;
  attachment.policy_generation = 7;
  attachment.evidence_uuid = image.authority.evidence_uuid;
  attachment.baseline = false;
  attachment.valid = false;
  attachment.diagnostic_code = "SB_AGENT_ATTACHMENT.TEST";
  image.attachments.push_back(attachment);

  agents::AgentEvidenceRecord evidence;
  evidence.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x13", 16);
  evidence.agent_type_id = instance.agent_type_id;
  evidence.instance_uuid = instance.instance_uuid;
  evidence.evidence_kind = "catalog_open";
  evidence.diagnostic_code = "SB_AGENT_CATALOG.PRODUCTION_AUTHORITY_ACCEPTED";
  evidence.detail = "full durable evidence payload";
  evidence.redaction_class = "restricted";
  evidence.created_at_microseconds = 100;
  evidence.expires_at_microseconds = 200;
  image.evidence.push_back(evidence);

  agents::DurableAgentApprovalRecord approval;
  approval.approval_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x14", 16);
  approval.action_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x15", 16);
  approval.principal_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x16", 16);
  approval.evidence_uuid = evidence.evidence_uuid;
  approval.approved = true;
  approval.approved_at_microseconds = 300;
  image.approvals.push_back(approval);

  agents::DurableAgentOverrideRecord override_record;
  override_record.override_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x17", 16);
  override_record.agent_type_id = instance.agent_type_id;
  override_record.scope = instance.scope;
  override_record.principal_uuid = approval.principal_uuid;
  override_record.expires_at_microseconds = 400;
  override_record.active = true;
  override_record.evidence_uuid = evidence.evidence_uuid;
  image.overrides.push_back(override_record);

  agents::DurableAgentHealthRecord health;
  health.instance_uuid = instance.instance_uuid;
  health.health_state = "unknown";
  health.diagnostic_code = "SB_AGENT_HEALTH.BOOTSTRAPPED";
  health.evidence_uuid = evidence.evidence_uuid;
  image.health.push_back(health);

  agents::DurableAgentHistoryRecord history;
  history.history_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x18", 16);
  history.subject_uuid = instance.instance_uuid;
  history.event_kind = "catalog_bootstrap";
  history.diagnostic_code = "SB_AGENT_CATALOG.HISTORY_RETAINED";
  history.evidence_uuid = evidence.evidence_uuid;
  image.retained_history.push_back(history);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "fixture durable catalog root digest failed");
  return image;
}

void TestProductionRefusals() {
  auto image = DurableCatalog();
  image.source = agents::AgentCatalogStateSource::in_memory_bootstrap;
  auto status = agents::ValidateDurableAgentCatalogForProduction(image);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_CATALOG.PRODUCTION_REQUIRES_DURABLE_CATALOG",
          "production accepted in-memory bootstrap catalog");

  image.source = agents::AgentCatalogStateSource::sidecar_legacy;
  status = agents::ValidateDurableAgentCatalogForProduction(image);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_CATALOG.PRODUCTION_REQUIRES_DURABLE_CATALOG",
          "production accepted sidecar legacy catalog");

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "legacy";
  const std::string legacy = "legacy|agent|policy|scope|running|0|0|0|0|0";
  const auto validation = agents::ValidateDurableAgentCatalogImage(legacy, true);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_CATALOG.LEGACY_PIPE_DELIMITED_REJECTED",
          "production accepted pipe-delimited legacy serialization");
}

void TestDurableCatalogLayoutsCoverRuntimeRecordFamilies() {
  const auto layouts = agents::AgentCatalogRecordLayouts();
  for (const std::string token :
       {"agent_instance(",
        "agent_policy(",
        "agent_policy_attachment(",
        "agent_evidence(",
        "agent_action(",
        "agent_approval(",
        "agent_override(",
        "agent_lease(",
        "agent_health(",
        "agent_retained_history(",
        "agent_state_migration("}) {
    Require(LayoutContains(layouts, token),
            "durable catalog layout missing " + token);
  }
}

std::string RewriteHeaderSchemaVersion(std::string encoded,
                                        const std::string& replacement) {
  Require(encoded.size() >= 80 && encoded.substr(0, 8) == "SBADC002",
          "binary catalog header missing");
  const auto version = static_cast<std::uint64_t>(std::stoull(replacement));
  for (unsigned i = 0; i < 8; ++i)
    encoded[8 + i] = static_cast<char>(version >> (8 * i));
  return encoded;
}

void RefreshImageChecksum(std::string* encoded) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(encoded->data() + 80),
         encoded->size() - 80, digest);
  constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    (*encoded)[16 + i * 2] = digits[digest[i] >> 4];
    (*encoded)[17 + i * 2] = digits[digest[i] & 15];
  }
}

void TestStructuredValidation() {
  const auto image = DurableCatalog();
  const auto encoded = agents::SerializeDurableAgentCatalogImage(image);
  const auto validation = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(validation.status.ok, "durable catalog image did not validate");
  Require(validation.checksum.size() == 64, "catalog checksum was not SHA-256");
  Require(validation.image.authority.catalog_root_digest.size() == 64,
          "catalog root digest was not SHA-256");
  Require(validation.image.authority.catalog_root_digest ==
              agents::DurableAgentCatalogRootDigest(validation.image),
          "catalog root digest did not bind payload");
  Require(validation.image.instances.size() == 1, "instance did not round-trip");
  Require(validation.image.instances.front().last_supervision_detail ==
              image.instances.front().last_supervision_detail,
          "instance supervision detail did not round-trip");
  Require(validation.image.instances.front().crash_loop_count == 2,
          "instance counters did not round-trip");
  Require(validation.image.policies.front().policy_name == "storage health policy",
          "policy name did not round-trip");
  Require(validation.image.policies.front().activation ==
              agents::AgentActivationProfile::recommend_only,
          "policy activation did not round-trip");
  Require(validation.image.policies.front().required_metric_families.size() == 2,
          "policy metric families did not round-trip");
  Require(validation.image.policies.front().config_fields.at("escaped") ==
              "value|with=separators;and%percent",
          "policy config fields did not preserve escaped separators");
  Require(validation.image.attachments.front().diagnostic_code ==
              "SB_AGENT_ATTACHMENT.TEST",
          "attachment diagnostic did not round-trip");
  Require(validation.image.evidence.front().redaction_class == "restricted",
          "evidence redaction class did not round-trip");
  Require(validation.image.approvals.size() == 1, "approval did not round-trip");
  Require(validation.image.approvals.front().approved_at_microseconds == 300,
          "approval timestamp did not round-trip");
  Require(validation.image.overrides.size() == 1, "override did not round-trip");
  Require(validation.image.overrides.front().principal_uuid ==
              std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x16", 16),
          "override principal did not round-trip");
  Require(validation.image.health.size() == 1, "health did not round-trip");
  Require(validation.image.retained_history.size() == 1, "history did not round-trip");
  Require(validation.image.migrations.empty(),
          "unexpected migration record in current schema image");

  Require(encoded.substr(0, 8) == "SBADC002", "binary format version missing");
  Require(encoded.find(image.instances.front().instance_uuid) != std::string::npos,
          "UUID16 was not retained verbatim");
  auto binary_fixture = image;
  binary_fixture.instances.front().instance_uuid =
      std::string("\x00\x7c\x0a\x09\x3b\x3d\x70\x25\x80\x00\xff\x7c\x00\x00\x00\x01", 16);
  binary_fixture.policies.front().required_metric_families = {"", std::string("a\0b", 3), "a|b"};
  binary_fixture.policies.front().config_fields[std::string("a\0b", 3)] = std::string("\0|;=%\n", 7);
  Require(agents::RefreshDurableAgentCatalogAuthorityDigest(
              &binary_fixture, binary_fixture.authority.evidence_uuid).ok,
          "binary fixture authority digest failed");
  const auto binary_encoded = agents::SerializeDurableAgentCatalogImage(binary_fixture);
  const auto binary_decoded = agents::ValidateDurableAgentCatalogImage(binary_encoded, true);
  Require(binary_decoded.status.ok &&
              binary_decoded.image.instances.front().instance_uuid == binary_fixture.instances.front().instance_uuid &&
              binary_decoded.image.policies.front().required_metric_families == binary_fixture.policies.front().required_metric_families &&
              binary_decoded.image.policies.front().config_fields == binary_fixture.policies.front().config_fields,
          "binary framing lost delimiters, NULs or empty collection entries");
  auto bad_identity = image;
  bad_identity.instances.front().instance_uuid = "019f0100-0000-7000-8000-000000000010";
  Require(agents::SerializeDurableAgentCatalogImage(bad_identity).empty(),
          "text UUID was admitted into binary catalog");
  for (const auto length : {std::size_t(0), std::size_t(7), std::size_t(15),
                            std::size_t(79), encoded.size() - 1}) {
    Require(!agents::ValidateDurableAgentCatalogImage(encoded.substr(0, length), false).status.ok,
            "truncated catalog admitted");
  }
  auto malformed = encoded;
  malformed.replace(80, 8, 8, static_cast<char>(0xff));
  RefreshImageChecksum(&malformed);
  auto invalid = agents::ValidateDurableAgentCatalogImage(malformed, false);
  Require(!invalid.status.ok && invalid.image.instances.empty(),
          "oversized frame length admitted or partially published");
  malformed = encoded + encoded.substr(80);
  RefreshImageChecksum(&malformed);
  invalid = agents::ValidateDurableAgentCatalogImage(malformed, false);
  Require(!invalid.status.ok && invalid.image.instances.empty(),
          "duplicated catalog records admitted or partially published");
  malformed = encoded;
  const auto identity_offset = malformed.find(image.instances.front().instance_uuid);
  malformed[identity_offset + 6] = 0;
  RefreshImageChecksum(&malformed);
  invalid = agents::ValidateDurableAgentCatalogImage(malformed, false);
  Require(!invalid.status.ok && invalid.image.instances.empty(),
          "malformed binary identity admitted or partially published");
  Require(!agents::ValidateDurableAgentCatalogImage(
              "SB_AGENT_DURABLE_CATALOG_IMAGE\tschema_version=1\n", false).status.ok,
          "retired escaped-text catalog admitted");

  std::string tampered = encoded;
  tampered.push_back('x');
  const auto rejected = agents::ValidateDurableAgentCatalogImage(tampered, true);
  Require(!rejected.status.ok &&
              rejected.status.diagnostic_code == "SB_AGENT_CATALOG.CHECKSUM_MISMATCH",
          "catalog checksum tamper was accepted");

  auto root_tampered = image;
  root_tampered.instances.front().scope = "tampered";
  const auto root_status =
      agents::ValidateDurableAgentCatalogForProduction(root_tampered);
  Require(!root_status.ok &&
              root_status.diagnostic_code == "SB_AGENT_CATALOG.ROOT_DIGEST_MISMATCH",
          "catalog root digest tamper was accepted");
}

void TestSchemaMigrationRecord() {
  const auto image = DurableCatalog();
  const auto legacy_encoded =
      RewriteHeaderSchemaVersion(agents::SerializeDurableAgentCatalogImage(image),
                                 "0");
  const auto migrated =
      agents::ValidateDurableAgentCatalogImage(legacy_encoded, true);
  Require(migrated.status.ok, "structured old schema did not migrate: " +
                                  migrated.status.diagnostic_code);
  Require(migrated.migrated, "old structured schema was not marked migrated");
  Require(migrated.image.schema_version == 1,
          "migrated schema version did not advance to current version");
  Require(migrated.image.migrations.size() == 1,
          "migration ledger record missing");
  Require(migrated.image.migrations.front().from_schema_version == 0,
          "migration source schema version mismatch");
  Require(migrated.image.migrations.front().to_schema_version == 1,
          "migration target schema version mismatch");
  Require(!migrated.image.migrations.front().evidence_uuid.empty(),
          "migration evidence UUID missing");
  Require(agents::ValidateDurableAgentCatalogForProduction(migrated.image).ok,
          "migrated catalog failed production validation");

  const auto encoded = agents::SerializeDurableAgentCatalogImage(migrated.image);
  const auto round_trip = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(round_trip.status.ok, "migrated catalog did not round-trip");
  Require(round_trip.image.migrations.size() == 1,
          "migration ledger did not round-trip");
}

void TestServiceEvidenceGates() {
  agents::AgentRuntimeService service;
  agents::AgentRuntimeServiceOpenRequest request;
  request.manifest = agents::CanonicalAgentManifest();
  request.catalog = DurableCatalog();
  request.production_live_path = true;
  request.service_owner_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x20", 16);
  request.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x21", 16);

  auto result = service.Open(request);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_SERVICE.FOREGROUND_PROTECTION_REQUIRED",
          "service opened without foreground protection");

  request.worker_foreground_protection_enabled = true;
  request.catalog.authority.mga_transaction_evidence = false;
  result = service.Open(request);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_CATALOG.MGA_DURABLE_AUTHORITY_REQUIRED",
          "service opened without MGA durable evidence");

  request.catalog = DurableCatalog();
  result = service.Open(request);
  Require(result.status.ok, "service open failed with durable MGA evidence");
  Require(!result.evidence.agents_are_transaction_authority &&
              !result.evidence.agents_are_finality_authority &&
              !result.evidence.agents_are_visibility_authority &&
              !result.evidence.agents_are_recovery_authority &&
              !result.evidence.agents_are_security_authority,
          "authority non-drift evidence was absent");

  result = service.Start(std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x22", 16));
  Require(result.status.ok, "service start failed with durable evidence");
  result = service.Drain(std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x23", 16), 400);
  Require(result.status.ok, "service drain failed");
}

void TestLeaseHeartbeatAndReplay() {
  auto image = DurableCatalog();
  agents::DurableLeaseRequest request;
  request.lease_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x30", 16);
  request.instance_uuid = image.instances.front().instance_uuid;
  request.owner_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x31", 16);
  request.now_microseconds = 1000;
  request.lease_duration_microseconds = 5000;
  request.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x32", 16);

  auto status = agents::AcquireDurableAgentLease(&image, request);
  Require(status.ok, "initial lease acquire failed");
  status = agents::AcquireDurableAgentLease(&image, request);
  Require(status.ok &&
              status.diagnostic_code == "SB_AGENT_LEASE.IDEMPOTENT_OWNER",
          "same owner acquire was not idempotent");

  auto duplicate = request;
  duplicate.owner_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x33", 16);
  status = agents::AcquireDurableAgentLease(&image, duplicate);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_LEASE.DUPLICATE_LIVE_OWNER_REFUSED",
          "duplicate live lease owner was accepted");

  request.now_microseconds = 1100;
  request.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x34", 16);
  status = agents::HeartbeatDurableAgentLease(&image, request);
  Require(status.ok, "heartbeat failed");
  Require(image.leases.front().heartbeat_generation == 1,
          "heartbeat generation was not persisted");

  agents::DurableAgentActionRecord action;
  action.action_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x35", 16);
  action.instance_uuid = request.instance_uuid;
  action.owner_uuid = request.owner_uuid;
  action.state = agents::DurableAgentActionState::running;
  image.actions.push_back(action);
  status = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &image, std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x35", 16));
  Require(status.ok, "running action setup did not refresh catalog root");

  const auto before_replay_root = image.authority.catalog_root_digest;
  const auto before_replay_generation = image.authority.catalog_generation;
  status = agents::RecoverDurableAgentCatalogAfterCrash(
      &image, 1200, std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x36", 16));
  Require(status.ok, "crash replay failed");
  Require(image.authority.catalog_generation > before_replay_generation,
          "crash replay did not advance catalog generation");
  Require(image.authority.previous_catalog_root_digest == before_replay_root,
          "crash replay did not retain previous catalog root");
  Require(image.authority.catalog_root_digest ==
              agents::DurableAgentCatalogRootDigest(image),
          "crash replay did not refresh catalog root");
  Require(image.leases.front().state == agents::DurableAgentLeaseState::replay_pending,
          "lease was not moved to deterministic replay state");
  Require(image.actions.front().state ==
              agents::DurableAgentActionState::replay_pending,
          "action was not moved to deterministic replay state");
}

void TestServiceRecoverRequiresCrashMode() {
  agents::AgentRuntimeServiceOpenRequest request;
  request.manifest = agents::CanonicalAgentManifest();
  request.catalog = DurableCatalog();
  request.production_live_path = true;
  request.worker_foreground_protection_enabled = true;
  request.service_owner_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x40", 16);
  request.evidence_uuid = std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x41", 16);

  agents::AgentRuntimeService service;
  auto result = service.Open(request);
  Require(result.status.ok, "service open failed");
  result = service.Recover(std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x42", 16), 2000);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_SERVICE.CRASH_RECOVERY_MODE_REQUIRED",
          "recover ran outside crash recovery mode");

  request.crash_recovery_mode = true;
  agents::AgentRuntimeService recovery_service;
  result = recovery_service.Open(request);
  Require(result.status.ok, "crash recovery service open failed");
  result = recovery_service.Recover(std::string("\x01\x9f\x01\x00\x00\x00\x70\x00\x80\x00\x00\x00\x00\x00\x00\x43", 16), 2000);
  Require(result.status.ok, "crash recovery service recover failed");
}

int main() {
  try {
    TestProductionRefusals();
  TestDurableCatalogLayoutsCoverRuntimeRecordFamilies();
  TestStructuredValidation();
  TestSchemaMigrationRecord();
  TestServiceEvidenceGates();
    TestLeaseHeartbeatAndReplay();
    TestServiceRecoverRequiresCrashMode();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
