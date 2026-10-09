#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "ddl/create_api.hpp"
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
#include <array>
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
    auto pattern = (std::filesystem::temp_directory_path() /
        ("scratchbird_document_provider_generation_" + name + "_XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto* created = ::mkdtemp(writable.data());
    Require(created != nullptr, "could not create isolated fixture directory");
    dir = created;
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
                       const std::string structured =
                           evidence.evidence_kind + "=" + scratchbird::tests::EvidenceTextFields(evidence.evidence_id);
                       return evidence.evidence_kind.find(value) !=
                                  std::string::npos ||
                              scratchbird::tests::EvidenceTextFind(evidence.evidence_id, value) !=
                                  std::string::npos ||
                              structured.find(value) !=
                                  std::string::npos;
                     });
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "string";
  typed.encoded_value = std::move(value);
  return typed;
}

struct OwnedContext : api::EngineRequestContext {
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  std::shared_ptr<scratchbird::tests::FixtureEngineStatement> statement;
};

OwnedContext Context(const std::filesystem::path& path,
                                  std::uint64_t request_ordinal,
                                  api::EngineUuid database_uuid = {},
                                  api::EngineUuid schema_uuid = {},
                                  const api::EngineRequestContext* restored_owner = nullptr) {
  namespace db = scratchbird::storage::database;
  namespace uuid = scratchbird::core::uuid;
  using scratchbird::core::platform::UuidKind;
  OwnedContext retained;
  api::EngineRequestContext context;
  context.request_id = "document-generation-fixture-" + std::to_string(request_ordinal);
  context.database_path = path.string();
  context.database_uuid = database_uuid.is_nil()
      ? api::GenerateCrudEngineUuid("database") : database_uuid;
  context.current_schema_uuid = schema_uuid.is_nil()
      ? api::GenerateCrudEngineUuid("schema") : schema_uuid;
  const bool create_new = !std::filesystem::exists(path);
  if (create_new) {
    db::DatabaseCreateConfig create;
    create.path = context.database_path;
    const auto database_id = uuid::MakeTypedUuid(UuidKind::database, context.database_uuid);
    const auto filespace_id = uuid::MakeTypedUuid(
        UuidKind::filespace, api::GenerateCrudEngineUuid("filespace"));
    Require(database_id.ok() && filespace_id.ok(), "fixture database identity invalid");
    create.database_uuid = database_id.value;
    create.filespace_uuid = filespace_id.value;
    create.page_size = 16384;
    create.creation_unix_epoch_millis = 1790000000203;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    Require(db::CreateDatabaseFile(create).ok(), "fixture database creation failed");
    const auto collection = context.current_schema_uuid;
    context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    context.current_schema_uuid = collection;
  } else {
    Require(restored_owner != nullptr && restored_owner->database_uuid == database_uuid,
            "restored fixture owner identity missing or mismatched");
    context = *restored_owner;
    context.database_path = path.string();
    context.session_uuid = api::GenerateCrudEngineUuid("object");
    context.current_schema_uuid = schema_uuid;
    scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  }
  context.request_id = "document-generation-fixture-" + std::to_string(request_ordinal);
  retained.owner_context = context;
  retained.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(context);
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  begin.transaction_policy_profile.encoded_profiles = {
      "fail_closed:true", "transaction_read_only:false", "transaction_read_mode:read_write"};
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok && begun.local_transaction_id != 0,
          "document fixture engine transaction begin failed");
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  if (create_new) {
    api::EngineCreateSchemaRequest schema;
    schema.context = context;
    schema.target_object.uuid = context.current_schema_uuid;
    schema.target_object.object_kind = "schema";
    schema.localized_names.push_back({"en", "primary", "", "document_schema", true});
    Require(api::EngineCreateSchema(schema).ok, "document fixture schema creation failed");
  }
  retained.statement = std::make_shared<scratchbird::tests::FixtureEngineStatement>(*retained.session, context);
  static_cast<api::EngineRequestContext&>(retained) = retained.statement->context;
  return retained;
}

void CommitTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  Require(api::EngineCommitTransaction(commit).ok,
          "document fixture engine transaction commit failed");
}

void InsertDocument(
    const api::EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& values) {
  api::EngineDocumentInsertRequest request;
  request.context = context;
  request.target_object.uuid = api::GenerateCrudEngineUuid("row");
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
  contract.provider_generation.database_uuid = context.database_uuid;
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

std::string ReadArtifactBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "fixture artifact cannot be read");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void ProveCloseReopenBackupRestoreAndProofRefusals() {
  TempDatabase database("persist_restore");
  auto writer = Context(database.path, 200);
  api::EngineDocumentProviderCleanup(writer, true);

  InsertDocument(writer, {{"tenant.id", "T1"}, {"status", "open"}});
  const auto generation = CurrentGeneration(writer);
  CommitTransaction(writer);
  Require(generation.database_identity == writer.database_path,
          "provider generation did not bind current database identity");
  Require(generation.database_uuid == writer.database_uuid,
          "provider generation did not bind database UUID");
  Require(generation.provider_id == api::kDocumentPathPhysicalProviderId,
          "provider id was not persisted");
  Require(generation.collection_uuid == writer.current_schema_uuid,
          "collection UUID was not persisted");
  Require(generation.generation_id != 0 && !generation.generation_uuid.is_nil(),
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
  // Restore the owning catalog companions too, not only the derived index.
  const auto prefix = database.path.filename().string() + ".sb.";
  for (const auto& entry : std::filesystem::directory_iterator(database.dir)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() && name.starts_with(prefix))
      CopyIfPresent(entry.path(), restored_path.string() +
          name.substr(database.path.filename().string().size()));
  }
  auto restored = Context(restored_path,
                          250,
                          writer.database_uuid,
                          writer.current_schema_uuid,
                          &writer.owner_context);
  const auto restored_bytes = ReadArtifactBytes(GenerationSidecar(restored_path));
  const auto restored_generation = api::LoadNoSqlProviderGeneration(
      restored,
      api::EngineNoSqlProviderFamily::kDocument,
      generation.provider_id,
      generation.collection_uuid);
  Require(restored_generation.ok,
          "restored provider generation did not load from backup sidecar");
  Require(restored_generation.metadata.database_uuid == generation.database_uuid &&
              restored_generation.metadata.collection_uuid == generation.collection_uuid &&
              restored_generation.metadata.generation_uuid == generation.generation_uuid &&
              restored_generation.metadata.generation_id == generation.generation_id &&
              ReadArtifactBytes(GenerationSidecar(restored_path)) == restored_bytes,
          "restore read changed native identity or rewrote generation evidence");
  Require(restored_generation.metadata.database_identity == restored.database_path,
          "restored generation was not rebound to restored database path");
  auto restored_find = FindByTenant(restored,
                                    Proof(restored, restored_generation.metadata));
  Require(restored_find.ok, "restored provider generation could not route find");
  RequireNoDescriptorFallback(restored_find);

  TempDatabase foreign_database("foreign_restore");
  auto foreign = Context(foreign_database.path, 251);
  CopyIfPresent(GenerationSidecar(database.path), GenerationSidecar(foreign_database.path));
  const auto foreign_bytes = ReadArtifactBytes(GenerationSidecar(foreign_database.path));
  Require(!api::LoadNoSqlProviderGeneration(foreign,
              api::EngineNoSqlProviderFamily::kDocument, generation.provider_id,
              generation.collection_uuid).ok,
          "document generation was rebound to a foreign database UUID");
  auto forged = static_cast<api::EngineRequestContext>(restored);
  forged.database_path = foreign_database.path.string();
  Require(!api::LoadNoSqlProviderGeneration(forged,
              api::EngineNoSqlProviderFamily::kDocument, generation.provider_id,
              generation.collection_uuid).ok &&
              ReadArtifactBytes(GenerationSidecar(foreign_database.path)) == foreign_bytes,
          "forged context bypassed destination header identity or rewrote foreign evidence");
  forged.database_path = (foreign_database.dir / "missing.sbdb").string();
  CopyIfPresent(GenerationSidecar(database.path), GenerationSidecar(forged.database_path));
  Require(!api::LoadNoSqlProviderGeneration(forged,
              api::EngineNoSqlProviderFamily::kDocument, generation.provider_id,
              generation.collection_uuid).ok,
          "document generation accepted a missing destination database");

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
  Require(text.size() > 53 && text.starts_with("SBDOCPATH"),
          "fixture binary document artifact missing");
  text.back() ^= 1;  // Alter the body without resealing its SHA-256 header.
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
      context.current_schema_uuid,
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
  const auto collection_uuid = concurrent.current_schema_uuid;
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
  InsertDocument(context, {{"tenant.id", "T1"}, {"status", "active"}});
  const auto generation = CurrentGeneration(context);
  const auto result = FindByTenant(context, Proof(context, generation));
  Require(result.ok, "authority evidence find failed");
  for (const auto& evidence : result.evidence) {
    const std::string text = evidence.evidence_kind + "=" + scratchbird::tests::EvidenceTextFields(evidence.evidence_id);
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

void ProveNativeUuidDataPublicationAndReplay() {
  TempDatabase database("uuid_data");
  auto writer = Context(database.path, 800);
  api::EngineColumnDefinition column;
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "uuid";
  column.descriptor.encoded_descriptor = "nullability=nullable";
  scratchbird::tests::BindFixtureColumnDatatype(writer,
      scratchbird::core::datatypes::CanonicalTypeId::uuid, column);
  std::array<std::vector<std::uint8_t>, 4> values;
  for (auto& bytes : values) bytes.resize(16);
  values[1][6] = 0x40; values[1][8] = 0x80; values[1][15] = 1;
  values[2].assign(16, 0xff);
  for (std::size_t i = 0; i < 16; ++i) values[3][i] = static_cast<std::uint8_t>(i * 17);
  for (const auto& bytes : values) {
    api::EngineTypedValue value;
    value.descriptor = column.descriptor;
    value.binary_value = bytes;
    api::EngineDocumentInsertRequest insert;
    insert.context = writer;
    insert.target_object.uuid = api::GenerateCrudEngineUuid("row");
    insert.assignments = {{"user_uuid", value}};
    auto missing = value;
    missing.binary_value.clear(); missing.setState(api::EngineValueState::missing);
    insert.assignments.push_back({"absent", missing});
    const auto published = api::EngineDocumentInsert(insert);
    Require(published.ok, "arbitrary binary16 document UUID publication failed");
  }
  api::EngineTypedValue null;
  null.descriptor = column.descriptor;
  // State is authoritative even when a legacy mirror has not been populated.
  null.state = api::EngineValueState::sql_null;
  api::EngineDocumentInsertRequest null_insert;
  null_insert.context = writer;
  null_insert.target_object.uuid = api::GenerateCrudEngineUuid("row");
  null_insert.assignments = {{"user_uuid", null}};
  Require(api::EngineDocumentInsert(null_insert).ok, "typed UUID NULL publication failed");
  const auto generation = CurrentGeneration(writer);
  const auto provider_before = ReadArtifactBytes(DocumentSidecar(database.path));
  const auto artifact_before = ReadArtifactBytes(ArtifactPath(database.path));
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    auto bad = null;
    bad.setState(api::EngineValueState::value);
    bad.binary_value = values[1];
    if (mutation == 0) bad.binary_value.pop_back();
    if (mutation == 1) bad.binary_value.push_back(0);
    if (mutation == 2) bad.encoded_value = "mixed carrier";
    if (mutation == 3) { bad.binary_value.clear(); bad.encoded_value = "019f0000-0000-7000-8000-000000000001"; }
    if (mutation == 4) bad.setState(api::EngineValueState::sql_null);
    if (mutation == 5) bad.setState(api::EngineValueState::missing);
    auto insert = null_insert;
    insert.target_object.uuid = api::GenerateCrudEngineUuid("row");
    insert.assignments = {{"user_uuid", bad}};
    Require(!api::EngineDocumentInsert(insert).ok, "malformed UUID document insert accepted");
    api::EngineDocumentUpdateRequest update;
    static_cast<api::EngineApiRequest&>(update) = insert;
    Require(!api::EngineDocumentUpdate(update).ok, "malformed UUID document update accepted");
    Require(ReadArtifactBytes(DocumentSidecar(database.path)) == provider_before &&
                ReadArtifactBytes(ArtifactPath(database.path)) == artifact_before &&
                CurrentGeneration(writer).generation_uuid == generation.generation_uuid,
            "malformed UUID mutation changed persisted document state");
  }
  CommitTransaction(writer);
  api::EngineDocumentProviderCleanup(writer, false);
  writer.statement.reset(); writer.session.reset();
  auto reader = Context(database.path, 801, writer.database_uuid,
                        writer.current_schema_uuid, &writer.owner_context);
  api::DocumentPathProviderOpenRequest open;
  open.artifact_path = ArtifactPath(database.path);
  const auto reopened = api::OpenDocumentPathPhysicalProvider(open);
  Require(reopened.ok && reopened.artifact.stats.path_count == 1 &&
              reopened.artifact.postings.size() == 5,
          "typed document paths were lost or missing paths invented on reopen");
  api::EngineDocumentFindRequest all;
  all.context = reader;
  all.path = "user_uuid";
  all.projected_paths = {"user_uuid"};
  all.descriptors = {column.descriptor};
  all.typed_rows_only = true;
  all.physical_proof = Proof(reader, CurrentGeneration(reader));
  const auto all_values = api::EngineDocumentFind(all);
  Require(all_values.ok && all_values.typed_rows.size() == 5 &&
              std::count_if(all_values.typed_rows.begin(), all_values.typed_rows.end(),
                  [](const auto& row) { return row.values[0].value.isSqlNull() &&
                      row.values[0].value.binary_value.empty() && row.values[0].value.encoded_value.empty(); }) == 1,
          "UUID SQL NULL and nil UUID were conflated during replay or projection");
  for (const auto& bytes : values) {
    const std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Require(std::count_if(reopened.artifact.postings.begin(), reopened.artifact.postings.end(),
                [&](const auto& posting) { return posting.scalar_type == "uuid" && posting.encoded_value == raw; }) == 1,
            "durable UUID posting lost native type or bytes");
    api::EngineDocumentFindRequest find;
    find.context = reader;
    find.path = "user_uuid";
    find.projected_paths = {"user_uuid"};
    find.descriptors = {column.descriptor};
    find.typed_rows_only = true;
    find.comparison_value_present = true;
    find.comparison_operator = "=";
    find.comparison_value.descriptor = column.descriptor;
    find.comparison_value.binary_value = bytes;
    find.physical_proof = Proof(reader, CurrentGeneration(reader));
    find.require_benchmark_clean_index_runtime = true;
    auto wrong_profile = find;
    wrong_profile.comparison_value.descriptor.encoded_descriptor += ";ordering=guid";
    const auto refused_profile = api::EngineDocumentFind(wrong_profile);
    Require(!refused_profile.ok && refused_profile.typed_rows.empty() &&
                refused_profile.result_shape.rows.empty(),
            "document comparison silently admitted an unbound UUID ordering profile");
    const auto found = api::EngineDocumentFind(find);
    if (!found.ok) for (const auto& diagnostic : found.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Require(found.ok && found.typed_rows.size() == 1 && found.typed_rows[0].values.size() == 1 &&
                found.typed_rows[0].values[0].value.binary_value == bytes &&
                found.typed_rows[0].values[0].value.encoded_value.empty() &&
                !found.typed_rows[0].values[0].value.isSqlNull() &&
                found.dml_summary.index_probes == 1 &&
                found.base_row_mga_recheck_complete && found.security_recheck_complete,
            "UUID equality after cache-independent replay lost data or authority");
    find.typed_rows_only = false;
    const auto generic = api::EngineDocumentFind(find);
    Require(generic.ok && generic.result_shape.rows.size() == 1,
            "generic document result publication failed");
    const auto field = std::ranges::find_if(generic.result_shape.rows[0].fields,
        [](const auto& value) { return value.first == "path:user_uuid"; });
    Require(field != generic.result_shape.rows[0].fields.end() &&
                field->second.binary_value == bytes && field->second.encoded_value.empty() &&
                field->second.descriptor.canonical_type_name == "uuid",
            "generic document result converted UUID data into untyped TEXT");
    find.comparison_value = null;
    for (const auto* operation : {"=", "<>", "<", "<=", ">", ">="}) {
      find.comparison_operator = operation;
      const auto unknown = api::EngineDocumentFind(find);
      Require(unknown.ok && unknown.typed_rows.empty() &&
                  unknown.result_shape.rows.empty(),
              "UUID NULL comparison was not unknown");
    }
    find.comparison_value.binary_value = bytes;
    const auto malformed_null = api::EngineDocumentFind(find);
    Require(!malformed_null.ok && malformed_null.typed_rows.empty() &&
                malformed_null.result_shape.rows.empty(),
            "UUID NULL comparison admitted substitute payload");
  }
  InsertDocument(reader, {{"note", "rebuild after replay"}});
  CommitTransaction(reader);
  const auto rebuilt = api::OpenDocumentPathPhysicalProvider(open);
  Require(rebuilt.ok && rebuilt.artifact.stats.path_count == 2 &&
              rebuilt.artifact.postings.size() == 6 &&
              std::count_if(rebuilt.artifact.postings.begin(), rebuilt.artifact.postings.end(),
                  [](const auto& posting) { return posting.scalar_type == "uuid" && posting.encoded_value.size() == 16; }) == 4,
          "provider rebuild after replay lost retained UUID scalar datatypes");
}

int main(int argc, char** argv) {
  const bool uuid_only = argc == 2 && std::string_view(argv[1]) == "--uuid-data-only";
  if (argc != 1 && !uuid_only) return 2;
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("document-provider-generation-native-fixture");
  try {
    if (!uuid_only) {
      ProveCloseReopenBackupRestoreAndProofRefusals();
      ProveProviderAndGenerationRepairLifecycle();
      ProveDropCleanupAndConcurrentLifecycle();
      ProveNoProviderIndexParserOrLogFinalityAuthority();
    }
    ProveNativeUuidDataPublicationAndReplay();
  } catch (const std::exception& ex) {
    std::cerr << "document_provider_generation_lifecycle_gate failed: "
              << ex.what() << '\n';
    return 1;
  }
  return 0;
}
