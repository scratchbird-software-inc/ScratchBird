// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "nosql/document_api.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "ORH-090/091/092 gate failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiDiagnostic& diagnostic,
                        std::string_view token) {
  return diagnostic.code.find(token) != std::string::npos ||
         diagnostic.detail.find(token) != std::string::npos;
}

bool StringEvidenceContains(const std::vector<std::string>& evidence,
                            std::string_view token) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(token) != std::string::npos;
  });
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, UniqueMillis() + salt);
  Require(generated.ok(), "ORH-091 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Require(false, message);
  }
}

void RequireLifecycleOk(const db::DatabaseLifecycleResult& result,
                        std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':'
              << result.diagnostic.message_key << '\n';
    Require(false, message);
  }
}

std::filesystem::path UniqueTempDir(std::string_view name) {
  const auto now =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("scratchbird_orh090_" + std::string(name) + "_" +
                     std::to_string(now));
  std::filesystem::create_directories(path);
  return path;
}

struct TempDatabase {
  std::filesystem::path dir;
  std::filesystem::path path;
  std::string database_uuid;
  std::string collection_uuid;

  explicit TempDatabase(std::string_view name) : dir(UniqueTempDir(name)) {
    path = dir / "orh090.sbdb";
    auto database = NewTypedUuid(platform::UuidKind::database, 100);
    auto filespace = NewTypedUuid(platform::UuidKind::filespace, 101);
    database_uuid = uuid::UuidToString(database.value);
    collection_uuid = NewUuidText(platform::UuidKind::object, 102);

    db::DatabaseCreateConfig create;
    create.path = path.string();
    create.database_uuid = database;
    create.filespace_uuid = filespace;
    create.creation_unix_epoch_millis = UniqueMillis();
    create.require_resource_seed_pack = false;
    create.allow_minimal_resource_bootstrap = true;
    create.allow_overwrite = true;
    RequireLifecycleOk(db::CreateDatabaseFile(create),
                       "ORH-091 database create failed");
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  std::uint64_t tx,
                                  const std::string& database_uuid,
                                  const std::string& collection_uuid) {
  api::EngineRequestContext context;
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.current_schema_uuid.canonical = collection_uuid;
  context.transaction_uuid.canonical = "orh090-tx-" + std::to_string(tx);
  context.local_transaction_id = tx;
  context.security_context_present = true;
  context.resource_epoch = 901;
  context.security_epoch = 902;
  context.catalog_generation_id = 903;
  context.trace_tags = {"optimizer_runtime_hot_path_orh_090_091_092_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

api::EngineRequestContext BaseContext(const TempDatabase& db,
                                      std::string request_id,
                                      std::uint64_t epoch = 901) {
  auto context = Context(db.path, 0, db.database_uuid, db.collection_uuid);
  context.request_id = std::move(request_id);
  context.transaction_uuid.canonical.clear();
  context.local_transaction_id = 0;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, epoch + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, epoch + 101);
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.resource_epoch = epoch;
  context.security_epoch = epoch + 1;
  context.catalog_generation_id = epoch + 2;
  context.name_resolution_epoch = epoch + 3;
  return context;
}

api::EngineRequestContext Begin(const TempDatabase& db,
                                std::string request_id,
                                std::uint64_t epoch = 901) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(db, std::move(request_id), epoch);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ORH-091 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request),
            "ORH-091 commit transaction failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "ORH-091 rollback transaction failed");
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

void AddFragment(api::EngineDocumentInsertRequest* request,
                 std::string path,
                 std::string value) {
  request->assignments.push_back({std::move(path), Value(std::move(value))});
}

api::EngineDocumentInsertResult InsertDocument(
    const api::EngineRequestContext& context,
    std::string uuid,
    std::vector<std::pair<std::string, std::string>> fragments) {
  api::EngineDocumentInsertRequest insert;
  insert.context = context;
  insert.target_object.uuid.canonical = std::move(uuid);
  for (const auto& [path, value] : fragments) {
    AddFragment(&insert, path, value);
  }
  auto result = api::EngineDocumentInsert(insert);
  Require(result.ok, "document insert failed");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "provider_generation_persisted=true"),
          "insert did not persist provider generation metadata");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "document_provider_concurrency_guard=mutex"),
          "insert did not use provider concurrency guard");
  return result;
}

api::EngineNoSqlProviderGenerationMetadata CurrentDocumentGeneration(
    const api::EngineRequestContext& context) {
  const auto generations = api::ListNoSqlProviderGenerations(context);
  Require(generations.size() == 1,
          "expected one document provider generation for temp database");
  const auto& metadata = generations.front();
  Require(metadata.family == api::EngineNoSqlProviderFamily::kDocument,
          "generation family mismatch");
  Require(metadata.provider_id == "nosql.local.document.path_provider",
          "generation provider id mismatch");
  Require(metadata.collection_uuid == context.current_schema_uuid.canonical,
          "generation collection identity mismatch");
  Require(metadata.generation_id != 0, "generation id missing");
  Require(!metadata.backup_metadata_ref.empty(),
          "backup metadata was not bound");
  Require(!metadata.restore_metadata_ref.empty(),
          "restore metadata was not bound");
  Require(!metadata.repair_metadata_ref.empty(),
          "repair metadata was not bound");
  Require(!metadata.support_bundle_evidence_id.empty(),
          "support bundle metadata was not bound");
  return metadata;
}

api::EngineDocumentPhysicalProof DocumentProof(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.document_path_index_runtime_proven = false;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kDocument;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = generation.provider_id;
  proof.provider_contract.fallback_provider_id =
      "nosql.local.document.shape_dictionary";
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.exact_fallback_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation =
      generation.generation_id;
  proof.provider_contract.index_generation.available_generation =
      generation.generation_id;
  proof.provider_contract.index_generation.index_uuid =
      api::DocumentPathProviderIdentityForContext(context, generation.generation_id)
          .index_uuid;
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.provider_generation.required = true;
  proof.provider_contract.provider_generation.proof_present = true;
  proof.provider_contract.provider_generation.visible_to_snapshot = true;
  proof.provider_contract.provider_generation.publish_state_bound = true;
  proof.provider_contract.provider_generation.validation_state_bound = true;
  proof.provider_contract.provider_generation
      .backup_restore_repair_metadata_bound = true;
  proof.provider_contract.provider_generation.support_bundle_evidence_bound = true;
  proof.provider_contract.provider_generation.required_generation =
      generation.generation_id;
  proof.provider_contract.provider_generation.available_generation =
      generation.generation_id;
  proof.provider_contract.provider_generation.descriptor_epoch =
      context.resource_epoch;
  proof.provider_contract.provider_generation.security_epoch =
      context.security_epoch;
  proof.provider_contract.provider_generation.redaction_epoch =
      context.security_epoch;
  proof.provider_contract.provider_generation.catalog_epoch =
      context.catalog_generation_id;
  proof.provider_contract.provider_generation.generation_uuid =
      generation.generation_uuid;
  proof.provider_contract.provider_generation.provider_id =
      generation.provider_id;
  proof.provider_contract.provider_generation.database_uuid =
      context.database_uuid.canonical;
  proof.provider_contract.provider_generation.collection_uuid =
      generation.collection_uuid;
  proof.provider_contract.provider_generation.publish_state = "published";
  proof.provider_contract.provider_generation.validation_state = "validated";
  proof.provider_contract.provider_generation.backup_metadata_ref =
      generation.backup_metadata_ref;
  proof.provider_contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  proof.provider_contract.provider_generation.repair_metadata_ref =
      generation.repair_metadata_ref;
  proof.provider_contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

void RequireNoForbiddenAuthorityEvidence(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const std::string forbidden :
         {"descriptor_scan_selected=true",
          "behavior_store_scan_selected=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "index_transaction_finality_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true",
          "wal_recovery_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "forbidden authority evidence leaked");
    }
  }
}

std::vector<api::DocumentPathRowEvidence> RowsFromArtifact(
    const api::DocumentPathProviderArtifact& artifact) {
  std::map<std::uint64_t, api::DocumentPathProviderPathEntry> paths;
  for (const auto& path : artifact.path_dictionary) {
    paths[path.path_id] = path;
  }
  std::map<std::tuple<std::string, std::string, std::uint64_t>,
           api::DocumentPathRowEvidence>
      rows;
  for (const auto& posting : artifact.postings) {
    const auto path = paths.find(posting.path_id);
    if (path == paths.end() || path->second.path_kind != "normalized") {
      continue;
    }
    auto& row =
        rows[{posting.row_uuid, posting.version_uuid, posting.row_ordinal}];
    row.document_uuid = posting.document_uuid;
    row.row_uuid = posting.row_uuid;
    row.version_uuid = posting.version_uuid;
    row.row_ordinal = posting.row_ordinal;
    api::DocumentPathValueEvidence value;
    value.path = posting.concrete_path;
    value.value.scalar_type = posting.scalar_type;
    value.value.encoded_value = posting.encoded_value;
    value.value.is_null = posting.scalar_type == "null";
    row.values.push_back(std::move(value));
  }
  std::vector<api::DocumentPathRowEvidence> out;
  for (auto& [key, row] : rows) {
    (void)key;
    out.push_back(std::move(row));
  }
  return out;
}

void RequireBenchmarkCleanDocumentPathResult(
    const api::EngineApiResult& result,
    std::string_view message) {
  Require(result.ok, message);
  Require(result.dml_summary.visible_rows_scanned == 0,
          "document path provider route scanned visible descriptor rows");
  Require(result.dml_summary.index_probes == 1,
          "document path provider route did not use one provider probe");
  Require(result.dml_summary.benchmark_clean,
          "document path provider route was not marked benchmark-clean");
  Require(result.dml_summary.fallback_reasons.empty(),
          "document path provider route recorded fallback reasons");
  Require(EvidenceContains(result,
                           "document_index_runtime_correctness",
                           "proven"),
          "document path runtime proof did not come from provider probe");
  Require(EvidenceContains(result,
                           "document_path_physical_provider",
                           "document_path_provider_index_consumed=true"),
          "document path provider artifact was not consumed");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "selected_access=local_physical_provider"),
          "document path route capability was not local physical provider");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "fallback_selected=false"),
          "document path route selected a fallback provider");
  Require(EvidenceContains(result,
                           "document_index_route_capability",
                           "complete"),
          "document path index route capability was not complete");
  Require(EvidenceContains(result,
                           "document_index_route_kind",
                           "nosql_document"),
          "document path index route kind evidence missing");
  Require(EvidenceContains(result,
                           "document_index_route_supports_equality_lookup",
                           "true"),
          "document path route equality lookup capability missing");
  Require(EvidenceContains(result,
                           "document_index_route_supports_ordered_range",
                           "false"),
          "document path route incorrectly advertised ordered range support");
  Require(EvidenceContains(result,
                           "document_index_route_requires_exact_recheck",
                           "true"),
          "document path route exact recheck capability missing");
  Require(EvidenceContains(result,
                           "document_exact_source_recheck",
                           "mga_visibility_security_and_value_passed"),
          "document path route did not preserve exact source recheck");
  Require(!EvidenceContains(result,
                            "document_index_runtime_blocker",
                            api::kDocumentPathIndexRuntimeUnproven),
          "document path runtime blocker leaked after ORH-091 closure");
  RequireNoForbiddenAuthorityEvidence(result);
}

void ProvePersistentGenerationAndIndexedProviderUse() {
  TempDatabase db("persistent_generation");
  auto writer = Begin(db, "orh091-writer");
  InsertDocument(writer,
                 "orh090-doc-a",
                 {{"tenant.id", "T1"},
                  {"status", "active"},
                  {"line_items.0.sku", "SKU-1"},
                  {"line_items.1.sku", "SKU-2"}});
  InsertDocument(writer,
                 "orh090-doc-b",
                 {{"tenant.id", "T2"},
                  {"status", "inactive"},
                  {"line_items.0.sku", "SKU-3"}});
  Commit(writer);

  const auto generation = CurrentDocumentGeneration(writer);
  api::EngineDocumentProviderCleanup(writer, false);
  const auto loaded = api::LoadNoSqlProviderGeneration(
      writer,
      api::EngineNoSqlProviderFamily::kDocument,
      generation.provider_id,
      generation.collection_uuid);
  Require(loaded.ok, "provider generation did not reload after close cleanup");
  Require(loaded.metadata.generation_id == generation.generation_id,
          "provider generation id changed across reload");

  auto reader = Begin(db, "orh091-reader");
  api::EngineDocumentFindRequest find;
  find.context = reader;
  find.path = "tenant.id";
  find.equals_value = "T1";
  find.projected_paths = {"tenant.id", "status"};
  find.physical_proof = DocumentProof(reader, generation);
  find.require_benchmark_clean_index_runtime = true;
  auto result = api::EngineDocumentFind(find);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "document provider path index lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "document provider path index returned wrong row count");
  Require(RowField(result, 0, "path:tenant.id") == "T1",
          "document provider path index returned wrong path value");
  Require(RowField(result, 0, "path:status") == "active",
          "document provider path index returned wrong projected path value");
  Require(EvidenceContains(result,
                           "document_path_physical_provider",
                           "document_path_provider_value_posting_map_consumed=true"),
          "document value posting map evidence missing");
  Require(EvidenceContains(result,
                           "document_source_candidates_visible",
                           "1"),
          "document source MGA visibility recheck did not preserve row");
  Require(EvidenceContains(result,
                           "document_provider_index_consumed",
                           "true"),
          "document find did not consume provider index structure");
  Require(EvidenceContains(result,
                           "nosql_provider_generation",
                           "provider_generation_validated=true"),
          "provider generation validation evidence missing");

  api::EngineDocumentFindRequest array_exact = find;
  array_exact.path = "line_items.1.sku";
  array_exact.equals_value = "SKU-2";
  array_exact.projected_paths = {"tenant.id"};
  result = api::EngineDocumentFind(array_exact);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "array path provider lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "array path provider lookup returned wrong row count");
  Require(RowField(result, 0, "path:tenant.id") == "T1",
          "array path provider lookup returned wrong projection");

  api::EngineDocumentFindRequest false_claim = find;
  false_claim.physical_proof.document_path_index_runtime_proven = true;
  false_claim.physical_proof.provider_contract.index_generation.index_uuid =
      "not-the-completed-document-path-index";
  result = api::EngineDocumentFind(false_claim);
  Require(!result.ok,
          "caller-supplied index runtime proof bypassed route validation");
  Require(DiagnosticContains(result, api::kDocumentPathIndexRuntimeUnproven),
          "caller-supplied index runtime proof diagnostic mismatch");

  api::EngineDocumentFindRequest wildcard;
  wildcard.context = reader;
  wildcard.path = "line_items.*.sku";
  wildcard.equals_value = "SKU-2";
  wildcard.wildcard_path = true;
  wildcard.projected_paths = {"tenant.id"};
  wildcard.require_benchmark_clean_index_runtime = true;
  wildcard.physical_proof = DocumentProof(reader, generation);
  result = api::EngineDocumentFind(wildcard);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "wildcard provider path index lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "wildcard provider path index returned wrong row count");
  Require(RowField(result, 0, "path:tenant.id") == "T1",
          "wildcard provider path index returned wrong projection");
  Require(EvidenceContains(result,
                           "document_physical_access",
                           "wildcard_shape_index_probe"),
          "wildcard provider path evidence missing");
  Require(EvidenceContains(result,
                           "document_path_physical_provider",
                           "document_path_provider_wildcard_posting_map_consumed=true"),
          "wildcard posting map evidence missing");
  Require(EvidenceContains(result,
                           "document_path_physical_provider",
                           "document_path_provider_array_expansion_map_consumed=true"),
          "array expansion map evidence missing");

  auto stale_proof = DocumentProof(reader, generation);
  stale_proof.provider_contract.provider_generation.required_generation =
      generation.generation_id + 1;
  stale_proof.provider_contract.provider_generation.available_generation =
      generation.generation_id + 1;
  find.physical_proof = stale_proof;
  result = api::EngineDocumentFind(find);
  Require(!result.ok, "stale provider generation was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderGenerationStale),
          "stale provider generation diagnostic mismatch");

  Rollback(reader);

  auto delete_tx = Begin(db, "orh091-delete");
  api::EngineDocumentDeleteRequest delete_request;
  delete_request.context = delete_tx;
  delete_request.target_object.uuid.canonical = "orh090-doc-b";
  auto delete_result = api::EngineDocumentDelete(delete_request);
  Require(delete_result.ok, "document delete did not publish provider generation");
  Require(EvidenceContains(delete_result,
                           "nosql_provider_generation",
                           "provider_generation_persisted=true"),
          "document delete did not persist a new provider generation");
  const auto delete_generation = CurrentDocumentGeneration(writer);
  Require(delete_generation.generation_id > generation.generation_id,
          "document delete did not advance provider generation");
  Commit(delete_tx);
}

void ProveRollbackReopenRepairAndMgaRecheck() {
  TempDatabase db("rollback_reopen_repair");
  auto committed_writer = Begin(db, "orh091-committed-writer");
  InsertDocument(committed_writer,
                 "orh091-committed-doc",
                 {{"tenant.id", "T-COMMITTED"},
                  {"status", "active"},
                  {"line_items.0.sku", "SKU-COMMITTED"}});
  Commit(committed_writer);

  auto rolled_back_writer = Begin(db, "orh091-rollback-writer");
  InsertDocument(rolled_back_writer,
                 "orh091-rolled-back-doc",
                 {{"tenant.id", "T-ROLLBACK"},
                  {"status", "rolled_back"},
                  {"line_items.0.sku", "SKU-ROLLBACK"}});
  const auto rolled_back_generation =
      CurrentDocumentGeneration(rolled_back_writer);
  Rollback(rolled_back_writer);

  api::EngineDocumentProviderCleanup(committed_writer, false);
  auto reader = Begin(db, "orh091-reopen-reader");
  api::EngineDocumentFindRequest visible;
  visible.context = reader;
  visible.path = "tenant.id";
  visible.equals_value = "T-COMMITTED";
  visible.projected_paths = {"tenant.id", "status"};
  visible.require_benchmark_clean_index_runtime = true;
  visible.physical_proof = DocumentProof(reader, rolled_back_generation);
  auto result = api::EngineDocumentFind(visible);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "reopened document path provider lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "reopened provider lost committed document");

  api::EngineDocumentFindRequest invisible = visible;
  invisible.equals_value = "T-ROLLBACK";
  result = api::EngineDocumentFind(invisible);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "rolled-back document path provider lookup failed closed");
  Require(result.result_shape.rows.empty(),
          "rolled-back document became visible through provider candidate");
  Require(EvidenceContains(result,
                           "document_source_candidates_rechecked",
                           "1"),
          "rolled-back provider candidate was not rechecked");
  Require(EvidenceContains(result,
                           "document_source_candidates_visible",
                           "0"),
          "rolled-back provider candidate passed MGA visibility recheck");

  api::DocumentPathProviderOpenRequest open;
  open.artifact_path = api::DocumentPathPhysicalProviderPath(reader);
  open.expected_identity = api::DocumentPathProviderIdentityForContext(
      reader,
      rolled_back_generation.generation_id,
      visible.physical_proof.provider_contract.index_generation.index_uuid);
  open.require_expected_identity = true;
  auto opened = api::OpenDocumentPathPhysicalProvider(open);
  Require(opened.ok, "document path provider did not reopen for repair seed");
  auto repair_rows = RowsFromArtifact(opened.artifact);
  Require(!repair_rows.empty(), "document path repair seed rows missing");

  {
    std::ofstream corrupt(open.artifact_path, std::ios::binary | std::ios::trunc);
    corrupt << "SBDOCPATH\nVERSION\t1\nCHECKSUM\tbad\nEND\n";
    corrupt.flush();
    Require(static_cast<bool>(corrupt),
            "document path provider corrupt write failed");
  }
  open.authoritative_source_rows = repair_rows;
  opened = api::OpenDocumentPathPhysicalProvider(open);
  Require(!opened.ok,
          "document path provider repair ran without repair admission");
  Require(DiagnosticContains(opened.diagnostic,
                             api::kDocumentPathPhysicalProviderRepairAdmissionRequired),
          "document path provider repair admission diagnostic mismatch");

  open.repair_admitted = true;
  opened = api::OpenDocumentPathPhysicalProvider(open);
  Require(opened.ok, "document path provider repair failed");
  Require(StringEvidenceContains(opened.evidence,
                                 "document_path_provider_repair_admitted=true"),
          "document path provider repair admission evidence missing");
  Require(StringEvidenceContains(
              opened.evidence,
              "document_path_provider_descriptor_scan_fallback=false"),
          "document path provider repair used descriptor scan fallback");

  result = api::EngineDocumentFind(visible);
  RequireBenchmarkCleanDocumentPathResult(
      result,
      "repaired document path provider route failed");
  Require(result.result_shape.rows.size() == 1,
          "repaired document path provider returned wrong row count");
  Rollback(reader);
}

void ProveLifecycleCleanupAndDatabaseIsolation() {
  TempDatabase a("lifecycle_a");
  TempDatabase b("lifecycle_b");
  auto ctx_a = Begin(a, "orh091-lifecycle-a");
  auto ctx_b = Begin(b, "orh091-lifecycle-b");
  InsertDocument(ctx_a, "orh090-doc-a", {{"tenant.id", "A"}});
  InsertDocument(ctx_b, "orh090-doc-b", {{"tenant.id", "B"}});
  Commit(ctx_a);
  Commit(ctx_b);
  const auto gen_a = CurrentDocumentGeneration(ctx_a);
  const auto gen_b = CurrentDocumentGeneration(ctx_b);

  api::EngineDocumentProviderCleanup(ctx_a, true);
  Require(api::ListNoSqlProviderGenerations(ctx_a).empty(),
          "drop cleanup leaked provider generation state for database A");
  Require(!api::ListNoSqlProviderGenerations(ctx_b).empty(),
          "drop cleanup for database A erased provider state for database B");

  api::EngineDocumentFindRequest find_b;
  auto reader_b = Begin(b, "orh091-lifecycle-reader-b");
  find_b.context = reader_b;
  find_b.path = "tenant.id";
  find_b.equals_value = "B";
  find_b.require_benchmark_clean_index_runtime = true;
  find_b.physical_proof = DocumentProof(reader_b, gen_b);
  const auto result_b = api::EngineDocumentFind(find_b);
  RequireBenchmarkCleanDocumentPathResult(
      result_b,
      "database B provider state was not isolated");
  Require(result_b.result_shape.rows.size() == 1,
          "database B provider lookup returned wrong row count");
  Rollback(reader_b);

  api::EngineDocumentFindRequest find_a;
  auto reader_a = Begin(a, "orh091-lifecycle-reader-a");
  find_a.context = reader_a;
  find_a.path = "tenant.id";
  find_a.equals_value = "A";
  find_a.require_benchmark_clean_index_runtime = true;
  find_a.physical_proof = DocumentProof(reader_a, gen_a);
  const auto result_a = api::EngineDocumentFind(find_a);
  Require(!result_a.ok,
          "dropped database A provider generation was accepted after cleanup");
  Require(DiagnosticContains(result_a, api::kNoSqlProviderGenerationUnavailable),
          "drop cleanup diagnostic mismatch");
  Rollback(reader_a);
}

}  // namespace

int main() {
  ProvePersistentGenerationAndIndexedProviderUse();
  ProveRollbackReopenRepairAndMgaRecheck();
  ProveLifecycleCleanupAndDatabaseIsolation();
  return EXIT_SUCCESS;
}
