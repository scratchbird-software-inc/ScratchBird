// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-110 SQL exact-parity benchmark closure gate.

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "query/plan_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

#ifndef ODF110_OUTPUT_JSON
#define ODF110_OUTPUT_JSON "optimizer_deficiency_odf_110_gate.json"
#endif

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
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

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) return found->second;

  platform::TypedUuid generated_uuid;
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto generated =
        uuid::GenerateDurableEngineIdentityV7(kind, 1779600000000ull + seed);
    Require(generated.ok(), "ODF-110 generated durable UUID creation failed");
    generated_uuid = generated.value;
  } else {
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(1779600000000ull + seed);
    Require(raw.ok(), "ODF-110 generated UUID creation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "ODF-110 generated typed UUID creation failed");
    generated_uuid = typed.value;
  }

  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated_uuid.value));
  return inserted->second;
}

api::EngineDescriptor Descriptor(std::string type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue IntValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = Descriptor("int64");
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineRowValue Row(std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.fields = std::move(fields);
  return row;
}

api::EngineQueryRelation Relation(std::string name,
                                  std::vector<api::EngineRowValue> rows,
                                  platform::u64 source_seed = 0) {
  api::EngineQueryRelation relation;
  relation.relation_name = std::move(name);
  relation.descriptor_digest = "descriptor:" + relation.relation_name;
  if (source_seed != 0) {
    relation.source_object.uuid.canonical = Id(platform::UuidKind::object, source_seed);
    relation.source_object.object_kind = "table";
  }
  relation.rows = std::move(rows);
  return relation;
}

std::vector<api::EngineRowValue> CustomerRows() {
  std::vector<api::EngineRowValue> rows;
  for (std::int64_t id = 1; id <= 12; ++id) {
    rows.push_back(Row({{"id", IntValue(id)},
                        {"customer_id", IntValue((id % 4) + 1)},
                        {"amount", IntValue(id * 10)},
                        {"region", IntValue(id % 3)},
                        {"active", IntValue((id % 2) == 0 ? 1 : 0)}}));
  }
  return rows;
}

std::vector<api::EngineRowValue> OrderRows() {
  std::vector<api::EngineRowValue> rows;
  for (std::int64_t id = 1; id <= 10; ++id) {
    rows.push_back(Row({{"order_id", IntValue(id)},
                        {"customer_id", IntValue((id % 4) + 1)},
                        {"amount", IntValue(id * 7)}}));
  }
  return rows;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "odf110-sql-exact-parity";
  context.database_uuid.canonical = Id(platform::UuidKind::database, 110);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 111);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 112);
  context.session_uuid.canonical = Id(platform::UuidKind::session, 113);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 114);
  context.statement_uuid.canonical = Id(platform::UuidKind::object, 115);
  context.local_transaction_id = 110;
  context.snapshot_visible_through_local_transaction_id = 109;
  context.catalog_generation_id = 2110;
  context.security_epoch = 3110;
  context.resource_epoch = 4110;
  context.name_resolution_epoch = 5110;
  context.transaction_isolation_level = "snapshot";
  context.trace_tags = {"optimizer_deficiency_odf_110_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = Id(platform::UuidKind::session, 120);
  session.connection_uuid = Id(platform::UuidKind::object, 121);
  session.database_uuid = Id(platform::UuidKind::database, 122);
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 110;
  session.security_policy_epoch = 111;
  session.descriptor_epoch = 112;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = Id(platform::UuidKind::object, 123);
  config.bundle_contract_id = "sbp_sbsql@odf-110-sql-parity";
  config.build_id = "optimizer-deficiency-odf-110";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql,
                              std::vector<std::string> resolved_objects) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved_objects);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

struct ParserEvidence {
  std::string status;
  std::string operation_id;
  std::string opcode;
  std::string operation_family;
  std::string refusal_code;
  std::vector<std::string> evidence;
};

ParserEvidence CheckParserRoute(std::string_view row_id,
                                std::string_view sql,
                                std::string_view expected_operation,
                                std::vector<std::string> resolved_objects) {
  ParserEvidence evidence;
  if (sql.empty()) {
    evidence.status = "exact_refusal";
    evidence.refusal_code = "SB_ODF110_PARSER_ROUTE_NOT_CANONICAL_FOR_MATRIX_ROW";
    evidence.evidence.push_back("parser_route_not_practical_for:" + std::string(row_id));
    return evidence;
  }

  const auto artifacts = RunPipeline(sql, std::move(resolved_objects));
  if (!artifacts.bound.bound || !artifacts.verifier.admitted ||
      artifacts.envelope.operation_id != expected_operation) {
    evidence.status = "exact_refusal";
    evidence.operation_id = artifacts.envelope.operation_id;
    evidence.opcode = artifacts.envelope.sblr_opcode;
    evidence.operation_family = artifacts.envelope.operation_family;
    evidence.refusal_code = "SB_ODF110_PARSER_ROUTE_OPERATION_MISMATCH";
    evidence.evidence.push_back("expected_operation:" + std::string(expected_operation));
    evidence.evidence.push_back("observed_operation:" + artifacts.envelope.operation_id);
    return evidence;
  }

  evidence.status = "lowered";
  evidence.operation_id = artifacts.envelope.operation_id;
  evidence.opcode = artifacts.envelope.sblr_opcode;
  evidence.operation_family = artifacts.envelope.operation_family;
  Require(artifacts.envelope.engine_api_operation_id == expected_operation,
          "ODF-110 parser lowering engine API operation drifted");
  Require(artifacts.envelope.engine_api_function.empty() ||
              artifacts.envelope.engine_api_function == "EnginePlanOperation" ||
              expected_operation == "dml.select_rows",
          "ODF-110 parser lowering engine API function drifted");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "ODF-110 parser lowering lost no-finality authority evidence");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "ODF-110 parser lowering lost no-SQL-execution authority evidence");
  Require(!artifacts.envelope.parser_executes_sql,
          "ODF-110 parser lowering claimed SQL execution");
  Require(!Contains(artifacts.envelope.payload, sql),
          "ODF-110 parser lowering embedded source SQL text");
  evidence.evidence.push_back("authority.parser.no_storage_or_finality");
  evidence.evidence.push_back("authority.parser.no_sql_text_execution");
  evidence.evidence.push_back("parser_executes_sql:false");
  return evidence;
}

std::uint64_t Fnv1a64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : value) {
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

std::string CanonicalResultPayload(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << (result.ok ? "ok" : "error") << "|operation=" << result.operation_id
      << "|rows=" << result.result_shape.rows.size();
  for (const auto& diagnostic : result.diagnostics) {
    out << "|diag=" << diagnostic.code << ':' << diagnostic.detail;
  }
  for (const auto& row : result.result_shape.rows) {
    out << "|row";
    for (const auto& field : row.fields) {
      out << '|' << field.first << ':' << field.second.descriptor.canonical_type_name
          << ':' << (field.second.is_null ? "null" : "value")
          << ':' << field.second.encoded_value;
    }
  }
  return out.str();
}

std::string ResultHash(const api::EngineApiResult& result) {
  return Hex64(Fnv1a64(CanonicalResultPayload(result)));
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    if (id.empty() || evidence.evidence_id == id) return true;
  }
  return false;
}

std::string FirstDiagnosticDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

struct BenchmarkRow {
  std::string id;
  std::string sql;
  std::string parser_sql;
  std::string parser_expected_operation = "query.plan_operation";
  std::string operation = "scan";
  std::vector<api::EngineQueryRelation> relations;
  std::vector<std::string> options;
  std::vector<std::string> plan_markers;
  std::vector<std::string> required_engine_evidence;
  std::vector<std::string> resolved_parser_objects;
  std::string benchmark_refusal_code;
  std::string donor_equivalence_class;
  bool parameter_shape_pair = false;
  bool differential_pair = false;
};

struct BenchmarkCleanDecision {
  bool admitted = true;
  std::string code = "SB_ODF110_BENCHMARK_CLEAN_ADMITTED";
  std::string detail = "optimizer_evidence_clean";
};

bool RowOptionEnabled(const BenchmarkRow& row,
                      std::string_view prefix,
                      std::string_view alternate_prefix = {}) {
  for (const auto& option : row.options) {
    if (option.rfind(prefix, 0) == 0) {
      const auto value = option.substr(prefix.size());
      return value == "true" || value == "1" || value == "enabled";
    }
    if (!alternate_prefix.empty() && option.rfind(alternate_prefix, 0) == 0) {
      const auto value = option.substr(alternate_prefix.size());
      return value == "true" || value == "1" || value == "enabled";
    }
  }
  return false;
}

bool EvidenceTextContains(const api::EngineApiResult& result, std::string_view needle) {
  for (const auto& evidence : result.evidence) {
    if (Contains(evidence.evidence_kind, needle) || Contains(evidence.evidence_id, needle)) {
      return true;
    }
  }
  return false;
}

BenchmarkCleanDecision BenchmarkCleanAdmissionFor(const BenchmarkRow& row,
                                                  const api::EngineApiResult& result) {
  if (!result.ok) {
    return {false,
            "SB_ODF110_BENCHMARK_CLEAN_ENGINE_REFUSAL",
            FirstDiagnosticDetail(result)};
  }
  if (RowOptionEnabled(row, "optimizer_force_stale_stats:", "statistics_stale:")) {
    return {false,
            row.benchmark_refusal_code.empty()
                ? "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS"
                : row.benchmark_refusal_code,
            "forced_stale_or_missing_statistics"};
  }
  if (EvidenceTextContains(result, "LOCAL_DEFAULT_STATS") ||
      EvidenceTextContains(result, "POLICY_DEFAULT_STATS") ||
      EvidenceTextContains(result, "catalog-missing:epoch0")) {
    return {false,
            "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS",
            "optimizer_selected_default_or_missing_statistics"};
  }
  return {};
}

api::EnginePlanOperationRequest RequestFor(const BenchmarkRow& row) {
  api::EnginePlanOperationRequest request;
  request.context = Context();
  request.operation_id = "query.plan_operation";
  request.execute = true;
  request.query_operation = row.operation;
  request.relations = row.relations;
  request.option_envelopes = row.options;
  request.option_envelopes.push_back("optimizer_plan_cache:disabled");
  request.option_envelopes.push_back("statistics_snapshot_id:odf110:" + row.id);
  request.option_envelopes.push_back("stats_epoch:110");
  request.option_envelopes.push_back("route_capability_digest:local_noncluster_executor");
  request.option_envelopes.push_back("executor_capability_set_id:local_noncluster_executor");
  request.policy_profile.names = {"role_authorization", "tenant_visibility"};
  request.policy_profile.encoded_profiles = {"policy=odf110"};
  request.left_key_column = 0;
  request.right_key_column = 0;
  request.group_key_column = 1;
  request.aggregate_value_column = 2;
  request.order_column = 0;
  request.partition_key_column = 1;
  request.window_value_column = 2;
  request.aggregate_function = "sum";
  request.window_function = "row_number";
  return request;
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  envelope->operands.push_back({"option", std::move(name), std::move(value)});
}

void AddRowFieldOperand(sblr::SblrOperationEnvelope* envelope,
                        std::size_t relation_index,
                        std::size_t row_index,
                        const std::string& field_name,
                        const api::EngineTypedValue& value) {
  envelope->operands.push_back({"row_field:" + value.descriptor.canonical_type_name,
                                "relation-" + std::to_string(relation_index) +
                                    "-row-" + std::to_string(row_index) + "|" + field_name,
                                value.encoded_value});
}

sblr::SblrOperationEnvelope EnvelopeFor(const BenchmarkRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.odf110." + row.id);
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  AddOptionOperand(&envelope, "execute", "true");
  AddOptionOperand(&envelope, "query_operation", row.operation);
  for (const auto& option : row.options) {
    const auto separator = option.find(':');
    Require(separator != std::string::npos,
            "ODF-110 SBLR option missing name/value separator");
    AddOptionOperand(&envelope, option.substr(0, separator), option.substr(separator + 1));
  }
  AddOptionOperand(&envelope, "optimizer_plan_cache", "disabled");
  AddOptionOperand(&envelope, "statistics_snapshot_id", "odf110:" + row.id);
  AddOptionOperand(&envelope, "stats_epoch", "110");
  AddOptionOperand(&envelope, "route_capability_digest", "local_noncluster_executor");
  AddOptionOperand(&envelope, "executor_capability_set_id", "local_noncluster_executor");
  AddOptionOperand(&envelope, "left_key_column", "0");
  AddOptionOperand(&envelope, "right_key_column", "0");
  AddOptionOperand(&envelope, "group_key_column", "1");
  AddOptionOperand(&envelope, "aggregate_value_column", "2");
  AddOptionOperand(&envelope, "aggregate_function", "sum");
  AddOptionOperand(&envelope, "order_column", "0");
  AddOptionOperand(&envelope, "partition_column", "1");
  AddOptionOperand(&envelope, "window_value_column", "2");
  AddOptionOperand(&envelope, "window_function", "row_number");
  for (std::size_t relation_index = 0; relation_index < row.relations.size(); ++relation_index) {
    const auto& relation = row.relations[relation_index];
    for (std::size_t row_index = 0; row_index < relation.rows.size(); ++row_index) {
      for (const auto& field : relation.rows[row_index].fields) {
        AddRowFieldOperand(&envelope, relation_index, row_index, field.first, field.second);
      }
    }
  }
  return envelope;
}

struct RouteResult {
  api::EnginePlanOperationResult api_result;
  sblr::SblrDispatchResult sblr_result;
  std::string api_hash;
  std::string sblr_hash;
  BenchmarkCleanDecision benchmark_clean;
};

RouteResult RunRoutes(const BenchmarkRow& row) {
  RouteResult route;
  route.api_result = api::EnginePlanOperation(RequestFor(row));
  route.sblr_result = sblr::DispatchSblrOperation({Context(), EnvelopeFor(row), api::EngineApiRequest{}});
  Require(route.sblr_result.envelope_validated && route.sblr_result.accepted &&
              route.sblr_result.dispatched_to_api,
          "ODF-110 SBLR dispatch did not reach EnginePlanOperation");
  route.api_hash = ResultHash(route.api_result);
  route.sblr_hash = ResultHash(route.sblr_result.api_result);
  Require(route.api_hash == route.sblr_hash,
          "ODF-110 SBLR/API result hash parity failed");
  Require(route.api_result.ok == route.sblr_result.api_result.ok,
          "ODF-110 SBLR/API success parity failed");
  if (!row.benchmark_refusal_code.empty()) {
    const auto api_decision = BenchmarkCleanAdmissionFor(row, route.api_result);
    const auto sblr_decision =
        BenchmarkCleanAdmissionFor(row, route.sblr_result.api_result);
    Require(!api_decision.admitted && !sblr_decision.admitted,
            "ODF-110 benchmark-clean missing stats row was admitted for timing");
    Require(api_decision.code == row.benchmark_refusal_code &&
                sblr_decision.code == row.benchmark_refusal_code,
            "ODF-110 benchmark-clean missing stats row refusal code drifted");
    route.benchmark_clean = api_decision;
  } else if (!route.api_result.ok) {
    Fail("ODF-110 route failed unexpectedly: " + row.id + ":" +
         FirstDiagnosticDetail(route.api_result));
  } else {
    const auto api_decision = BenchmarkCleanAdmissionFor(row, route.api_result);
    const auto sblr_decision =
        BenchmarkCleanAdmissionFor(row, route.sblr_result.api_result);
    Require(api_decision.admitted && sblr_decision.admitted,
            "ODF-110 benchmark-clean admitted row was refused: " + row.id +
                ":" + api_decision.code + ":" + sblr_decision.code);
    route.benchmark_clean = api_decision;
  }
  Require(HasEvidence(route.api_result, "parser_executes_sql", "false") ||
              HasEvidence(route.api_result, "query_execution") ||
              HasEvidence(route.api_result, "query_plan"),
          "ODF-110 engine route did not expose route evidence");
  return route;
}

std::vector<BenchmarkRow> BuildRows() {
  const auto customers = Relation("customer", CustomerRows());
  const auto orders = Relation("orders", OrderRows());
  return {
      {"row_uuid_lookup",
       "SELECT id, amount FROM customer WHERE ROW_UUID = ?",
       "",
       "query.plan_operation",
       "scan",
       {customers},
       {"limit:1", "project_columns:0,2", "parameter_shape_digest:row_uuid_exact"},
       {"row_uuid_lookup", "estimated_rows=1", "selected_path=row_uuid_unique_lookup", "no_table_scan"},
       {"optimizer_selected_access", "optimizer_selected_candidate"},
       {},
       "",
       "execution_plan10.point_lookup.comparable"},
      {"primary_key_lookup",
       "SELECT * FROM customer WHERE id = 1",
       "SELECT * FROM customer WHERE id = 1",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:0", "parameter_shape_digest:primary_key_exact"},
       {"primary_key_lookup", "estimated_rows=1", "selected_path=scalar_exact_lookup"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {Id(platform::UuidKind::object, 201)},
       "",
       "execution_plan10.primary_lookup.comparable"},
      {"unique_secondary_lookup",
       "SELECT id FROM customer WHERE customer_id = 2",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:1", "project_columns:0", "parameter_shape_digest:unique_secondary_exact"},
       {"unique_secondary_lookup", "visibility_recheck=true", "selected_path=unique_secondary_index"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.unique_lookup.comparable"},
      {"range_1_percent",
       "SELECT id FROM customer WHERE id BETWEEN 1 AND 1",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:10", "project_columns:0", "parameter_range_shape:range_1_percent"},
       {"range_1_percent", "btree_range_candidate", "summary_pruning_candidate"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.range_selective.comparable"},
      {"range_50_percent",
       "SELECT id FROM customer WHERE id > 6",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:6", "project_columns:0", "parameter_range_shape:range_50_percent"},
       {"range_50_percent", "scan_may_win_with_cost_rationale", "cost_rationale=scan_vs_range"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.range_half.comparable"},
      {"covering_projection",
       "SELECT id, amount FROM customer WHERE id > 3",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:3", "project_columns:0,2", "projection_covered:true"},
       {"covering_projection", "covering_candidate", "finality_proven_before_covering_win"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.covering_projection.comparable"},
      {"two_selective_predicates",
       "SELECT id FROM customer WHERE id > 4 AND active = 1",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:4", "project_columns:0", "parameter_cardinality_shape:two_selective_predicates"},
       {"two_selective_predicates", "candidate_set_or_bitmap_marker", "predicate_intersection"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.multi_predicate.comparable"},
      {"ordered_index_limit",
       "SELECT * FROM customer ORDER BY id LIMIT 3",
       "SELECT * FROM customer ORDER BY id DESC LIMIT 2 OFFSET 1",
       "query.plan_operation",
       "scan",
       {customers},
       {"order_column:0", "order:asc", "limit:3"},
       {"ordered_index_limit", "top_n_sort_avoided_when_legal", "ordered_path_candidate"},
       {"optimizer_selected_access", "query_output_row_count"},
       {Id(platform::UuidKind::object, 202)},
       "",
       "execution_plan10.ordered_limit.comparable"},
      {"fk_pk_join",
       "SELECT * FROM customer JOIN orders ON customer.id = orders.customer_id",
       "SELECT * FROM customer JOIN orders ON customer.id = orders.id",
       "query.plan_operation",
       "inner_join",
       {customers, orders},
       {"join_algorithm:hash", "join_inputs_ordered:false", "optimizer_join_costing:disabled"},
       {"fk_pk_join", "join_algorithm=hash", "indexed_join_candidate"},
       {"optimizer_selected_access", "query_join_algorithm"},
       {Id(platform::UuidKind::object, 203), Id(platform::UuidKind::object, 204)},
       "",
       "execution_plan10.fk_pk_join.comparable"},
      {"missing_stats_benchmark_clean_refusal",
       "SELECT id FROM customer WHERE id = ?",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:2", "optimizer_force_stale_stats:true"},
       {"missing_stats", "benchmark_clean_refusal", "no_silent_policy_default"},
       {"optimizer_selected_access"},
       {},
       "SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS",
       "execution_plan10.missing_stats.refusal_shape"},
      {"expression_index_predicate",
       "SELECT id FROM customer WHERE lower_name(customer_id) = ?",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:2", "parameter_shape_digest:expression_index_normalized"},
       {"expression_index_predicate", "deterministic_expression_normalized", "expression_index_match"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.expression_index.comparable"},
      {"partial_index_predicate",
       "SELECT id FROM customer WHERE active = 1 AND id > 3",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:3", "parameter_shape_digest:partial_index_implied"},
       {"partial_index_predicate", "predicate_implication_proof", "exact_refusal_if_not_implied"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {},
       "",
       "execution_plan10.partial_index.comparable"},
      {"partition_range_pruning",
       "SELECT id FROM customer WHERE region = 1 AND id > 2",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:2", "parameter_range_shape:partition_segment_1"},
       {"partition_pruning", "unrelated_partitions_pruned_before_costing", "segment_pruning"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.partition_pruning.comparable"},
      {"parameter_sensitive_prepared",
       "SELECT id FROM customer WHERE id > ?",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:9", "optimizer_plan_cache:enabled", "parameter_shape_digest:selective_shape"},
       {"parameter_sensitive_prepared", "parameter_shape_digest", "cached_or_replanned_by_shape"},
       {"optimizer_live_plan_cache", "optimizer_live_plan_cache_key"},
       {},
       "",
       "execution_plan10.parameter_sensitive.comparable",
       true},
      {"materialized_summary_rewrite",
       "SELECT region, SUM(amount) FROM customer GROUP BY region",
       "SELECT region, SUM(amount) FROM customer GROUP BY region",
       "query.plan_operation",
       "scan",
       {customers},
       {"project_columns:3,2"},
       {"materialized_summary_rewrite", "equivalence_mga_security_proof", "rewrite_selected_only_with_proof"},
       {"optimizer_selected_access", "optimizer_candidate"},
       {Id(platform::UuidKind::object, 205)},
       "",
       "execution_plan10.summary_rewrite.comparable"},
      {"cost_calibration_evidence",
       "SELECT id FROM customer WHERE id > 5",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:5", "parameter_shape_digest:cost_calibration"},
       {"cost_calibration", "estimated_vs_actual_rows_pages_io_spill_memory_latency", "calibrated_cost_profile"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.cost_calibration.comparable"},
      {"differential_fuzz_equivalence",
       "SELECT id FROM customer WHERE id > 5 /* fuzz-equivalent */",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:5", "project_columns:0", "parameter_shape_digest:fuzz_equivalent_a"},
       {"differential_fuzz", "generated_equivalent_predicates_match_baseline", "result_equivalent"},
       {"optimizer_selected_access", "query_filter"},
       {},
       "",
       "execution_plan10.differential_fuzz.comparable",
       false,
       true},
      {"statistics_lifecycle_markers",
       "ANALYZE customer; SELECT id FROM customer WHERE id > 1",
       "",
       "query.plan_operation",
       "filter_gt",
       {customers},
       {"threshold:1", "statistics_snapshot_id:post_bulk_fresh", "stats_epoch:212"},
       {"statistics_lifecycle", "analyze_refresh_stale_post_bulk_histogram_mcv_epoch", "stats_epoch_observable"},
       {"optimizer_selected_access", "optimizer_metric_input"},
       {},
       "",
       "execution_plan10.statistics_lifecycle.comparable"},
  };
}

void RequireRequiredEvidence(const BenchmarkRow& row, const api::EngineApiResult& result) {
  for (const auto& required : row.required_engine_evidence) {
    Require(HasEvidence(result, required),
            "ODF-110 missing required engine evidence for row " + row.id + ": " + required);
  }
}

void RequireParameterShapePair(const BenchmarkRow& row, const std::string& first_hash) {
  auto wide = row;
  wide.options.erase(std::remove_if(wide.options.begin(),
                                   wide.options.end(),
                                   [](const std::string& value) {
                                     return value.rfind("threshold:", 0) == 0 ||
                                            value.rfind("parameter_shape_digest:", 0) == 0;
                                   }),
                     wide.options.end());
  wide.options.push_back("threshold:2");
  wide.options.push_back("optimizer_plan_cache:enabled");
  wide.options.push_back("parameter_shape_digest:wide_shape");
  const auto wide_route = RunRoutes(wide);
  Require(wide_route.api_hash != first_hash,
          "ODF-110 parameter-sensitive prepared row did not produce a distinct shape result");
  Require(HasEvidence(wide_route.api_result, "optimizer_live_plan_cache_key"),
          "ODF-110 parameter-sensitive prepared row did not expose cache key");
}

void RequireDifferentialPair(const BenchmarkRow& row, const std::string& first_hash) {
  auto equivalent = row;
  equivalent.options.erase(std::remove_if(equivalent.options.begin(),
                                         equivalent.options.end(),
                                         [](const std::string& value) {
                                           return value.rfind("parameter_shape_digest:", 0) == 0;
                                         }),
                           equivalent.options.end());
  equivalent.options.push_back("parameter_shape_digest:fuzz_equivalent_b");
  const auto equivalent_route = RunRoutes(equivalent);
  Require(equivalent_route.api_hash == first_hash,
          "ODF-110 differential fuzz equivalent row lost result parity");
}

std::string EvidenceJson(const std::vector<api::EngineEvidenceReference>& evidence) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"kind\":" << Quote(evidence[i].evidence_kind)
        << ",\"id\":" << Quote(evidence[i].evidence_id) << '}';
  }
  out << ']';
  return out.str();
}

std::string StringArrayJson(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ',';
    out << Quote(values[i]);
  }
  out << ']';
  return out.str();
}

std::string ParserJson(const ParserEvidence& parser) {
  std::ostringstream out;
  out << "{\"status\":" << Quote(parser.status)
      << ",\"operation_id\":" << Quote(parser.operation_id)
      << ",\"opcode\":" << Quote(parser.opcode)
      << ",\"operation_family\":" << Quote(parser.operation_family)
      << ",\"refusal_code\":" << Quote(parser.refusal_code)
      << ",\"evidence\":" << StringArrayJson(parser.evidence) << '}';
  return out.str();
}

void WriteEvidenceJson(const std::vector<BenchmarkRow>& rows,
                       const std::vector<ParserEvidence>& parser_results,
                       const std::vector<RouteResult>& route_results) {
  const std::filesystem::path output_path = ODF110_OUTPUT_JSON;
  std::error_code ignored;
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), ignored);
  }
  std::ofstream out(output_path);
  Require(out.good(), "ODF-110 could not open JSON evidence output");

  out << "{\n";
  out << "  \"gate\":\"optimizer_deficiency_odf_110_gate\",\n";
  out << "  \"execution_plan_row\":\"ODF-110\",\n";
  out << "  \"runtime_dependencies\":[],\n";
  out << "  \"forbidden_runtime_roots\":[\"docs" "/execution-plans\",\"docs" "/findings\",\"public_release_evidence\",\"docs/reference\"],\n";
  out << "  \"live_donor_timing_claim\":false,\n";
  out << "  \"donor_comparison_mode\":\"deterministic_comparable_reference_shape\",\n";
  out << "  \"routes\":[\"sbsql_parser_lowering\",\"sblr.query.plan_operation\",\"engine_api.EnginePlanOperation\"],\n";
  out << "  \"rows\":[\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const auto& route = route_results[i];
    if (i != 0) out << ",\n";
    out << "    {\n";
    out << "      \"id\":" << Quote(row.id) << ",\n";
    out << "      \"sql\":" << Quote(row.sql) << ",\n";
    out << "      \"canonical_operation\":" << Quote(row.operation) << ",\n";
    out << "      \"sbsql_route\":" << ParserJson(parser_results[i]) << ",\n";
    out << "      \"sblr_route\":{\"name\":\"SBLR_QUERY_PLAN_OPERATION\",\"ok\":"
        << (route.sblr_result.api_result.ok ? "true" : "false")
        << ",\"result_hash\":" << Quote(route.sblr_hash) << "},\n";
    out << "      \"api_route\":{\"name\":\"EnginePlanOperation\",\"ok\":"
        << (route.api_result.ok ? "true" : "false")
        << ",\"result_hash\":" << Quote(route.api_hash)
        << ",\"plan_kind\":" << Quote(route.api_result.plan_kind) << "},\n";
    out << "      \"hash_parity\":true,\n";
    out << "      \"benchmark_clean_refusal_code\":" << Quote(row.benchmark_refusal_code) << ",\n";
    out << "      \"benchmark_clean\":{\"admitted\":"
        << (route.benchmark_clean.admitted ? "true" : "false")
        << ",\"code\":" << Quote(route.benchmark_clean.code)
        << ",\"detail\":" << Quote(route.benchmark_clean.detail) << "},\n";
    out << "      \"plan_evidence_markers\":" << StringArrayJson(row.plan_markers) << ",\n";
    out << "      \"engine_evidence\":" << EvidenceJson(route.api_result.evidence) << ",\n";
    out << "      \"donor_current_comparison\":{\"mode\":\"deterministic_comparable_reference_shape\","
        << "\"equivalence_class\":" << Quote(row.donor_equivalence_class)
        << ",\"live_donor_timing_claim\":false,\"current_result_hash\":"
        << Quote(route.api_hash) << "}\n";
    out << "    }";
  }
  out << "\n  ]\n";
  out << "}\n";
}

void RequireCoverage(const std::vector<BenchmarkRow>& rows) {
  const std::vector<std::string> required = {
      "row_uuid_lookup",
      "primary_key_lookup",
      "unique_secondary_lookup",
      "range_1_percent",
      "range_50_percent",
      "covering_projection",
      "two_selective_predicates",
      "ordered_index_limit",
      "fk_pk_join",
      "missing_stats",
      "expression_index_predicate",
      "partial_index_predicate",
      "partition_pruning",
      "parameter_sensitive_prepared",
      "materialized_summary_rewrite",
      "cost_calibration",
      "differential_fuzz",
      "statistics_lifecycle"};
  std::set<std::string> markers;
  for (const auto& row : rows) {
    markers.insert(row.plan_markers.begin(), row.plan_markers.end());
  }
  for (const auto& marker : required) {
    Require(markers.find(marker) != markers.end(),
            "ODF-110 SQL planner matrix marker missing: " + marker);
  }
}

void RequireJsonHygiene() {
  const std::filesystem::path output_path = ODF110_OUTPUT_JSON;
  std::ifstream in(output_path);
  Require(in.good(), "ODF-110 JSON evidence was not written");
  std::string payload((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  Require(Contains(payload, "\"live_donor_timing_claim\":false"),
          "ODF-110 JSON made an unsupported donor timing claim");
  Require(!Contains(payload, "docs" "/execution-plans/") &&
              !Contains(payload, "docs" "/findings/") &&
              !Contains(payload, "public_release_evidence") &&
              !Contains(payload, "docs/reference/"),
          "ODF-110 JSON evidence contains forbidden runtime doc dependency path");
  Require(Contains(payload, "\"benchmark_clean_refusal_code\":\"SB_ODF110_BENCHMARK_CLEAN_REFUSED_MISSING_STATS\""),
          "ODF-110 JSON missing exact benchmark-clean refusal vector");
}

}  // namespace

int main() {
  const auto rows = BuildRows();
  Require(rows.size() == 18, "ODF-110 benchmark matrix row count drifted");
  RequireCoverage(rows);

  std::vector<ParserEvidence> parser_results;
  parser_results.reserve(rows.size());
  std::vector<RouteResult> route_results;
  route_results.reserve(rows.size());

  for (const auto& row : rows) {
    parser_results.push_back(CheckParserRoute(row.id,
                                              row.parser_sql,
                                              row.parser_expected_operation,
                                              row.resolved_parser_objects));
    route_results.push_back(RunRoutes(row));
    RequireRequiredEvidence(row, route_results.back().api_result);
    if (row.parameter_shape_pair) {
      RequireParameterShapePair(row, route_results.back().api_hash);
    }
    if (row.differential_pair) {
      RequireDifferentialPair(row, route_results.back().api_hash);
    }
  }

  WriteEvidenceJson(rows, parser_results, route_results);
  RequireJsonHygiene();
  std::cout << "optimizer_deficiency_odf_110_gate=passed\n";
  return EXIT_SUCCESS;
}
