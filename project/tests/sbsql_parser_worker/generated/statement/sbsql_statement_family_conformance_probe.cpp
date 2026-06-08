// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "cst/cst.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "statement/statement_catalog.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace scratchbird::parser::sbsql;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << "\n";
  return condition;
}

bool IsStatementRow(const GeneratedSurfaceRegistryRow& row) {
  return row.family != "expression_runtime";
}

bool ValidateStatementDescriptorCoverage() {
  bool ok = true;
  std::size_t registry_count = 0;
  std::size_t grammar_count = 0;
  std::size_t canonical_count = 0;
  std::size_t native_now_count = 0;
  std::size_t cluster_private_status_count = 0;
  std::size_t cluster_private_scope_count = 0;
  std::size_t active_statement_worker_count = 0;
  std::size_t active_statement_worker_grammar_count = 0;
  std::size_t active_statement_worker_canonical_count = 0;
  std::size_t exact_refusal_count = 0;
  std::map<std::string_view, std::size_t> active_family_counts;
  std::set<std::string_view> descriptor_ids;

  for (const auto& descriptor : BuiltinStatementSurfaceDescriptors()) {
    ok &= Require(!descriptor.surface_id.empty(), "statement descriptor has empty surface_id");
    ok &= Require(!descriptor.fixed_uuid_v7.empty(), "statement descriptor has empty UUID");
    ok &= Require(!descriptor.canonical_name.empty(), "statement descriptor has empty canonical name");
    ok &= Require(!descriptor.family.empty(), std::string(descriptor.surface_id) + " missing family");
    ok &= Require(!descriptor.owner_lane.empty(), std::string(descriptor.surface_id) + " missing owner lane");
    ok &= Require(!descriptor.sblr_operation_family.empty(),
                  std::string(descriptor.surface_id) + " missing SBLR operation family");
    ok &= Require(!descriptor.parser_handler_key.empty(),
                  std::string(descriptor.surface_id) + " missing parser handler");
    ok &= Require(!descriptor.lowering_descriptor_key.empty(),
                  std::string(descriptor.surface_id) + " missing lowering descriptor");
    ok &= Require(!descriptor.server_admission_key.empty(),
                  std::string(descriptor.surface_id) + " missing server admission key");
    ok &= Require(!descriptor.engine_rule_key.empty(),
                  std::string(descriptor.surface_id) + " missing engine rule");
    ok &= Require(!descriptor.diagnostic_key.empty(),
                  std::string(descriptor.surface_id) + " missing diagnostic key");
    ok &= Require(!descriptor.ast_node_kind.empty(),
                  std::string(descriptor.surface_id) + " missing AST node kind");
    ok &= Require(!descriptor.statement_form.empty(),
                  std::string(descriptor.surface_id) + " missing statement form");
    ok &= Require(!descriptor.binding_contract_key.empty(),
                  std::string(descriptor.surface_id) + " missing binding contract");
    ok &= Require(!descriptor.admission_contract_key.empty(),
                  std::string(descriptor.surface_id) + " missing admission contract");
    ok &= Require(!descriptor.behavior_descriptor_key.empty(),
                  std::string(descriptor.surface_id) + " missing behavior descriptor");
    ok &= Require(descriptor_ids.insert(descriptor.surface_id).second,
                  std::string(descriptor.surface_id) + " appears more than once");
  }

  for (const auto& row : GeneratedSurfaceRegistryRows()) {
    if (!IsStatementRow(row)) continue;
    ++registry_count;
    const auto* descriptor = FindStatementSurfaceById(row.surface_id);
    ok &= Require(descriptor != nullptr, std::string(row.surface_id) + " missing statement descriptor");
    if (descriptor == nullptr) continue;

    const auto parsed_kind = ParseStatementSurfaceKind(row.surface_kind);
    const auto parsed_category = ParseStatementParserCategory(row.family);
    ok &= Require(parsed_kind.has_value(), std::string(row.surface_id) + " invalid surface kind");
    ok &= Require(parsed_category.has_value(), std::string(row.surface_id) + " invalid family category");
    ok &= Require(descriptor->kind == parsed_kind.value(),
                  std::string(row.surface_id) + " surface kind mismatch");
    ok &= Require(descriptor->category == parsed_category.value(),
                  std::string(row.surface_id) + " parser category mismatch");
    ok &= Require(descriptor->fixed_uuid_v7 == row.fixed_uuid_v7,
                  std::string(row.surface_id) + " UUID mismatch");
    ok &= Require(descriptor->canonical_name == row.canonical_name,
                  std::string(row.surface_id) + " canonical name mismatch");
    ok &= Require(descriptor->family == row.family,
                  std::string(row.surface_id) + " family mismatch");
    ok &= Require(descriptor->source_status == row.source_status,
                  std::string(row.surface_id) + " source status mismatch");
    ok &= Require(descriptor->cluster_scope == row.cluster_scope,
                  std::string(row.surface_id) + " cluster scope mismatch");
    ok &= Require(descriptor->owner_lane == row.owner_lane,
                  std::string(row.surface_id) + " owner lane mismatch");
    ok &= Require(descriptor->sblr_operation_family == row.sblr_operation_family,
                  std::string(row.surface_id) + " SBLR family mismatch");
    ok &= Require(descriptor->parser_handler_key == row.parser_handler_key,
                  std::string(row.surface_id) + " parser handler mismatch");
    ok &= Require(descriptor->lowering_descriptor_key == row.lowering_handler_key,
                  std::string(row.surface_id) + " lowering key mismatch");
    ok &= Require(descriptor->server_admission_key == row.server_admission_key,
                  std::string(row.surface_id) + " server admission key mismatch");
    ok &= Require(descriptor->engine_rule_key == row.engine_rule_key,
                  std::string(row.surface_id) + " engine rule mismatch");
    ok &= Require(descriptor->diagnostic_key == row.diagnostic_key,
                  std::string(row.surface_id) + " diagnostic key mismatch");
    ok &= Require(descriptor->final_acceptance_rule == row.final_acceptance_rule,
                  std::string(row.surface_id) + " final acceptance rule mismatch");

    if (row.surface_kind == "grammar_production") ++grammar_count;
    if (row.surface_kind == "canonical_surface") ++canonical_count;
    if (row.source_status == "native_now") ++native_now_count;
    if (row.source_status == "cluster_private") ++cluster_private_status_count;
    if (row.cluster_scope == "cluster_private") ++cluster_private_scope_count;
    if (descriptor->exact_refusal_required) ++exact_refusal_count;

    if (row.owner_lane == "statement parser worker") {
      ++active_statement_worker_count;
      ++active_family_counts[row.family];
      if (row.surface_kind == "grammar_production") ++active_statement_worker_grammar_count;
      if (row.surface_kind == "canonical_surface") ++active_statement_worker_canonical_count;
      ok &= Require(row.source_status == "native_now",
                    std::string(row.surface_id) + " active statement row is not native_now");
      ok &= Require(row.cluster_scope == "noncluster_or_profile_scoped",
                    std::string(row.surface_id) + " active statement row is cluster scoped");
      ok &= Require(!descriptor->exact_refusal_required,
                    std::string(row.surface_id) + " active statement row requires refusal");
      ok &= Require(descriptor->behavior_descriptor_key ==
                        "behavior.statement.parse_ast_bind_lower_engine_rule",
                    std::string(row.surface_id) + " active statement behavior mismatch");
    }

    if (row.source_status == "cluster_private" || row.cluster_scope == "cluster_private") {
      ok &= Require(descriptor->exact_refusal_required,
                    std::string(row.surface_id) + " cluster-private row lacks exact refusal");
      ok &= Require(descriptor->behavior_descriptor_key ==
                        "behavior.statement.cluster_private.fail_closed_or_profile_gate",
                    std::string(row.surface_id) + " cluster-private behavior mismatch");
    }
  }

  ok &= Require(BuiltinStatementSurfaceDescriptors().size() == registry_count,
                "statement descriptor count does not match generated non-expression rows");
  ok &= Require(registry_count == 1049, "expected 1049 non-expression statement/grammar rows");
  ok &= Require(grammar_count == 1010, "expected 1010 grammar-production statement rows");
  ok &= Require(canonical_count == 39, "expected 39 canonical statement rows");
  ok &= Require(native_now_count == 1014, "expected 1014 native_now statement rows");
  ok &= Require(cluster_private_status_count == 35, "expected 35 cluster_private status rows");
  ok &= Require(cluster_private_scope_count == 46, "expected 46 cluster_private scope rows");
  ok &= Require(exact_refusal_count >= cluster_private_scope_count,
                "exact refusal count must cover cluster-private scope rows");

  ok &= Require(active_statement_worker_count == 453, "expected 453 FSPE-005 active rows");
  ok &= Require(active_statement_worker_grammar_count == 429,
                "expected 429 FSPE-005 grammar-production rows");
  ok &= Require(active_statement_worker_canonical_count == 24,
                "expected 24 FSPE-005 canonical-surface rows");
  ok &= Require(active_family_counts["ddl_catalog"] == 176, "ddl_catalog active count mismatch");
  ok &= Require(active_family_counts["multi_model"] == 70, "multi_model active count mismatch");
  ok &= Require(active_family_counts["query"] == 43, "query active count mismatch");
  ok &= Require(active_family_counts["observability"] == 37, "observability active count mismatch");
  ok &= Require(active_family_counts["dml"] == 36, "dml active count mismatch");
  ok &= Require(active_family_counts["transaction"] == 24, "transaction active count mismatch");
  ok &= Require(active_family_counts["security"] == 21, "security active count mismatch");
  return ok;
}

bool ValidateLookupDescriptors() {
  bool ok = true;
  const auto* select = FindStatementSurfaceByName("select");
  ok &= Require(select != nullptr, "missing select descriptor");
  if (select != nullptr) {
    ok &= Require(select->kind == StatementSurfaceKind::kCanonicalSurface,
                  "select should be canonical surface");
    ok &= Require(select->category == StatementParserCategory::kQuery,
                  "select category mismatch");
    ok &= Require(select->top_level_candidate, "select should be top-level candidate");
  }

  const auto* create_object = FindStatementSurfaceByName("create_object");
  ok &= Require(create_object != nullptr, "missing create_object descriptor");
  if (create_object != nullptr) {
    ok &= Require(create_object->requires_authority_resolution,
                  "create_object should require authority resolution later");
  }

  const auto* call = FindStatementSurfaceByName("call");
  ok &= Require(call != nullptr, "missing call descriptor");
  if (call != nullptr) {
    ok &= Require(call->owner_lane == "parser grammar/AST worker",
                  "call descriptor owner lane mismatch");
  }

  const auto* cluster = FindStatementSurfaceByName("cluster_setting_stmt");
  ok &= Require(cluster != nullptr, "missing cluster_setting_stmt descriptor");
  if (cluster != nullptr) {
    ok &= Require(cluster->exact_refusal_required,
                  "cluster_setting_stmt should require exact refusal/profile gate");
  }

  ok &= Require(StatementSurfaceKindName(StatementSurfaceKind::kCanonicalSurface) ==
                    "canonical_surface",
                "canonical surface kind name mismatch");
  ok &= Require(StatementParserCategoryName(StatementParserCategory::kDml) == "dml",
                "DML category name mismatch");
  return ok;
}

bool ValidateAstStatementDescriptorBridge() {
  struct Case {
    std::string_view sql;
    StatementFamily family;
    std::string_view statement_kind;
    std::string_view category;
    std::string_view canonical_name;
  };
  const std::vector<Case> cases = {
      {"SELECT 1", StatementFamily::kQuery, "query", "query", "select"},
      {"VALUES (1)", StatementFamily::kValues, "values", "general", "values_stmt"},
      {"INSERT INTO t VALUES (1)", StatementFamily::kInsert, "insert", "dml", "insert"},
      {"UPDATE t SET a = 1", StatementFamily::kUpdate, "update", "dml", "update"},
      {"DELETE FROM t", StatementFamily::kDelete, "delete", "dml", "delete"},
      {"MERGE INTO t USING s ON t.id = s.id", StatementFamily::kMerge, "merge", "dml", "merge"},
      {"UPSERT INTO t VALUES (1)", StatementFamily::kUpsert, "upsert", "dml", "upsert"},
      {"CREATE TABLE t (id int)", StatementFamily::kCatalog, "catalog", "ddl_catalog", "create_object"},
      {"ALTER TABLE t", StatementFamily::kCatalog, "catalog", "ddl_catalog", "alter_object"},
      {"DROP TABLE t", StatementFamily::kCatalog, "catalog", "ddl_catalog", "drop_object"},
      {"SHOW METRICS", StatementFamily::kShow, "show", "observability", "show"},
      {"DESCRIBE t", StatementFamily::kObservability, "observability", "observability", "describe"},
      {"EXPLAIN SELECT 1", StatementFamily::kObservability, "observability", "observability", "explain"},
      {"SET x = 1", StatementFamily::kSession, "session", "general", "set_session"},
      {"SET ROLE app_role", StatementFamily::kSecurity, "security", "security", "set_role_stmt"},
      {"SET TRANSACTION READ WRITE", StatementFamily::kTransaction, "transaction", "transaction", "set_transaction_stmt"},
      {"BEGIN", StatementFamily::kTransaction, "transaction", "transaction", "begin_stmt"},
      {"BEGIN TRANSACTION", StatementFamily::kTransaction, "transaction", "transaction", "begin_transaction"},
      {"COMMIT", StatementFamily::kTransaction, "transaction", "transaction", "commit"},
      {"ROLLBACK", StatementFamily::kTransaction, "transaction", "transaction", "rollback"},
      {"SAVEPOINT s", StatementFamily::kTransaction, "transaction", "transaction", "savepoint"},
      {"EXECUTE p", StatementFamily::kExecute, "execute", "general", "execute_stmt"},
      {"CALL p()", StatementFamily::kCall, "call", "general", "call"},
      {"GRANT SELECT ON t TO r", StatementFamily::kSecurity, "security", "security", "grant"},
      {"REVOKE SELECT ON t FROM r", StatementFamily::kSecurity, "security", "security", "revoke"},
      {"CHECKPOINT", StatementFamily::kStorageManagement, "storage_management", "storage_management", "checkpoint_stmt"},
      {"COPY t TO 'x.csv'", StatementFamily::kInsert, "insert", "dml", "copy_import_export"},
      {"LOCK TABLE t", StatementFamily::kTransaction, "transaction", "transaction", "lock_table"},
  };

  bool ok = true;
  for (const auto& item : cases) {
    const auto cst = BuildCst(item.sql);
    const auto ast = BuildAst(cst);
    if (ast.messages.has_errors()) {
      std::cerr << "AST produced error for " << item.sql << "\n";
      ok = false;
      continue;
    }
    const auto* expected = FindStatementSurfaceByName(item.canonical_name);
    ok &= Require(expected != nullptr, std::string("missing expected descriptor ") +
                                    std::string(item.canonical_name));
    ok &= Require(ast.family == item.family,
                  std::string("statement family mismatch for ") + std::string(item.sql));
    ok &= Require(ast.statement_kind == item.statement_kind,
                  std::string("statement kind mismatch for ") + std::string(item.sql));
    ok &= Require(ast.statement_parser_category == item.category,
                  std::string("statement category mismatch for ") + std::string(item.sql));
    ok &= Require(ast.statement_surface_name == item.canonical_name,
                  std::string("statement surface name mismatch for ") + std::string(item.sql));
    if (expected != nullptr) {
      ok &= Require(ast.statement_surface_id == expected->surface_id,
                    std::string("statement surface ID mismatch for ") + std::string(item.sql));
      ok &= Require(ast.parser_handler_key == expected->parser_handler_key,
                    std::string("parser handler mismatch for ") + std::string(item.sql));
      ok &= Require(ast.statement_binding_contract_key == expected->binding_contract_key,
                    std::string("binding contract mismatch for ") + std::string(item.sql));
      ok &= Require(ast.statement_admission_contract_key == expected->admission_contract_key,
                    std::string("admission contract mismatch for ") + std::string(item.sql));
      ok &= Require(ast.diagnostic_key == expected->diagnostic_key,
                    std::string("diagnostic key mismatch for ") + std::string(item.sql));
    }
    ok &= Require(!ast.statement_behavior_descriptor_key.empty(),
                  std::string("behavior descriptor missing for ") + std::string(item.sql));
    ok &= Require(!ast.requires_cluster_profile,
                  std::string("public sample unexpectedly requires cluster profile for ") + std::string(item.sql));
  }

  const auto unknown = BuildAst(BuildCst("WAL CHECKPOINT"));
  ok &= Require(unknown.messages.has_errors(), "unknown WAL statement did not produce parser error");
  ok &= Require(unknown.statement_surface_id.empty(),
                "unknown WAL statement unexpectedly attached statement descriptor");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateStatementDescriptorCoverage();
  ok &= ValidateLookupDescriptors();
  ok &= ValidateAstStatementDescriptorBridge();
  return ok ? 0 : 1;
}
