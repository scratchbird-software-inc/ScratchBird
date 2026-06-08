// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/document_api.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

api::EngineRequestContext Context(const std::string& database_path,
                                  api::EngineApiU64 tx) {
  api::EngineRequestContext context;
  context.database_path = database_path;
  context.local_transaction_id = tx;
  context.database_uuid.canonical = "019df072-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df072-0000-7000-8000-000000000077";
  context.security_context_present = true;
  return context;
}

void SeedCrudTransaction(const std::string& database_path) {
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t77\t019df072-0000-7000-8000-000000000077\n";
  crud << "SBCRUD1\tTX_BEGIN\t90\t019df072-0000-7000-8000-000000000090\n";
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

api::EngineDocumentPhysicalProof DocumentProof() {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kDocument;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf072.local.document.path.provider";
  proof.provider_contract.fallback_provider_id = "odf072.local.document.shape.dictionary";
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
  proof.provider_contract.index_generation.required_generation = 11;
  proof.provider_contract.index_generation.available_generation = 11;
  proof.provider_contract.index_generation.index_uuid = "odf072-document-path-index";
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
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

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool RowHasField(const api::EngineApiResult& result,
                 std::size_t row_index,
                 std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) { return false; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return true; }
  }
  return false;
}

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts",
          "behavior_store_scan_selected=true", "descriptor_scan_selected=true",
          "parser_executes_sql=true", "wal_recovery_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-072 evidence leaked forbidden authority or document token");
    }
  }
}

void InsertDocument(const std::string& database_path,
                    const std::string& uuid,
                    const std::string& name,
                    const std::vector<std::pair<std::string, std::string>>& fragments) {
  api::EngineDocumentInsertRequest insert;
  insert.context = Context(database_path, 77);
  insert.target_object.uuid.canonical = uuid;
  insert.localized_names.push_back({"en", "primary", "", name, true});
  for (const auto& [path, value] : fragments) {
    AddFragment(&insert, path, value);
  }
  const auto result = api::EngineDocumentInsert(insert);
  Require(result.ok, "ODF-072 document insert failed");
  Require(EvidenceContains(result, "document_physical_provider",
                           "write_through_path_provider"),
          "ODF-072 document insert did not populate the physical provider");
}

void ExactWildcardProjectionAndShapeEvidence() {
  const std::string database_path = "/tmp/sb_odf_072_gate_api.sbdb";
  SeedCrudTransaction(database_path);
  InsertDocument(database_path,
                 "doc-customer-a",
                 "customer-a",
                 {{"customer.id", "A1"},
                  {"customer.tier", "gold"},
                  {"line_items.0.sku", "SKU-1"},
                  {"line_items.1.sku", "SKU-2"},
                  {"private.ssn", "redacted"}});
  InsertDocument(database_path,
                 "doc-customer-b",
                 "customer-b",
                 {{"customer.id", "B1"},
                  {"customer.tier", "silver"},
                  {"line_items.0.sku", "SKU-3"},
                  {"line_items.1.sku", "SKU-4"},
                  {"private.ssn", "redacted"}});

  api::EngineDocumentFindRequest exact;
  exact.context = Context(database_path, 90);
  exact.path = "customer.id";
  exact.equals_value = "A1";
  exact.projected_paths = {"customer.id", "customer.tier"};
  exact.physical_proof = DocumentProof();
  auto result = api::EngineDocumentFind(exact);
  Require(result.ok, "ODF-072 exact document path lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "ODF-072 exact path lookup returned the wrong row count");
  Require(RowField(result, 0, "path:customer.id") == "A1",
          "ODF-072 exact path lookup returned the wrong path fragment");
  Require(RowField(result, 0, "path:customer.tier") == "gold",
          "ODF-072 partial materialization missed a projected path");
  Require(!RowHasField(result, 0, "payload"),
          "ODF-072 partial materialization returned a full payload");
  Require(!RowHasField(result, 0, "path:private.ssn"),
          "ODF-072 partial materialization returned an unprojected path");
  Require(EvidenceContains(result, "document_physical_access",
                           "exact_path_index_probe"),
          "ODF-072 exact lookup did not use a targeted path index");
  Require(EvidenceContains(result, "document_partial_materialization",
                           "projected_paths_only"),
          "ODF-072 exact lookup lacked partial materialization evidence");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-072 exact lookup lacked row MGA recheck evidence");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-072 exact lookup lacked row security recheck evidence");
  RequireEvidenceHygiene(result);

  api::EngineDocumentFindRequest wildcard;
  wildcard.context = Context(database_path, 90);
  wildcard.path = "line_items.*.sku";
  wildcard.equals_value = "SKU-2";
  wildcard.wildcard_path = true;
  wildcard.projected_paths = {"customer.id"};
  wildcard.physical_proof = DocumentProof();
  result = api::EngineDocumentFind(wildcard);
  Require(result.ok, "ODF-072 wildcard document path lookup failed");
  Require(result.result_shape.rows.size() == 1,
          "ODF-072 wildcard path lookup returned the wrong row count");
  Require(RowField(result, 0, "path:customer.id") == "A1",
          "ODF-072 wildcard path lookup did not return projected fragments");
  Require(!RowHasField(result, 0, "payload"),
          "ODF-072 wildcard lookup returned a full payload");
  Require(EvidenceContains(result, "document_physical_access",
                           "wildcard_shape_index_probe"),
          "ODF-072 wildcard lookup lacked wildcard-capable index proof");
  Require(EvidenceContains(result, "document_shape_fallback",
                           "shape_dictionary_proved"),
          "ODF-072 wildcard lookup lacked shape fallback proof");
  Require(EvidenceContains(result, "document_shape_dictionary",
                           "document_shape_"),
          "ODF-072 shape dictionary evidence was missing");
  Require(EvidenceContains(result, "document_structural_sharing",
                           "shape_ref_count=2"),
          "ODF-072 structural sharing evidence was missing");
  RequireEvidenceHygiene(result);

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

void FailClosedCasesPreserveAuthority() {
  const std::string database_path = "/tmp/sb_odf_072_fail_closed.sbdb";
  SeedCrudTransaction(database_path);
  InsertDocument(database_path,
                 "doc-closed",
                 "closed",
                 {{"customer.id", "C1"}, {"line_items.0.sku", "SKU-C"}});

  api::EngineDocumentFindRequest request;
  request.context = Context(database_path, 90);
  request.path = "customer.id";
  auto result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 exact path lookup without proof did not fail closed");
  Require(DiagnosticContains(result, api::kDocumentExactPathProofMissing),
          "ODF-072 exact proof refusal diagnostic changed");

  request = api::EngineDocumentFindRequest{};
  request.context = Context(database_path, 90);
  request.wildcard_path = true;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 wildcard intent without path/proof fell back to descriptor scan");
  Require(DiagnosticContains(result, api::kDocumentWildcardShapeProofMissing),
          "ODF-072 wildcard intent refusal diagnostic changed");

  request = api::EngineDocumentFindRequest{};
  request.context = Context(database_path, 90);
  request.projected_paths = {"customer.id"};
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 projection-only materialization without proof fell back to descriptor scan");
  Require(DiagnosticContains(result, api::kDocumentExactPathProofMissing),
          "ODF-072 projection-only proof refusal diagnostic changed");

  request = api::EngineDocumentFindRequest{};
  request.context = Context(database_path, 90);
  request.path = "line_items.*.sku";
  request.wildcard_path = true;
  request.physical_proof = DocumentProof();
  request.physical_proof.wildcard_shape_index_proof = false;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 wildcard fallback without shape proof did not fail closed");
  Require(DiagnosticContains(result, api::kDocumentWildcardShapeProofMissing),
          "ODF-072 wildcard shape refusal diagnostic changed");

  request = api::EngineDocumentFindRequest{};
  request.context = Context(database_path, 90);
  request.path = "customer.id";
  request.projected_paths = {"customer.id"};
  request.physical_proof = DocumentProof();
  request.physical_proof.partial_materialization_proof = false;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 missing partial materialization proof did not fail closed");
  Require(DiagnosticContains(result,
                             api::kDocumentPartialMaterializationProofMissing),
          "ODF-072 partial materialization diagnostic changed");

  request.physical_proof = DocumentProof();
  request.physical_proof.provider_contract.descriptor_visibility
      .descriptor_scan_selected = true;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 descriptor scan was accepted as document path access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderDescriptorScanNotPhysicalProvider),
          "ODF-072 descriptor scan refusal diagnostic changed");

  request.physical_proof = DocumentProof();
  request.physical_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 behavior-store scan was accepted as document path access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-072 behavior-store scan refusal diagnostic changed");

  request.physical_proof = DocumentProof();
  request.physical_proof.provider_contract.mga_recheck
      .parser_claims_transaction_finality_authority = true;
  result = api::EngineDocumentFind(request);
  Require(!result.ok,
          "ODF-072 parser finality authority claim did not fail closed");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderParserFinalityAuthorityRefused),
          "ODF-072 parser authority refusal diagnostic changed");

  request.physical_proof = DocumentProof();
  request.option_envelopes.push_back("cluster.route=required");
  result = api::EngineDocumentFind(request);
  Require(!result.ok && result.cluster_authority_required,
          "ODF-072 cluster option did not retain fail-closed authority behavior");

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

}  // namespace

int main() {
  ExactWildcardProjectionAndShapeEvidence();
  FailClosedCasesPreserveAuthority();
  return EXIT_SUCCESS;
}
