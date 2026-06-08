// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "artifacts/artifact_api.hpp"
#include "catalog/catalog_lookup_api.hpp"
#include "catalog/schema_tree_api.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

std::string GeneratedObjectUuid(std::uint64_t seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      1771401000000ull + seed);
  if (!generated.ok()) { return {}; }
  return scratchbird::core::uuid::UuidToString(generated.value.value);
}

struct Args {
  std::string source_path;
  std::string target_path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--source") { args->source_path = value; }
    else if (key == "--target") { args->target_path = value; }
    else { return false; }
  }
  return !args->source_path.empty() && !args->target_path.empty();
}

EngineRequestContext Base(const std::string& path) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "catalog-artifact-roundtrip-probe";
  context.database_path = path;
  return context;
}

EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

void SetField(EngineRowValue* row, const std::string& field, const std::string& value) {
  for (auto& [name, typed] : row->fields) {
    if (name == field) {
      typed.encoded_value = value;
      return;
    }
  }
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  row->fields.push_back({field, typed});
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

bool HasDiagnosticDetail(const EngineApiResult& result, const std::string& detail_prefix) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.rfind(detail_prefix, 0) == 0 ||
        diagnostic.detail.find(":" + detail_prefix) != std::string::npos) {
      return true;
    }
  }
  return false;
}

EngineCreateSchemaResult CreateSourceSchema(const EngineRequestContext& context) {
  EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.object_kind = "schema";
  request.localized_names.push_back({"en", "default", "portable", "portable", true});
  request.option_envelopes.push_back("localized_comment:en:Portable schema");
  return EngineCreateSchema(request);
}

EngineExportCatalogArtifactsResult ExportArtifacts(const EngineRequestContext& context) {
  EngineExportCatalogArtifactsRequest request;
  request.context = context;
  return EngineExportCatalogArtifacts(request);
}

EngineImportCatalogArtifactsResult ImportArtifacts(const EngineRequestContext& context,
                                                   std::vector<EngineRowValue> rows,
                                                   std::vector<std::string> options = {}) {
  EngineImportCatalogArtifactsRequest request;
  request.context = context;
  request.rows = std::move(rows);
  request.option_envelopes = std::move(options);
  return EngineImportCatalogArtifacts(request);
}

bool LookupSchemaByUuid(const EngineRequestContext& context, const std::string& uuid, const std::string& expected_name) {
  EngineLookupObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "schema";
  const auto result = EngineLookupObject(request);
  if (!result.ok || result.primary_object.uuid.canonical != uuid) { return false; }
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "object_uuid") == uuid && FieldValue(row, "name") == expected_name) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_catalog_artifact_roundtrip_probe --source PATH --target PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) {
    std::filesystem::remove(args.source_path);
    std::filesystem::remove(args.target_path);
  }
  { std::ofstream bootstrap(args.source_path, std::ios::binary | std::ios::app); }
  { std::ofstream bootstrap(args.target_path, std::ios::binary | std::ios::app); }

  const auto source_base = Base(args.source_path);
  const auto source_tx = Begin(source_base);
  const auto source_context = Tx(source_base, source_tx);
  const auto created = CreateSourceSchema(source_context);
  const bool source_commit = created.ok && Commit(source_context);

  const auto export_tx = Begin(source_base);
  const auto export_context = Tx(source_base, export_tx);
  const auto exported = ExportArtifacts(export_context);
  const bool export_commit = Commit(export_context);
  const bool export_ok = exported.ok && export_commit && !exported.result_shape.rows.empty() &&
                         HasEvidence(exported, "git_runtime_authority", "false");

  const auto target_base = Base(args.target_path);
  const auto import_tx = Begin(target_base);
  const auto import_context = Tx(target_base, import_tx);
  const auto imported = ImportArtifacts(import_context, exported.result_shape.rows);
  const bool import_commit = imported.ok && Commit(import_context);

  const auto read_tx = Begin(target_base);
  const auto read_context = Tx(target_base, read_tx);
  const bool preserve_lookup = LookupSchemaByUuid(read_context, created.primary_object.uuid.canonical, "portable");
  const bool read_commit = Commit(read_context);

  const auto conflict_tx = Begin(target_base);
  const auto conflict_context = Tx(target_base, conflict_tx);
  const auto conflict = ImportArtifacts(conflict_context, exported.result_shape.rows);
  const bool conflict_rejected = !conflict.ok && HasDiagnosticDetail(conflict, "artifact_uuid_conflict:");
  const bool conflict_commit = Commit(conflict_context);

  std::vector<EngineRowValue> remap_rows = exported.result_shape.rows;
  const std::string remap_uuid = GeneratedObjectUuid(852);
  for (auto& row : remap_rows) {
    SetField(&row, "remap_uuid", remap_uuid);
    SetField(&row, "default_name", "portable_copy");
    SetField(&row, "payload", "localized_name_count=1;localized_name=en,default,portable_copy,portable_copy,default");
  }
  const auto remap_tx = Begin(target_base);
  const auto remap_context = Tx(target_base, remap_tx);
  const auto remapped = ImportArtifacts(remap_context, remap_rows, {"uuid_mode:remap"});
  const bool remap_commit = remapped.ok && Commit(remap_context);

  const auto remap_read_tx = Begin(target_base);
  const auto remap_read_context = Tx(target_base, remap_read_tx);
  const bool remap_lookup = LookupSchemaByUuid(remap_read_context, remap_uuid, "portable_copy");
  const bool remap_read_commit = Commit(remap_read_context);

  EngineRowValue name_only;
  SetField(&name_only, "artifact_format", "sb.catalog.artifact.v1");
  SetField(&name_only, "object_kind", "schema");
  SetField(&name_only, "default_name", "name_only");
  SetField(&name_only, "payload", "localized_name_count=1;localized_name=en,default,name_only,name_only,default");
  const auto name_only_tx = Begin(target_base);
  const auto name_only_context = Tx(target_base, name_only_tx);
  const auto name_only_import = ImportArtifacts(name_only_context, {name_only});
  const bool name_only_rejected = !name_only_import.ok &&
                                  HasDiagnosticDetail(name_only_import, "artifact_object_uuid_required");
  const bool name_only_commit = Commit(name_only_context);

  const bool ok = source_tx.ok && source_commit && export_tx.ok && export_ok && import_tx.ok && import_commit &&
                  read_tx.ok && preserve_lookup && read_commit && conflict_tx.ok && conflict_rejected && conflict_commit &&
                  !remap_uuid.empty() && remap_tx.ok && remap_commit && remap_read_tx.ok && remap_lookup && remap_read_commit &&
                  name_only_tx.ok && name_only_rejected && name_only_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("export_ok", export_ok, true);
  PrintBool("preserve_import_ok", import_commit, true);
  PrintBool("preserve_lookup_by_uuid", preserve_lookup, true);
  PrintBool("conflict_rejected", conflict_rejected, true);
  PrintBool("remap_import_ok", remap_commit, true);
  PrintBool("remap_lookup_by_uuid", remap_lookup, true);
  PrintBool("name_only_rejected", name_only_rejected, true);
  PrintBool("git_not_runtime_authority", HasEvidence(exported, "git_runtime_authority", "false"), false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
