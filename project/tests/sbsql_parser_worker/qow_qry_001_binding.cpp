// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

static_assert(static_cast<int>(
                  sbsql::NativeCatalogRelationResolutionState::kUnresolved) ==
              0);
static_assert(static_cast<int>(
                  sbsql::NativeCatalogRelationResolutionState::kBound) == 1);

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   const std::string_view diagnostic_id) {
  return std::ranges::any_of(messages.diagnostics, [diagnostic_id](const auto& diagnostic) {
    return diagnostic.code == diagnostic_id;
  });
}

std::string_view SourceForRange(const std::string_view source,
                                const sbsql::SourceRange& range) {
  if (range.offset > source.size() ||
      range.length > source.size() - range.offset) {
    return {};
  }
  return source.substr(range.offset, range.length);
}

sbsql::NativeRelationalBindingContext ValuesBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000101";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000102";
  context.security_context_uuid = "019f0000-0000-7110-8000-000000000102";
  context.local_transaction_id = UINT64_C(0x0102030405060708);
  context.statement_snapshot_id = UINT64_C(0xfedcba9876543210);

  sbsql::NativeDescriptorBindingInput numeric;
  numeric.descriptor_id = 1;
  numeric.descriptor_uuid = "019f0000-0000-7200-8000-000000000103";
  numeric.type_uuid = "019f0000-0000-7300-8000-000000000104";
  numeric.nullability = sbsql::BoundNullability::kNonNull;
  numeric.width_precision_scale.precision = 18;
  numeric.width_precision_scale.scale = 0;
  context.descriptors.push_back(numeric);

  sbsql::NativeDescriptorBindingInput text;
  text.descriptor_id = 2;
  text.descriptor_uuid = "019f0000-0000-7200-8000-000000000105";
  text.type_uuid = "019f0000-0000-7300-8000-000000000106";
  text.nullability = sbsql::BoundNullability::kNullable;
  text.width_precision_scale.width = 64;
  text.collation_uuid = "019f0000-0000-7400-8000-000000000107";
  context.descriptors.push_back(text);

  context.expressions = {
      {1, 1, std::nullopt},
      {2, 2, std::nullopt},
      {3, 1, std::nullopt},
      {4, 2, std::nullopt},
  };
  context.outputs = {
      {1, 1, "", 1, true, 0},
      {2, 2, "column_2", 2, true, 1},
  };
  return context;
}

sbsql::NativeRelationalBindingContext CatalogBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000201";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000202";
  context.security_context_uuid = "019f0000-0000-7110-8000-000000000203";
  context.local_transaction_id =
      std::numeric_limits<std::uint64_t>::max();
  context.statement_snapshot_id = UINT64_C(0x8000000100000001);

  sbsql::NativeDescriptorBindingInput text;
  text.descriptor_id = 1;
  text.descriptor_uuid = "019f0000-0000-7200-8000-000000000204";
  text.type_uuid = "019f0000-0000-7300-8000-000000000205";
  text.nullability = sbsql::BoundNullability::kNonNull;
  text.width_precision_scale.width = 128;
  text.collation_uuid = "019f0000-0000-7400-8000-000000000206";
  context.descriptors.push_back(text);

  sbsql::NativeDescriptorBindingInput timestamp;
  timestamp.descriptor_id = 2;
  timestamp.descriptor_uuid = "019f0000-0000-7200-8000-000000000207";
  timestamp.type_uuid = "019f0000-0000-7300-8000-000000000208";
  timestamp.nullability = sbsql::BoundNullability::kNullable;
  timestamp.width_precision_scale.precision = 6;
  timestamp.width_precision_scale.scale = 0;
  timestamp.timezone_profile_id = "timezone.utc.v1";
  context.descriptors.push_back(timestamp);

  sbsql::NativeCatalogRelationBindingInput relation;
  relation.source_id = 1;
  relation.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  relation.object_uuid = "019f0000-0000-7500-8000-000000000209";
  relation.resolved_object_type = "relation";
  relation.resolved_schema_uuid = "019f0000-0000-7500-8000-00000000020a";
  relation.parent_object_uuid = "019f0000-0000-7500-8000-00000000020b";
  relation.catalog_generation_id = 17;
  relation.security_epoch = 19;
  relation.resource_epoch = 23;
  relation.columns = {
      {0, "019f0000-0000-7600-8000-00000000020c", 1, "order_id"},
      {1, "019f0000-0000-7600-8000-00000000020d", 2, "order_note"},
  };
  context.catalog_relations.push_back(std::move(relation));
  return context;
}

sbsql::ParserConfig ParserConfigForTest() {
  sbsql::ParserConfig config;
  config.parser_uuid = "019f0000-0000-7500-8000-000000000108";
  config.bundle_contract_id = "sbp_sbsql@qow-qry-001-binding-v1";
  config.build_id = "qow-qry-001-binding-v1";
  return config;
}

sbsql::SessionContext SessionForTest() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7600-8000-000000000109";
  session.connection_uuid = "019f0000-0000-7600-8000-00000000010a";
  session.database_uuid = "019f0000-0000-7600-8000-00000000010b";
  session.dialect_profile_uuid = "019f0000-0000-7600-8000-00000000010c";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

bool ValidateTypedBinding() {
  const auto cst = sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');");
  const auto ast = sbsql::BuildAst(cst);
  const auto context = ValuesBindingContext();
  const auto native = sbsql::BindNativeRelationalAst(ast.native_relational, context);

  bool passed = true;
  passed &= Require(native.bound, "typed VALUES binding was refused");
  passed &= Require(!native.messages.has_errors(), "typed VALUES binding emitted errors");
  passed &= Require(native.bound_ast_uuid == context.bound_ast_uuid,
                    "BoundAST UUID handle differs");
  passed &= Require(native.security_context_uuid ==
                        context.security_context_uuid,
                    "security context UUID handle differs");
  passed &= Require(
      native.local_transaction_id == context.local_transaction_id &&
          native.statement_snapshot_id == context.statement_snapshot_id,
      "ordinary binding changed, swapped, defaulted, or narrowed MGA statement identities");
  passed &= Require(native.root_relation_id == 1 && native.root_scope_id == 1,
                    "bound root handles differ");
  passed &= Require(native.relations.size() == 1,
                    "bound VALUES relation count differs");
  passed &= Require(!native.relations[0].bound_object_uuid.has_value(),
                    "VALUES acquired a catalog object UUID");
  passed &= Require(!native.relations[0].lateral,
                    "VALUES acquired a lateral binding flag");
  passed &= Require(native.expressions.size() == 4,
                    "bound expression count differs");
  passed &= Require(native.values_rows.size() == 2 &&
                        native.values_rows[0].expression_ids ==
                            std::vector<std::uint32_t>({1, 2}) &&
                        native.values_rows[1].expression_ids ==
                            std::vector<std::uint32_t>({3, 4}) &&
                        native.relations[0].values_row_ids ==
                            std::vector<std::uint32_t>({1, 2}),
                    "typed VALUES row membership was not preserved");
  passed &= Require(native.expressions[0].literal_kind ==
                        sbsql::NativeLiteralAstKind::kNumeric &&
                        native.expressions[1].literal_kind ==
                            sbsql::NativeLiteralAstKind::kString,
                    "typed literal kinds were not preserved");
  passed &= Require(native.descriptors.size() == 2,
                    "bound descriptor count differs");
  passed &= Require(native.outputs.size() == 2,
                    "bound output count differs");
  passed &= Require(native.outputs[0].output_name_utf8.empty(),
                    "explicit empty output name was not preserved");
  passed &= Require(native.scopes.size() == 1,
                    "bound scope count differs");
  passed &= Require(native.scopes[0].visible_relation_ids ==
                        std::vector<std::uint32_t>({1}),
                    "scope relation handles differ");
  passed &= Require(native.scopes[0].visible_projection_ids ==
                        std::vector<std::uint32_t>({1, 2}),
                    "scope output handles differ");
  passed &= Require(native.scopes[0].catalog_epoch_uuid ==
                        context.catalog_epoch_uuid,
                    "catalog epoch UUID handle differs");

  const auto bound = sbsql::BindAst(ast, cst, ParserConfigForTest(),
                                    SessionForTest(), {}, &context);
  passed &= Require(bound.bound, "BindAst did not retain typed binding success");
  passed &= Require(bound.native_relational.bound,
                    "BindAst did not retain the typed BoundAST");
  passed &= Require(bound.requires_descriptor_authority,
                    "typed relation did not require descriptor authority");
  passed &= Require(bound.descriptor_refs.size() == 2,
                    "BindAst descriptor UUID handle count differs");
  passed &= Require(bound.transaction_authority_key ==
                        "authority.not_required.parser_syntax_only",
                    "VALUES binder acquired transaction authority");
  return passed;
}

bool ValidateCatalogRelationBinding() {
  constexpr std::string_view sql =
      "SELECT * FROM tenant.sales.orders AS o;";
  const auto cst = sbsql::BuildCst(sql);
  const auto ast = sbsql::BuildAst(cst);
  const auto context = CatalogBindingContext();
  const auto native =
      sbsql::BindNativeRelationalAst(ast.native_relational, context);

  bool passed = true;
  passed &= Require(native.bound && !native.messages.has_errors(),
                    "catalog relation binding was refused");
  passed &= Require(
      native.local_transaction_id == context.local_transaction_id &&
          native.statement_snapshot_id == context.statement_snapshot_id,
      "catalog binding changed, swapped, defaulted, or narrowed MGA statement identities");
  passed &= Require(native.root_relation_id == 1 && native.root_scope_id == 1,
                    "catalog bound root handles differ");
  passed &= Require(native.relations.size() == 1 &&
                        native.relations[0].relation_kind ==
                            sbsql::NativeRelationAstKind::kCatalogSource &&
                        native.relations[0].semantic_variant_id ==
                            "catalog.relation-source.v1" &&
                        native.relations[0].bound_object_uuid ==
                            context.catalog_relations[0].object_uuid,
                    "catalog relation identity binding differs");
  passed &= Require(native.relations[0].output_expression_ids.empty() &&
                        native.relations[0].bound_expression_ids.empty() &&
                        native.expressions.empty() && native.outputs.empty(),
                    "catalog source binding expanded the wildcard projection");
  passed &= Require(native.catalog_relation_sources.size() == 1,
                    "catalog bound source count differs");
  if (native.catalog_relation_sources.size() == 1) {
    const auto& source = native.catalog_relation_sources.front();
    const auto& input = context.catalog_relations.front();
    passed &= Require(
        source.source_id == 1 &&
            source.source_kind ==
                sbsql::NativeRelationSourceAstKind::kCatalogRelation &&
            source.resolution_state ==
                sbsql::NativeCatalogRelationResolutionState::kBound,
        "catalog source state differs");
    passed &= Require(source.object_uuid == input.object_uuid &&
                          source.resolved_object_type ==
                              input.resolved_object_type &&
                          source.resolved_schema_uuid ==
                              input.resolved_schema_uuid &&
                          source.parent_object_uuid ==
                              input.parent_object_uuid &&
                          source.catalog_generation_id == 17 &&
                          source.security_epoch == 19 &&
                          source.resource_epoch == 23,
                      "catalog source UUID or epoch evidence differs");
    passed &= Require(source.qualified_name.size() == 3 &&
                          source.qualified_name[0].spelling == "tenant" &&
                          source.qualified_name[1].spelling == "sales" &&
                          source.qualified_name[2].spelling == "orders" &&
                          SourceForRange(sql, source.qualified_name_range) ==
                              "tenant.sales.orders" &&
                          source.alias.has_value() &&
                          source.alias->spelling == "o" &&
                          source.alias_is_explicit &&
                          SourceForRange(sql, source.range) ==
                              "tenant.sales.orders AS o",
                      "catalog source name and alias provenance differs");
    passed &= Require(
        source.columns.size() == 2 && source.columns[0].ordinal == 0 &&
            source.columns[0].column_uuid == input.columns[0].column_uuid &&
            source.columns[0].descriptor_id == 1 &&
            source.columns[0].canonical_name_key ==
                input.columns[0].canonical_name_key &&
            source.columns[0].canonical_name_key == "order_id" &&
            source.columns[1].ordinal == 1 &&
            source.columns[1].column_uuid == input.columns[1].column_uuid &&
            source.columns[1].descriptor_id == 2 &&
            source.columns[1].canonical_name_key ==
                input.columns[1].canonical_name_key &&
            source.columns[1].canonical_name_key == "order_note",
        "catalog column identity inventory differs");
  }
  passed &= Require(native.descriptors.size() == 2 &&
                        native.descriptors[0].collation_uuid ==
                            context.descriptors[0].collation_uuid &&
                        native.descriptors[1].timezone_profile_id ==
                            context.descriptors[1].timezone_profile_id,
                    "catalog descriptor collation or timezone evidence differs");
  passed &= Require(native.values_rows.empty() &&
                        native.grouping_sets.empty(),
                    "catalog source acquired source-free VALUES state");
  passed &= Require(native.scopes.size() == 1 &&
                        native.scopes[0].visible_relation_ids ==
                            std::vector<std::uint32_t>({1}) &&
                        native.scopes[0].visible_projection_ids.empty() &&
                        native.scopes[0].catalog_epoch_uuid ==
                            context.catalog_epoch_uuid,
                    "catalog source scope evidence differs");

  const auto session = SessionForTest();
  const auto bound = sbsql::BindAst(ast, cst, ParserConfigForTest(), session,
                                    {}, &context);
  passed &= Require(bound.bound && bound.native_relational.bound &&
                        !bound.messages.has_errors(),
                    "BindAst did not retain catalog source binding success");
  passed &= Require(bound.resolved_object_uuids ==
                        std::vector<std::string>{
                            context.catalog_relations[0].object_uuid},
                    "BindAst did not expose the authoritative relation UUID");
  passed &= Require(bound.transaction_authority_key ==
                        "authority.not_required.parser_syntax_only",
                    "catalog source binder acquired transaction authority");
  passed &= Require(!ast.produces_sblr,
                    "source-only catalog AST became lowering eligible");
  const auto lowered = sbsql::LowerToSblr(bound, cst, session);
  passed &= Require(lowered.messages.has_errors() && lowered.payload.empty() &&
                        HasDiagnostic(lowered.messages,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE"),
                    "source-only catalog BoundAST did not fail closed in lowering");
  return passed;
}

bool ValidateQuotedCatalogRelationBinding() {
  constexpr std::string_view sql =
      "SELECT * FROM app.\"Order Ledger\" ledger;";
  const auto ast = sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  auto context = CatalogBindingContext();
  context.catalog_relations.front().parent_object_uuid = std::nullopt;
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);

  bool passed = true;
  passed &= Require(bound.bound && bound.catalog_relation_sources.size() == 1,
                    "quoted catalog relation binding was refused");
  if (bound.catalog_relation_sources.size() == 1) {
    const auto& source = bound.catalog_relation_sources.front();
    passed &= Require(source.qualified_name.size() == 2 &&
                          source.qualified_name[1].spelling == "Order Ledger" &&
                          source.qualified_name[1].quoted &&
                          source.alias.has_value() &&
                          source.alias->spelling == "ledger" &&
                          !source.alias_is_explicit,
                      "quoted relation provenance differs");
    passed &= Require(source.object_uuid ==
                              context.catalog_relations.front().object_uuid &&
                          !source.parent_object_uuid.has_value(),
                      "quoted source identity or absent-parent state differs");
  }
  return passed;
}

bool RequireAtomicCatalogRefusal(
    const sbsql::NativeRelationalBindingContext& context,
    const std::string_view diagnostic_id,
    const std::string_view message) {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("SELECT * FROM tenant.sales.orders AS o;"));
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);
  const bool atomic =
      !bound.bound && bound.bound_ast_uuid.empty() &&
      bound.security_context_uuid.empty() &&
      bound.local_transaction_id == 0 && bound.statement_snapshot_id == 0 &&
      bound.root_relation_id == 0 &&
      bound.root_scope_id == 0 && bound.descriptors.empty() &&
      bound.expressions.empty() && bound.values_rows.empty() &&
      bound.grouping_sets.empty() && bound.outputs.empty() &&
      bound.relations.empty() && bound.catalog_relation_sources.empty() &&
      bound.scopes.empty();
  return Require(atomic && HasDiagnostic(bound.messages, diagnostic_id), message);
}

bool ValidateCatalogRelationRefusals() {
  bool passed = true;
  auto context = CatalogBindingContext();
  context.catalog_relations[0].resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kUnresolved;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "unresolved catalog relation did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations.clear();
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "missing catalog relation packet did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations.push_back(context.catalog_relations.front());
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "duplicate catalog relation packet did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].source_id = 2;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "catalog source-ID mismatch did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].object_uuid = "not-a-uuid";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "malformed relation UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].object_uuid =
      "00000000-0000-0000-0000-000000000000";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "nil relation UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].resolved_schema_uuid = "not-a-uuid";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "malformed schema UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].resolved_schema_uuid =
      "00000000-0000-0000-0000-000000000000";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "nil schema UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].parent_object_uuid = "not-a-uuid";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "malformed parent UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].parent_object_uuid =
      "00000000-0000-0000-0000-000000000000";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "nil parent UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].resolved_object_type = "routine";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "non-relational object type did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].catalog_generation_id = 0;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "zero catalog generation did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].security_epoch = 0;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "zero security epoch did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].resource_epoch = 0;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-RELATION",
      "zero resource epoch did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[0].column_uuid = "not-a-uuid";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "malformed column UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[0].canonical_name_key.clear();
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "empty canonical column name did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[0].column_uuid =
      "00000000-0000-0000-0000-000000000000";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "nil column UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[1].ordinal = 2;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "gapped column ordinal did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[1].column_uuid =
      context.catalog_relations[0].columns[0].column_uuid;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "duplicate column UUID did not refuse atomically");

  context = CatalogBindingContext();
  context.catalog_relations[0].columns[1].descriptor_id = 99;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "missing column descriptor did not refuse atomically");

  context = CatalogBindingContext();
  context.descriptors[0].nullability = sbsql::BoundNullability::kUnknown;
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "unknown catalog nullability did not refuse atomically");

  context = CatalogBindingContext();
  context.descriptors[1].timezone_profile_id = "";
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "empty timezone profile did not refuse atomically");

  context = CatalogBindingContext();
  auto unused = context.descriptors.back();
  unused.descriptor_id = 3;
  unused.descriptor_uuid = "019f0000-0000-7200-8000-00000000020e";
  context.descriptors.push_back(std::move(unused));
  passed &= RequireAtomicCatalogRefusal(
      context, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
      "unused catalog descriptor did not refuse atomically");
  return passed;
}

bool ValidateCatalogNameIsProvenanceOnly() {
  auto context = CatalogBindingContext();
  const auto authoritative_uuid = context.catalog_relations[0].object_uuid;
  constexpr std::string_view sql =
      "SELECT * FROM cluster_scope.adversary.shadow.renamed AS forged;";
  const auto ast = sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);

  bool passed = true;
  passed &= Require(bound.bound && bound.catalog_relation_sources.size() == 1,
                    "provenance-only name mutation was not bindable");
  if (bound.catalog_relation_sources.size() == 1) {
    const auto& source = bound.catalog_relation_sources.front();
    passed &= Require(source.object_uuid == authoritative_uuid &&
                          source.qualified_name.size() == 4 &&
                          source.qualified_name[0].spelling ==
                              "cluster_scope" &&
                          source.qualified_name[1].spelling == "adversary" &&
                          source.qualified_name[2].spelling == "shadow" &&
                          source.qualified_name.back().spelling == "renamed" &&
                          source.alias.has_value() &&
                          source.alias->spelling == "forged",
                      "catalog spelling substituted for authoritative UUID identity");
  }
  return passed;
}

bool ValidateMissingContextRefusal() {
  const auto cst = sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');");
  const auto ast = sbsql::BuildAst(cst);
  const auto bound =
      sbsql::BindAst(ast, cst, ParserConfigForTest(), SessionForTest());
  bool passed = true;
  passed &= Require(!bound.bound, "typed relation bound without authority context");
  passed &= Require(HasDiagnostic(bound.messages, "QOW-DIAG-BOUNDAST-SCOPE"),
                    "missing authority context diagnostic differs");
  passed &= Require(!bound.native_relational.bound,
                    "missing context produced a usable BoundAST");
  return passed;
}

bool ValidateDescriptorRefusal() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  auto context = ValuesBindingContext();
  context.descriptors[0].descriptor_uuid = "not-a-uuid";
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);
  bool passed = true;
  passed &= Require(!bound.bound, "invalid descriptor UUID was accepted");
  passed &= Require(HasDiagnostic(bound.messages,
                                  "QOW-DIAG-BOUNDAST-DESCRIPTOR"),
                    "invalid descriptor diagnostic differs");
  passed &= Require(bound.descriptors.empty() && bound.expressions.empty(),
                    "descriptor refusal retained partial BoundAST state");
  return passed;
}

bool ValidateExpressionRefusal() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  auto context = ValuesBindingContext();
  context.expressions.pop_back();
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);
  bool passed = true;
  passed &= Require(!bound.bound, "missing expression handle was accepted");
  passed &= Require(HasDiagnostic(bound.messages,
                                  "QOW-DIAG-BOUNDAST-EXPRESSION"),
                    "missing expression diagnostic differs");
  passed &= Require(bound.root_relation_id == 0 && bound.scopes.empty(),
                    "expression refusal retained partial BoundAST state");
  return passed;
}

bool ValidateExpressionCycleRefusal() {
  auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  ast.expressions[0].expression_kind =
      sbsql::NativeExpressionAstKind::kParenthesized;
  ast.expressions[0].child_expression_ids = {1};
  const auto bound =
      sbsql::BindNativeRelationalAst(ast, ValuesBindingContext());
  bool passed = true;
  passed &= Require(!bound.bound, "cyclic expression graph was accepted");
  passed &= Require(HasDiagnostic(bound.messages,
                                  "QOW-DIAG-BOUNDAST-EXPRESSION"),
                    "cyclic expression diagnostic differs");
  passed &= Require(bound.expressions.empty(),
                    "cycle refusal retained partial expression state");
  return passed;
}

bool ValidateOutputRefusal() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  auto context = ValuesBindingContext();
  context.outputs[1].ordinal = 0;
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);
  bool passed = true;
  passed &= Require(!bound.bound, "duplicate output ordinal was accepted");
  passed &= Require(HasDiagnostic(bound.messages, "QOW-DIAG-BOUNDAST-OUTPUT"),
                    "duplicate output diagnostic differs");
  return passed;
}

bool ValidateIdentifierAuthorityRefusal() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (unresolved_name, 'a'), (2, 'b');"));
  const auto bound =
      sbsql::BindNativeRelationalAst(ast, ValuesBindingContext());
  bool passed = true;
  passed &= Require(!bound.bound, "unresolved identifier was accepted");
  passed &= Require(HasDiagnostic(bound.messages,
                                  "QOW-DIAG-BOUNDAST-EXPRESSION"),
                    "unresolved identifier diagnostic differs");
  return passed;
}

bool ValidateValuesRowRefusal() {
  auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  ast.values_rows[1].expression_ids.pop_back();
  const auto bound =
      sbsql::BindNativeRelationalAst(ast, ValuesBindingContext());
  bool passed = true;
  passed &= Require(!bound.bound, "inconsistent VALUES row arity was accepted");
  passed &= Require(HasDiagnostic(bound.messages,
                                  "QOW-DIAG-BOUNDAST-RELATION"),
                    "VALUES row diagnostic differs");
  passed &= Require(bound.values_rows.empty(),
                    "VALUES row refusal retained partial BoundAST state");
  return passed;
}

bool ValidateScopeRefusal() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  auto context = ValuesBindingContext();
  context.catalog_epoch_uuid.clear();
  const auto bound = sbsql::BindNativeRelationalAst(ast, context);
  bool passed = true;
  passed &= Require(!bound.bound, "missing catalog epoch UUID was accepted");
  passed &= Require(HasDiagnostic(bound.messages, "QOW-DIAG-BOUNDAST-SCOPE"),
                    "missing catalog epoch diagnostic differs");
  return passed;
}

bool ValidateMgaStatementIdentityRefusals() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  const auto require_atomic_refusal = [&](const auto& context,
                                          const std::string_view message) {
    const auto bound = sbsql::BindNativeRelationalAst(ast, context);
    const bool atomic =
        !bound.bound && bound.bound_ast_uuid.empty() &&
        bound.security_context_uuid.empty() &&
        bound.local_transaction_id == 0 &&
        bound.statement_snapshot_id == 0 && bound.root_relation_id == 0 &&
        bound.root_scope_id == 0 && bound.descriptors.empty() &&
        bound.expressions.empty() && bound.values_rows.empty() &&
        bound.grouping_sets.empty() && bound.outputs.empty() &&
        bound.relations.empty() && bound.catalog_relation_sources.empty() &&
        bound.scopes.empty() &&
        HasDiagnostic(bound.messages, "QOW-DIAG-BOUNDAST-SCOPE");
    return Require(atomic, message);
  };

  bool passed = true;
  auto context = ValuesBindingContext();
  context.local_transaction_id = 0;
  passed &= require_atomic_refusal(
      context, "zero local transaction identity did not refuse atomically");

  context = ValuesBindingContext();
  context.statement_snapshot_id = 0;
  passed &= require_atomic_refusal(
      context, "zero statement snapshot identity did not refuse atomically");

  context = ValuesBindingContext();
  context.local_transaction_id = 0;
  context.statement_snapshot_id = 0;
  passed &= require_atomic_refusal(
      context, "zero MGA statement identities did not refuse atomically");
  return passed;
}

} // namespace

// QOW-TEST-QRY-001-BINDING-V1
int main() {
  bool passed = true;
  passed &= ValidateTypedBinding();
  passed &= ValidateCatalogRelationBinding();
  passed &= ValidateQuotedCatalogRelationBinding();
  passed &= ValidateCatalogRelationRefusals();
  passed &= ValidateCatalogNameIsProvenanceOnly();
  passed &= ValidateMissingContextRefusal();
  passed &= ValidateDescriptorRefusal();
  passed &= ValidateExpressionRefusal();
  passed &= ValidateExpressionCycleRefusal();
  passed &= ValidateOutputRefusal();
  passed &= ValidateIdentifierAuthorityRefusal();
  passed &= ValidateValuesRowRefusal();
  passed &= ValidateScopeRefusal();
  passed &= ValidateMgaStatementIdentityRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
