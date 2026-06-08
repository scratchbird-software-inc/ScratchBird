// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_CATALOG_BACKUP_EXPORT_GATE

#include "backup_archive/backup_archive_api.hpp"
#include "cluster_catalog_manifest.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace catalog = scratchbird::core::catalog;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::string MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, 1772000000000ull + offset);
  Require(generated.ok(), "cluster catalog backup UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

std::string ValueForColumn(const catalog::ClusterCatalogColumnManifest& column,
                           u64 offset) {
  if (column.type_name == "uuid") {
    return MakeUuid(column.column_name == "cluster_uuid" ? UuidKind::cluster
                                                         : UuidKind::object,
                    offset);
  }
  if (column.type_name == "uint64") {
    return std::to_string(1000 + offset);
  }
  if (column.type_name == "digest") {
    return "sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  }
  if (column.column_name == "status") {
    return "active";
  }
  return "catalog_value_" + std::to_string(offset);
}

catalog::ClusterCatalogRecord MakeRecord(std::string_view table_path,
                                         u64 offset,
                                         std::string* primary_uuid) {
  const auto* table =
      catalog::FindBuiltinClusterCatalogTableManifestByPath(table_path);
  Require(table != nullptr, "cluster catalog table fixture missing");

  catalog::ClusterCatalogRecord record;
  record.table_path = std::string(table_path);
  record.codec_version = catalog::kClusterCatalogRecordCodecVersionCurrent;
  record.schema_version = table->manifest_version;
  for (const auto& column : table->columns) {
    record.fields.push_back(
        {column.column_name, ValueForColumn(column, offset++)});
  }
  *primary_uuid = catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  Require(!primary_uuid->empty(), "cluster catalog primary UUID fixture missing");
  Require(catalog::ValidateClusterCatalogRecord(record).ok(),
          "cluster catalog record fixture did not validate");
  return record;
}

catalog::ClusterCatalogNameResolverRow NameResolver(std::string target_uuid,
                                                    std::string table_path,
                                                    u64 offset) {
  catalog::ClusterCatalogNameResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, offset);
  row.target_record_uuid = std::move(target_uuid);
  row.target_table_path = std::move(table_path);
  row.language_tag = "en";
  row.identifier_profile_uuid = MakeUuid(UuidKind::object, offset + 1);
  row.name_class = "display";
  row.raw_name_text = "cluster object";
  row.display_name = "Cluster Object";
  row.normalized_lookup_key = "cluster_object";
  row.exact_lookup_key = "Cluster Object";
  row.full_path_lookup_key = "cluster.sys.catalog.node.Cluster Object";
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogCommentResolverRow CommentResolver(
    std::string target_uuid,
    std::string table_path,
    u64 offset) {
  catalog::ClusterCatalogCommentResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, offset);
  row.comment_uuid = MakeUuid(UuidKind::object, offset + 1);
  row.target_record_uuid = std::move(target_uuid);
  row.target_table_path = std::move(table_path);
  row.language_tag = "en";
  row.comment_text = "cluster catalog public release identity proof";
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogRecordSet MakeRecordSet() {
  constexpr std::string_view kNodeTable = "cluster.sys.catalog.node";
  std::string first_uuid;
  std::string second_uuid;
  catalog::ClusterCatalogRecordSet set;
  set.records.push_back(MakeRecord(kNodeTable, 10, &first_uuid));
  set.records.push_back(MakeRecord(kNodeTable, 80, &second_uuid));
  set.name_resolver_rows.push_back(NameResolver(first_uuid, std::string(kNodeTable), 200));
  set.name_resolver_rows.push_back(NameResolver(second_uuid, std::string(kNodeTable), 220));
  set.comment_resolver_rows.push_back(
      CommentResolver(first_uuid, std::string(kNodeTable), 240));
  set.comment_resolver_rows.push_back(
      CommentResolver(second_uuid, std::string(kNodeTable), 260));
  Require(catalog::ValidateClusterCatalogRecordSet(set).ok(),
          "cluster catalog record-set fixture did not validate");
  return set;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.database_uuid.canonical = "database:public-cluster-backup-export-pcr104";
  context.cluster_uuid.canonical = "cluster:public-cluster-backup-export-pcr104";
  context.principal_uuid.canonical = "principal:public-cluster-backup-export-pcr104";
  context.trace_tags.push_back("public_cluster_catalog_backup_export_gate");
  return context;
}

api::ClusterCatalogIdentityTransferRule RuleFor(
    const catalog::ClusterCatalogRecord& record,
    api::ClusterCatalogIdentityDisposition disposition,
    std::string target_uuid = {}) {
  api::ClusterCatalogIdentityTransferRule rule;
  rule.table_path = record.table_path;
  rule.source_record_uuid = catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  rule.target_record_uuid = std::move(target_uuid);
  rule.disposition = disposition;
  rule.explicit_remap_authorized =
      disposition == api::ClusterCatalogIdentityDisposition::remap;
  rule.no_uuid_reuse_proven =
      disposition == api::ClusterCatalogIdentityDisposition::preserve ||
      disposition == api::ClusterCatalogIdentityDisposition::remap;
  rule.resolver_evidence_proven = true;
  rule.comment_evidence_proven = true;
  rule.security_binding_proven = true;
  rule.projection_integrity_proven = true;
  rule.provider_digest_verified = true;
  if (disposition == api::ClusterCatalogIdentityDisposition::reject) {
    rule.disposition_reason = "identity_conflicts_with_target_cluster";
  } else if (disposition == api::ClusterCatalogIdentityDisposition::quarantine) {
    rule.disposition_reason = "projection_integrity_requires_review";
  }
  return rule;
}

api::EngineEvaluateClusterCatalogBackupRestoreIdentityRequest RequestFor(
    api::ClusterCatalogTransferOperation operation) {
  api::EngineEvaluateClusterCatalogBackupRestoreIdentityRequest request;
  request.context = Context();
  request.operation_id = "public_cluster_catalog_backup_export_gate";
  request.transfer_operation = operation;
  request.record_set = MakeRecordSet();
  request.cluster_catalog_present = true;
  request.joined_cluster_catalog_state = true;
  request.external_provider_available = true;
  return request;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view fragment) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(fragment) != std::string::npos ||
        diagnostic.code.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void TestPreserveRemapRejectQuarantinePolicy() {
  auto request =
      RequestFor(api::ClusterCatalogTransferOperation::restore);
  const auto& first = request.record_set.records[0];
  const auto& second = request.record_set.records[1];
  request.identity_rules.push_back(
      RuleFor(first, api::ClusterCatalogIdentityDisposition::preserve));
  request.identity_rules.push_back(
      RuleFor(second,
              api::ClusterCatalogIdentityDisposition::remap,
              MakeUuid(UuidKind::object, 500)));

  const auto result =
      api::EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(request);
  Require(result.ok, "cluster catalog restore identity policy was refused");
  Require(!result.fail_closed, "successful identity policy stayed fail-closed");
  Require(result.preserved_count == 1, "preserve count mismatch");
  Require(result.remapped_count == 1, "remap count mismatch");
  Require(result.rejected_count == 0, "reject count mismatch");
  Require(result.quarantined_count == 0, "quarantine count mismatch");
  Require(!result.mutation_performed, "identity policy performed mutation");
  Require(!result.local_runtime_execution_enabled,
          "identity policy enabled local cluster execution");
  Require(!result.cluster_recovery_authority,
          "identity policy claimed cluster recovery authority");
  Require(result.resolver_comment_security_projection_proven,
          "identity policy lost resolver/comment/security/projection proof");

  request = RequestFor(api::ClusterCatalogTransferOperation::import);
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[0],
              api::ClusterCatalogIdentityDisposition::reject));
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[1],
              api::ClusterCatalogIdentityDisposition::quarantine));
  const auto terminal =
      api::EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(request);
  Require(terminal.ok, "reject/quarantine identity policy was refused");
  Require(terminal.rejected_count == 1, "reject decision count mismatch");
  Require(terminal.quarantined_count == 1, "quarantine decision count mismatch");
  Require(!terminal.mutation_performed,
          "reject/quarantine policy performed mutation");
}

void TestFailClosedRefusals() {
  auto request =
      RequestFor(api::ClusterCatalogTransferOperation::backup);
  request.external_provider_available = false;
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[0],
              api::ClusterCatalogIdentityDisposition::preserve));
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[1],
              api::ClusterCatalogIdentityDisposition::preserve));
  auto result =
      api::EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(request);
  Require(!result.ok && result.fail_closed,
          "cluster catalog identity policy accepted missing provider");
  Require(HasDiagnostic(result, "CLUSTER_CATALOG_EXTERNAL_PROVIDER_REQUIRED"),
          "missing provider diagnostic mismatch");

  request = RequestFor(api::ClusterCatalogTransferOperation::restore);
  const std::string reused_target = MakeUuid(UuidKind::object, 700);
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[0],
              api::ClusterCatalogIdentityDisposition::remap,
              reused_target));
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[1],
              api::ClusterCatalogIdentityDisposition::remap,
              reused_target));
  result = api::EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(request);
  Require(!result.ok && result.fail_closed,
          "cluster catalog identity policy accepted target UUID reuse");
  Require(HasDiagnostic(result, "CLUSTER_CATALOG_TARGET_UUID_REUSED"),
          "target UUID reuse diagnostic mismatch");

  request = RequestFor(api::ClusterCatalogTransferOperation::public_export);
  request.public_export_manifest_clean = false;
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[0],
              api::ClusterCatalogIdentityDisposition::preserve));
  request.identity_rules.push_back(
      RuleFor(request.record_set.records[1],
              api::ClusterCatalogIdentityDisposition::preserve));
  result = api::EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(request);
  Require(!result.ok && result.fail_closed,
          "cluster catalog public export accepted unclean manifest");
  Require(HasDiagnostic(result, "CLUSTER_CATALOG_PUBLIC_EXPORT_MANIFEST_UNCLEAN"),
          "unclean public export diagnostic mismatch");
}

}  // namespace

int main() {
  TestPreserveRemapRejectQuarantinePolicy();
  TestFailClosedRefusals();
  return EXIT_SUCCESS;
}
