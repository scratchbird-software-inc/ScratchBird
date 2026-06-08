// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/key_value_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : evidence) {
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

api::EngineRequestContext Context(const std::string& database_path,
                                  api::EngineApiU64 tx) {
  api::EngineRequestContext context;
  context.database_path = database_path;
  context.local_transaction_id = tx;
  context.database_uuid.canonical = "019df071-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df071-0000-7000-8000-000000000077";
  return context;
}

void SeedCrudTransaction(const std::string& database_path) {
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t77\t019df071-0000-7000-8000-000000000077\n";
  crud << "SBCRUD1\tTX_BEGIN\t90\t019df071-0000-7000-8000-000000000090\n";
}

api::EngineKeyValuePhysicalProof ExactProof() {
  api::EngineKeyValuePhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_key_index_proof = true;
  proof.ttl_visibility_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kKeyValue;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf071.local.kv.provider";
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation = 7;
  proof.provider_contract.index_generation.available_generation = 7;
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

api::EngineKeyValuePhysicalProof PrefixProof() {
  auto proof = ExactProof();
  proof.prefix_index_proof = true;
  return proof;
}

api::EngineKeyValuePutResult Put(const std::string& database_path,
                                 const std::string& key,
                                 const std::string& value,
                                 api::EngineApiU64 expires_after_tx = 0) {
  api::EngineKeyValuePutRequest request;
  request.context = Context(database_path, 77);
  request.key = key;
  request.value = value;
  request.target_object.uuid.canonical = "object-" + key;
  request.localized_names.push_back({"en", "primary", "", key, true});
  request.expires_after_local_transaction_id = expires_after_tx;
  const auto result = api::EngineKeyValuePut(request);
  Require(result.ok, "ODF-071 key/value put failed");
  Require(EvidenceContains(result.evidence, "kv_physical_access",
                           "write_through_exact_prefix_provider"),
          "ODF-071 put did not update the physical KV provider");
  Require(EvidenceContains(result.evidence, "mga_finality_authority",
                           "engine_transaction_inventory"),
          "ODF-071 put did not preserve MGA finality authority evidence");
  return result;
}

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts",
          "behavior_store_scan_selected=true", "parser_executes_sql=true",
          "wal_recovery_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-071 evidence leaked forbidden authority or document token");
    }
  }
}

void ExactPrefixTtlAndMultiGetUsePhysicalProofs() {
  const std::string database_path = "/tmp/sb_odf_071_gate_api.sbdb";
  SeedCrudTransaction(database_path);
  Put(database_path, "acct:1", "alpha");
  Put(database_path, "acct:2", "beta");
  Put(database_path, "acct:expired", "gone", 80);

  api::EngineKeyValueGetRequest exact;
  exact.context = Context(database_path, 77);
  exact.key = "acct:1";
  exact.physical_proof = ExactProof();
  auto exact_result = api::EngineKeyValueGet(exact);
  Require(exact_result.ok, "ODF-071 exact get failed");
  Require(exact_result.result_shape.rows.size() == 1,
          "ODF-071 exact get did not return one row");
  Require(RowField(exact_result, 0, "value") == "alpha",
          "ODF-071 exact get returned the wrong value");
  Require(EvidenceContains(exact_result.evidence, "kv_physical_access",
                           "exact_key_index_probe"),
          "ODF-071 exact get did not use exact-key physical access");
  Require(exact_result.dml_summary.visible_rows_scanned == 0,
          "ODF-071 exact get reported behavior-store row scans");

  api::EngineKeyValueGetRequest prefix;
  prefix.context = Context(database_path, 77);
  prefix.prefix = "acct:";
  prefix.physical_proof = PrefixProof();
  const auto prefix_result = api::EngineKeyValueGet(prefix);
  Require(prefix_result.ok, "ODF-071 prefix get failed");
  Require(prefix_result.result_shape.rows.size() == 3,
          "ODF-071 prefix get did not use prefix provider coverage");
  Require(EvidenceContains(prefix_result.evidence, "kv_physical_access",
                           "prefix_index_probe"),
          "ODF-071 prefix get lacked prefix physical access evidence");

  exact.context.local_transaction_id = 90;
  exact.key = "acct:expired";
  exact_result = api::EngineKeyValueGet(exact);
  Require(exact_result.ok, "ODF-071 TTL exact get failed");
  Require(exact_result.result_shape.rows.empty(),
          "ODF-071 expired TTL row was returned by a physical KV query");
  Require(EvidenceContains(exact_result.evidence, "ttl_visibility_evidence",
                           "deterministic_local_transaction_id"),
          "ODF-071 TTL exclusion lacked deterministic visibility evidence");

  api::EngineKeyValueMultiGetRequest multiget;
  multiget.context = Context(database_path, 90);
  multiget.keys = {"acct:1", "acct:2", "acct:expired", "missing"};
  multiget.physical_proof = ExactProof();
  const auto multi_result = api::EngineKeyValueMultiGet(multiget);
  Require(multi_result.ok, "ODF-071 MultiGet failed");
  Require(multi_result.result_shape.rows.size() == 2,
          "ODF-071 MultiGet did not exclude missing and TTL-expired rows");
  Require(EvidenceContains(multi_result.evidence, "kv_physical_access",
                           "multiget_exact_key_index_probe"),
          "ODF-071 MultiGet lacked exact-key batch evidence");
  Require(EvidenceContains(multi_result.evidence, "kv_multiget_keys", "4"),
          "ODF-071 MultiGet lacked batch counter evidence");

  RequireEvidenceHygiene(exact_result);
  RequireEvidenceHygiene(prefix_result);
  RequireEvidenceHygiene(multi_result);
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

void PipelineAdmissionAndAtomicProgramsAreDeterministic() {
  const std::string database_path = "/tmp/sb_odf_071_pipeline.sbdb";
  SeedCrudTransaction(database_path);

  api::EngineKeyValuePipelineRequest pipeline;
  pipeline.context = Context(database_path, 77);
  pipeline.max_admitted_operations = 3;
  pipeline.physical_proof = ExactProof();
  pipeline.puts.push_back({"pipe:1", "one", 0});
  pipeline.puts.push_back({"pipe:2", "two", 0});
  pipeline.get_keys.push_back("pipe:1");
  auto pipeline_result = api::EngineKeyValuePipeline(pipeline);
  Require(pipeline_result.ok, "ODF-071 admitted pipeline failed");
  Require(pipeline_result.dml_summary.rows_changed == 2,
          "ODF-071 pipeline did not report batch write counters");
  Require(pipeline_result.result_shape.rows.size() == 1,
          "ODF-071 pipeline did not execute admitted exact get");
  Require(EvidenceContains(pipeline_result.evidence,
                           "kv_pipeline_admitted_operations", "3"),
          "ODF-071 pipeline lacked admission evidence");

  pipeline.get_keys.push_back("pipe:2");
  pipeline_result = api::EngineKeyValuePipeline(pipeline);
  Require(!pipeline_result.ok,
          "ODF-071 pipeline exceeded admission but did not fail closed");
  Require(DiagnosticContains(pipeline_result,
                             api::kKeyValuePipelineAdmissionRefused),
          "ODF-071 pipeline refusal diagnostic changed");

  api::EngineKeyValueAtomicProgramRequest atomic;
  atomic.context = Context(database_path, 77);
  atomic.physical_proof = ExactProof();
  atomic.steps.push_back({"set", "counter", "10"});
  atomic.steps.push_back({"increment_i64", "counter", "7"});
  const auto atomic_result = api::EngineKeyValueAtomicProgram(atomic);
  Require(atomic_result.ok, "ODF-071 deterministic atomic program failed");
  Require(RowField(atomic_result, 1, "value") == "17",
          "ODF-071 atomic read-compute-write result was not deterministic");
  Require(EvidenceContains(atomic_result.evidence, "kv_atomic_program",
                           "deterministic_sblr_read_compute_write"),
          "ODF-071 atomic program lacked SBLR-style evidence");
  Require(EvidenceContains(atomic_result.evidence,
                           "parser_transaction_finality_authority", "false"),
          "ODF-071 atomic program exposed parser/client finality authority");
  RequireEvidenceHygiene(atomic_result);

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

void MissingProofsBehaviorScansAndClusterOptionsFailClosed() {
  const std::string database_path = "/tmp/sb_odf_071_fail_closed.sbdb";
  SeedCrudTransaction(database_path);
  Put(database_path, "closed:1", "value");

  api::EngineKeyValueGetRequest missing_exact;
  missing_exact.context = Context(database_path, 77);
  missing_exact.key = "closed:1";
  missing_exact.physical_proof = ExactProof();
  missing_exact.physical_proof.exact_key_index_proof = false;
  auto result = api::EngineKeyValueGet(missing_exact);
  Require(!result.ok, "ODF-071 missing exact proof did not fail closed");
  Require(DiagnosticContains(result, api::kKeyValueExactKeyProofMissing),
          "ODF-071 missing exact proof diagnostic changed");

  api::EngineKeyValueGetRequest missing_prefix;
  missing_prefix.context = Context(database_path, 77);
  missing_prefix.prefix = "closed:";
  missing_prefix.physical_proof = ExactProof();
  result = api::EngineKeyValueGet(missing_prefix);
  Require(!result.ok, "ODF-071 missing prefix proof did not fail closed");
  Require(DiagnosticContains(result, api::kKeyValuePrefixProofMissing),
          "ODF-071 missing prefix proof diagnostic changed");

  auto behavior_scan_proof = ExactProof();
  behavior_scan_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  api::EngineKeyValueMultiGetRequest scan_request;
  scan_request.context = Context(database_path, 77);
  scan_request.keys = {"closed:1"};
  scan_request.physical_proof = behavior_scan_proof;
  const auto scan_result = api::EngineKeyValueMultiGet(scan_request);
  Require(!scan_result.ok,
          "ODF-071 behavior-store scan was accepted as physical KV access");
  Require(DiagnosticContains(scan_result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-071 behavior-store scan refusal diagnostic changed");

  api::EngineKeyValueGetRequest cluster;
  cluster.context = Context(database_path, 77);
  cluster.key = "closed:1";
  cluster.option_envelopes.push_back("cluster.route=required");
  cluster.physical_proof = ExactProof();
  result = api::EngineKeyValueGet(cluster);
  Require(!result.ok && result.cluster_authority_required,
          "ODF-071 cluster option did not retain fail-closed authority behavior");

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

void SblrSurfaceRegistersAndDispatchesKvBatchOperations() {
  const struct {
    const char* operation_id;
    const char* opcode;
  } routes[] = {
      {"nosql.key_value_multiget", "SBLR_NOSQL_KEY_VALUE_MULTIGET"},
      {"nosql.key_value_pipeline", "SBLR_NOSQL_KEY_VALUE_PIPELINE"},
      {"nosql.key_value_atomic_program", "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM"},
  };

  for (const auto& route : routes) {
    const auto* by_operation = sblr::LookupSblrOperation(route.operation_id);
    Require(by_operation != nullptr, "ODF-071 SBLR operation was not registered");
    Require(by_operation->opcode == route.opcode,
            "ODF-071 SBLR operation opcode registration changed");
    Require(by_operation->requires_transaction_context,
            "ODF-071 SBLR KV batch operation did not require transaction context");

    const auto* by_opcode = sblr::LookupSblrOpcode(route.opcode);
    Require(by_opcode != nullptr && by_opcode->operation_id == route.operation_id,
            "ODF-071 SBLR opcode reverse lookup was not registered");

    sblr::SblrDispatchRequest request;
    request.context = Context("/tmp/sb_odf_071_sblr.sbdb", 77);
    request.context.security_context_present = true;
    request.envelope = sblr::MakeSblrEnvelope(route.operation_id, route.opcode);
    request.envelope.requires_transaction_context = true;
    request.envelope.requires_security_context = true;
    const auto dispatched = sblr::DispatchSblrOperation(request);
    Require(dispatched.envelope_validated && dispatched.accepted &&
                dispatched.dispatched_to_api,
            "ODF-071 SBLR KV batch route did not dispatch");
    Require(dispatched.api_result.operation_id == route.operation_id,
            "ODF-071 SBLR KV batch route returned the wrong API operation");
  }
}

}  // namespace

int main() {
  ExactPrefixTtlAndMultiGetUsePhysicalProofs();
  PipelineAdmissionAndAtomicProgramsAreDeterministic();
  MissingProofsBehaviorScansAndClusterOptionsFailClosed();
  SblrSurfaceRegistersAndDispatchesKvBatchOperations();
  return EXIT_SUCCESS;
}
