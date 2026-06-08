// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/protected_material_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseUuid = "019e18d0-1100-7000-8000-000000000010";
constexpr std::string_view kMaterialUuid = "019e18d0-1101-7000-8000-000000000010";
constexpr std::string_view kVersionOneUuid = "019e18d0-1102-7000-8000-000000000010";
constexpr std::string_view kVersionTwoUuid = "019e18d0-1103-7000-8000-000000000010";
constexpr std::string_view kVersionThreeUuid = "019e18d0-1104-7000-8000-000000000010";
constexpr std::string_view kRetentionPolicyUuid = "019e18d0-1110-7000-8000-000000000010";
constexpr std::string_view kAccessPolicyUuid = "019e18d0-1111-7000-8000-000000000010";
constexpr std::string_view kReleasePolicyUuid = "019e18d0-1112-7000-8000-000000000010";
constexpr std::string_view kPurgePolicyUuid = "019e18d0-1113-7000-8000-000000000010";
constexpr std::string_view kAuditPolicyUuid = "019e18d0-1114-7000-8000-000000000010";
constexpr std::string_view kPlaintext = "CorrectHorseBatteryStaple-PCF011";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

const std::filesystem::path& DatabasePath() {
  static const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      "scratchbird_cloud_ops_protected_material_catalog" /
      "cloud_ops.sbdb";
  return path;
}

api::EngineRequestContext Context(std::uint64_t tx,
                                  std::uint64_t visible_through,
                                  std::uint64_t epoch = 1000) {
  api::EngineRequestContext context;
  context.database_path = DatabasePath().string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.principal_uuid.canonical = "019e18d0-1120-7000-8000-000000000010";
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  context.local_transaction_id = tx;
  context.snapshot_visible_through_local_transaction_id = visible_through;
  context.security_epoch = 7;
  context.resource_epoch = epoch;
  return context;
}

api::EngineProtectedMaterialPolicySet Policy(std::uint64_t retention_until = 0) {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = std::string(kRetentionPolicyUuid);
  policy.access_policy_uuid = std::string(kAccessPolicyUuid);
  policy.release_policy_uuid = std::string(kReleasePolicyUuid);
  policy.purge_policy_uuid = std::string(kPurgePolicyUuid);
  policy.audit_policy_uuid = std::string(kAuditPolicyUuid);
  policy.retention_until_epoch_millis = retention_until;
  policy.release_purposes.push_back("cloud_ops_use");
  return policy;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || diagnostic.detail == code) { return true; }
  }
  return false;
}

std::string Flatten(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << '\n' << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << '\n' << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
    }
  }
  return out.str();
}

void RequireNoPlaintextLeak(const api::EngineApiResult& result) {
  Require(Flatten(result).find(kPlaintext) == std::string::npos,
          "protected material plaintext leaked through API result");
}

api::EngineCreateProtectedMaterialResult CreateMaterial() {
  api::EngineCreateProtectedMaterialRequest request;
  request.context = Context(1, 1);
  request.protected_material_uuid = std::string(kMaterialUuid);
  request.owner_scope_uuid = "019e18d0-1121-7000-8000-000000000010";
  request.purpose_class = "cloud_ops_use";
  request.storage_class = "wrapped";
  request.policy = Policy();
  request.initial_version_uuid = std::string(kVersionOneUuid);
  request.protected_reference = "kms-ref:v1:tenant-a/material-one";
  request.envelope_reference = "envelope:v1:wrapped-material-one";
  request.payload_hash = "sha256:version-one";
  request.option_envelopes.push_back("protected_material_authority:engine");
  const auto result = api::EngineCreateProtectedMaterial(request);
  Require(result.ok && result.created && result.initial_version_created,
          "protected material create failed");
  Require(!result.plaintext_material_stored, "create stored plaintext material");
  RequireNoPlaintextLeak(result);
  return result;
}

api::EngineAddProtectedMaterialVersionResult AddVersion(std::uint64_t tx,
                                                        std::string_view version_uuid,
                                                        std::string_view hash,
                                                        std::uint64_t retention_until) {
  api::EngineAddProtectedMaterialVersionRequest request;
  request.context = Context(tx, tx);
  request.protected_material_uuid = std::string(kMaterialUuid);
  request.protected_material_version_uuid = std::string(version_uuid);
  request.protected_reference = "kms-ref:v1:tenant-a/" + std::string(version_uuid);
  request.envelope_reference = "envelope:v1:" + std::string(hash);
  request.payload_hash = std::string(hash);
  request.storage_class = "wrapped";
  request.rotation_reason = "scheduled_rotation";
  request.policy_override = Policy(retention_until);
  request.option_envelopes.push_back("protected_material_authority:engine");
  const auto result = api::EngineAddProtectedMaterialVersion(request);
  Require(result.ok && result.version_added && result.active_version_changed,
          "protected material add version failed");
  Require(!result.plaintext_material_stored, "add version stored plaintext material");
  RequireNoPlaintextLeak(result);
  return result;
}

void TestResolveReleaseAndPurgePolicy() {
  CreateMaterial();
  AddVersion(2, kVersionTwoUuid, "sha256:version-two", 10000);

  api::EngineResolveProtectedMaterialRequest old_snapshot;
  old_snapshot.context = Context(0, 1);
  old_snapshot.protected_material_uuid = std::string(kMaterialUuid);
  old_snapshot.purpose = "cloud_ops_use";
  old_snapshot.option_envelopes.push_back("protected_material_authority:engine");
  const auto old_resolved = api::EngineResolveProtectedMaterial(old_snapshot);
  Require(old_resolved.ok && old_resolved.resolved &&
              old_resolved.protected_material_version_uuid == kVersionOneUuid,
          "old MGA snapshot did not resolve the prior active version");

  api::EngineResolveProtectedMaterialRequest current;
  current.context = Context(0, 2);
  current.protected_material_uuid = std::string(kMaterialUuid);
  current.purpose = "cloud_ops_use";
  current.option_envelopes.push_back("protected_material_authority:engine");
  const auto resolved = api::EngineResolveProtectedMaterial(current);
  Require(resolved.ok && resolved.resolved &&
              resolved.protected_material_version_uuid == kVersionTwoUuid,
          "current MGA snapshot did not resolve the active rotated version");

  api::EngineReleaseProtectedMaterialRequest denied;
  denied.context = Context(0, 2);
  denied.protected_material_uuid = std::string(kMaterialUuid);
  denied.purpose = "wrong_purpose";
  denied.option_envelopes.push_back("protected_material_authority:engine");
  const auto denied_release = api::EngineReleaseProtectedMaterial(denied);
  Require(!denied_release.ok && denied_release.policy_denied &&
              HasDiagnostic(denied_release, "SECURITY.PROTECTED_MATERIAL.POLICY_DENIED"),
          "release with wrong purpose was not denied by policy");

  api::EngineReleaseProtectedMaterialRequest allowed;
  allowed.context = Context(0, 2);
  allowed.protected_material_uuid = std::string(kMaterialUuid);
  allowed.purpose = "cloud_ops_use";
  allowed.option_envelopes.push_back("protected_material_authority:engine");
  const auto released = api::EngineReleaseProtectedMaterial(allowed);
  Require(released.ok && released.released && !released.plaintext_material_returned,
          "purpose-bound release failed or returned plaintext");
  RequireNoPlaintextLeak(released);

  api::EnginePurgeProtectedMaterialVersionRequest retained;
  retained.context = Context(3, 3, 2000);
  retained.protected_material_uuid = std::string(kMaterialUuid);
  retained.protected_material_version_uuid = std::string(kVersionTwoUuid);
  retained.purge_reason = "retention_policy_test";
  retained.option_envelopes.push_back("protected_material_authority:engine");
  const auto retained_purge = api::EnginePurgeProtectedMaterialVersion(retained);
  Require(!retained_purge.ok && retained_purge.refused_by_retention &&
              retained_purge.audit_preserved,
          "retention-blocked purge was not refused");

  AddVersion(4, kVersionThreeUuid, "sha256:version-three", 0);
  api::EnginePurgeProtectedMaterialVersionRequest purge;
  purge.context = Context(5, 5, 20000);
  purge.protected_material_uuid = std::string(kMaterialUuid);
  purge.protected_material_version_uuid = std::string(kVersionThreeUuid);
  purge.purge_reason = "policy_admitted_cleanup";
  purge.option_envelopes.push_back("protected_material_authority:engine");
  const auto purged = api::EnginePurgeProtectedMaterialVersion(purge);
  Require(purged.ok && purged.purged && purged.audit_preserved &&
              !purged.protected_reference_reachable,
          "policy-admitted purge failed");
}

void TestPlaintextRefusalAndInspectRedaction() {
  api::EngineCreateProtectedMaterialRequest plaintext;
  plaintext.context = Context(6, 6);
  plaintext.protected_material_uuid = "019e18d0-1130-7000-8000-000000000010";
  plaintext.owner_scope_uuid = "019e18d0-1131-7000-8000-000000000010";
  plaintext.purpose_class = "cloud_ops_use";
  plaintext.storage_class = "wrapped";
  plaintext.policy = Policy();
  plaintext.initial_version_uuid = "019e18d0-1132-7000-8000-000000000010";
  plaintext.protected_reference = std::string("password=") + std::string(kPlaintext);
  plaintext.payload_hash = "sha256:plaintext-refusal";
  plaintext.option_envelopes.push_back("protected_material_authority:engine");
  const auto refused = api::EngineCreateProtectedMaterial(plaintext);
  Require(!refused.ok &&
              HasDiagnostic(refused, "SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED"),
          "plaintext protected material input was not refused");
  RequireNoPlaintextLeak(refused);

  api::EngineInspectProtectedMaterialCatalogRequest inspect;
  inspect.context = Context(0, 6);
  inspect.protected_material_uuid = std::string(kMaterialUuid);
  inspect.option_envelopes.push_back("protected_material_authority:engine");
  const auto inspected = api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(inspected.ok && inspected.protected_material_redacted &&
              !inspected.audit_events.empty(),
          "protected material inspect did not return redacted audit evidence");
  Require(Flatten(inspected).find("kms-ref:v1") == std::string::npos,
          "inspect leaked protected reference");
}

}  // namespace

int main() {
  std::error_code ignored;
  std::filesystem::remove_all(DatabasePath().parent_path(), ignored);
  std::filesystem::create_directories(DatabasePath().parent_path());
  TestResolveReleaseAndPurgePolicy();
  TestPlaintextRefusalAndInspectRedaction();
  return EXIT_SUCCESS;
}
