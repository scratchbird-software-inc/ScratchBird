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

void AddArtifactRow(EngineApiResult* result,
                    const std::string& artifact_kind,
                    const std::string& object_uuid,
                    const std::string& object_kind,
                    const std::string& default_name,
                    const std::string& payload) {
  AddApiBehaviorRow(result,
                    {{"artifact_format", "sb.catalog.artifact.v1"},
                     {"artifact_kind", artifact_kind},
                     {"object_uuid", object_uuid},
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

std::vector<EngineLocalizedName> LocalizedNamesFromPayload(const std::string& payload,
                                                           const std::string& default_name) {
  std::vector<EngineLocalizedName> names;
  for (const auto& part : SplitArtifactPayload(payload, ';')) {
    if (!StartsWith(part, "localized_name=")) { continue; }
    const auto fields = SplitArtifactPayload(part.substr(15), ',');
    if (fields.size() < 5) { continue; }
    names.push_back({fields[0], fields[1], fields[2], fields[3], fields[4] == "default" || fields[4] == "1"});
  }
  if (names.empty() && !default_name.empty()) {
    names.push_back({"en", "default", default_name, default_name, true});
  }
  return names;
}

std::string ParentSchemaFromPayload(const std::string& payload) {
  for (const auto& part : SplitArtifactPayload(payload, ';')) {
    if (StartsWith(part, "schema=")) { return part.substr(7); }
    if (StartsWith(part, "parent_schema_uuid=")) { return part.substr(19); }
  }
  return {};
}

bool PayloadFailsPolicyValidation(const std::string& payload) {
  return payload.find("policy_status:invalid") != std::string::npos ||
         payload.find("unsafe_profile:true") != std::string::npos;
}

bool ExistingArtifactObjectVisible(const EngineRequestContext& context,
                                   const std::string& object_uuid,
                                   std::uint64_t observer_tx) {
  if (FindVisibleSchemaTreeRecord(context, object_uuid, observer_tx)) { return true; }
  if (FindVisibleApiBehaviorRecord(context, object_uuid, observer_tx)) { return true; }
  return false;
}

EngineApiDiagnostic ValidateArtifactImportRow(const EngineImportCatalogArtifactsRequest& request,
                                              const EngineRowValue& row,
                                              const std::string& target_uuid,
                                              const std::string& object_kind,
                                              const std::string& payload,
                                              const std::set<std::string>& staged_uuids) {
  if (FieldValue(row, "artifact_format") != "sb.catalog.artifact.v1") {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_format_invalid");
  }
  if (FieldValue(row, "object_uuid").empty()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_object_uuid_required");
  }
  if (object_kind.empty()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_object_kind_required");
  }
  if (target_uuid.empty()) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_target_uuid_required");
  }
  if (staged_uuids.contains(target_uuid)) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_duplicate_uuid_in_batch:" + target_uuid);
  }
  const std::string conflict_policy = OptionValue(request, "conflict_policy:", "reject");
  if (conflict_policy != "reject" && conflict_policy != "replace") {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_conflict_policy_invalid");
  }
  if (conflict_policy == "reject" &&
      ExistingArtifactObjectVisible(request.context, target_uuid, request.context.local_transaction_id)) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_uuid_conflict:" + target_uuid);
  }
  if (PayloadFailsPolicyValidation(payload)) {
    return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_policy_validation_failed");
  }
  if (object_kind == "schema") {
    const std::string parent_schema_uuid = ParentSchemaFromPayload(payload);
    if (!parent_schema_uuid.empty() &&
        !FindVisibleSchemaTreeRecord(request.context, parent_schema_uuid, request.context.local_transaction_id) &&
        !staged_uuids.contains(parent_schema_uuid)) {
      return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_parent_schema_not_visible");
    }
    if (!HasOption(request, "allow_name_conflict:true")) {
      const auto names = LocalizedNamesFromPayload(payload, FieldValue(row, "default_name"));
      if (const auto conflict = SchemaTreePathConflict(request.context,
                                                      target_uuid,
                                                      parent_schema_uuid,
                                                      names,
                                                      request.context.local_transaction_id)) {
        return MakeInvalidRequestDiagnostic("artifact.import_catalog", "artifact_schema_path_conflict:" + *conflict);
      }
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

ApiBehaviorRecord ArtifactRecordFromRow(const EngineImportCatalogArtifactsRequest& request,
                                        const EngineRowValue& row,
                                        const std::string& target_uuid) {
  ApiBehaviorRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.operation_id = "artifact.import_catalog";
  record.object_uuid = target_uuid;
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
  for (const auto& schema : VisibleSchemaTreeRecords(request.context, request.context.local_transaction_id)) {
    AddArtifactRow(&result, "catalog_object", schema.schema_uuid, "schema", schema.default_name, schema.payload);
    ++count;
  }
  for (const auto& record : VisibleApiBehaviorRecords(request.context, {}, request.context.local_transaction_id)) {
    if (record.object_kind == "schema") { continue; }
    AddArtifactRow(&result,
                   "api_behavior_record",
                   record.object_uuid,
                   record.object_kind,
                   record.default_name,
                   record.payload);
    ++count;
  }
  AddApiBehaviorEvidence(&result, "catalog_artifact_format", "sb.catalog.artifact.v1");
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
  std::set<std::string> staged_uuids;
  for (const auto& row : request.rows) {
    const std::string source_uuid = FieldValue(row, "object_uuid");
    const std::string remap_uuid = FieldValue(row, "remap_uuid");
    const std::string target_uuid = uuid_mode == "remap" ? remap_uuid : source_uuid;
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
    result.primary_object.uuid.canonical = record.object_uuid;
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
  return result;
}

}  // namespace scratchbird::engine::internal_api
