// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-113 NoSQL family benchmark closure gate.

#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

#ifndef ODF113_OUTPUT_JSON
#define ODF113_OUTPUT_JSON "optimizer_deficiency_odf_113_gate.json"
#endif

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
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

bool EvidenceContainsExact(const api::EngineApiResult& result,
                           std::string_view kind,
                           std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string EvidenceIdContaining(const api::EngineApiResult& result,
                                 std::string_view kind,
                                 std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return item.evidence_id;
    }
  }
  Fail("ODF-113 expected API evidence was missing");
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

bool RowHasField(const api::EngineApiResult& result,
                 std::size_t row_index,
                 std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) {
    return false;
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return true;
    }
  }
  return false;
}

bool RowMatches(const api::EngineApiResult& result,
                std::initializer_list<std::pair<std::string_view,
                                                std::string_view>> fields) {
  for (std::size_t row_index = 0; row_index < result.result_shape.rows.size();
       ++row_index) {
    bool matched = true;
    for (const auto& [field, expected] : fields) {
      if (RowField(result, row_index, field) != expected) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

bool AnyRowFieldEquals(const api::EngineApiResult& result,
                       std::string_view field,
                       std::string_view expected) {
  for (std::size_t row_index = 0; row_index < result.result_shape.rows.size();
       ++row_index) {
    if (RowField(result, row_index, field) == expected) {
      return true;
    }
  }
  return false;
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path UniqueTempDir(std::string_view name) {
  static std::uint64_t counter = 0;
  const auto path = std::filesystem::temp_directory_path() /
                    ("scratchbird_odf113_" + std::string(name) + "_" +
                     std::to_string(NowMillis()) + "_" +
                     std::to_string(++counter));
  std::filesystem::create_directories(path);
  return path;
}

struct TempDatabase {
  std::filesystem::path dir;
  std::filesystem::path path;

  explicit TempDatabase(std::string_view name) : dir(UniqueTempDir(name)) {
    path = dir / "odf113.sbdb";
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  api::EngineApiU64 tx) {
  api::EngineRequestContext context;
  context.database_path = database_path.string();
  context.local_transaction_id = tx;
  context.database_uuid.canonical = "odf113-database";
  context.transaction_uuid.canonical = "odf113-transaction-" + std::to_string(tx);
  context.security_context_present = true;
  context.request_id = "odf113-request-" + std::to_string(tx);
  context.trace_tags = {"optimizer_deficiency_odf_113_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

void SeedCrudTransaction(const std::filesystem::path& database_path) {
  std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t113\todf113-transaction-113\n";
  crud << "SBCRUD1\tTX_BEGIN\t130\todf113-transaction-130\n";
  crud.flush();
  Require(static_cast<bool>(crud), "ODF-113 could not seed CRUD transaction log");
}

void RequireNoForbiddenEvidence(const api::EngineApiResult& result,
                                std::string_view scenario) {
  for (const auto& item : result.evidence) {
    for (const auto token : {"docs" "/execution-plans",
                             "docs" "/findings",
                             "public_release_evidence",
                             "docs/reference",
                             "execution_plan",
                             "findings",
                             "contracts",
                             "references",
                             "local_descriptor_scan",
                             "specialized_descriptor_fallback",
                             "behavior_store_scan_selected=true",
                             "descriptor_scan_selected=true",
                             "parser_executes_sql=true",
                             "wal_recovery_authority=true",
                             "provider_transaction_finality_authority=true",
                             "provider_visibility_authority=true",
                             "index_transaction_finality_authority=true",
                             "delta_overlay_transaction_finality_authority=true",
                             "parser_transaction_finality_authority=true",
                             "write_ahead_log_transaction_finality_authority=true",
                             "client_autocommit_authority=true"}) {
      if (item.evidence_kind.find(token) != std::string::npos ||
          item.evidence_id.find(token) != std::string::npos) {
        std::cerr << "Forbidden ODF-113 evidence token in " << scenario << ": "
                  << item.evidence_kind << '=' << item.evidence_id << '\n';
        Fail("ODF-113 runtime evidence leaked a forbidden token");
      }
    }
  }
}

void RequirePhysicalProviderHygiene(const api::EngineApiResult& result,
                                    std::string_view scenario) {
  RequireOk(result, scenario);
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-113 physical-provider path reported descriptor rows scanned");
  Require(!EvidenceContains(result, "descriptor_scan_selected", "true"),
          "ODF-113 descriptor scan was selected by a benchmark-clean path");
  Require(!EvidenceContains(result, "behavior_store_scan_selected", "true"),
          "ODF-113 behavior-store scan was selected by a benchmark-clean path");
  Require(!EvidenceContains(result, "parser_executes_sql", "true"),
          "ODF-113 parser SQL execution appeared in physical-provider path");
  Require(!EvidenceContains(result, "wal_recovery_authority", "true"),
          "ODF-113 WAL authority appeared in physical-provider path");
  RequireNoForbiddenEvidence(result, scenario);
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned>(static_cast<unsigned char>(ch));
          out += escaped.str();
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::string Quote(std::string_view value) {
  return "\"" + JsonEscape(value) + "\"";
}

std::string StableHash(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : input) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

struct ScenarioEvidence {
  std::string name;
  std::string family;
  std::string route;
  std::uint64_t rows_changed = 0;
  std::uint64_t rows_returned = 0;
  bool no_descriptor_scan = true;
  bool no_behavior_store_scan = true;
  bool benchmark_clean = true;
  bool live_speed_numbers = false;
  bool fail_closed = false;
  std::vector<std::pair<std::string, std::string>> proofs;
  std::string result_hash;
};

void AddProof(ScenarioEvidence* scenario,
              std::string key,
              std::string value) {
  scenario->proofs.emplace_back(std::move(key), std::move(value));
}

std::string ScenarioHashSeed(const ScenarioEvidence& scenario) {
  std::ostringstream seed;
  seed << scenario.name << '|' << scenario.family << '|' << scenario.route << '|'
       << scenario.rows_changed << '|' << scenario.rows_returned << '|'
       << (scenario.no_descriptor_scan ? "true" : "false") << '|'
       << (scenario.no_behavior_store_scan ? "true" : "false") << '|'
       << (scenario.benchmark_clean ? "true" : "false") << '|'
       << (scenario.live_speed_numbers ? "true" : "false") << '|'
       << (scenario.fail_closed ? "true" : "false");
  for (const auto& proof : scenario.proofs) {
    seed << '|' << proof.first << '=' << proof.second;
  }
  return seed.str();
}

void FinalizeScenario(ScenarioEvidence* scenario) {
  scenario->result_hash = StableHash(ScenarioHashSeed(*scenario));
}

api::EngineNoSqlPhysicalProviderContract BaseProviderContract(
    api::EngineNoSqlProviderFamily family,
    std::string provider_id,
    std::string index_id,
    std::uint64_t generation) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = family;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = std::move(provider_id);
  contract.fallback_provider_id = "none";
  contract.local_provider_available = true;
  contract.exact_fallback_available = false;
  contract.estimated_rows = 8;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.descriptor_visibility.proof_id = "odf113-descriptor-visible";
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.security_redaction.proof_id = "odf113-security-bound";
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = generation;
  contract.index_generation.available_generation = generation;
  contract.index_generation.index_uuid = std::move(index_id);
  contract.index_generation.proof_id = "odf113-index-generation";
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.policy.policy_snapshot_uuid = "odf113-policy-snapshot";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

api::EngineKeyValuePhysicalProof KvExactProof() {
  api::EngineKeyValuePhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kKeyValue,
      "odf113.local.kv.provider",
      "odf113-kv-exact-prefix-index",
      113);
  proof.proof_supplied = true;
  proof.exact_key_index_proof = true;
  proof.ttl_visibility_proof = true;
  return proof;
}

api::EngineKeyValuePhysicalProof KvPrefixProof() {
  auto proof = KvExactProof();
  proof.prefix_index_proof = true;
  return proof;
}

api::EngineDocumentPhysicalProof DocumentProof() {
  api::EngineDocumentPhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kDocument,
      "odf113.local.document.path.provider",
      "odf113-document-path-shape-index",
      114);
  proof.provider_contract.fallback_provider_id =
      "odf113.local.document.shape.dictionary";
  proof.provider_contract.exact_fallback_available = true;
  proof.proof_supplied = true;
  proof.exact_path_index_proof = true;
  proof.wildcard_shape_index_proof = true;
  proof.shape_dictionary_proof = true;
  proof.structural_sharing_proof = true;
  proof.partial_materialization_proof = true;
  return proof;
}

api::EngineSearchPhysicalProof SearchProof() {
  api::EngineSearchPhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kSearch,
      "odf113.local.search.provider",
      "odf113-search-segment-index",
      115);
  proof.proof_supplied = true;
  proof.mutable_buffer_proof = true;
  proof.sealed_inverted_segment_proof = true;
  proof.bm25_statistics_proof = true;
  proof.sparse_vector_score_proof = true;
  proof.maxscore_wand_topk_proof = true;
  proof.bloom_negative_pruning_proof = true;
  return proof;
}

api::EngineVectorPhysicalProof VectorProof() {
  api::EngineVectorPhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kVector,
      "odf113.local.vector.provider",
      "odf113-vector-tiered-index",
      116);
  proof.provider_contract.fallback_provider_id = "odf113.local.exact.vector";
  proof.provider_contract.exact_fallback_available = true;
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
  return proof;
}

api::EngineGraphPhysicalProof GraphProof() {
  api::EngineGraphPhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kGraph,
      "odf113.local.graph.provider",
      "odf113-graph-adjacency-index",
      117);
  proof.proof_supplied = true;
  proof.vertex_index_proof = true;
  proof.edge_index_proof = true;
  proof.adjacency_store_proof = true;
  proof.adjacency_page_proof = true;
  proof.frontier_batching_proof = true;
  proof.visited_cycle_policy_proof = true;
  proof.bidirectional_search_proof = true;
  proof.fusion_seed_proof = true;
  return proof;
}

api::EngineTimeSeriesPhysicalProof TimeSeriesProof() {
  api::EngineTimeSeriesPhysicalProof proof;
  proof.provider_contract = BaseProviderContract(
      api::EngineNoSqlProviderFamily::kTimeSeries,
      "odf113.local.time_series.provider",
      "odf113-time-series-bucket-index",
      118);
  proof.proof_supplied = true;
  proof.time_meta_bucket_store_proof = true;
  proof.columnar_metric_page_proof = true;
  proof.summary_min_max_count_sum_proof = true;
  proof.rollup_materialization_proof = true;
  proof.late_arrival_delta_merge_proof = true;
  return proof;
}

void AddFragment(api::EngineDocumentInsertRequest* request,
                 std::string path,
                 std::string value) {
  api::EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  request->assignments.push_back({std::move(path), std::move(typed)});
}

std::vector<api::EngineSearchDocumentInput> SearchCorpus() {
  return {
      {"doc-alpha-strong", "alpha alpha alpha beta search search", true},
      {"doc-alpha-mutable", "alpha mutable buffer entry", false},
      {"doc-beta-sealed", "beta sealed segment entry", true},
      {"doc-gamma", "gamma delta epsilon", true},
  };
}

std::vector<api::EngineVectorCorpusRow> VectorCorpus() {
  return {
      {"row-alpha",
       {1.0, 0.0},
       {{"alpha", 1.0}},
       {{"tenant", "blue"}, {"kind", "primary"}}},
      {"row-beta",
       {0.9, 0.1},
       {{"beta", 1.0}},
       {{"tenant", "red"}, {"kind", "secondary"}}},
      {"row-gamma",
       {0.0, 1.0},
       {{"alpha", 0.2}, {"boost", 1.0}},
       {{"tenant", "blue"}, {"kind", "secondary"}}},
      {"row-delta",
       {0.4, 0.6},
       {{"boost", 3.0}},
       {{"tenant", "green"}, {"kind", "primary"}}},
  };
}

api::EngineVectorSearchRequest BaseVectorRequest() {
  api::EngineVectorSearchRequest request;
  request.context.database_path = "odf113-vector-transient";
  request.context.local_transaction_id = 113;
  request.context.database_uuid.canonical = "odf113-vector-database";
  request.context.transaction_uuid.canonical = "odf113-vector-transaction";
  request.context.security_context_present = true;
  request.query_vector = {1.0, 0.0};
  request.top_k = 3;
  request.vector_corpus_rows = VectorCorpus();
  request.physical_proof = VectorProof();
  return request;
}

std::vector<api::EngineGraphVertexInput> GraphVertices() {
  return {
      {"A", {"person", "seed"}, {{"tenant", "blue"}, {"name", "alpha"}}},
      {"B", {"person"}, {{"tenant", "green"}, {"name", "beta"}}},
      {"C", {"account"}, {{"tenant", "blue"}, {"name", "connector"}}},
      {"D", {"account"}, {{"tenant", "red"}, {"name", "detour"}}},
      {"E", {"person"}, {{"tenant", "blue"}, {"name", "cycle"}}},
  };
}

std::vector<api::EngineGraphEdgeInput> GraphEdges() {
  return {
      {"e-ac", "A", "C", "knows", {{"since", "2024"}}, 1.0},
      {"e-ad", "A", "D", "knows", {{"since", "2025"}}, 2.0},
      {"e-cb", "C", "B", "knows", {{"since", "2026"}}, 1.5},
      {"e-db", "D", "B", "knows", {{"since", "2026"}}, 2.5},
      {"e-ba", "B", "A", "blocks", {{"since", "2023"}}, 3.0},
      {"e-ae", "A", "E", "blocks", {{"since", "2022"}}, 4.0},
      {"e-ea", "E", "A", "knows", {{"since", "2022"}}, 5.0},
  };
}

api::EngineGraphQueryRequest BaseGraphRequest() {
  api::EngineGraphQueryRequest request;
  request.context.database_path = "odf113-graph-transient";
  request.context.local_transaction_id = 113;
  request.context.database_uuid.canonical = "odf113-graph-database";
  request.context.transaction_uuid.canonical = "odf113-graph-transaction";
  request.context.security_context_present = true;
  request.physical_query = true;
  request.vertices = GraphVertices();
  request.edges = GraphEdges();
  request.edge_type_filter = "knows";
  request.physical_proof = GraphProof();
  return request;
}

std::vector<api::EngineTimeSeriesPoint> TimeSeriesPoints() {
  const std::vector<api::EngineTimeSeriesPointTag> east_tags = {
      {"host", "a"}, {"region", "east"}};
  const std::vector<api::EngineTimeSeriesPointTag> west_tags = {
      {"host", "b"}, {"region", "west"}};
  return {
      {10, "temp", 20.0, east_tags},
      {20, "temp", 22.0, east_tags},
      {30, "humidity", 0.4, east_tags},
      {65, "temp", 30.0, east_tags},
      {5, "temp", 18.0, east_tags},
      {15, "temp", 50.0, west_tags},
  };
}

api::EngineTimeSeriesAppendRequest BaseTimeSeriesRequest() {
  api::EngineTimeSeriesAppendRequest request;
  request.context.database_path = "odf113-time-series-transient";
  request.context.local_transaction_id = 113;
  request.context.database_uuid.canonical = "odf113-time-series-database";
  request.context.transaction_uuid.canonical = "odf113-time-series-transaction";
  request.context.security_context_present = true;
  request.physical_append = true;
  request.points = TimeSeriesPoints();
  request.bucket_duration_ns = 60;
  request.rollup_intervals_ns = {120};
  request.late_arrival_watermark_ns = 10;
  request.late_arrival_policy =
      api::EngineTimeSeriesLateArrivalPolicy::kDeltaMergeReopen;
  request.physical_proof = TimeSeriesProof();
  return request;
}

ScenarioEvidence KvPhysicalProviderScenario() {
  TempDatabase database("kv");
  SeedCrudTransaction(database.path);

  const auto tx113 = Context(database.path, 113);
  const auto tx130 = Context(database.path, 130);

  const auto put = [&](std::string key,
                       std::string value,
                       api::EngineApiU64 expires_after_tx = 0) {
    api::EngineKeyValuePutRequest request;
    request.context = tx113;
    request.key = std::move(key);
    request.value = std::move(value);
    request.target_object.uuid.canonical = request.key;
    request.localized_names.push_back({"en", "primary", "", request.key, true});
    request.expires_after_local_transaction_id = expires_after_tx;
    const auto result = api::EngineKeyValuePut(request);
    RequireOk(result, "ODF-113 KV put failed");
    Require(EvidenceContains(result,
                             "kv_physical_access",
                             "write_through_exact_prefix_provider"),
            "ODF-113 KV put did not write the physical provider");
    Require(EvidenceContains(result,
                             "mga_finality_authority",
                             "engine_transaction_inventory"),
            "ODF-113 KV put lost MGA finality evidence");
  };
  put("kv:1", "alpha");
  put("kv:2", "beta");
  put("kv:expired", "gone", 120);

  api::EngineKeyValueGetRequest exact;
  exact.context = tx113;
  exact.key = "kv:1";
  exact.physical_proof = KvExactProof();
  const auto exact_result = api::EngineKeyValueGet(exact);
  RequirePhysicalProviderHygiene(exact_result, "ODF-113 KV exact get");
  Require(exact_result.result_shape.rows.size() == 1,
          "ODF-113 KV exact get row count changed");
  Require(RowField(exact_result, 0, "value") == "alpha",
          "ODF-113 KV exact get returned wrong value");
  Require(EvidenceContains(exact_result,
                           "kv_physical_access",
                           "exact_key_index_probe"),
          "ODF-113 KV exact get did not use exact physical access");

  api::EngineKeyValueGetRequest prefix;
  prefix.context = tx113;
  prefix.prefix = "kv:";
  prefix.physical_proof = KvPrefixProof();
  const auto prefix_result = api::EngineKeyValueGet(prefix);
  RequirePhysicalProviderHygiene(prefix_result, "ODF-113 KV prefix get");
  Require(prefix_result.result_shape.rows.size() == 3,
          "ODF-113 KV prefix get row count changed");
  Require(EvidenceContains(prefix_result,
                           "kv_physical_access",
                           "prefix_index_probe"),
          "ODF-113 KV prefix get did not use prefix physical access");

  exact.context = tx130;
  exact.key = "kv:expired";
  const auto ttl_result = api::EngineKeyValueGet(exact);
  RequirePhysicalProviderHygiene(ttl_result, "ODF-113 KV TTL exact get");
  Require(ttl_result.result_shape.rows.empty(),
          "ODF-113 KV TTL expired row was returned");
  Require(EvidenceContains(ttl_result,
                           "ttl_visibility_evidence",
                           "deterministic_local_transaction_id"),
          "ODF-113 KV TTL evidence missing");

  api::EngineKeyValueMultiGetRequest multiget;
  multiget.context = tx130;
  multiget.keys = {"kv:1", "kv:2", "kv:expired", "missing"};
  multiget.physical_proof = KvExactProof();
  const auto multiget_result = api::EngineKeyValueMultiGet(multiget);
  RequirePhysicalProviderHygiene(multiget_result, "ODF-113 KV MultiGet");
  Require(multiget_result.result_shape.rows.size() == 2,
          "ODF-113 KV MultiGet did not exclude missing or expired rows");
  Require(EvidenceContains(multiget_result,
                           "kv_physical_access",
                           "multiget_exact_key_index_probe"),
          "ODF-113 KV MultiGet did not use exact-key batch access");
  Require(EvidenceContains(multiget_result, "kv_multiget_keys", "4"),
          "ODF-113 KV MultiGet key-count evidence missing");

  api::EngineKeyValuePipelineRequest pipeline;
  pipeline.context = tx113;
  pipeline.max_admitted_operations = 3;
  pipeline.physical_proof = KvExactProof();
  pipeline.puts.push_back({"pipe:1", "one", 0});
  pipeline.puts.push_back({"pipe:2", "two", 0});
  pipeline.get_keys.push_back("pipe:1");
  const auto pipeline_result = api::EngineKeyValuePipeline(pipeline);
  RequirePhysicalProviderHygiene(pipeline_result, "ODF-113 KV pipeline");
  Require(pipeline_result.dml_summary.rows_changed == 2,
          "ODF-113 KV pipeline row-change count changed");
  Require(EvidenceContains(pipeline_result,
                           "kv_pipeline_admitted_operations",
                           "3"),
          "ODF-113 KV pipeline admission evidence missing");

  api::EngineKeyValueAtomicProgramRequest atomic;
  atomic.context = tx113;
  atomic.physical_proof = KvExactProof();
  atomic.steps.push_back({"set", "counter", "10"});
  atomic.steps.push_back({"increment_i64", "counter", "7"});
  const auto atomic_result = api::EngineKeyValueAtomicProgram(atomic);
  RequirePhysicalProviderHygiene(atomic_result, "ODF-113 KV atomic program");
  Require(RowField(atomic_result, 1, "value") == "17",
          "ODF-113 KV atomic program result changed");
  Require(EvidenceContains(atomic_result,
                           "kv_atomic_program",
                           "deterministic_sblr_read_compute_write"),
          "ODF-113 KV atomic program SBLR-style evidence missing");
  Require(EvidenceContainsExact(atomic_result,
                                "parser_transaction_finality_authority",
                                "false"),
          "ODF-113 KV atomic program parser finality evidence missing");

  ScenarioEvidence scenario;
  scenario.name = "kv_physical_provider_exact_prefix_multiget_pipeline_atomic";
  scenario.family = "key_value";
  scenario.route = "odf113.local.kv.provider";
  scenario.rows_changed = 7;
  scenario.rows_returned = exact_result.result_shape.rows.size() +
                           prefix_result.result_shape.rows.size() +
                           multiget_result.result_shape.rows.size() +
                           pipeline_result.result_shape.rows.size() +
                           atomic_result.result_shape.rows.size();
  AddProof(&scenario, "kv_physical_access", "exact_key_index_probe");
  AddProof(&scenario, "kv_physical_access", "prefix_index_probe");
  AddProof(&scenario, "kv_physical_access", "multiget_exact_key_index_probe");
  AddProof(&scenario, "kv_pipeline_admitted_operations", "3");
  AddProof(&scenario,
           "kv_atomic_program",
           "deterministic_sblr_read_compute_write");
  AddProof(&scenario, "ttl_visibility_evidence", "deterministic_local_transaction_id");
  AddProof(&scenario, "visible_rows_scanned", "0");
  AddProof(&scenario, "parser_transaction_finality_authority", "false");
  AddProof(&scenario, "mga_finality_authority", "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  return scenario;
}

void InsertDocument(const std::filesystem::path& database_path,
                    const std::string& uuid,
                    const std::string& name,
                    const std::vector<std::pair<std::string, std::string>>& fragments) {
  api::EngineDocumentInsertRequest insert;
  insert.context = Context(database_path, 113);
  insert.target_object.uuid.canonical = uuid;
  insert.localized_names.push_back({"en", "primary", "", name, true});
  for (const auto& [path, value] : fragments) {
    AddFragment(&insert, path, value);
  }
  const auto result = api::EngineDocumentInsert(insert);
  RequireOk(result, "ODF-113 document insert failed");
  Require(EvidenceContains(result,
                           "document_physical_provider",
                           "write_through_path_provider"),
          "ODF-113 document insert did not populate physical provider");
}

ScenarioEvidence DocumentPhysicalProviderScenario() {
  TempDatabase database("document");
  SeedCrudTransaction(database.path);
  InsertDocument(database.path,
                 "doc-customer-a",
                 "customer-a",
                 {{"customer.id", "A1"},
                  {"customer.tier", "gold"},
                  {"line_items.0.sku", "SKU-1"},
                  {"line_items.1.sku", "SKU-2"},
                  {"private.ssn", "redacted"}});
  InsertDocument(database.path,
                 "doc-customer-b",
                 "customer-b",
                 {{"customer.id", "B1"},
                  {"customer.tier", "silver"},
                  {"line_items.0.sku", "SKU-3"},
                  {"line_items.1.sku", "SKU-4"},
                  {"private.ssn", "redacted"}});

  api::EngineDocumentFindRequest exact;
  exact.context = Context(database.path, 130);
  exact.path = "customer.id";
  exact.equals_value = "A1";
  exact.projected_paths = {"customer.id", "customer.tier"};
  exact.physical_proof = DocumentProof();
  const auto exact_result = api::EngineDocumentFind(exact);
  RequirePhysicalProviderHygiene(exact_result, "ODF-113 document exact path");
  Require(exact_result.result_shape.rows.size() == 1,
          "ODF-113 document exact path row count changed");
  Require(RowField(exact_result, 0, "path:customer.id") == "A1",
          "ODF-113 document exact path returned wrong fragment");
  Require(RowField(exact_result, 0, "path:customer.tier") == "gold",
          "ODF-113 document projected materialization missed path");
  Require(!RowHasField(exact_result, 0, "payload"),
          "ODF-113 document exact path materialized full payload");
  Require(!RowHasField(exact_result, 0, "path:private.ssn"),
          "ODF-113 document exact path materialized unprojected field");
  Require(EvidenceContains(exact_result,
                           "document_physical_access",
                           "exact_path_index_probe"),
          "ODF-113 document exact path did not use targeted path index");
  Require(EvidenceContains(exact_result,
                           "document_partial_materialization",
                           "projected_paths_only"),
          "ODF-113 document exact path lacked partial materialization proof");
  Require(EvidenceContains(exact_result, "row_mga_recheck_evidence", "required"),
          "ODF-113 document exact path lacked row MGA recheck evidence");
  Require(EvidenceContains(exact_result,
                           "row_security_recheck_evidence",
                           "required"),
          "ODF-113 document exact path lacked row security recheck evidence");

  api::EngineDocumentFindRequest wildcard;
  wildcard.context = Context(database.path, 130);
  wildcard.path = "line_items.*.sku";
  wildcard.equals_value = "SKU-2";
  wildcard.wildcard_path = true;
  wildcard.projected_paths = {"customer.id"};
  wildcard.physical_proof = DocumentProof();
  const auto wildcard_result = api::EngineDocumentFind(wildcard);
  RequirePhysicalProviderHygiene(wildcard_result,
                                 "ODF-113 document wildcard path");
  Require(wildcard_result.result_shape.rows.size() == 1,
          "ODF-113 document wildcard path row count changed");
  Require(RowField(wildcard_result, 0, "path:customer.id") == "A1",
          "ODF-113 document wildcard path returned wrong projection");
  Require(!RowHasField(wildcard_result, 0, "payload"),
          "ODF-113 document wildcard materialized full payload");
  Require(EvidenceContains(wildcard_result,
                           "document_physical_access",
                           "wildcard_shape_index_probe"),
          "ODF-113 document wildcard did not use shape index");
  Require(EvidenceContains(wildcard_result,
                           "document_shape_fallback",
                           "shape_dictionary_proved"),
          "ODF-113 document wildcard lacked shape dictionary proof");
  Require(EvidenceContains(wildcard_result,
                           "document_structural_sharing",
                           "shape_ref_count=2"),
          "ODF-113 document structural sharing proof missing");

  api::EngineDocumentFindRequest missing;
  missing.context = Context(database.path, 130);
  missing.path = "customer.id";
  missing.equals_value = "A1";
  missing.projected_paths = {"customer.id"};
  missing.physical_proof = DocumentProof();
  missing.physical_proof.partial_materialization_proof = false;
  const auto missing_result = api::EngineDocumentFind(missing);
  Require(!missing_result.ok,
          "ODF-113 document missing partial materialization proof did not fail closed");
  Require(DiagnosticContains(missing_result,
                             api::kDocumentPartialMaterializationProofMissing),
          "ODF-113 document missing proof diagnostic changed");

  ScenarioEvidence scenario;
  scenario.name = "document_physical_provider_path_shape_partial_materialization";
  scenario.family = "document";
  scenario.route = "odf113.local.document.path.provider";
  scenario.rows_changed = 2;
  scenario.rows_returned = exact_result.result_shape.rows.size() +
                           wildcard_result.result_shape.rows.size();
  AddProof(&scenario, "document_physical_access", "exact_path_index_probe");
  AddProof(&scenario, "document_physical_access", "wildcard_shape_index_probe");
  AddProof(&scenario,
           "document_partial_materialization",
           "projected_paths_only");
  AddProof(&scenario, "document_shape_fallback", "shape_dictionary_proved");
  AddProof(&scenario, "row_mga_recheck_evidence", "required");
  AddProof(&scenario, "row_security_recheck_evidence", "required");
  AddProof(&scenario,
           "fail_closed_missing_partial_materialization",
           api::kDocumentPartialMaterializationProofMissing);
  AddProof(&scenario, "visible_rows_scanned", "0");
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence SearchPhysicalProviderScenario() {
  api::EngineSearchQueryRequest request;
  request.context.database_path = "odf113-search-transient";
  request.context.database_uuid.canonical = "odf113-search-database";
  request.context.transaction_uuid.canonical = "odf113-search-transaction";
  request.context.local_transaction_id = 113;
  request.context.security_context_present = true;
  request.query_text = "alpha beta absentterm";
  request.top_k = 3;
  request.document_corpus = SearchCorpus();
  request.physical_proof = SearchProof();
  const auto result = api::EngineSearchQuery(request);
  RequirePhysicalProviderHygiene(result, "ODF-113 search physical query");
  Require(result.result_shape.rows.size() == 3,
          "ODF-113 search BM25 row count changed");
  Require(RowField(result, 0, "document_uuid") == "doc-alpha-strong",
          "ODF-113 search ranking winner changed");
  Require(EvidenceContains(result,
                           "search_physical_access",
                           "mutable_buffer_and_sealed_segment_bm25"),
          "ODF-113 search did not use mutable/sealed BM25 provider");
  Require(EvidenceContains(result,
                           "search_bm25_statistics",
                           "document_frequency_idf_avgdl"),
          "ODF-113 search BM25 statistics proof missing");
  Require(EvidenceContains(result,
                           "search_sparse_vector_score",
                           "per_term_contributions_recorded"),
          "ODF-113 search sparse-vector scoring proof missing");
  Require(EvidenceContains(result,
                           "search_bloom_negative_pruning",
                           "absent_terms=1"),
          "ODF-113 search Bloom negative pruning proof missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-113 search row MGA recheck proof missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-113 search row security recheck proof missing");

  request.query_text = "alpha beta";
  request.top_k = 1;
  const auto topk_result = api::EngineSearchQuery(request);
  RequirePhysicalProviderHygiene(topk_result, "ODF-113 search WAND top-K");
  Require(topk_result.result_shape.rows.size() == 1,
          "ODF-113 search top-K row count changed");
  Require(EvidenceContains(topk_result,
                           "search_wand_topk_pruning",
                           "candidates_pruned="),
          "ODF-113 search WAND pruning proof missing");
  Require(!EvidenceContains(topk_result,
                            "search_wand_topk_pruning",
                            "candidates_pruned=0"),
          "ODF-113 search WAND did not prune candidates");

  ScenarioEvidence scenario;
  scenario.name = "search_physical_provider_segments_bm25_wand_bloom";
  scenario.family = "search";
  scenario.route = "odf113.local.search.provider";
  scenario.rows_returned = result.result_shape.rows.size() +
                           topk_result.result_shape.rows.size();
  AddProof(&scenario,
           "search_physical_access",
           "mutable_buffer_and_sealed_segment_bm25");
  AddProof(&scenario, "search_bm25_statistics", "document_frequency_idf_avgdl");
  AddProof(&scenario,
           "search_sparse_vector_score",
           "per_term_contributions_recorded");
  AddProof(&scenario,
           "search_wand_topk_pruning",
           EvidenceIdContaining(topk_result,
                                "search_wand_topk_pruning",
                                "candidates_pruned="));
  AddProof(&scenario, "search_bloom_negative_pruning", "absent_terms=1");
  AddProof(&scenario, "row_mga_recheck_evidence", "required");
  AddProof(&scenario, "row_security_recheck_evidence", "required");
  AddProof(&scenario, "visible_rows_scanned", "0");
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence VectorPhysicalProviderScenario() {
  auto exact = BaseVectorRequest();
  exact.requested_access_tier = api::EngineVectorAccessTier::kExact;
  const auto exact_result = api::EngineVectorSearch(exact);
  RequirePhysicalProviderHygiene(exact_result, "ODF-113 vector exact tier");
  Require(RowField(exact_result, 0, "row_uuid") == "row-alpha",
          "ODF-113 vector exact winner changed");
  Require(EvidenceContains(exact_result,
                           "vector_physical_access",
                           "selected_tier=exact"),
          "ODF-113 vector exact tier proof missing");
  Require(EvidenceContains(exact_result,
                           "vector_generation_visibility",
                           "engine_owned_mga_publish_barrier"),
          "ODF-113 vector generation visibility proof missing");
  Require(EvidenceContains(exact_result,
                           "vector_exact_rerank",
                           "exact_dense_tiebreak"),
          "ODF-113 vector exact rerank proof missing");
  Require(EvidenceContains(exact_result, "row_mga_recheck_evidence", "required"),
          "ODF-113 vector row MGA recheck proof missing");
  Require(EvidenceContains(exact_result,
                           "row_security_recheck_evidence",
                           "required"),
          "ODF-113 vector row security recheck proof missing");

  std::uint64_t tier_rows_returned = exact_result.result_shape.rows.size();
  const struct {
    api::EngineVectorAccessTier tier;
    const char* evidence;
  } tier_cases[] = {{api::EngineVectorAccessTier::kHnsw, "selected_tier=hnsw"},
                    {api::EngineVectorAccessTier::kIvf, "selected_tier=ivf"},
                    {api::EngineVectorAccessTier::kPq, "selected_tier=pq"},
                    {api::EngineVectorAccessTier::kDiskAnnLike,
                     "selected_tier=diskann_like"}};
  for (const auto& item : tier_cases) {
    auto request = BaseVectorRequest();
    request.requested_access_tier = item.tier;
    const auto result = api::EngineVectorSearch(request);
    RequirePhysicalProviderHygiene(result, "ODF-113 vector tiered provider");
    Require(EvidenceContains(result, "vector_physical_access", item.evidence),
            "ODF-113 vector tiered provider proof missing");
    tier_rows_returned += result.result_shape.rows.size();
  }

  std::uint64_t filter_rows_returned = 0;
  const struct {
    api::EngineVectorFilteredStrategy strategy;
    const char* evidence_kind;
    const char* evidence_id;
  } filter_cases[] = {{api::EngineVectorFilteredStrategy::kPreFilter,
                       "vector_pre_filter",
                       "applied=true"},
                      {api::EngineVectorFilteredStrategy::kPostFilter,
                       "vector_post_filter",
                       "applied=true"},
                      {api::EngineVectorFilteredStrategy::kIterativeFilter,
                       "vector_iterative_filter",
                       "applied=true"}};
  for (const auto& item : filter_cases) {
    auto request = BaseVectorRequest();
    request.top_k = 2;
    request.filtered_strategy = item.strategy;
    request.metadata_filters.push_back({"tenant", "blue"});
    const auto result = api::EngineVectorSearch(request);
    RequirePhysicalProviderHygiene(result, "ODF-113 vector filtered provider");
    Require(result.result_shape.rows.size() == 2,
            "ODF-113 vector filtered result count changed");
    Require(EvidenceContains(result, item.evidence_kind, item.evidence_id),
            "ODF-113 vector filtered strategy proof missing");
    filter_rows_returned += result.result_shape.rows.size();
  }

  auto hybrid = BaseVectorRequest();
  hybrid.top_k = 1;
  hybrid.sparse_terms.push_back({"boost", 2.0});
  const auto hybrid_result = api::EngineVectorSearch(hybrid);
  RequirePhysicalProviderHygiene(hybrid_result, "ODF-113 vector hybrid search");
  Require(RowField(hybrid_result, 0, "row_uuid") == "row-delta",
          "ODF-113 vector hybrid winner changed");
  Require(EvidenceContains(hybrid_result,
                           "vector_hybrid_dense_sparse",
                           "dense_plus_sparse_score"),
          "ODF-113 vector hybrid proof missing");

  ScenarioEvidence scenario;
  scenario.name = "vector_physical_provider_tiered_filtered_hybrid_rerank";
  scenario.family = "vector";
  scenario.route = "odf113.local.vector.provider";
  scenario.rows_returned = tier_rows_returned + filter_rows_returned +
                           hybrid_result.result_shape.rows.size();
  AddProof(&scenario, "vector_physical_access", "selected_tier=exact");
  AddProof(&scenario, "vector_physical_access", "selected_tier=hnsw");
  AddProof(&scenario, "vector_physical_access", "selected_tier=ivf");
  AddProof(&scenario, "vector_physical_access", "selected_tier=pq");
  AddProof(&scenario, "vector_physical_access", "selected_tier=diskann_like");
  AddProof(&scenario, "vector_pre_filter", "applied=true");
  AddProof(&scenario, "vector_post_filter", "applied=true");
  AddProof(&scenario, "vector_iterative_filter", "applied=true");
  AddProof(&scenario, "vector_hybrid_dense_sparse", "dense_plus_sparse_score");
  AddProof(&scenario, "vector_exact_rerank", "exact_dense_tiebreak");
  AddProof(&scenario,
           "vector_generation_visibility",
           "engine_owned_mga_publish_barrier");
  AddProof(&scenario, "row_mga_recheck_evidence", "required");
  AddProof(&scenario, "row_security_recheck_evidence", "required");
  AddProof(&scenario, "visible_rows_scanned", "0");
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence GraphPhysicalProviderScenario() {
  auto seeded = BaseGraphRequest();
  seeded.seed_label = "person";
  seeded.seed_property_key = "tenant";
  seeded.seed_property_value = "blue";
  seeded.max_depth = 2;
  seeded.option_envelopes.push_back("graph.frontier_batch_size=1");
  const auto seeded_result = api::EngineGraphQuery(seeded);
  RequirePhysicalProviderHygiene(seeded_result,
                                 "ODF-113 graph seeded traversal");
  Require(RowField(seeded_result, 0, "vertex_id") == "A",
          "ODF-113 graph seed result changed");
  Require(AnyRowFieldEquals(seeded_result, "path", "A->C->B"),
          "ODF-113 graph frontier traversal missed A->C->B");
  Require(EvidenceContains(seeded_result,
                           "graph_seed_index",
                           "vertex_property_index"),
          "ODF-113 graph seed property index proof missing");
  Require(EvidenceContains(seeded_result,
                           "graph_adjacency_store",
                           "compressed_adjacency_pages"),
          "ODF-113 graph compressed adjacency proof missing");
  Require(EvidenceContains(seeded_result,
                           "graph_frontier_batching",
                           "batches="),
          "ODF-113 graph frontier batching proof missing");
  Require(EvidenceContains(seeded_result, "graph_cycle_policy", "visited_set"),
          "ODF-113 graph cycle policy proof missing");
  Require(EvidenceContains(seeded_result, "row_mga_recheck_evidence", "required"),
          "ODF-113 graph row MGA recheck proof missing");
  Require(EvidenceContains(seeded_result,
                           "row_security_recheck_evidence",
                           "required"),
          "ODF-113 graph row security recheck proof missing");

  auto bidirectional = BaseGraphRequest();
  bidirectional.bidirectional_start_vertex_id = "A";
  bidirectional.bidirectional_end_vertex_id = "B";
  bidirectional.max_depth = 4;
  const auto bidirectional_result = api::EngineGraphQuery(bidirectional);
  RequirePhysicalProviderHygiene(bidirectional_result,
                                 "ODF-113 graph bidirectional search");
  Require(RowField(bidirectional_result, 2, "path") == "A->C->B",
          "ODF-113 graph bidirectional path changed");
  Require(EvidenceContains(bidirectional_result,
                           "graph_bidirectional_search",
                           "two_sided_meet"),
          "ODF-113 graph bidirectional proof missing");

  auto vector_fusion = BaseGraphRequest();
  vector_fusion.fusion_source_kind = api::EngineGraphFusionSourceKind::kVector;
  vector_fusion.fused_candidate_seed_vertex_ids = {"C"};
  vector_fusion.max_depth = 1;
  const auto vector_fusion_result = api::EngineGraphQuery(vector_fusion);
  RequirePhysicalProviderHygiene(vector_fusion_result,
                                 "ODF-113 graph vector fusion");
  Require(EvidenceContains(vector_fusion_result,
                           "graph_vector_search_fusion",
                           "candidate_seed_intersection_applied"),
          "ODF-113 graph vector fusion proof missing");

  auto search_fusion = BaseGraphRequest();
  search_fusion.fusion_source_kind = api::EngineGraphFusionSourceKind::kSearch;
  search_fusion.fused_candidate_seed_vertex_ids = {"A"};
  search_fusion.max_depth = 1;
  const auto search_fusion_result = api::EngineGraphQuery(search_fusion);
  RequirePhysicalProviderHygiene(search_fusion_result,
                                 "ODF-113 graph search fusion");
  Require(EvidenceContains(search_fusion_result,
                           "graph_search_fusion",
                           "candidate_seed_intersection_applied"),
          "ODF-113 graph search fusion proof missing");

  ScenarioEvidence scenario;
  scenario.name = "graph_physical_provider_indexed_adjacency_frontier_fusion";
  scenario.family = "graph";
  scenario.route = "odf113.local.graph.provider";
  scenario.rows_returned = seeded_result.result_shape.rows.size() +
                           bidirectional_result.result_shape.rows.size() +
                           vector_fusion_result.result_shape.rows.size() +
                           search_fusion_result.result_shape.rows.size();
  AddProof(&scenario, "graph_seed_index", "vertex_property_index");
  AddProof(&scenario, "graph_adjacency_store", "compressed_adjacency_pages");
  AddProof(&scenario,
           "graph_frontier_batching",
           EvidenceIdContaining(seeded_result,
                                "graph_frontier_batching",
                                "batches="));
  AddProof(&scenario, "graph_cycle_policy", "visited_set");
  AddProof(&scenario, "graph_bidirectional_search", "two_sided_meet");
  AddProof(&scenario,
           "graph_vector_search_fusion",
           "candidate_seed_intersection_applied");
  AddProof(&scenario,
           "graph_search_fusion",
           "candidate_seed_intersection_applied");
  AddProof(&scenario, "row_mga_recheck_evidence", "required");
  AddProof(&scenario, "row_security_recheck_evidence", "required");
  AddProof(&scenario, "visible_rows_scanned", "0");
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence TimeSeriesPhysicalProviderScenario() {
  const auto result = api::EngineTimeSeriesAppend(BaseTimeSeriesRequest());
  RequirePhysicalProviderHygiene(result, "ODF-113 time-series append");
  Require(result.dml_summary.rows_changed == 6,
          "ODF-113 time-series row-change count changed");
  Require(RowMatches(result,
                     {{"row_kind", "bucket"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"bucket_end_ns", "60"},
                      {"late_arrival_count", "1"}}),
          "ODF-113 time-series bucket row missing");
  Require(RowMatches(result,
                     {{"row_kind", "column_page"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"metric_name", "temp"},
                      {"column_layout", "metric_value_columnar_page"},
                      {"values", "20.000000,22.000000,18.000000"}}),
          "ODF-113 time-series columnar metric page missing");
  Require(RowMatches(result,
                     {{"row_kind", "summary"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"metric_name", "temp"},
                      {"min", "18.000000"},
                      {"max", "22.000000"},
                      {"count", "3"},
                      {"sum", "60.000000"}}),
          "ODF-113 time-series summary row changed");
  Require(RowMatches(result,
                     {{"row_kind", "rollup"},
                      {"rollup_interval_ns", "120"},
                      {"rollup_start_ns", "0"},
                      {"meta_key", "host=a;region=east"},
                      {"metric_name", "temp"},
                      {"aggregate", "min_max_count_sum"},
                      {"count", "4"},
                      {"sum", "90.000000"}}),
          "ODF-113 time-series rollup row changed");
  Require(RowMatches(result,
                     {{"row_kind", "late_arrival_delta"},
                      {"timestamp_ns", "5"},
                      {"metric_name", "temp"},
                      {"late_path", "delta_page_merge_reopen_bucket"},
                      {"merge_policy", "delta_merge_reopen"}}),
          "ODF-113 time-series late-arrival delta merge row missing");
  Require(EvidenceContains(result,
                           "time_series_physical_access",
                           "local_time_meta_bucket_provider"),
          "ODF-113 time-series physical provider proof missing");
  Require(EvidenceContains(result,
                           "time_series_columnar_metric_pages",
                           "metric_pages="),
          "ODF-113 time-series columnar page proof missing");
  Require(EvidenceContains(result,
                           "time_series_summary_maintenance",
                           "min_max_count_sum"),
          "ODF-113 time-series summary proof missing");
  Require(EvidenceContains(result,
                           "time_series_rollup_materialization",
                           "rows="),
          "ODF-113 time-series rollup proof missing");
  Require(EvidenceContains(result,
                           "time_series_late_arrival_policy",
                           "delta_rows=1"),
          "ODF-113 time-series late-arrival proof missing");
  Require(EvidenceContains(result,
                           "mga_finality_authority",
                           "engine_transaction_inventory"),
          "ODF-113 time-series MGA finality proof missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-113 time-series row MGA recheck proof missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-113 time-series row security recheck proof missing");

  ScenarioEvidence scenario;
  scenario.name = "time_series_physical_provider_buckets_columnar_rollups_late_merge";
  scenario.family = "time_series";
  scenario.route = "odf113.local.time_series.provider";
  scenario.rows_changed = result.dml_summary.rows_changed;
  scenario.rows_returned = result.result_shape.rows.size();
  AddProof(&scenario,
           "time_series_physical_access",
           "local_time_meta_bucket_provider");
  AddProof(&scenario,
           "time_series_bucket_selection",
           EvidenceIdContaining(result,
                                "time_series_bucket_selection",
                                "time_bucket_duration_ns=60"));
  AddProof(&scenario,
           "time_series_columnar_metric_pages",
           EvidenceIdContaining(result,
                                "time_series_columnar_metric_pages",
                                "metric_pages="));
  AddProof(&scenario,
           "time_series_summary_maintenance",
           EvidenceIdContaining(result,
                                "time_series_summary_maintenance",
                                "min_max_count_sum"));
  AddProof(&scenario,
           "time_series_rollup_materialization",
           EvidenceIdContaining(result,
                                "time_series_rollup_materialization",
                                "rows="));
  AddProof(&scenario,
           "time_series_late_arrival_policy",
           EvidenceIdContaining(result,
                                "time_series_late_arrival_policy",
                                "delta_rows=1"));
  AddProof(&scenario, "mga_finality_authority", "engine_transaction_inventory");
  AddProof(&scenario, "row_mga_recheck_evidence", "required");
  AddProof(&scenario, "row_security_recheck_evidence", "required");
  AddProof(&scenario, "visible_rows_scanned", "0");
  FinalizeScenario(&scenario);
  return scenario;
}

template <typename TProof>
void MutateDescriptorScan(TProof* proof) {
  proof->provider_contract.descriptor_visibility.descriptor_scan_selected = true;
}

template <typename TProof>
void MutateBehaviorStoreScan(TProof* proof) {
  proof->provider_contract.descriptor_visibility.behavior_store_scan_selected = true;
}

template <typename TProof>
void MutateParserFinalityAuthority(TProof* proof) {
  proof->provider_contract.mga_recheck.parser_claims_transaction_finality_authority =
      true;
}

template <typename TProof>
void MutateProviderFinalityAuthority(TProof* proof) {
  proof->provider_contract.mga_recheck.provider_claims_transaction_finality_authority =
      true;
}

template <typename TProof>
void MutateWriteAheadFinalityAuthority(TProof* proof) {
  proof->provider_contract.mga_recheck
      .write_ahead_log_claims_transaction_finality_authority = true;
}

void AddFailClosedProof(ScenarioEvidence* scenario,
                        std::string family,
                        std::string case_name,
                        std::string diagnostic) {
  AddProof(scenario,
           std::move(family) + "." + std::move(case_name),
           std::move(diagnostic));
}

template <typename TResult>
void RequireFailClosedDiagnostic(const TResult& result,
                                 std::string_view diagnostic,
                                 std::string_view case_name) {
  Require(!result.ok, "ODF-113 fail-closed mutation was accepted");
  if (!DiagnosticContains(result, diagnostic)) {
    if (!result.diagnostics.empty()) {
      std::cerr << "Expected diagnostic " << diagnostic << " in " << case_name
                << " but saw " << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail("ODF-113 fail-closed diagnostic changed");
  }
}

void RunKvRefusalMatrix(const std::filesystem::path& database_path,
                        ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    api::EngineKeyValueMultiGetRequest request;
    request.context = Context(database_path, 113);
    request.keys = {"kv:1"};
    request.physical_proof = KvExactProof();
    mutate(&request.physical_proof);
    const auto result = api::EngineKeyValueMultiGet(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "key_value", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineKeyValuePhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineKeyValuePhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineKeyValuePhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineKeyValuePhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineKeyValuePhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

void RunDocumentRefusalMatrix(const std::filesystem::path& database_path,
                              ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    api::EngineDocumentFindRequest request;
    request.context = Context(database_path, 113);
    request.path = "customer.id";
    request.equals_value = "A1";
    request.projected_paths = {"customer.id"};
    request.physical_proof = DocumentProof();
    mutate(&request.physical_proof);
    const auto result = api::EngineDocumentFind(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "document", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineDocumentPhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineDocumentPhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineDocumentPhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineDocumentPhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineDocumentPhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

void RunSearchRefusalMatrix(ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    api::EngineSearchQueryRequest request;
    request.context.database_path = "odf113-search-refusal-transient";
    request.context.local_transaction_id = 113;
    request.context.database_uuid.canonical = "odf113-search-refusal-database";
    request.context.transaction_uuid.canonical = "odf113-search-refusal-transaction";
    request.context.security_context_present = true;
    request.query_text = "alpha";
    request.top_k = 2;
    request.document_corpus = SearchCorpus();
    request.physical_proof = SearchProof();
    mutate(&request.physical_proof);
    const auto result = api::EngineSearchQuery(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "search", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineSearchPhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineSearchPhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineSearchPhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineSearchPhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineSearchPhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

void RunVectorRefusalMatrix(ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    auto request = BaseVectorRequest();
    mutate(&request.physical_proof);
    const auto result = api::EngineVectorSearch(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "vector", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineVectorPhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineVectorPhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineVectorPhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineVectorPhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineVectorPhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

void RunGraphRefusalMatrix(ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    auto request = BaseGraphRequest();
    request.seed_label = "person";
    request.seed_property_key = "tenant";
    request.seed_property_value = "blue";
    request.max_depth = 1;
    mutate(&request.physical_proof);
    const auto result = api::EngineGraphQuery(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "graph", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineGraphPhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineGraphPhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineGraphPhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineGraphPhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineGraphPhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

void RunTimeSeriesRefusalMatrix(ScenarioEvidence* scenario) {
  const auto run = [&](auto mutate,
                       const char* case_name,
                       const char* diagnostic) {
    auto request = BaseTimeSeriesRequest();
    mutate(&request.physical_proof);
    const auto result = api::EngineTimeSeriesAppend(request);
    RequireFailClosedDiagnostic(result, diagnostic, case_name);
    AddFailClosedProof(scenario, "time_series", case_name, diagnostic);
  };
  run(MutateDescriptorScan<api::EngineTimeSeriesPhysicalProof>,
      "descriptor_scan",
      api::kNoSqlProviderDescriptorScanNotPhysicalProvider);
  run(MutateBehaviorStoreScan<api::EngineTimeSeriesPhysicalProof>,
      "behavior_store_scan",
      api::kNoSqlProviderBehaviorScanNotPhysicalProvider);
  run(MutateParserFinalityAuthority<api::EngineTimeSeriesPhysicalProof>,
      "parser_finality_authority",
      api::kNoSqlProviderParserFinalityAuthorityRefused);
  run(MutateProviderFinalityAuthority<api::EngineTimeSeriesPhysicalProof>,
      "provider_finality_authority",
      api::kNoSqlProviderFinalityAuthorityRefused);
  run(MutateWriteAheadFinalityAuthority<api::EngineTimeSeriesPhysicalProof>,
      "write_ahead_finality_authority",
      api::kNoSqlProviderWriteAheadFinalityAuthorityRefused);
}

ScenarioEvidence PhysicalProviderFailClosedMatrixScenario() {
  TempDatabase kv_database("kv_refusal");
  SeedCrudTransaction(kv_database.path);
  TempDatabase document_database("document_refusal");
  SeedCrudTransaction(document_database.path);

  ScenarioEvidence scenario;
  scenario.name = "nosql_physical_provider_fail_closed_matrix";
  scenario.family = "all_nosql_families";
  scenario.route = "provider_contract.fail_closed";
  scenario.fail_closed = true;
  RunKvRefusalMatrix(kv_database.path, &scenario);
  RunDocumentRefusalMatrix(document_database.path, &scenario);
  RunSearchRefusalMatrix(&scenario);
  RunVectorRefusalMatrix(&scenario);
  RunGraphRefusalMatrix(&scenario);
  RunTimeSeriesRefusalMatrix(&scenario);
  AddProof(&scenario, "fallback_selected", "false");
  AddProof(&scenario, "descriptor_scan_fallback_accepted", "false");
  AddProof(&scenario, "behavior_store_scan_fallback_accepted", "false");
  AddProof(&scenario, "authority_source", "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  return scenario;
}

void WriteJson(const std::vector<ScenarioEvidence>& scenarios) {
  const std::filesystem::path output_path = ODF113_OUTPUT_JSON;
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream json(output_path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(json), "ODF-113 could not open JSON output file");
  json << "{\n";
  json << "  \"gate\": \"optimizer_deficiency_odf_113_gate\",\n";
  json << "  \"odf\": \"ODF-113\",\n";
  json << "  \"scenario_count\": " << scenarios.size() << ",\n";
  json << "  \"benchmark_clean\": true,\n";
  json << "  \"live_speed_numbers\": false,\n";
  json << "  \"families\": [\"key_value\", \"document\", \"search\", \"vector\", \"graph\", \"time_series\"],\n";
  json << "  \"scenarios\": [\n";
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto& scenario = scenarios[index];
    json << "    {\n";
    json << "      \"name\": " << Quote(scenario.name) << ",\n";
    json << "      \"family\": " << Quote(scenario.family) << ",\n";
    json << "      \"route_provider_id\": " << Quote(scenario.route) << ",\n";
    json << "      \"rows_changed\": " << scenario.rows_changed << ",\n";
    json << "      \"rows_returned\": " << scenario.rows_returned << ",\n";
    json << "      \"no_descriptor_scan\": "
         << (scenario.no_descriptor_scan ? "true" : "false") << ",\n";
    json << "      \"no_behavior_store_scan\": "
         << (scenario.no_behavior_store_scan ? "true" : "false") << ",\n";
    json << "      \"benchmark_clean\": "
         << (scenario.benchmark_clean ? "true" : "false") << ",\n";
    json << "      \"live_speed_numbers\": "
         << (scenario.live_speed_numbers ? "true" : "false") << ",\n";
    json << "      \"fail_closed\": "
         << (scenario.fail_closed ? "true" : "false") << ",\n";
    json << "      \"result_hash\": " << Quote(scenario.result_hash) << ",\n";
    json << "      \"proofs\": [\n";
    for (std::size_t proof_index = 0; proof_index < scenario.proofs.size();
         ++proof_index) {
      const auto& [key, value] = scenario.proofs[proof_index];
      json << "        {\"key\": " << Quote(key) << ", \"value\": "
           << Quote(value) << "}";
      if (proof_index + 1 != scenario.proofs.size()) {
        json << ',';
      }
      json << '\n';
    }
    json << "      ]\n";
    json << "    }";
    if (index + 1 != scenarios.size()) {
      json << ',';
    }
    json << '\n';
  }
  json << "  ]\n";
  json << "}\n";
  json.flush();
  Require(static_cast<bool>(json), "ODF-113 JSON output write failed");

  std::ifstream verify(output_path, std::ios::binary);
  std::ostringstream buffer;
  buffer << verify.rdbuf();
  const std::string text = buffer.str();
  for (const auto token : {"docs" "/execution-plans",
                           "docs" "/findings",
                           "public_release_evidence",
                           "docs/reference",
                           "execution_plan",
                           "findings",
                           "contracts",
                           "references"}) {
    Require(text.find(token) == std::string::npos,
            "ODF-113 JSON leaked forbidden documentation token");
  }
}

}  // namespace

int main() {
  std::vector<ScenarioEvidence> scenarios;
  scenarios.push_back(KvPhysicalProviderScenario());
  scenarios.push_back(DocumentPhysicalProviderScenario());
  scenarios.push_back(SearchPhysicalProviderScenario());
  scenarios.push_back(VectorPhysicalProviderScenario());
  scenarios.push_back(GraphPhysicalProviderScenario());
  scenarios.push_back(TimeSeriesPhysicalProviderScenario());
  scenarios.push_back(PhysicalProviderFailClosedMatrixScenario());
  WriteJson(scenarios);
  return EXIT_SUCCESS;
}
