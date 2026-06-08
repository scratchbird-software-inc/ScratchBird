// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_provider_generation_store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct TempDatabase {
  std::filesystem::path dir;
  std::filesystem::path path;

  explicit TempDatabase(const std::string& name) {
    dir = std::filesystem::temp_directory_path() /
          ("scratchbird_document_provider_generation_" + name + "_" +
           api::GenerateCrudEngineUuid("database"));
    std::filesystem::create_directories(dir);
    path = dir / "database.sbdb";
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

bool DiagnosticContains(const api::EngineApiResult& result,
                        const std::string& detail) {
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&](const auto& diagnostic) {
                       return diagnostic.detail == detail ||
                              diagnostic.detail.find(detail) != std::string::npos ||
                              diagnostic.code == detail ||
                              diagnostic.message_key == detail;
                     });
}

bool DiagnosticContains(const api::EngineNoSqlProviderGenerationResult& result,
                        const std::string& detail) {
  return result.diagnostic.detail == detail ||
         result.diagnostic.detail.find(detail) != std::string::npos ||
         result.diagnostic.code == detail ||
         result.diagnostic.message_key == detail;
}

bool ProviderDiagnosticContains(const api::DocumentPathProviderResult& result,
                                const std::string& detail) {
  return result.diagnostic.detail == detail ||
         result.diagnostic.detail.find(detail) != std::string::npos ||
         result.diagnostic.code == detail ||
         result.diagnostic.message_key == detail;
}

bool EvidenceContains(const std::vector<std::string>& evidence,
                      const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool EvidenceTextContains(const api::EngineApiResult& result,
                          const std::string& value) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& evidence) {
                       return evidence.evidence_kind.find(value) !=
                                  std::string::npos ||
                              evidence.evidence_id.find(value) !=
                                  std::string::npos;
                     });
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "string";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRequestContext Context(const std::filesystem::path& path,
                                  std::uint64_t tx,
                                  std::string database_uuid = {},
                                  std::string schema_uuid = {}) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical =
      database_uuid.empty() ? api::GenerateCrudEngineUuid("database")
                            : std::move(database_uuid);
  context.current_schema_uuid.canonical =
      schema_uuid.empty() ? api::GenerateCrudEngineUuid("schema")
                          : std::move(schema_uuid);
  context.local_transaction_id = tx;
  context.transaction_uuid.canonical = api::GenerateCrudEngineUuid("transaction");
  context.catalog_generation_id = 101;
  context.security_epoch = 102;
  context.resource_epoch = 103;
  context.cluster_authority_available = true;
  context.security_context_present = true;
  return context;
}

void SeedTransaction(const api::EngineRequestContext& context) {
  std::ofstream crud(context.database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t" << context.local_transaction_id << '\t'
       << context.transaction_uuid.canonical << '\n';
  crud << "SBCRUD1\tTX_BEGIN\t" << (context.local_transaction_id + 1) << '\t'
       << api::GenerateCrudEngineUuid("transaction") << '\n';
  crud.flush();
  Require(static_cast<bool>(crud), "could not seed transaction inventory");
}

void InsertDocument(
    const api::EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& values) {
  api::EngineDocumentInsertRequest request;
  request.context = context;
  request.target_object.uuid.canonical = api::GenerateCrudEngineUuid("row");
  for (const auto& [path, value] : values) {
    request.assignments.push_back({path, Value(value)});
  }
  const auto result = api::EngineDocumentInsert(request);
  Require(result.ok, "document insert did not publish provider generation");
}

api::EngineNoSqlProviderGenerationMetadata CurrentGeneration(
    const api::EngineRequestContext& context) {
  const auto generations = api::ListNoSqlProviderGenerations(context);
  Require(generations.size() == 1, "expected exactly one provider generation");
  return generations.front();
}

api::EngineDocumentPhysicalProof Proof(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.document_path_index_runtime_proven = true;
  auto& contract = proof.provider_contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = api::kDocumentPathPhysicalProviderId;
  contract.local_provider_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = generation.generation_id;
  contract.index_generation.available_generation = generation.generation_id;
  contract.index_generation.index_uuid =
      api::DocumentPathProviderIdentityForContext(context, generation.generation_id)
          .index_uuid;
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = generation.generation_id;
  contract.provider_generation.available_generation = generation.generation_id;
  contract.provider_generation.descriptor_epoch = context.resource_epoch;
  contract.provider_generation.security_epoch = context.security_epoch;
  contract.provider_generation.redaction_epoch = context.security_epoch;
  contract.provider_generation.catalog_epoch = context.catalog_generation_id;
  contract.provider_generation.generation_uuid = generation.generation_uuid;
  contract.provider_generation.provider_id = generation.provider_id;
  contract.provider_generation.database_uuid = context.database_uuid.canonical;
  contract.provider_generation.collection_uuid = generation.collection_uuid;
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref = generation.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref = generation.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return proof;
}

api::EngineDocumentFindResult FindByTenant(
    const api::EngineRequestContext& context,
    const api::EngineDocumentPhysicalProof& proof) {
  api::EngineDocumentFindRequest request;
  request.context = context;
  request.path = "tenant.id";
  request.equals_value = "T1";
  request.projected_paths = {"tenant.id"};
  request.require_benchmark_clean_index_runtime = true;
  request.physical_proof = proof;
  return api::EngineDocumentFind(request);
}

void RequireNoDescriptorFallback(const api::EngineApiResult& result) {
  Require(!EvidenceTextContains(result, "descriptor_scan_selected=true"),
          "descriptor scan fallback was selected");
  Require(!EvidenceTextContains(result, "behavior_store_scan_selected=true"),
          "behavior store scan fallback was selected");
  Require(EvidenceTextContains(result, "descriptor_scan_selected=false") ||
              EvidenceTextContains(result,
                                   "document_provider_fail_closed_before_fallback"),
          "fail-closed route did not prove descriptor fallback refusal");
}

std::filesystem::path GenerationSidecar(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".sb.nosql_provider_generations");
}

std::filesystem::path DocumentSidecar(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".sb.nosql_document_provider");
}

std::filesystem::path ArtifactPath(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".sb.document_path_provider");
}

void CopyIfPresent(const std::filesystem::path& from,
                   const std::filesystem::path& to) {
  if (!std::filesystem::exists(from)) { return; }
  std::filesystem::copy_file(from,
                             to,
                             std::filesystem::copy_options::overwrite_existing);
}

void ProveCloseReopenBackupRestoreAndProofRefusals() {
  TempDatabase database("persist_restore");
  auto writer = Context(database.path, 200);
  SeedTransaction(writer);
  api::EngineDocumentProviderCleanup(writer, true);
  SeedTransaction(writer);

  InsertDocument(writer, {{"tenant.id", "T1"}, {"status", "open"}});
  const auto generation = CurrentGeneration(writer);
  Require(generation.database_identity == writer.database_path,
          "provider generation did not bind current database identity");
  Require(generation.database_uuid == writer.database_uuid.canonical,
          "provider generation did not bind database UUID");
  Require(generation.provider_id == api::kDocumentPathPhysicalProviderId,
          "provider id was not persisted");
  Require(generation.collection_uuid == writer.current_schema_uuid.canonical,
          "collection UUID was not persisted");
  Require(generation.generation_id != 0 && !generation.generation_uuid.empty(),
          "generation identity was not persisted");
  Require(generation.descriptor_epoch == writer.resource_epoch &&
              generation.security_epoch == writer.security_epoch &&
              generation.redaction_epoch == writer.security_epoch &&
              generation.catalog_epoch == writer.catalog_generation_id,
          "provider generation epochs were not persisted");
  Require(generation.publish_state == "published" &&
              generation.validation_state == "validated",
          "provider generation state was not persisted");
  Require(!generation.backup_metadata_ref.empty() &&
              !generation.restore_metadata_ref.empty() &&
              !generation.repair_metadata_ref.empty() &&
              !generation.support_bundle_evidence_id.empty(),
          "backup/restore/repair/support metadata refs were not persisted");

  api::EngineDocumentProviderCleanup(writer, false);
  const auto reopened = api::LoadNoSqlProviderGeneration(
      writer,
      api::EngineNoSqlProviderFamily::kDocument,
      generation.provider_id,
      generation.collection_uuid);
  Require(reopened.ok, "provider generation did not reload after close");
  Require(reopened.metadata.generation_uuid == generation.generation_uuid,
          "generation UUID changed across close/reopen");

  const auto restored_path = database.dir / "restored.sbdb";
  CopyIfPresent(database.path, restored_path);
  CopyIfPresent(GenerationSidecar(database.path), GenerationSidecar(restored_path));
  CopyIfPresent(DocumentSidecar(database.path), DocumentSidecar(restored_path));
  CopyIfPresent(ArtifactPath(database.path), ArtifactPath(restored_path));
  auto restored = Context(restored_path,
                          250,
                          writer.database_uuid.canonical,
                          writer.current_schema_uuid.canonical);
  const auto restored_generation = api::LoadNoSqlProviderGeneration(
      restored,
      api::EngineNoSqlProviderFamily::kDocument,
      generation.provider_id,
      generation.collection_uuid);
  Require(restored_generation.ok,
          "restored provider generation did not load from backup sidecar");
  Require(restored_generation.metadata.database_identity == restored.database_path,
          "restored generation was not rebound to restored database path");
  auto restored_find = FindByTenant(restored,
                                    Proof(restored, restored_generation.metadata));
  Require(restored_find.ok, "restored provider generation could not route find");
  RequireNoDescriptorFallback(restored_find);

  auto stale_proof = Proof(writer, generation);
  stale_proof.provider_contract.provider_generation.required_generation += 1;
  stale_proof.provider_contract.provider_generation.available_generation += 1;
  auto stale = FindByTenant(writer, stale_proof);
  Require(!stale.ok, "stale generation proof was accepted");
  Require(DiagnosticContains(stale, api::kNoSqlProviderGenerationStale),
          "stale generation diagnostic mismatch");
  RequireNoDescriptorFallback(stale);

  auto missing_proof = Proof(writer, generation);
  missing_proof.provider_contract.provider_generation.proof_present = false;
  auto missing = FindByTenant(writer, missing_proof);
  Require(!missing.ok, "missing generation proof was accepted");
  Require(DiagnosticContains(missing, api::kNoSqlProviderGenerationProofMissing),
          "missing generation proof diagnostic mismatch");
  RequireNoDescriptorFallback(missing);

  api::EngineDocumentFindRequest no_path_stale;
  no_path_stale.context = writer;
  no_path_stale.require_benchmark_clean_index_runtime = true;
  no_path_stale.physical_proof = stale_proof;
  auto no_path_result = api::EngineDocumentFind(no_path_stale);
  Require(!no_path_result.ok,
          "benchmark-clean stale generation fell back to descriptor scan");
  Require(DiagnosticContains(no_path_result, api::kNoSqlProviderGenerationStale),
          "benchmark-clean stale no-path diagnostic mismatch");
  RequireNoDescriptorFallback(no_path_result);
}

api::DocumentPathProviderBuildRequest BuildFixture(
    const api::EngineRequestContext& context,
    const std::filesystem::path& artifact_path) {
  api::DocumentPathProviderBuildRequest build;
  build.artifact_path = artifact_path.string();
  build.identity = api::DocumentPathProviderIdentityForContext(context, 7);
  api::DocumentPathRowEvidence row;
  row.document_uuid = api::GenerateCrudEngineUuid("row");
  row.row_uuid = api::GenerateCrudEngineUuid("row");
  row.version_uuid = api::GenerateCrudEngineUuid("row");
  row.row_ordinal = 7;
  row.values.push_back({"tenant.id", {"string", "T1", false}});
  row.values.push_back({"items.0.sku", {"string", "SKU-1", false}});
  build.rows.push_back(std::move(row));
  return build;
}

void CorruptChecksum(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  const auto pos = text.find("STATS");
  Require(pos != std::string::npos, "fixture artifact missing STATS tag");
  text.replace(pos, 5, "STATE");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

void ProveProviderAndGenerationRepairLifecycle() {
  TempDatabase database("repair");
  auto context = Context(database.path, 300);
  const auto artifact = database.dir / "repair.artifact";
  auto build = BuildFixture(context, artifact);
  const auto built = api::BuildDocumentPathPhysicalProvider(build);
  Require(built.ok, "repair fixture build failed");
  CorruptChecksum(artifact);

  api::DocumentPathProviderOpenRequest no_source;
  no_source.artifact_path = artifact.string();
  no_source.expected_identity = build.identity;
  no_source.require_expected_identity = true;
  no_source.repair_admitted = true;
  const auto no_source_result = api::OpenDocumentPathPhysicalProvider(no_source);
  Require(!no_source_result.ok &&
              ProviderDiagnosticContains(
                  no_source_result,
                  api::kDocumentPathPhysicalProviderRepairSourceRequired),
          "provider repair without source rows was admitted");

  auto no_admission = no_source;
  no_admission.repair_admitted = false;
  no_admission.authoritative_source_rows = build.rows;
  const auto no_admission_result =
      api::OpenDocumentPathPhysicalProvider(no_admission);
  Require(!no_admission_result.ok &&
              ProviderDiagnosticContains(
                  no_admission_result,
                  api::kDocumentPathPhysicalProviderRepairAdmissionRequired),
          "provider repair without explicit admission was admitted");

  auto admitted = no_source;
  admitted.authoritative_source_rows = build.rows;
  const auto repaired = api::OpenDocumentPathPhysicalProvider(admitted);
  Require(repaired.ok, "admitted provider repair did not rebuild artifact");
  Require(EvidenceContains(repaired.evidence,
                           "document_path_provider_repair_admitted=true"),
          "provider repair evidence missing");

  const auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context,
      api::kDocumentPathPhysicalProviderId,
      context.current_schema_uuid.canonical,
      7);
  const auto published = api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok, "generation publish failed before repair tests");

  api::EngineNoSqlProviderGenerationRepairRequest repair_request;
  repair_request.family = api::EngineNoSqlProviderFamily::kDocument;
  repair_request.provider_id = metadata.provider_id;
  repair_request.collection_uuid = metadata.collection_uuid;
  repair_request.authoritative_source_generations = {published.metadata};
  auto repair_without_admission =
      api::RepairNoSqlProviderGeneration(context, repair_request);
  Require(!repair_without_admission.ok &&
              DiagnosticContains(
                  repair_without_admission,
                  api::kNoSqlProviderGenerationRepairAdmissionRequired),
          "generation repair without admission was accepted");

  repair_request.repair_admitted = true;
  repair_request.authoritative_source_generations.clear();
  auto repair_without_source =
      api::RepairNoSqlProviderGeneration(context, repair_request);
  Require(!repair_without_source.ok &&
              DiagnosticContains(
                  repair_without_source,
                  api::kNoSqlProviderGenerationRepairSourceMissing),
          "generation repair without source metadata was accepted");

  auto dropped = api::DropNoSqlProviderGeneration(
      context,
      api::EngineNoSqlProviderFamily::kDocument,
      metadata.provider_id,
      metadata.collection_uuid);
  Require(dropped.ok, "generation drop before repair failed");
  repair_request.authoritative_source_generations = {published.metadata};
  const auto repaired_generation =
      api::RepairNoSqlProviderGeneration(context, repair_request);
  Require(repaired_generation.ok, "admitted generation repair failed");
  Require(EvidenceContains(repaired_generation.evidence,
                           "provider_generation_repair_published=true"),
          "generation repair publish evidence missing");
}

void ProveDropCleanupAndConcurrentLifecycle() {
  TempDatabase database("drop");
  auto context = Context(database.path, 400);
  SeedTransaction(context);
  InsertDocument(context, {{"tenant.id", "T1"}, {"status", "active"}});
  const auto generation = CurrentGeneration(context);

  api::EngineDocumentProviderCleanup(context, true);
  Require(api::ListNoSqlProviderGenerations(context).empty(),
          "drop cleanup left provider generation state");
  Require(!std::filesystem::exists(GenerationSidecar(database.path)),
          "drop cleanup left generation sidecar");
  Require(!std::filesystem::exists(DocumentSidecar(database.path)),
          "drop cleanup left document provider sidecar");
  Require(!std::filesystem::exists(ArtifactPath(database.path)),
          "drop cleanup left document path artifact");

  auto refused = FindByTenant(context, Proof(context, generation));
  Require(!refused.ok, "dropped generation was accepted");
  Require(DiagnosticContains(refused, api::kNoSqlProviderGenerationUnavailable),
          "dropped generation diagnostic mismatch");
  RequireNoDescriptorFallback(refused);

  TempDatabase concurrent_db("concurrent");
  auto concurrent = Context(concurrent_db.path, 500);
  const auto collection_uuid = concurrent.current_schema_uuid.canonical;
  std::vector<std::thread> threads;
  threads.emplace_back([&]() {
    for (std::uint64_t i = 1; i <= 12; ++i) {
      const auto metadata = api::MakeDocumentProviderGenerationMetadata(
          concurrent,
          api::kDocumentPathPhysicalProviderId,
          collection_uuid,
          i);
      (void)api::PublishNoSqlProviderGeneration(concurrent, metadata);
    }
  });
  threads.emplace_back([&]() {
    for (std::uint64_t i = 0; i < 12; ++i) {
      (void)api::CleanupNoSqlProviderGenerations(concurrent, false);
    }
  });
  threads.emplace_back([&]() {
    for (std::uint64_t i = 0; i < 12; ++i) {
      (void)api::ListNoSqlProviderGenerations(concurrent);
    }
  });
  for (auto& thread : threads) {
    thread.join();
  }

  const auto final_metadata = api::MakeDocumentProviderGenerationMetadata(
      concurrent,
      api::kDocumentPathPhysicalProviderId,
      collection_uuid,
      99);
  const auto final_publish =
      api::PublishNoSqlProviderGeneration(concurrent, final_metadata);
  Require(final_publish.ok, "final concurrent publish failed");
  Require(EvidenceContains(final_publish.evidence,
                           "provider_generation_concurrency_guard=mutex"),
          "generation publish did not report concurrency guard");
  const auto final_list = api::ListNoSqlProviderGenerations(concurrent);
  Require(final_list.size() == 1 && final_list.front().generation_id == 99,
          "concurrent cleanup/list/publish did not end deterministically");
}

void ProveNoProviderIndexParserOrLogFinalityAuthority() {
  TempDatabase database("authority");
  auto context = Context(database.path, 600);
  SeedTransaction(context);
  InsertDocument(context, {{"tenant.id", "T1"}, {"status", "active"}});
  const auto generation = CurrentGeneration(context);
  const auto result = FindByTenant(context, Proof(context, generation));
  Require(result.ok, "authority evidence find failed");
  for (const auto& evidence : result.evidence) {
    const std::string text = evidence.evidence_kind + "=" + evidence.evidence_id;
    for (const auto* forbidden :
         {"provider_finality_authority=true",
          "provider_visibility_authority=true",
          "index_transaction_finality_authority=true",
          "parser_transaction_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(text.find(forbidden) == std::string::npos,
              "forbidden finality authority evidence leaked");
    }
  }
  Require(EvidenceTextContains(result,
                               "mga_finality_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
}

}  // namespace

int main() {
  try {
    ProveCloseReopenBackupRestoreAndProofRefusals();
    ProveProviderAndGenerationRepairLifecycle();
    ProveDropCleanupAndConcurrentLifecycle();
    ProveNoProviderIndexParserOrLogFinalityAuthority();
  } catch (const std::exception& ex) {
    std::cerr << "document_provider_generation_lifecycle_gate failed: "
              << ex.what() << '\n';
    return 1;
  }
  return 0;
}
