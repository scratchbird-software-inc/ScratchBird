// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "artifacts/artifact_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_public_external_git_versioning.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for public external Git versioning gate");
  return std::filesystem::path(made);
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  Require(static_cast<bool>(out), "file write failed");
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code_or_detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code_or_detail || diagnostic.detail == code_or_detail) { return true; }
    if (diagnostic.detail.find(code_or_detail) != std::string::npos) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view evidence_id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == evidence_id) { return true; }
  }
  return false;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue ArtifactRow(std::string uuid,
                                std::string kind,
                                std::string name,
                                std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = uuid + "-row";
  row.fields.push_back({"artifact_format", TextValue("sb.catalog.artifact.v1")});
  row.fields.push_back({"object_uuid", TextValue(std::move(uuid))});
  row.fields.push_back({"object_kind", TextValue(std::move(kind))});
  row.fields.push_back({"default_name", TextValue(std::move(name))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

std::string RowField(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value.encoded_value; }
  }
  return {};
}

bool RowHasField(const api::EngineRowValue& row,
                 std::string_view name,
                 std::string_view expected) {
  return RowField(row, name) == expected;
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  std::uint64_t tx) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = "019e3900-0000-7000-8000-00000000e911";
  context.session_uuid.canonical = "019e3900-0000-7000-8000-00000000e912";
  context.transaction_uuid.canonical = "019e3900-0000-7000-8000-00000000e913";
  context.principal_uuid.canonical = "019e3900-0000-7000-8000-00000000e914";
  context.local_transaction_id = tx;
  context.snapshot_visible_through_local_transaction_id = tx;
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

sblr::SblrDispatchResult DispatchEncoded(std::string operation_id,
                                         std::string opcode,
                                         api::EngineRequestContext context,
                                         api::EngineApiRequest api_request = {}) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "public_external_git_versioning_gate");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.contains_sql_text = false;
  const auto encoded = sblr::EncodeSblrEnvelope(envelope);
  return sblr::DecodeAndDispatchSblrOperation(encoded, std::move(context), std::move(api_request));
}

std::vector<api::EngineRowValue> MutatedCandidateRows(const api::EngineApiResult& exported,
                                                       std::string_view uuid) {
  std::vector<api::EngineRowValue> rows;
  for (auto row : exported.result_shape.rows) {
    if (RowHasField(row, "snapshot_entry_kind", "manifest")) { continue; }
    if (RowHasField(row, "object_uuid", uuid)) {
      for (auto& [name, value] : row.fields) {
        if (name == "payload") {
          value.encoded_value += ";localized_name=en,alias,git_review_schema,git_review_alias,alias";
        }
        if (name == "content_hash") { value.encoded_value.clear(); }
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

void RunGate() {
  constexpr std::string_view kObjectUuid = "019e3900-0000-7000-8000-00000000e915";
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "external_git_versioning.sbdb";
  WriteFile(database_path, "SBDB_EXTERNAL_GIT_VERSIONING_PUBLIC_GATE");

  api::EngineImportCatalogArtifactsRequest import;
  import.context = Context(database_path, 900);
  import.option_envelopes.push_back("external_git_policy:enabled");
  import.rows.push_back(ArtifactRow(std::string(kObjectUuid),
                                    "schema",
                                    "git_review_schema",
                                    "localized_name=en,default,git_review_schema,git_review_schema,default"));
  const auto imported = api::EngineImportCatalogArtifacts(import);
  Require(imported.ok, "external Git seed import failed");
  Require(HasEvidence(imported,
                      "external_git_import_authority",
                      "authorized_catalog_api_not_git_repository"),
          "external Git import did not preserve catalog authority");
  Require(HasEvidence(imported,
                      "mga_transaction_authority",
                      "local_mga_transaction_inventory"),
          "external Git import did not preserve MGA authority");

  api::EngineExportExternalGitSnapshotRequest export_request;
  export_request.context = Context(database_path, 901);
  export_request.option_envelopes.push_back("external_git_policy:enabled");
  const auto exported = api::EngineExportExternalGitSnapshot(export_request);
  Require(exported.ok, "external Git snapshot export failed");
  Require(HasEvidence(exported,
                      "external_git_versioning",
                      "convenience_snapshot_review_only"),
          "external Git snapshot omitted convenience-boundary evidence");
  Require(HasEvidence(exported, "git_runtime_authority", "false"),
          "external Git snapshot became runtime authority");
  Require(HasEvidence(exported,
                      "catalog_runtime_authority",
                      "ScratchBird_catalog_api"),
          "external Git snapshot omitted ScratchBird catalog authority");

  const auto candidate_rows = MutatedCandidateRows(exported, kObjectUuid);
  Require(!candidate_rows.empty(), "external Git snapshot exported no object rows");

  api::EngineDiffExternalGitSnapshotRequest diff_request;
  diff_request.context = Context(database_path, 902);
  diff_request.option_envelopes.push_back("external_git_policy:enabled");
  diff_request.rows = candidate_rows;
  const auto diff = api::EngineDiffExternalGitSnapshot(diff_request);
  Require(diff.ok, "external Git snapshot diff failed");
  Require(HasEvidence(diff, "external_git_diff_count", "1"),
          "external Git diff did not detect one candidate change");
  bool saw_modified = false;
  for (const auto& row : diff.result_shape.rows) {
    saw_modified = saw_modified || RowHasField(row, "diff_kind", "modified");
  }
  Require(saw_modified, "external Git diff omitted modified row");

  api::EnginePlanExternalGitRollbackRequest rollback_request;
  rollback_request.context = Context(database_path, 903);
  rollback_request.option_envelopes.push_back("external_git_policy:enabled");
  rollback_request.rows = candidate_rows;
  const auto rollback_plan = api::EnginePlanExternalGitRollback(rollback_request);
  Require(rollback_plan.ok, "external Git rollback plan failed");
  Require(HasEvidence(rollback_plan, "external_git_rollback_plan_count", "1"),
          "external Git rollback plan did not detect one candidate change");
  Require(HasEvidence(rollback_plan,
                      "external_git_rollback_apply_route",
                      "authorized_catalog_api_not_git_repository"),
          "external Git rollback plan omitted authorized apply route");

  auto corrupt_rows = exported.result_shape.rows;
  for (auto& row : corrupt_rows) {
    if (!RowHasField(row, "object_uuid", kObjectUuid)) { continue; }
    for (auto& [name, value] : row.fields) {
      if (name == "payload") { value.encoded_value += ";policy_status:invalid_by_hash"; }
    }
  }
  api::EngineDiffExternalGitSnapshotRequest corrupt_request;
  corrupt_request.context = Context(database_path, 904);
  corrupt_request.option_envelopes.push_back("external_git_policy:enabled");
  corrupt_request.rows = corrupt_rows;
  const auto corrupt = api::EngineDiffExternalGitSnapshot(corrupt_request);
  Require(!corrupt.ok && HasDiagnostic(corrupt, "external_git_snapshot_hash_mismatch"),
          "external Git diff accepted a corrupt snapshot hash");

  api::EngineImportCatalogArtifactsRequest forbidden_import;
  forbidden_import.context = Context(database_path, 905);
  forbidden_import.option_envelopes.push_back("external_git_policy:enabled");
  forbidden_import.option_envelopes.push_back("external_git_direct_apply:true");
  forbidden_import.rows.push_back(import.rows.front());
  const auto forbidden = api::EngineImportCatalogArtifacts(forbidden_import);
  Require(!forbidden.ok && HasDiagnostic(forbidden, "external_git_authority_forbidden"),
          "catalog import accepted external Git direct authority");

  api::EngineApiRequest sblr_request;
  sblr_request.context = Context(database_path, 906);
  sblr_request.option_envelopes.push_back("external_git_policy:enabled");
  const auto sblr_export = DispatchEncoded("artifact.external_git.export_snapshot",
                                           "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT",
                                           sblr_request.context,
                                           sblr_request);
  Require(sblr_export.accepted && sblr_export.dispatched_to_api &&
              sblr_export.api_result.ok,
          "encoded SBLR external Git export route failed");
  Require(HasEvidence(sblr_export.api_result, "git_runtime_authority", "false"),
          "encoded SBLR external Git export became runtime authority");

  std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main() {
  RunGate();
  std::cout << "PUBLIC_EXTERNAL_GIT_VERSIONING_GATE=passed\n";
  return EXIT_SUCCESS;
}

