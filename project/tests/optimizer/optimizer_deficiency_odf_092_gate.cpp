// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "batch_point_lookup.hpp"
#include "batch_point_lookup_executor.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "uuid.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-092 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x92;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-092 typed UUIDv7 creation failed");
  return typed.value;
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

idx::BatchPointLookupProviderRow ProviderRow(
    std::string key,
    platform::byte suffix,
    std::string payload,
    bool exact = true,
    bool visible = true,
    bool authorized = true) {
  idx::BatchPointLookupProviderRow row;
  row.encoded_key = std::move(key);
  row.candidate.row_uuid =
      V7(platform::UuidKind::row, 1710000092000ull, suffix);
  row.candidate.score = static_cast<double>(suffix);
  row.candidate.exact_predicate_match = exact;
  row.candidate.mga_visible = visible;
  row.candidate.security_authorized = authorized;
  row.candidate.exact_payload_available = true;
  row.candidate.source = "odf092";
  row.payload = std::move(payload);
  row.attributes.push_back(
      {"payload_family", "ordered_batch_point_lookup"});
  return row;
}

idx::BatchPointLookupPlan Plan(idx::BatchPointLookupPurpose purpose,
                               std::vector<std::string> keys) {
  idx::BatchPointLookupPlan plan;
  plan.purpose = purpose;
  plan.plan_id = "odf092";
  plan.keys.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    plan.keys.push_back({keys[i], static_cast<platform::u64>(i)});
  }
  return plan;
}

using ProviderRows =
    std::map<std::string, std::vector<idx::BatchPointLookupProviderRow>>;

idx::BatchPointLookupProvider Provider(ProviderRows rows_by_key) {
  return [rows_by_key = std::move(rows_by_key)](
             const idx::BatchPointLookupProviderRequest& request) {
    idx::BatchPointLookupProviderResult result;
    result.status = {platform::StatusCode::ok, platform::Severity::info,
                     platform::Subsystem::engine};
    result.evidence.push_back(
        "batch_point_lookup.provider=odf092_memory_exact_index");
    result.evidence.push_back(
        "batch_point_lookup.provider.transaction_finality_authority=false");
    for (const auto& key : request.ordered_unique_keys) {
      const auto found = rows_by_key.find(key.encoded_key);
      if (found == rows_by_key.end()) {
        continue;
      }
      for (auto row : found->second) {
        result.rows.push_back(std::move(row));
      }
    }
    return result;
  };
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool ApiEvidenceHas(const api::EngineApiResult& result,
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

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
  }
  return {};
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-092 evidence leaked forbidden document or authority token");
    }
  }
}

void RequireApiEvidenceHygiene(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "behavior_store_scan_selected=true", "parser_executes_sql=true",
          "wal_recovery_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-092 API evidence leaked forbidden document or authority token");
    }
  }
}

void StableOrderingDuplicatesAndMisses() {
  ProviderRows rows;
  rows["b"].push_back(ProviderRow("b", 0x02, "bravo"));
  rows["a"].push_back(ProviderRow("a", 0x01, "alpha"));
  rows["c"].push_back(ProviderRow("c", 0x03, "charlie"));

  const auto result = idx::RunBatchPointLookup(
      Plan(idx::BatchPointLookupPurpose::key_value,
           {"b", "a", "b", "missing", "c"}),
      Authority(),
      Provider(std::move(rows)));
  Require(result.ok(), "ODF-092 ordered batch lookup failed");
  Require(result.rows.size() == 4,
          "ODF-092 duplicate preserving output row count changed");
  Require(result.misses.size() == 1,
          "ODF-092 per-key miss diagnostic count changed");
  Require(result.rows[0].encoded_key == "b" && result.rows[0].input_ordinal == 0,
          "ODF-092 first row did not preserve input order");
  Require(result.rows[1].encoded_key == "a" && result.rows[1].input_ordinal == 1,
          "ODF-092 second row did not preserve input order");
  Require(result.rows[2].encoded_key == "b" && result.rows[2].input_ordinal == 2 &&
              result.rows[2].duplicate_key && result.rows[2].duplicate_ordinal == 1,
          "ODF-092 duplicate key occurrence was not preserved");
  Require(result.misses[0].input_ordinal == 3 &&
              result.misses[0].reason == "key_not_found",
          "ODF-092 miss diagnostic lost input ordinal or reason");
  Require(result.duplicate_key_occurrences == 1,
          "ODF-092 duplicate-key counter changed");
  Require(EvidenceHas(result.evidence,
                      "batch_point_lookup.duplicate_key_occurrences=1"),
          "ODF-092 duplicate-key evidence missing");
  Require(EvidenceHas(result.evidence,
                      "batch_point_lookup.output_order=input_ordinal_then_row_uuid"),
          "ODF-092 stable output ordering evidence missing");
  Require(result.final_rows_authorized,
          "ODF-092 final row authorization evidence was not retained");
  RequireEvidenceHygiene(result.evidence);
}

void NonUniqueForeignKeyRowsStayRowUuidOrdered() {
  ProviderRows rows;
  rows["fk"].push_back(ProviderRow("fk", 0x20, "right"));
  rows["fk"].push_back(ProviderRow("fk", 0x10, "left"));

  const auto result = idx::RunBatchPointLookup(
      Plan(idx::BatchPointLookupPurpose::foreign_key_check, {"fk"}),
      Authority(),
      Provider(std::move(rows)));
  Require(result.ok(), "ODF-092 FK batch lookup failed");
  Require(result.rows.size() == 2,
          "ODF-092 FK point lookup did not retain nonunique exact rows");
  Require(SameUuid(result.rows[0].row_uuid,
                   V7(platform::UuidKind::row, 1710000092000ull, 0x10)),
          "ODF-092 FK rows were not sorted by exact row UUID");
  Require(SameUuid(result.rows[1].row_uuid,
                   V7(platform::UuidKind::row, 1710000092000ull, 0x20)),
          "ODF-092 FK rows lost stable row UUID ordering");
  Require(EvidenceHas(result.evidence,
                      "batch_point_lookup.purpose=foreign_key_check"),
          "ODF-092 FK purpose evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void AllPurposeProfilesUseSamePrimitive() {
  const std::vector<idx::BatchPointLookupPurpose> purposes = {
      idx::BatchPointLookupPurpose::key_value,
      idx::BatchPointLookupPurpose::document_payload,
      idx::BatchPointLookupPurpose::vector_rerank_payload,
      idx::BatchPointLookupPurpose::graph_frontier,
      idx::BatchPointLookupPurpose::search_payload,
      idx::BatchPointLookupPurpose::foreign_key_check,
      idx::BatchPointLookupPurpose::time_series_bucket};
  for (const auto purpose : purposes) {
    ProviderRows rows;
    rows["k"].push_back(ProviderRow("k", 0x30, "payload"));
    const auto result =
        idx::RunBatchPointLookup(Plan(purpose, {"k"}), Authority(),
                                 Provider(std::move(rows)));
    Require(result.ok(), "ODF-092 purpose profile failed");
    Require(result.rows.size() == 1,
            "ODF-092 purpose profile did not return exact row");
    Require(EvidenceHas(result.evidence,
                        std::string("batch_point_lookup.purpose=") +
                            idx::BatchPointLookupPurposeName(purpose)),
            "ODF-092 purpose profile evidence missing");
    RequireEvidenceHygiene(result.evidence);
  }
}

void ExecutorWrapperRequiresExactMsaSecurityRecheck() {
  ProviderRows rows;
  rows["rerank"].push_back(ProviderRow("rerank", 0x40, "vector-payload"));
  const auto result = exec::ExecuteBatchPointLookupForExecutor(
      Plan(idx::BatchPointLookupPurpose::vector_rerank_payload, {"rerank"}),
      Authority(),
      Provider(std::move(rows)));
  Require(result.ok(), "ODF-092 executor batch lookup failed");
  Require(result.rows.size() == 1,
          "ODF-092 executor did not return the authorized row");
  Require(EvidenceHas(result.evidence,
                      "executor.batch_point_lookup.requires_mga_recheck=true"),
          "ODF-092 executor MGA recheck evidence missing");

  auto unsafe_authority = Authority();
  unsafe_authority.security_context_bound = false;
  rows["rerank"].push_back(ProviderRow("rerank", 0x40, "vector-payload"));
  const auto refused = exec::ExecuteBatchPointLookupForExecutor(
      Plan(idx::BatchPointLookupPurpose::vector_rerank_payload, {"rerank"}),
      unsafe_authority,
      Provider(std::move(rows)));
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 executor accepted missing security recheck");
  Require(refused.lookup.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.SECURITY_RECHECK_REQUIRED",
          "ODF-092 executor security diagnostic changed");
  RequireEvidenceHygiene(refused.evidence);
}

void CorruptPlansAndProvidersFailClosed() {
  auto plan = Plan(idx::BatchPointLookupPurpose::search_payload, {"k"});
  plan.exact_key_recheck_required = false;
  auto refused = idx::RunBatchPointLookup(plan, Authority(), Provider({}));
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 accepted a plan without exact recheck");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.RECHECK_CONTRACT_REQUIRED",
          "ODF-092 recheck contract diagnostic changed");

  auto authority = Authority();
  authority.provider_finality_or_visibility_authority = true;
  refused = idx::RunBatchPointLookup(
      Plan(idx::BatchPointLookupPurpose::search_payload, {"k"}),
      authority,
      Provider({}));
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 accepted provider finality authority");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.UNSAFE_AUTHORITY",
          "ODF-092 unsafe authority diagnostic changed");

  plan = Plan(idx::BatchPointLookupPurpose::graph_frontier, {"k"});
  plan.cluster_route_requested = true;
  plan.cluster_guard_checked = false;
  refused = idx::RunBatchPointLookup(plan, Authority(), Provider({}));
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 accepted an unguarded cluster route");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.CLUSTER_GUARD_REQUIRED",
          "ODF-092 cluster guard diagnostic changed");

  refused = idx::RunBatchPointLookup(
      Plan(idx::BatchPointLookupPurpose::search_payload, {"k"}),
      Authority(),
      [](const idx::BatchPointLookupProviderRequest&) {
        idx::BatchPointLookupProviderResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::engine};
        auto row = ProviderRow("k", 0x50, "payload");
        row.exact_key_match = false;
        result.rows.push_back(std::move(row));
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 accepted an untrusted provider row");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_UNTRUSTED",
          "ODF-092 untrusted provider diagnostic changed");

  refused = idx::RunBatchPointLookup(
      Plan(idx::BatchPointLookupPurpose::search_payload, {"k"}),
      Authority(),
      [](const idx::BatchPointLookupProviderRequest&) {
        idx::BatchPointLookupProviderResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::engine};
        result.rows.push_back(ProviderRow("outside", 0x51, "payload"));
        return result;
      });
  Require(!refused.ok() && refused.fail_closed,
          "ODF-092 accepted an unrequested provider key");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_OUT_OF_PLAN",
          "ODF-092 unrequested provider key diagnostic changed");
}

api::EngineRequestContext Context(const std::string& database_path,
                                  api::EngineApiU64 tx) {
  api::EngineRequestContext context;
  context.database_path = database_path;
  context.local_transaction_id = tx;
  context.database_uuid.canonical = "019df092-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical =
      "019df092-0000-7000-8000-000000000077";
  return context;
}

void SeedCrudTransaction(const std::string& database_path) {
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t77\t019df092-0000-7000-8000-000000000077\n";
  crud << "SBCRUD1\tTX_BEGIN\t90\t019df092-0000-7000-8000-000000000090\n";
}

api::EngineKeyValuePhysicalProof ExactProof() {
  api::EngineKeyValuePhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_key_index_proof = true;
  proof.ttl_visibility_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kKeyValue;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf092.local.kv.provider";
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
  proof.provider_contract.index_generation.required_generation = 92;
  proof.provider_contract.index_generation.available_generation = 92;
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

api::EngineNoSqlPhysicalProviderContract ProviderContract(
    api::EngineNoSqlProviderFamily family,
    std::string provider_id) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = family;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = std::move(provider_id);
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
  contract.index_generation.required_generation = 92;
  contract.index_generation.available_generation = 92;
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

api::EngineDocumentPhysicalProof DocumentProof() {
  api::EngineDocumentPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  proof.provider_contract = ProviderContract(
      api::EngineNoSqlProviderFamily::kDocument,
      "odf092.local.document.provider");
  return proof;
}

api::EngineVectorPhysicalProof VectorProof() {
  api::EngineVectorPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_vector_proof = true;
  proof.hnsw_proof = true;
  proof.ivf_proof = true;
  proof.pq_proof = true;
  proof.diskann_like_proof = true;
  proof.generation_visibility_proof = true;
  proof.filtered_planner_proof = true;
  proof.pre_filter_proof = true;
  proof.post_filter_proof = true;
  proof.iterative_filter_proof = true;
  proof.hybrid_dense_sparse_proof = true;
  proof.exact_rerank_proof = true;
  proof.provider_contract = ProviderContract(
      api::EngineNoSqlProviderFamily::kVector,
      "odf092.local.vector.provider");
  return proof;
}

api::EngineSearchPhysicalProof SearchProof() {
  api::EngineSearchPhysicalProof proof;
  proof.proof_supplied = true;
  proof.mutable_buffer_proof = true;
  proof.sealed_inverted_segment_proof = true;
  proof.bm25_statistics_proof = true;
  proof.sparse_vector_score_proof = true;
  proof.maxscore_wand_topk_proof = true;
  proof.bloom_negative_pruning_proof = true;
  proof.provider_contract = ProviderContract(
      api::EngineNoSqlProviderFamily::kSearch,
      "odf092.local.search.provider");
  return proof;
}

api::EngineGraphPhysicalProof GraphProof() {
  api::EngineGraphPhysicalProof proof;
  proof.proof_supplied = true;
  proof.vertex_index_proof = true;
  proof.edge_index_proof = true;
  proof.adjacency_store_proof = true;
  proof.adjacency_page_proof = true;
  proof.frontier_batching_proof = true;
  proof.visited_cycle_policy_proof = true;
  proof.bidirectional_search_proof = true;
  proof.fusion_seed_proof = true;
  proof.provider_contract = ProviderContract(
      api::EngineNoSqlProviderFamily::kGraph,
      "odf092.local.graph.provider");
  return proof;
}

api::EngineTimeSeriesPhysicalProof TimeSeriesProof() {
  api::EngineTimeSeriesPhysicalProof proof;
  proof.proof_supplied = true;
  proof.time_meta_bucket_store_proof = true;
  proof.columnar_metric_page_proof = true;
  proof.summary_min_max_count_sum_proof = true;
  proof.rollup_materialization_proof = true;
  proof.late_arrival_delta_merge_proof = true;
  proof.provider_contract = ProviderContract(
      api::EngineNoSqlProviderFamily::kTimeSeries,
      "odf092.local.time_series.provider");
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
  request.localized_names.push_back({"en", "primary", "", key, true});
  request.expires_after_local_transaction_id = expires_after_tx;
  const auto result = api::EngineKeyValuePut(request);
  Require(result.ok, "ODF-092 key/value put failed");
  return result;
}

void NoSqlKeyValueRoutesThroughPrimitive() {
  const std::string database_path = "/tmp/sb_odf_092_gate_api.sbdb";
  SeedCrudTransaction(database_path);
  Put(database_path, "acct:1", "alpha");
  Put(database_path, "acct:2", "beta");
  Put(database_path, "acct:expired", "gone", 80);

  api::EngineKeyValueMultiGetRequest multiget;
  multiget.context = Context(database_path, 90);
  multiget.keys = {"acct:2", "acct:1", "acct:2", "acct:expired", "missing"};
  multiget.physical_proof = ExactProof();
  const auto multi_result = api::EngineKeyValueMultiGet(multiget);
  Require(multi_result.ok, "ODF-092 NoSQL MultiGet failed");
  Require(multi_result.result_shape.rows.size() == 3,
          "ODF-092 NoSQL MultiGet did not preserve duplicate and visible rows");
  Require(RowField(multi_result, 0, "key") == "acct:2" &&
              RowField(multi_result, 1, "key") == "acct:1" &&
              RowField(multi_result, 2, "key") == "acct:2",
          "ODF-092 NoSQL MultiGet did not preserve requested key order");
  Require(RowField(multi_result, 2, "duplicate_key") == "true",
          "ODF-092 NoSQL MultiGet lost duplicate-key row evidence");
  Require(ApiEvidenceHas(multi_result,
                         "kv_ordered_batch_lookup_primitive",
                         "ODF-092"),
          "ODF-092 NoSQL MultiGet did not route through primitive");
  Require(ApiEvidenceHas(multi_result,
                         "batch_point_lookup",
                         "batch_point_lookup.miss_count=2"),
          "ODF-092 NoSQL MultiGet miss evidence missing");
  Require(ApiEvidenceHas(multi_result,
                         "batch_point_lookup_miss",
                         "key_not_found"),
          "ODF-092 NoSQL MultiGet per-key miss diagnostic missing");
  RequireApiEvidenceHygiene(multi_result);

  api::EngineKeyValuePipelineRequest pipeline;
  pipeline.context = Context(database_path, 77);
  pipeline.max_admitted_operations = 3;
  pipeline.physical_proof = ExactProof();
  pipeline.puts.push_back({"pipe:1", "one", 0});
  pipeline.puts.push_back({"pipe:2", "two", 0});
  pipeline.get_keys.push_back("pipe:2");
  auto pipeline_result = api::EngineKeyValuePipeline(pipeline);
  Require(pipeline_result.ok, "ODF-092 NoSQL pipeline failed");
  Require(pipeline_result.result_shape.rows.size() == 1,
          "ODF-092 NoSQL pipeline did not route get through primitive");
  Require(RowField(pipeline_result, 0, "key") == "pipe:2",
          "ODF-092 NoSQL pipeline returned the wrong key");
  Require(ApiEvidenceHas(pipeline_result,
                         "kv_ordered_batch_lookup_primitive",
                         "ODF-092"),
          "ODF-092 pipeline primitive evidence missing");
  RequireApiEvidenceHygiene(pipeline_result);

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

void NoSqlPhysicalFamiliesRouteThroughPrimitive() {
  const std::string database_path = "/tmp/sb_odf_092_gate_families.sbdb";
  const auto row_uuid =
      uuid::UuidToString(V7(platform::UuidKind::row,
                            1710000092000ull,
                            0x60)
                             .value);

  api::EngineDocumentFindRequest document;
  document.context = Context(database_path, 90);
  document.path = "profile.name";
  document.equals_value = "amy";
  document.physical_proof = DocumentProof();
  auto document_result = api::EngineDocumentFind(document);
  Require(document_result.ok, "ODF-092 document route failed");
  Require(ApiEvidenceHas(document_result,
                         "nosql_ordered_batch_lookup_primitive",
                         "document:ODF-092"),
          "ODF-092 document route did not use ordered batch lookup");
  Require(ApiEvidenceHas(document_result,
                         "batch_point_lookup",
                         "batch_point_lookup.purpose=document_payload"),
          "ODF-092 document batch purpose evidence missing");
  RequireApiEvidenceHygiene(document_result);

  api::EngineVectorSearchRequest vector;
  vector.context = Context(database_path, 90);
  vector.query_vector = {1.0, 0.0};
  vector.top_k = 1;
  vector.vector_corpus_rows.push_back(
      {row_uuid, {1.0, 0.0}, {}, {{"tenant", "a"}}});
  vector.metadata_filters.push_back({"tenant", "a"});
  vector.filtered_strategy = api::EngineVectorFilteredStrategy::kPreFilter;
  vector.physical_proof = VectorProof();
  auto vector_result = api::EngineVectorSearch(vector);
  Require(vector_result.ok, "ODF-092 vector route failed");
  Require(ApiEvidenceHas(vector_result,
                         "nosql_ordered_batch_lookup_primitive",
                         "vector:ODF-092"),
          "ODF-092 vector route did not use ordered batch lookup");
  Require(ApiEvidenceHas(vector_result,
                         "batch_point_lookup",
                         "batch_point_lookup.purpose=vector_rerank_payload"),
          "ODF-092 vector batch purpose evidence missing");
  RequireApiEvidenceHygiene(vector_result);

  api::EngineSearchQueryRequest search;
  search.context = Context(database_path, 90);
  search.query_text = "alpha";
  search.top_k = 1;
  search.document_corpus.push_back({row_uuid, "alpha beta", true});
  search.physical_proof = SearchProof();
  auto search_result = api::EngineSearchQuery(search);
  Require(search_result.ok, "ODF-092 search route failed");
  Require(ApiEvidenceHas(search_result,
                         "nosql_ordered_batch_lookup_primitive",
                         "search:ODF-092"),
          "ODF-092 search route did not use ordered batch lookup");
  Require(ApiEvidenceHas(search_result,
                         "batch_point_lookup",
                         "batch_point_lookup.purpose=search_payload"),
          "ODF-092 search batch purpose evidence missing");
  RequireApiEvidenceHygiene(search_result);

  api::EngineGraphQueryRequest graph;
  graph.context = Context(database_path, 90);
  graph.physical_query = true;
  graph.vertices.push_back({"v1", {"Account"}, {{"tenant", "a"}}});
  graph.seed_vertex_ids.push_back("v1");
  graph.physical_proof = GraphProof();
  auto graph_result = api::EngineGraphQuery(graph);
  Require(graph_result.ok, "ODF-092 graph route failed");
  Require(ApiEvidenceHas(graph_result,
                         "nosql_ordered_batch_lookup_primitive",
                         "graph:ODF-092"),
          "ODF-092 graph route did not use ordered batch lookup");
  Require(ApiEvidenceHas(graph_result,
                         "batch_point_lookup",
                         "batch_point_lookup.purpose=graph_frontier"),
          "ODF-092 graph batch purpose evidence missing");
  RequireApiEvidenceHygiene(graph_result);

  api::EngineTimeSeriesAppendRequest time_series;
  time_series.context = Context(database_path, 90);
  time_series.physical_append = true;
  time_series.bucket_duration_ns = 1000;
  time_series.points.push_back({1500, "cpu", 42.0, {{"host", "a"}}});
  time_series.rollup_intervals_ns.push_back(5000);
  time_series.physical_proof = TimeSeriesProof();
  auto time_series_result = api::EngineTimeSeriesAppend(time_series);
  Require(time_series_result.ok, "ODF-092 time-series route failed");
  Require(ApiEvidenceHas(time_series_result,
                         "nosql_ordered_batch_lookup_primitive",
                         "time_series:ODF-092"),
          "ODF-092 time-series route did not use ordered batch lookup");
  Require(ApiEvidenceHas(time_series_result,
                         "batch_point_lookup",
                         "batch_point_lookup.purpose=time_series_bucket"),
          "ODF-092 time-series batch purpose evidence missing");
  RequireApiEvidenceHygiene(time_series_result);
}

}  // namespace

int main() {
  StableOrderingDuplicatesAndMisses();
  NonUniqueForeignKeyRowsStayRowUuidOrdered();
  AllPurposeProfilesUseSamePrimitive();
  ExecutorWrapperRequiresExactMsaSecurityRecheck();
  CorruptPlansAndProvidersFailClosed();
  NoSqlKeyValueRoutesThroughPrimitive();
  NoSqlPhysicalFamiliesRouteThroughPrimitive();
  return EXIT_SUCCESS;
}
