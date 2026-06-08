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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

std::uint64_t Fnva64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "failed writing test artifact");
}

std::string Rechecksum(std::string text) {
  const auto checksum_pos = text.find("CHECKSUM\t");
  Require(checksum_pos != std::string::npos, "checksum header missing");
  const auto checksum_end = text.find('\n', checksum_pos);
  Require(checksum_end != std::string::npos, "checksum line missing newline");
  const auto body_start = checksum_end + 1;
  const auto end_pos = text.rfind("END\n");
  Require(end_pos != std::string::npos, "END missing");
  const auto body = text.substr(body_start, end_pos - body_start);
  text.replace(checksum_pos,
               checksum_end - checksum_pos,
               "CHECKSUM\t" + Hex64(Fnva64(body)));
  return text;
}

std::string MutateFirstTag(const std::string& text,
                           const std::string& tag,
                           const std::string& key,
                           const std::string& value) {
  std::istringstream in(text);
  std::ostringstream out;
  std::string line;
  bool changed = false;
  while (std::getline(in, line)) {
    if (!changed && line.rfind(tag + "\t", 0) == 0) {
      auto pairs = api::DecodeCrudPairs(line.substr(tag.size() + 1));
      bool found = false;
      for (auto& [pair_key, pair_value] : pairs) {
        if (pair_key == key) {
          pair_value = value;
          found = true;
          break;
        }
      }
      Require(found, "mutation key missing: " + key);
      line = tag + "\t" + api::EncodeCrudPairs(pairs);
      changed = true;
    }
    out << line << '\n';
  }
  Require(changed, "mutation tag missing: " + tag);
  return Rechecksum(out.str());
}

bool EvidenceContains(const api::EngineApiResult& result,
                      const std::string& kind,
                      const std::string& id) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& evidence) {
                       return evidence.evidence_kind == kind &&
                              evidence.evidence_id == id;
                     });
}

bool ProviderDiagnosticContains(const api::DocumentPathProviderResult& result,
                                const std::string& detail) {
  return result.diagnostic.detail == detail ||
         result.diagnostic.detail.find(detail) != std::string::npos ||
         result.diagnostic.code == detail ||
         result.diagnostic.message_key == detail;
}

bool RowHasField(const api::EngineRowValue& row, const std::string& field) {
  return std::any_of(row.fields.begin(), row.fields.end(), [&](const auto& pair) {
    return pair.first == field;
  });
}

api::EngineTypedValue Value(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.canonical_type_name = "string";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRequestContext Context(const std::filesystem::path& path,
                                  std::uint64_t tx) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = api::GenerateCrudEngineUuid("database");
  context.current_schema_uuid.canonical = api::GenerateCrudEngineUuid("schema");
  context.local_transaction_id = tx;
  context.transaction_uuid.canonical = api::GenerateCrudEngineUuid("transaction");
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.cluster_authority_available = true;
  context.security_context_present = true;
  return context;
}

void SeedTransaction(const api::EngineRequestContext& context) {
  std::ofstream crud(context.database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t" << context.local_transaction_id << '\t'
       << context.transaction_uuid.canonical << '\n';
  crud << "SBCRUD1\tTX_BEGIN\t" << (context.local_transaction_id + 50) << '\t'
       << api::GenerateCrudEngineUuid("transaction") << '\n';
  crud.flush();
  Require(static_cast<bool>(crud), "could not seed transaction inventory");
}

api::EngineDocumentInsertResult InsertDocument(
    const api::EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& values) {
  api::EngineDocumentInsertRequest request;
  request.context = context;
  request.target_object.uuid.canonical = api::GenerateCrudEngineUuid("row");
  for (const auto& [path, value] : values) {
    request.assignments.push_back({path, Value(value)});
  }
  auto result = api::EngineDocumentInsert(request);
  std::string diagnostics;
  for (const auto& diagnostic : result.diagnostics) {
    if (!diagnostics.empty()) { diagnostics += ";"; }
    diagnostics += diagnostic.detail.empty() ? diagnostic.code : diagnostic.detail;
  }
  Require(result.ok, "document insert failed: " + diagnostics);
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "document_path_provider_persisted=true"),
          "document insert did not persist physical provider artifact");
  return result;
}

api::EngineNoSqlProviderGenerationMetadata CurrentGeneration(
    const api::EngineRequestContext& context) {
  const auto generations = api::ListNoSqlProviderGenerations(context);
  Require(generations.size() == 1, "expected one document generation");
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
  return proof;
}

api::DocumentPathProviderBuildRequest BuildFixtureRequest(
    const std::filesystem::path& artifact_path,
    const api::EngineRequestContext& context) {
  api::DocumentPathProviderBuildRequest request;
  request.artifact_path = artifact_path.string();
  request.identity = api::DocumentPathProviderIdentityForContext(context, 1);
  api::DocumentPathRowEvidence row;
  row.document_uuid = api::GenerateCrudEngineUuid("row");
  row.row_uuid = api::GenerateCrudEngineUuid("row");
  row.version_uuid = api::GenerateCrudEngineUuid("row");
  row.row_ordinal = 1;
  row.values.push_back({"line_items.0.sku", {"string", "SKU-1", false}});
  row.values.push_back({"customer.id", {"string", "C-1", false}});
  request.rows.push_back(std::move(row));
  return request;
}

void RequireOpenDiagnostic(const std::filesystem::path& path,
                           const api::DocumentPathProviderIdentity& identity,
                           const std::string& expected) {
  api::DocumentPathProviderOpenRequest open;
  open.artifact_path = path.string();
  open.expected_identity = identity;
  open.require_expected_identity = true;
  const auto result = api::OpenDocumentPathPhysicalProvider(open);
  Require(!result.ok, "corrupt open unexpectedly succeeded: " + expected);
  Require(ProviderDiagnosticContains(result, expected),
          "wrong diagnostic, expected " + expected + " got " +
              result.diagnostic.detail);
}

void ProviderRuntimeScenario(const std::filesystem::path& db_path) {
  auto writer = Context(db_path, 100);
  SeedTransaction(writer);
  api::EngineDocumentProviderCleanup(writer, true);
  SeedTransaction(writer);

  const auto first_document =
      InsertDocument(writer,
                     {{"customer.id", "C-1"},
                      {"line_items.0.sku", "SKU-1"},
                      {"line_items.1.sku", "SKU-2"},
                      {"status", "open"}});
  const auto second_document =
      InsertDocument(writer,
                     {{"customer.id", "C-2"},
                      {"line_items.0.sku", "SKU-3"},
                      {"status", "closed"}});

  const auto generation = CurrentGeneration(writer);
  Require(generation.provider_id == api::kDocumentPathPhysicalProviderId,
          "wrong provider generation id");

  auto reader = writer;
  reader.local_transaction_id = 150;
  api::EngineDocumentProviderCleanup(reader, false);

  api::EngineDocumentFindRequest exact;
  exact.context = reader;
  exact.path = "customer.id";
  exact.equals_value = "C-1";
  exact.projected_paths = {"customer.id"};
  exact.require_benchmark_clean_index_runtime = true;
  exact.physical_proof = Proof(writer, generation);
  auto exact_result = api::EngineDocumentFind(exact);
  Require(exact_result.ok, "exact document provider probe failed");
  Require(exact_result.result_shape.rows.size() == 1,
          "exact probe returned wrong row count");
  Require(exact_result.dml_summary.visible_rows_scanned == 0,
          "exact probe scanned visible document rows");
  Require(exact_result.dml_summary.index_probes == 1,
          "exact probe did not use one physical index probe");
  Require(exact_result.dml_summary.benchmark_clean,
          "runtime proof was not consumed");
  Require(EvidenceContains(exact_result,
                           "document_path_physical_provider",
                           "document_path_provider_index_consumed=true"),
          "provider consumption evidence missing");
  Require(EvidenceContains(exact_result,
                           "document_path_physical_provider",
                           "descriptor_scan_selected=false"),
          "descriptor scan fallback was not refused");
  Require(EvidenceContains(exact_result,
                           "document_path_physical_provider",
                           "projection_fetch_full_payload=false"),
          "projection fetched full payload unexpectedly");
  Require(!RowHasField(exact_result.result_shape.rows.front(), "payload"),
          "partial materialization leaked full payload");

  api::EngineDocumentFindRequest path_only = exact;
  path_only.equals_value.clear();
  auto path_only_result = api::EngineDocumentFind(path_only);
  Require(path_only_result.ok, "path-only document provider probe failed");
  Require(path_only_result.result_shape.rows.size() == 2,
          "path-only probe returned wrong row count");
  Require(path_only_result.dml_summary.visible_rows_scanned == 0,
          "path-only probe fell back to visible document scan");
  Require(EvidenceContains(path_only_result,
                           "document_path_physical_provider",
                           "document_path_provider_index_consumed=true"),
          "path-only provider consumption evidence missing");

  api::EngineDocumentFindRequest wildcard = exact;
  wildcard.path = "line_items.*.sku";
  wildcard.equals_value = "SKU-2";
  wildcard.wildcard_path = true;
  wildcard.projected_paths.clear();
  auto wildcard_result = api::EngineDocumentFind(wildcard);
  Require(wildcard_result.ok, "wildcard document provider probe failed");
  Require(wildcard_result.result_shape.rows.size() == 1,
          "wildcard probe returned wrong row count");
  Require(EvidenceContains(wildcard_result,
                           "document_path_physical_provider",
                           "array_expansion_entries=2"),
          "array expansion evidence missing");

  api::EngineDocumentFindRequest missing = wildcard;
  missing.equals_value = "missing-sku";
  auto missing_result = api::EngineDocumentFind(missing);
  Require(missing_result.ok, "missing-value probe failed closed");
  Require(missing_result.result_shape.rows.empty(),
          "missing path/value did not return empty candidate set");
  Require(missing_result.dml_summary.visible_rows_scanned == 0,
          "missing path/value fell back to scan");

  api::EngineDocumentUpdateRequest update;
  update.context = writer;
  update.target_object = first_document.primary_object;
  update.assignments.push_back({"customer.id", Value("C-1")});
  update.assignments.push_back({"status", Value("archived")});
  auto update_result = api::EngineDocumentUpdate(update);
  Require(update_result.ok, "document update did not rebuild provider");
  Require(EvidenceContains(update_result,
                           "document_physical_provider",
                           "document_path_provider_persisted=true"),
          "document update did not persist rebuilt provider");
  const auto update_generation = CurrentGeneration(writer);
  api::EngineDocumentFindRequest updated = exact;
  updated.equals_value = "archived";
  updated.path = "status";
  updated.projected_paths = {"status"};
  updated.physical_proof = Proof(writer, update_generation);
  auto updated_result = api::EngineDocumentFind(updated);
  Require(updated_result.ok, "updated document provider probe failed");
  Require(updated_result.result_shape.rows.size() == 1,
          "updated document provider returned wrong row count");

  api::EngineDocumentDeleteRequest delete_request;
  delete_request.context = writer;
  delete_request.target_object = second_document.primary_object;
  auto delete_result = api::EngineDocumentDelete(delete_request);
  Require(delete_result.ok, "document delete did not rebuild provider");
  Require(EvidenceContains(delete_result,
                           "document_physical_provider",
                           "document_path_provider_persisted=true"),
          "document delete did not persist rebuilt provider");
  const auto delete_generation = CurrentGeneration(writer);
  api::EngineDocumentFindRequest deleted = exact;
  deleted.equals_value = "C-2";
  deleted.physical_proof = Proof(writer, delete_generation);
  auto deleted_result = api::EngineDocumentFind(deleted);
  Require(deleted_result.ok, "deleted document provider probe failed");
  Require(deleted_result.result_shape.rows.empty(),
          "deleted document remained in provider index");
}

void DirectProviderScenario(const std::filesystem::path& base_path) {
  auto context = Context(base_path, 200);
  auto artifact_path = base_path;
  artifact_path += ".artifact";
  auto build = BuildFixtureRequest(artifact_path, context);
  auto built = api::BuildDocumentPathPhysicalProvider(build);
  Require(built.ok, "direct provider build failed");
  Require(!built.artifact.identity.index_uuid.empty(),
          "document path provider index UUID missing");
  Require(!built.artifact.identity.segment_uuid.empty(),
          "document path provider segment UUID missing");
  Require(built.artifact.identity.index_uuid != built.artifact.identity.segment_uuid,
          "document path provider reused one UUID for index and segment");
  auto other_context = Context(base_path.string() + "_other", 201);
  const auto other_identity =
      api::DocumentPathProviderIdentityForContext(other_context, 1);
  Require(other_identity.index_uuid != built.artifact.identity.index_uuid,
          "document path provider index UUID did not vary by context");
  Require(other_identity.segment_uuid != built.artifact.identity.segment_uuid,
          "document path provider segment UUID did not vary by context");
  Require(built.artifact.stats.path_count >= 4, "path dictionary too small");
  Require(built.artifact.stats.shape_count == 1, "shape dictionary missing");
  Require(built.artifact.stats.posting_count >= 3, "typed postings missing");
  Require(built.artifact.stats.wildcard_path_count == 1,
          "wildcard path dictionary missing");
  Require(built.artifact.stats.array_expansion_count == 1,
          "array expansion dictionary missing");

  api::DocumentPathProviderProbeRequest probe;
  probe.artifact_path = artifact_path.string();
  probe.expected_identity = build.identity;
  probe.path = "line_items.*.sku";
  probe.wildcard_path = true;
  probe.equals_value = {"string", "SKU-1", false};
  auto probed = api::ProbeDocumentPathPhysicalProvider(probe);
  Require(probed.ok, "direct wildcard probe failed");
  Require(probed.projection_plan.candidates.size() == 1,
          "direct wildcard probe returned wrong candidates");
  Require(probed.projection_plan.fetch_candidate_rows_only,
          "projection plan does not restrict candidate rows");
  Require(probed.projection_plan.fetch_projected_paths_only,
          "projection plan does not restrict projected paths");

  auto rebuild_path = artifact_path;
  rebuild_path += ".rebuild";
  auto rebuild_build = build;
  rebuild_build.artifact_path = rebuild_path.string();
  auto rebuilt_base = api::BuildDocumentPathPhysicalProvider(rebuild_build);
  Require(rebuilt_base.ok, "authoritative rebuild fixture build failed");
  api::DocumentPathProviderMutationRequest mutation;
  mutation.artifact_path = rebuild_path.string();
  mutation.admitted_authoritative_rebuild = true;
  mutation.authoritative_source_rows = rebuild_build.rows;
  mutation.authoritative_source_rows.front().values.erase(
      std::remove_if(mutation.authoritative_source_rows.front().values.begin(),
                     mutation.authoritative_source_rows.front().values.end(),
                     [](const auto& value) {
                       return value.path == "line_items.0.sku";
                     }),
      mutation.authoritative_source_rows.front().values.end());
  auto rebuilt = api::DeleteOrUpdateDocumentPathPhysicalProvider(mutation);
  Require(rebuilt.ok, "authoritative delete/update rebuild failed");
  Require(rebuilt.artifact.identity.provider_generation ==
              rebuilt_base.artifact.identity.provider_generation + 1,
          "authoritative rebuild did not advance provider generation");
  api::DocumentPathProviderProbeRequest removed_probe = probe;
  removed_probe.artifact_path = rebuild_path.string();
  removed_probe.expected_identity = rebuilt.artifact.identity;
  auto removed = api::ProbeDocumentPathPhysicalProvider(removed_probe);
  Require(removed.ok, "post-rebuild removed-value probe failed");
  Require(removed.projection_plan.candidates.empty(),
          "authoritative rebuild retained removed posting");
  api::DocumentPathProviderProbeRequest retained_probe;
  retained_probe.artifact_path = rebuild_path.string();
  retained_probe.expected_identity = rebuilt.artifact.identity;
  retained_probe.path = "customer.id";
  retained_probe.equals_value = {"string", "C-1", false};
  auto retained = api::ProbeDocumentPathPhysicalProvider(retained_probe);
  Require(retained.ok && retained.projection_plan.candidates.size() == 1,
          "authoritative rebuild lost retained posting");

  auto invalid = build;
  invalid.identity.database_uuid = "not-a-uuid";
  Require(!api::BuildDocumentPathPhysicalProvider(invalid).ok,
          "invalid UUID was accepted");
  auto unsafe = build;
  unsafe.rows.front().values.front().path = "line items.0.sku";
  auto unsafe_result = api::BuildDocumentPathPhysicalProvider(unsafe);
  Require(!unsafe_result.ok &&
              ProviderDiagnosticContains(
                  unsafe_result,
                  api::kDocumentPathPhysicalProviderUnsafePathToken),
          "unsafe path token was not rejected");
  auto descriptor_scan = build;
  descriptor_scan.rows.front().descriptor_scan_claim = true;
  auto descriptor_result =
      api::BuildDocumentPathPhysicalProvider(descriptor_scan);
  Require(!descriptor_result.ok &&
              ProviderDiagnosticContains(
                  descriptor_result,
                  api::kDocumentPathPhysicalProviderDescriptorScanRefused),
          "descriptor scan claim was not rejected");
  auto authority = build;
  authority.rows.front().write_ahead_log_finality_authority_claim = true;
  auto authority_result = api::BuildDocumentPathPhysicalProvider(authority);
  Require(!authority_result.ok &&
              ProviderDiagnosticContains(
                  authority_result,
                  api::kDocumentPathPhysicalProviderAuthorityClaimRefused),
          "finality authority claim was not rejected");

  const auto clean = ReadFile(artifact_path);
  auto bad_checksum = artifact_path;
  bad_checksum += ".bad_checksum";
  WriteFile(bad_checksum, clean + "x");
  api::DocumentPathProviderOpenRequest open_bad;
  open_bad.artifact_path = bad_checksum.string();
  auto bad_checksum_result = api::OpenDocumentPathPhysicalProvider(open_bad);
  Require(!bad_checksum_result.ok &&
              ProviderDiagnosticContains(
                  bad_checksum_result,
                  api::kDocumentPathPhysicalProviderTruncatedPayload),
          "truncated appended payload was not classified");

  auto checksum_path = artifact_path;
  checksum_path += ".checksum";
  auto checksum_text = clean;
  checksum_text.replace(checksum_text.find("STATS"), 5, "STATE");
  WriteFile(checksum_path, checksum_text);
  auto checksum_result = api::OpenDocumentPathPhysicalProvider(
      {checksum_path.string(), {}, false, false, {}});
  Require(!checksum_result.ok &&
              ProviderDiagnosticContains(
                  checksum_result,
                  api::kDocumentPathPhysicalProviderBadChecksum),
          "bad checksum was not classified");

  auto stale_format_path = artifact_path;
  stale_format_path += ".format";
  auto stale_format = clean;
  stale_format.replace(0, 8, "SBDOCOLD");
  WriteFile(stale_format_path, stale_format);
  RequireOpenDiagnostic(stale_format_path,
                        build.identity,
                        api::kDocumentPathPhysicalProviderStaleFormat);

  auto stale_identity = build.identity;
  stale_identity.provider_generation += 1;
  RequireOpenDiagnostic(artifact_path,
                        stale_identity,
                        api::kDocumentPathPhysicalProviderStaleGeneration);
  auto mismatch = build.identity;
  mismatch.database_uuid = api::GenerateCrudEngineUuid("database");
  RequireOpenDiagnostic(artifact_path,
                        mismatch,
                        api::kDocumentPathPhysicalProviderIdentityMismatch);

  auto malformed_path = artifact_path;
  malformed_path += ".path";
  WriteFile(malformed_path, MutateFirstTag(clean, "PATH", "path_id", "0"));
  RequireOpenDiagnostic(malformed_path,
                        build.identity,
                        api::kDocumentPathPhysicalProviderMalformedPathDictionary);

  auto malformed_shape = artifact_path;
  malformed_shape += ".shape";
  WriteFile(malformed_shape,
            MutateFirstTag(clean, "SHAPE", "path_ids", "99999"));
  RequireOpenDiagnostic(
      malformed_shape,
      build.identity,
      api::kDocumentPathPhysicalProviderMalformedShapeDictionary);

  auto malformed_posting = artifact_path;
  malformed_posting += ".posting";
  WriteFile(malformed_posting,
            MutateFirstTag(clean, "POST", "row_uuid", "not-a-uuid"));
  RequireOpenDiagnostic(malformed_posting,
                        build.identity,
                        api::kDocumentPathPhysicalProviderMalformedPostings);

  auto malformed_stats = artifact_path;
  malformed_stats += ".stats";
  WriteFile(malformed_stats,
            MutateFirstTag(clean, "STATS", "posting_count", "999"));
  RequireOpenDiagnostic(malformed_stats,
                        build.identity,
                        api::kDocumentPathPhysicalProviderMalformedPostings);

  auto unsafe_path = artifact_path;
  unsafe_path += ".unsafe";
  WriteFile(unsafe_path,
            MutateFirstTag(clean, "PATH", "normalized_path", "line items"));
  RequireOpenDiagnostic(unsafe_path,
                        build.identity,
                        api::kDocumentPathPhysicalProviderUnsafePathToken);

  auto truncated = artifact_path;
  truncated += ".truncated";
  auto truncated_text = clean.substr(0, clean.size() - 4);
  WriteFile(truncated, truncated_text);
  RequireOpenDiagnostic(truncated,
                        build.identity,
                        api::kDocumentPathPhysicalProviderTruncatedPayload);

  api::DocumentPathProviderOpenRequest repair;
  repair.artifact_path = checksum_path.string();
  repair.expected_identity = build.identity;
  repair.require_expected_identity = true;
  repair.repair_admitted = true;
  auto repair_result = api::OpenDocumentPathPhysicalProvider(repair);
  Require(!repair_result.ok &&
              ProviderDiagnosticContains(
                  repair_result,
                  api::kDocumentPathPhysicalProviderRepairSourceRequired),
          "repair without authoritative source rows was admitted");
}

}  // namespace

int main() {
  try {
    const auto suffix = api::GenerateCrudEngineUuid("database");
    const auto base =
        std::filesystem::temp_directory_path() /
        ("scratchbird_document_path_provider_" + suffix);
    ProviderRuntimeScenario(base);
    DirectProviderScenario(base.string() + "_direct");
  } catch (const std::exception& ex) {
    std::cerr << "document_path_physical_provider_gate failed: " << ex.what()
              << '\n';
    return 1;
  }
  return 0;
}
