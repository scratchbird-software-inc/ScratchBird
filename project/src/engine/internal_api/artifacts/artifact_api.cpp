// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "artifacts/artifact_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/schema_tree_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

std::vector<std::string> SplitArtifactPayload(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

EngineUuid FieldUuid(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name,value] : row.fields) {
    if (name != field) continue;
    if (value.is_null || value.state != EngineValueState::value ||
        !value.encoded_value.empty() || value.binary_value.size() != 16) return {};
    EngineUuid id;
    std::copy_n(value.binary_value.begin(),16,id.bytes.begin());
    return core::uuid::IsEngineIdentityUuid(id) ? id : EngineUuid{};
  }
  return {};
}

bool ArtifactIdentityFieldsValid(const EngineRowValue& row) {
  std::set<std::string> seen;
  for (const auto& [name,value] : row.fields) {
    if (name != "object_uuid" && name != "remap_uuid" &&
        name != "target_database_uuid" && name != "target_schema_uuid" &&
        name != "target_object_uuid") continue;
    if (!seen.insert(name).second || !value.encoded_value.empty() ||
        value.binary_value.size()!=16 || value.is_null ||
        value.state!=EngineValueState::value) return false;
    EngineUuid id;
    std::copy_n(value.binary_value.begin(),16,id.bytes.begin());
    if (!id.is_nil() && !core::uuid::IsEngineIdentityUuid(id)) return false;
    if (name=="object_uuid" && id.is_nil()) return false;
  }
  return seen.contains("object_uuid");
}

void AddArtifactRow(EngineApiResult* result,
                    const std::string& artifact_kind,
                    const EngineUuid& object_uuid,
                    const std::string& object_kind,
                    const std::string& default_name,
                    const std::string& payload,
                    const EngineUuid& database_uuid,
                    const EngineUuid& schema_uuid,
                    const EngineUuid& target_object_uuid) {
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.catalog.artifact.v2"},
                     {"artifact_kind", artifact_kind},
                     {"object_uuid", object_uuid},
                     {"target_database_uuid", database_uuid},
                     {"target_schema_uuid", schema_uuid},
                     {"target_object_uuid", target_object_uuid},
                     {"object_kind", object_kind},
                     {"default_name", default_name},
                     {"payload", payload},
                     {"identity_authority", "uuid"},
                     {"runtime_authority", "false"}});
}

bool HasOption(const EngineApiRequest& request, const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (candidate == option) { return true; }
  }
  return false;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix, const std::string& fallback) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return fallback;
}

std::uint64_t Fnv1a64(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

struct ArtifactSnapshotEntry {
  EngineUuid object_uuid;
  EngineUuid target_database_uuid;
  EngineUuid target_schema_uuid;
  EngineUuid target_object_uuid;
  std::string object_kind;
  std::string default_name;
  std::string payload;
  std::string content_hash;
};

std::string SnapshotSignature(const ArtifactSnapshotEntry& entry) {
  std::string bytes;
  for (const auto* field : {&entry.object_kind,&entry.default_name,&entry.payload}) {
    if (!catalog_record_codec::Put(bytes,*field)) return {};
  }
  for (const auto* id : {&entry.target_database_uuid,&entry.target_schema_uuid,&entry.target_object_uuid})
    bytes.append(reinterpret_cast<const char*>(id->bytes.data()),16);
  return bytes;
}

std::string StableArtifactHash(const ArtifactSnapshotEntry& entry) {
  std::string bytes(reinterpret_cast<const char*>(entry.object_uuid.bytes.data()),16);
  bytes += SnapshotSignature(entry);
  return std::to_string(Fnv1a64(bytes));
}

void AddExternalGitAuthorityEvidence(EngineApiResult* result) {
  AddApiBehaviorEvidence(result, "external_git_versioning", "convenience_snapshot_review_only");
  AddApiBehaviorEvidence(result, "git_runtime_authority", "false");
  AddApiBehaviorEvidence(result, "external_git_repository_authority", "false");
  AddApiBehaviorEvidence(result, "catalog_runtime_authority", "ScratchBird_catalog_api");
  AddApiBehaviorEvidence(result, "mga_transaction_authority", "local_mga_transaction_inventory");
}

EngineApiDiagnostic ValidateExternalGitRequest(const EngineApiRequest& request,
                                               const std::string& operation_id,
                                               bool require_rows) {
  const auto context_status = ValidateApiBehaviorContext(request.context, operation_id, true, true);
  if (context_status.error) { return context_status; }
  if (!HasOption(request, "external_git_policy:enabled") &&
      !HasOption(request, "allow_external_git_versioning:true")) {
    return MakeInvalidRequestDiagnostic(operation_id, "external_git_policy_required");
  }
  if (HasOption(request, "git_runtime_authority:true") ||
      HasOption(request, "external_git_direct_authority:true") ||
      HasOption(request, "external_git_direct_apply:true")) {
    return MakeInvalidRequestDiagnostic(operation_id, "external_git_authority_forbidden");
  }
  if (require_rows && request.rows.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "external_git_snapshot_rows_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<ArtifactSnapshotEntry> CurrentArtifactSnapshot(const EngineRequestContext& context, EngineApiDiagnostic& diagnostic) {
  std::vector<ArtifactSnapshotEntry> rows;
  std::set<EngineUuid> schema_tree_uuids;
  const auto schemas = VisibleSchemaTreeRecords(context, context.local_transaction_id, diagnostic);
  if (diagnostic.error) return {};
  for (const auto& schema : schemas) {
    schema_tree_uuids.insert(schema.schema_uuid);
    ArtifactSnapshotEntry entry;
    entry.object_uuid = schema.schema_uuid;
    entry.target_database_uuid = context.database_uuid;
    entry.target_schema_uuid = schema.parent_schema_uuid;
    entry.object_kind = "schema";
    entry.default_name = schema.default_name;
    entry.payload = schema.payload;
    entry.content_hash =
        StableArtifactHash(entry);
    rows.push_back(std::move(entry));
  }
  const auto behavior_records = VisibleApiBehaviorRecords(context, {}, context.local_transaction_id, diagnostic);
  if (diagnostic.error) return {};
  for (const auto& record : behavior_records) {
    if (record.object_kind == "schema" && schema_tree_uuids.contains(record.object_uuid)) {
      continue;
    }
    ArtifactSnapshotEntry entry;
    entry.object_uuid = record.object_uuid;
    entry.target_database_uuid = record.target_database_uuid;
    entry.target_schema_uuid = record.target_schema_uuid;
    entry.target_object_uuid = record.target_object_uuid;
    entry.object_kind = record.object_kind;
    entry.default_name = record.default_name;
    entry.payload = record.payload;
    entry.content_hash =
        StableArtifactHash(entry);
    rows.push_back(std::move(entry));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.object_uuid < rhs.object_uuid;
  });
  return rows;
}

EngineApiDiagnostic SnapshotRowsFromRequest(const EngineApiRequest& request,
                                            const std::string& operation_id,
                                            std::vector<ArtifactSnapshotEntry>* rows) {
  std::set<EngineUuid> seen;
  for (const auto& row : request.rows) {
    const std::string entry_kind = FieldValue(row, "snapshot_entry_kind");
    if (entry_kind == "manifest") { continue; }
    const std::string format = FieldValue(row, "artifact_format");
    if (!format.empty() && format != "sb.catalog.artifact.v2" &&
        format != "sb.external_git.catalog_snapshot.v2") {
      return MakeInvalidRequestDiagnostic(operation_id, "external_git_snapshot_format_invalid");
    }
    if (!ArtifactIdentityFieldsValid(row))
      return MakeInvalidRequestDiagnostic(operation_id,"binary_artifact_identities_required");
    ArtifactSnapshotEntry entry;
    entry.object_uuid = FieldUuid(row, "object_uuid");
    entry.target_database_uuid = FieldUuid(row,"target_database_uuid");
    entry.target_schema_uuid = FieldUuid(row,"target_schema_uuid");
    entry.target_object_uuid = FieldUuid(row,"target_object_uuid");
    entry.object_kind = FieldValue(row, "object_kind");
    entry.default_name = FieldValue(row, "default_name");
    entry.payload = FieldValue(row, "payload");
    if (entry.object_uuid.is_nil() || entry.object_kind.empty()) {
      return MakeInvalidRequestDiagnostic(operation_id, "external_git_snapshot_object_required");
    }
    if (seen.contains(entry.object_uuid)) {
      return MakeInvalidRequestDiagnostic(operation_id,
                                          "external_git_snapshot_duplicate_uuid");
    }
    entry.content_hash =
        StableArtifactHash(entry);
    const std::string supplied_hash = FieldValue(row, "content_hash");
    if (!supplied_hash.empty() && supplied_hash != entry.content_hash) {
      return MakeInvalidRequestDiagnostic(operation_id,
                                          "external_git_snapshot_hash_mismatch");
    }
    rows->push_back(std::move(entry));
    seen.insert(rows->back().object_uuid);
  }
  std::sort(rows->begin(), rows->end(), [](const auto& lhs, const auto& rhs) {
    return lhs.object_uuid < rhs.object_uuid;
  });
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::map<EngineUuid, ArtifactSnapshotEntry> SnapshotMap(
    const std::vector<ArtifactSnapshotEntry>& rows) {
  std::map<EngineUuid, ArtifactSnapshotEntry> out;
  for (const auto& row : rows) { out[row.object_uuid] = row; }
  return out;
}

void AddExternalGitManifestRow(EngineApiResult* result,
                               const EngineRequestContext& context,
                               const std::string& entry_count,
                               const std::string& mode) {
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.external_git.catalog_snapshot.v2"},
                     {"snapshot_entry_kind", "manifest"},
                     {"snapshot_mode", mode},
                     {"database_uuid", context.database_uuid},
                     {"local_transaction_id", std::to_string(context.local_transaction_id)},
                     {"catalog_artifact_format", "sb.catalog.artifact.v2"},
                     {"entry_count", entry_count},
                     {"identity_authority", "uuid"},
                     {"catalog_runtime_authority", "ScratchBird_catalog_api"},
                     {"mga_transaction_authority", "local_mga_transaction_inventory"},
                     {"git_runtime_authority", "false"},
                     {"external_git_repository_authority", "false"}});
}

void AddExternalGitObjectRow(EngineApiResult* result,
                             const ArtifactSnapshotEntry& entry,
                             const std::string& snapshot_mode) {
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.external_git.catalog_snapshot.v2"},
                     {"catalog_artifact_format", "sb.catalog.artifact.v2"},
                     {"snapshot_entry_kind", "object"},
                     {"snapshot_mode", snapshot_mode},
                     {"object_uuid", entry.object_uuid},
                     {"target_database_uuid", entry.target_database_uuid},
                     {"target_schema_uuid", entry.target_schema_uuid},
                     {"target_object_uuid", entry.target_object_uuid},
                     {"object_kind", entry.object_kind},
                     {"default_name", entry.default_name},
                     {"payload", entry.payload},
                     {"content_hash", entry.content_hash},
                     {"identity_authority", "uuid"},
                     {"runtime_authority", "false"}});
}

void AddExternalGitDiffRow(EngineApiResult* result,
                           const std::string& diff_kind,
                           const ArtifactSnapshotEntry* current,
                           const ArtifactSnapshotEntry* candidate) {
  const ArtifactSnapshotEntry* effective = current != nullptr ? current : candidate;
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.external_git.catalog_diff.v1"},
                     {"diff_kind", diff_kind},
                     {"object_uuid", effective == nullptr ? EngineUuid{} : effective->object_uuid},
                     {"object_kind", effective == nullptr ? "" : effective->object_kind},
                     {"current_hash", current == nullptr ? "" : current->content_hash},
                     {"candidate_hash", candidate == nullptr ? "" : candidate->content_hash},
                     {"git_runtime_authority", "false"},
                     {"requires_authorized_catalog_import", "true"},
                     {"mga_transaction_authority", "local_mga_transaction_inventory"}});
}

void AddExternalGitRollbackRow(EngineApiResult* result,
                               const std::string& action,
                               const ArtifactSnapshotEntry& entry,
                               const std::string& target_hash) {
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.external_git.rollback_plan.v1"},
                     {"rollback_action", action},
                     {"object_uuid", entry.object_uuid},
                     {"target_database_uuid", entry.target_database_uuid},
                     {"target_schema_uuid", entry.target_schema_uuid},
                     {"target_object_uuid", entry.target_object_uuid},
                     {"object_kind", entry.object_kind},
                     {"default_name", entry.default_name},
                     {"payload", entry.payload},
                     {"restore_hash", entry.content_hash},
                     {"target_hash", target_hash},
                     {"apply_route", "authorized_catalog_api"},
                     {"git_runtime_authority", "false"},
                     {"plan_runtime_authority", "false"}});
}

std::vector<EngineLocalizedName> LocalizedNamesFromPayload(const std::string& payload,
                                                           const std::string&) {
  std::vector<EngineLocalizedName> names;
  std::vector<std::pair<std::string,std::string>> comments;
  if (!DecodeSchemaTreeMetadata(payload,&names,&comments)) return {};
  return names;
}

bool PayloadFailsPolicyValidation(const std::string& payload) {
  return payload.find("policy_status:invalid") != std::string::npos ||
         payload.find("unsafe_profile:true") != std::string::npos;
}

bool ExistingArtifactObjectVisible(const EngineRequestContext& context,
                                   const EngineUuid& object_uuid,
                                   std::uint64_t observer_tx, EngineApiDiagnostic& diagnostic) {
  const auto schema = FindVisibleSchemaTreeRecord(context, object_uuid, observer_tx, diagnostic);
  if (diagnostic.error) return false;
  if (schema) return true;
  if (FindVisibleApiBehaviorRecord(context, object_uuid, observer_tx, diagnostic)) { return true; }
  return false;
}

EngineApiDiagnostic ValidateArtifactImportRow(const EngineImportCatalogArtifactsRequest& request,
                                              const EngineRowValue& row,
                                              const EngineUuid& target_uuid,
                                              const std::string& object_kind,
                                              const std::string& payload,
                                              const std::set<EngineUuid>& staged_uuids) {
  if (FieldValue(row, "artifact_format") != "sb.catalog.artifact.v2") {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_format_invalid");
  }
  if (!ArtifactIdentityFieldsValid(row) || FieldUuid(row, "object_uuid").is_nil()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_object_uuid_required");
  }
  if (object_kind.empty()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_object_kind_required");
  }
  if (target_uuid.is_nil()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_target_uuid_required");
  }
  if (staged_uuids.contains(target_uuid)) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_duplicate_uuid_in_batch");
  }
  const std::string conflict_policy = OptionValue(request, "conflict_policy:", "reject");
  if (conflict_policy != "reject" && conflict_policy != "replace") {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_conflict_policy_invalid");
  }
  EngineApiDiagnostic schema_diagnostic;
  const bool exists = ExistingArtifactObjectVisible(request.context, target_uuid,
      request.context.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error) return schema_diagnostic;
  if (conflict_policy == "reject" && exists) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_uuid_conflict");
  }
  if (PayloadFailsPolicyValidation(payload)) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_policy_validation_failed");
  }
  if (object_kind == "schema") {
    const EngineUuid parent_schema_uuid = FieldUuid(row,"target_schema_uuid");
    std::vector<EngineLocalizedName> checked_names;
    std::vector<std::pair<std::string,std::string>> checked_comments;
    if (!DecodeSchemaTreeMetadata(payload,&checked_names,&checked_comments))
      return MakeInvalidRequestDiagnostic("artifact.import_catalog","binary_schema_payload_required");
    const auto parent = FindVisibleSchemaTreeRecord(request.context, parent_schema_uuid,
        request.context.local_transaction_id, schema_diagnostic);
    if (schema_diagnostic.error) return schema_diagnostic;
    if (!parent_schema_uuid.is_nil() && !parent && !staged_uuids.contains(parent_schema_uuid)) {
      return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_parent_schema_not_visible");
    }
    if (!HasOption(request, "allow_name_conflict:true")) {
      const auto names = LocalizedNamesFromPayload(payload, FieldValue(row, "default_name"));
      const auto conflict = SchemaTreePathConflict(request.context, target_uuid, parent_schema_uuid,
          names, request.context.local_transaction_id, schema_diagnostic);
      if (schema_diagnostic.error) return schema_diagnostic;
      if (conflict) {
        return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_schema_path_conflict:" + *conflict);
      }
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

ApiBehaviorRecord ArtifactRecordFromRow(const EngineImportCatalogArtifactsRequest& request,
                                        const EngineRowValue& row,
                                        const EngineUuid& target_uuid) {
  ApiBehaviorRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.operation_id = "artifact.import_catalog";
  record.object_uuid = target_uuid;
  record.target_database_uuid = FieldUuid(row,"target_database_uuid");
  record.target_schema_uuid = FieldUuid(row,"target_schema_uuid");
  record.target_object_uuid = FieldUuid(row,"target_object_uuid");
  record.object_kind = FieldValue(row, "object_kind");
  record.default_name = FieldValue(row, "default_name");
  record.payload = FieldValue(row, "payload");
  record.state = "active";
  record.deleted = false;
  if (record.object_kind == "schema" && record.payload.empty()) {
    const std::vector<EngineLocalizedName> names = {{"en", "default", record.default_name, record.default_name, true}};
    record.payload = SchemaTreePayload({}, names, {});
  }
  return record;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_ARTIFACT_API_BEHAVIOR
EngineExportCatalogArtifactsResult EngineExportCatalogArtifacts(const EngineExportCatalogArtifactsRequest& request) {
  const auto context_status = ValidateApiBehaviorContext(request.context, "artifact.export_catalog", true, true);
  if (context_status.error) {
    return MakeApiBehaviorDiagnostic<EngineExportCatalogArtifactsResult>(
        request.context,
        "artifact.export_catalog",
        context_status);
  }
  auto result = MakeApiBehaviorSuccess<EngineExportCatalogArtifactsResult>(request.context, "artifact.export_catalog");
  std::size_t count = 0;
  std::set<EngineUuid> schema_tree_uuids;
  EngineApiDiagnostic schema_diagnostic;
  const auto schemas = VisibleSchemaTreeRecords(request.context, request.context.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EngineExportCatalogArtifactsResult>(
      request.context, "artifact.export_catalog", schema_diagnostic);
  for (const auto& schema : schemas) {
    schema_tree_uuids.insert(schema.schema_uuid);
    AddArtifactRow(&result, "catalog_object", schema.schema_uuid, "schema", schema.default_name, schema.payload, request.context.database_uuid, schema.parent_schema_uuid, {});
    ++count;
  }
  const auto behavior_records = VisibleApiBehaviorRecords(request.context, {}, request.context.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EngineExportCatalogArtifactsResult>(
      request.context, "artifact.export_catalog", schema_diagnostic);
  for (const auto& record : behavior_records) {
    if (record.object_kind == "schema" && schema_tree_uuids.contains(record.object_uuid)) {
      continue;
    }
    AddArtifactRow(&result,
                   "api_behavior_record",
                   record.object_uuid,
                   record.object_kind,
                   record.default_name,
                   record.payload, record.target_database_uuid,
                   record.target_schema_uuid, record.target_object_uuid);
    ++count;
  }
  AddApiBehaviorEvidence(&result, "catalog_artifact_format", "sb.catalog.artifact.v2");
  AddApiBehaviorEvidence(&result, "catalog_artifact_export_count", std::to_string(count));
  AddApiBehaviorEvidence(&result, "git_runtime_authority", "false");
  return result;
}

EngineImportCatalogArtifactsResult EngineImportCatalogArtifacts(const EngineImportCatalogArtifactsRequest& request) {
  const auto context_status = ValidateApiBehaviorContext(request.context, "artifact.import_catalog", true, true);
  if (context_status.error) {
    return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
        request.context,
        "artifact.import_catalog",
        context_status);
  }
  if (HasOption(request, "git_runtime_authority:true") ||
      HasOption(request, "external_git_direct_authority:true") ||
      HasOption(request, "external_git_direct_apply:true")) {
    return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
        request.context,
        "artifact.import_catalog",
        MakeInvalidRequestDiagnostic("artifact.import_catalog", "external_git_authority_forbidden"));
  }
  if (request.rows.empty()) {
    return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
        request.context,
        "artifact.import_catalog",
        MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_rows_required"));
  }
  const std::string uuid_mode = OptionValue(request, "uuid_mode:", "preserve");
  if (uuid_mode != "preserve" && uuid_mode != "remap") {
    return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
        request.context,
        "artifact.import_catalog",
        MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_uuid_mode_invalid"));
  }
  std::vector<ApiBehaviorRecord> staged;
  std::set<EngineUuid> staged_uuids;
  for (const auto& row : request.rows) {
    const EngineUuid source_uuid = FieldUuid(row, "object_uuid");
    const EngineUuid remap_uuid = FieldUuid(row, "remap_uuid");
    const EngineUuid target_uuid = uuid_mode == "remap" ? remap_uuid : source_uuid;
    const std::string object_kind = FieldValue(row, "object_kind");
    const std::string payload = FieldValue(row, "payload");
    const auto row_status = ValidateArtifactImportRow(request, row, target_uuid, object_kind, payload, staged_uuids);
    if (row_status.error) {
      return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
          request.context,
          "artifact.import_catalog",
          row_status);
    }
    staged.push_back(ArtifactRecordFromRow(request, row, target_uuid));
    staged_uuids.insert(target_uuid);
  }
  for (const auto& record : staged) {
    const auto appended = AppendApiBehaviorEvent(request.context, MakeApiBehaviorRecordEvent(record));
    if (appended.error) {
      return MakeApiBehaviorDiagnostic<EngineImportCatalogArtifactsResult>(
          request.context,
          "artifact.import_catalog",
          appended);
    }
  }
  auto result = MakeApiBehaviorSuccess<EngineImportCatalogArtifactsResult>(request.context, "artifact.import_catalog");
  for (const auto& record : staged) {
    result.primary_object.uuid = record.object_uuid;
    result.primary_object.object_kind = record.object_kind;
    AddApiBehaviorRow(&result,
                      {{"object_uuid", record.object_uuid},
                       {"object_kind", record.object_kind},
                       {"name", record.default_name},
                       {"payload", record.payload}});
    AddApiBehaviorEvidence(&result, "catalog_artifact_imported", record.object_uuid);
  }
  AddApiBehaviorEvidence(&result, "catalog_artifact_import_count", std::to_string(staged.size()));
  AddApiBehaviorEvidence(&result, "git_runtime_authority", "false");
  if (HasOption(request, "external_git_policy:enabled") ||
      HasOption(request, "allow_external_git_versioning:true")) {
    AddApiBehaviorEvidence(&result,
                           "external_git_import_authority",
                           "authorized_catalog_api_not_git_repository");
    AddApiBehaviorEvidence(&result,
                           "mga_transaction_authority",
                           "local_mga_transaction_inventory");
  }
  return result;
}

EngineExportExternalGitSnapshotResult EngineExportExternalGitSnapshot(
    const EngineExportExternalGitSnapshotRequest& request) {
  const auto status =
      ValidateExternalGitRequest(request, "artifact.external_git.export_snapshot", false);
  if (status.error) {
    return MakeApiBehaviorDiagnostic<EngineExportExternalGitSnapshotResult>(
        request.context,
        "artifact.external_git.export_snapshot",
        status);
  }
  auto result = MakeApiBehaviorSuccess<EngineExportExternalGitSnapshotResult>(
      request.context, "artifact.external_git.export_snapshot");
  EngineApiDiagnostic schema_diagnostic;
  const auto rows = CurrentArtifactSnapshot(request.context, schema_diagnostic);
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EngineExportExternalGitSnapshotResult>(
      request.context, "artifact.external_git.export_snapshot", schema_diagnostic);
  AddExternalGitManifestRow(&result,
                            request.context,
                            std::to_string(rows.size()),
                            "export_snapshot");
  for (const auto& row : rows) { AddExternalGitObjectRow(&result, row, "export_snapshot"); }
  AddExternalGitAuthorityEvidence(&result);
  AddApiBehaviorEvidence(&result, "external_git_snapshot_export_count", std::to_string(rows.size()));
  return result;
}

EngineDiffExternalGitSnapshotResult EngineDiffExternalGitSnapshot(
    const EngineDiffExternalGitSnapshotRequest& request) {
  const auto status =
      ValidateExternalGitRequest(request, "artifact.external_git.diff_snapshot", true);
  if (status.error) {
    return MakeApiBehaviorDiagnostic<EngineDiffExternalGitSnapshotResult>(
        request.context,
        "artifact.external_git.diff_snapshot",
        status);
  }
  std::vector<ArtifactSnapshotEntry> candidate_rows;
  const auto row_status =
      SnapshotRowsFromRequest(request, "artifact.external_git.diff_snapshot", &candidate_rows);
  if (row_status.error) {
    return MakeApiBehaviorDiagnostic<EngineDiffExternalGitSnapshotResult>(
        request.context,
        "artifact.external_git.diff_snapshot",
        row_status);
  }
  auto result = MakeApiBehaviorSuccess<EngineDiffExternalGitSnapshotResult>(
      request.context, "artifact.external_git.diff_snapshot");
  EngineApiDiagnostic schema_diagnostic;
  const auto current = SnapshotMap(CurrentArtifactSnapshot(request.context, schema_diagnostic));
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EngineDiffExternalGitSnapshotResult>(
      request.context, "artifact.external_git.diff_snapshot", schema_diagnostic);
  const auto candidate = SnapshotMap(candidate_rows);
  std::size_t changed = 0;
  for (const auto& [uuid, current_entry] : current) {
    const auto candidate_it = candidate.find(uuid);
    if (candidate_it == candidate.end()) {
      AddExternalGitDiffRow(&result, "removed_from_candidate", &current_entry, nullptr);
      ++changed;
      continue;
    }
    if (SnapshotSignature(current_entry) != SnapshotSignature(candidate_it->second)) {
      AddExternalGitDiffRow(&result, "modified", &current_entry, &candidate_it->second);
      ++changed;
    }
  }
  for (const auto& [uuid, candidate_entry] : candidate) {
    if (!current.contains(uuid)) {
      AddExternalGitDiffRow(&result, "added_in_candidate", nullptr, &candidate_entry);
      ++changed;
    }
  }
  if (changed == 0) {
    AddApiBehaviorRow(&result,
                      {{"artifact_format", "sb.external_git.catalog_diff.v1"},
                       {"diff_kind", "unchanged"},
                       {"git_runtime_authority", "false"},
                       {"requires_authorized_catalog_import", "true"}});
  }
  AddExternalGitAuthorityEvidence(&result);
  AddApiBehaviorEvidence(&result, "external_git_diff_count", std::to_string(changed));
  return result;
}

EnginePlanExternalGitRollbackResult EnginePlanExternalGitRollback(
    const EnginePlanExternalGitRollbackRequest& request) {
  const auto status =
      ValidateExternalGitRequest(request, "artifact.external_git.rollback_plan", true);
  if (status.error) {
    return MakeApiBehaviorDiagnostic<EnginePlanExternalGitRollbackResult>(
        request.context,
        "artifact.external_git.rollback_plan",
        status);
  }
  std::vector<ArtifactSnapshotEntry> target_rows;
  const auto row_status =
      SnapshotRowsFromRequest(request, "artifact.external_git.rollback_plan", &target_rows);
  if (row_status.error) {
    return MakeApiBehaviorDiagnostic<EnginePlanExternalGitRollbackResult>(
        request.context,
        "artifact.external_git.rollback_plan",
        row_status);
  }
  auto result = MakeApiBehaviorSuccess<EnginePlanExternalGitRollbackResult>(
      request.context, "artifact.external_git.rollback_plan");
  EngineApiDiagnostic schema_diagnostic;
  const auto current = SnapshotMap(CurrentArtifactSnapshot(request.context, schema_diagnostic));
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EnginePlanExternalGitRollbackResult>(
      request.context, "artifact.external_git.rollback_plan", schema_diagnostic);
  const auto target = SnapshotMap(target_rows);
  std::size_t plan_rows = 0;
  for (const auto& [uuid, current_entry] : current) {
    const auto target_it = target.find(uuid);
    if (target_it == target.end()) {
      AddExternalGitRollbackRow(&result,
                                "restore_current_catalog_artifact",
                                current_entry,
                                "");
      ++plan_rows;
      continue;
    }
    if (SnapshotSignature(current_entry) != SnapshotSignature(target_it->second)) {
      AddExternalGitRollbackRow(&result,
                                "restore_current_catalog_artifact",
                                current_entry,
                                target_it->second.content_hash);
      ++plan_rows;
    }
  }
  for (const auto& [uuid, target_entry] : target) {
    if (!current.contains(uuid)) {
      AddExternalGitRollbackRow(&result,
                                "reject_candidate_only_object_until_authorized_catalog_create",
                                target_entry,
                                target_entry.content_hash);
      ++plan_rows;
    }
  }
  if (plan_rows == 0) {
    AddApiBehaviorRow(&result,
                      {{"artifact_format", "sb.external_git.rollback_plan.v1"},
                       {"rollback_action", "no_action_required"},
                       {"git_runtime_authority", "false"},
                       {"plan_runtime_authority", "false"}});
  }
  AddExternalGitAuthorityEvidence(&result);
  AddApiBehaviorEvidence(&result, "external_git_rollback_plan_count", std::to_string(plan_rows));
  AddApiBehaviorEvidence(&result,
                         "external_git_rollback_apply_route",
                         "authorized_catalog_api_not_git_repository");
  return result;
}

}  // namespace scratchbird::engine::internal_api
