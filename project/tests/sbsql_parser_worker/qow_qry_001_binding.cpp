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
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
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

void SetEngineStatementAuthority(
    sbsql::NativeRelationalBindingContext* context) {
  auto& authority = context->engine_statement_authority;
  authority.statement_uuid = context->statement_uuid;
  authority.transaction_uuid = context->owning_transaction_uuid;
  authority.statement_snapshot_uuid = context->statement_snapshot_uuid;
  authority.statement_metadata_snapshot_uuid =
      context->statement_metadata_snapshot_uuid;
  authority.catalog_epoch_uuid = context->catalog_epoch_uuid;
  authority.local_transaction_id = context->local_transaction_id;
  authority.snapshot_visible_through_local_transaction_id =
      context->snapshot_visible_through_local_transaction_id;
}

sbsql::NativeRelationalBindingContext ValuesBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000101";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000102";
  context.security_context_uuid = "019f0000-0000-7110-8000-000000000102";
  context.statement_uuid = "019f0000-0000-7120-8000-000000000110";
  context.owning_transaction_uuid = "019f0000-0000-7130-8000-000000000111";
  context.statement_snapshot_uuid = "019f0000-0000-7140-8000-000000000112";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7150-8000-000000000113";
  context.local_transaction_id = UINT64_C(0x0102030405060708);
  context.snapshot_visible_through_local_transaction_id =
      UINT64_C(0xfedcba9876543210);
  SetEngineStatementAuthority(&context);

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
  context.statement_uuid = "019f0000-0000-7120-8000-000000000210";
  context.owning_transaction_uuid = "019f0000-0000-7130-8000-000000000211";
  context.statement_snapshot_uuid = "019f0000-0000-7140-8000-000000000212";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7150-8000-000000000213";
  context.local_transaction_id =
      std::numeric_limits<std::uint64_t>::max();
  context.snapshot_visible_through_local_transaction_id =
      UINT64_C(0x8000000100000001);
  SetEngineStatementAuthority(&context);

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

sbsql::NativeRelationalBindingContext WindowBindingContext() {
  auto context = CatalogBindingContext();
  context.catalog_relations.front().columns[0].canonical_name_key =
      "account_id";
  context.catalog_relations.front().columns[1].canonical_name_key =
      "created_at";

  sbsql::NativeDescriptorBindingInput frame_offset;
  frame_offset.descriptor_id = 3;
  frame_offset.descriptor_uuid = "019f0000-0000-7200-8000-00000000020e";
  frame_offset.type_uuid = "019f0000-0000-7300-8000-00000000020f";
  frame_offset.nullability = sbsql::BoundNullability::kNonNull;
  context.descriptors.push_back(frame_offset);
  sbsql::NativeDescriptorBindingInput row_number;
  row_number.descriptor_id = 4;
  row_number.descriptor_uuid = "019f0000-0000-7200-8000-000000000210";
  row_number.type_uuid = "019f0000-0000-7300-8000-000000000211";
  row_number.nullability = sbsql::BoundNullability::kNonNull;
  context.descriptors.push_back(row_number);
  context.expressions = {
      {1, 1, std::nullopt,
       context.catalog_relations.front().columns[0].column_uuid},
      {2, 2, std::nullopt,
       context.catalog_relations.front().columns[1].column_uuid},
      {3, 3, std::nullopt, std::nullopt},
      {4, 4, "019de5fc-2400-7539-bcce-00eef3ae7220", std::nullopt},
  };
  context.outputs = {
      {1, 1, "account_id", 1, false, 0, 1},
      {2, 2, "created_at", 2, false, 1, 1},
      {3, 4, "sequence_no", 4, true, 0, 2},
  };
  context.relations = {{2, "window.row-number.v1"}};
  context.window_functions = {
      {1, 4, 1, "sb.window.row_number",
       "019de5fc-2400-7539-bcce-00eef3ae7220", true, 4}};
  return context;
}

bool HasExactMgaStatementContext(
    const sbsql::BoundNativeRelationalDocument& bound,
    const sbsql::NativeRelationalBindingContext& context) {
  return bound.statement_uuid == context.statement_uuid &&
         bound.owning_transaction_uuid == context.owning_transaction_uuid &&
         bound.statement_snapshot_uuid == context.statement_snapshot_uuid &&
         bound.statement_metadata_snapshot_uuid ==
             context.statement_metadata_snapshot_uuid &&
         bound.local_transaction_id == context.local_transaction_id &&
         bound.snapshot_visible_through_local_transaction_id ==
             context.snapshot_visible_through_local_transaction_id &&
         bound.scopes.size() == 1 &&
         bound.scopes.front().catalog_epoch_uuid == context.catalog_epoch_uuid;
}

bool HasScrubbedMgaStatementContext(
    const sbsql::BoundNativeRelationalDocument& bound) {
  return bound.statement_uuid.empty() &&
         bound.owning_transaction_uuid.empty() &&
         bound.statement_snapshot_uuid.empty() &&
         bound.statement_metadata_snapshot_uuid.empty() &&
         bound.local_transaction_id == 0 &&
         bound.snapshot_visible_through_local_transaction_id == 0;
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
  passed &= Require(HasExactMgaStatementContext(native, context),
                    "ordinary binding changed, swapped, defaulted, or narrowed the engine-supplied MGA statement context");
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
  passed &= Require(HasExactMgaStatementContext(native, context),
                    "catalog binding changed, swapped, defaulted, or narrowed the engine-supplied MGA statement context");
  passed &= Require(
      native.local_transaction_id ==
          std::numeric_limits<std::uint64_t>::max(),
      "catalog binding narrowed UINT64_MAX local transaction number");
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
      HasScrubbedMgaStatementContext(bound) &&
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

bool ValidateMgaStatementContext() {
  const auto ast = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');"));
  const auto require_atomic_refusal = [&](const auto& context,
                                          const std::string_view message) {
    const auto bound = sbsql::BindNativeRelationalAst(ast, context);
    const bool atomic =
        !bound.bound && bound.bound_ast_uuid.empty() &&
        bound.security_context_uuid.empty() &&
        HasScrubbedMgaStatementContext(bound) && bound.root_relation_id == 0 &&
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
      context, "zero local transaction number did not refuse atomically");

  using UuidMember = std::string sbsql::NativeRelationalBindingContext::*;
  const std::array<std::pair<UuidMember, std::string_view>, 4> uuid_fields{{
      {&sbsql::NativeRelationalBindingContext::statement_uuid,
       "statement_uuid"},
      {&sbsql::NativeRelationalBindingContext::owning_transaction_uuid,
       "owning_transaction_uuid"},
      {&sbsql::NativeRelationalBindingContext::statement_snapshot_uuid,
       "statement_snapshot_uuid"},
      {&sbsql::NativeRelationalBindingContext::statement_metadata_snapshot_uuid,
       "statement_metadata_snapshot_uuid"},
  }};
  for (const auto& [member, field_name] : uuid_fields) {
    context = ValuesBindingContext();
    (context.*member).clear();
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " missing UUID did not refuse atomically");

    context = ValuesBindingContext();
    context.*member = "not-a-uuid";
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " invalid UUID did not refuse atomically");

    context = ValuesBindingContext();
    context.*member = "00000000-0000-0000-0000-000000000000";
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " nil UUID did not refuse atomically");
  }

  using Authority = sbsql::NativeRelationalEngineStatementAuthority;
  using AuthorityUuidMember = std::string Authority::*;
  const std::array<std::pair<AuthorityUuidMember, std::string_view>, 5>
      authority_uuid_fields{{
          {&Authority::statement_uuid, "authority statement_uuid"},
          {&Authority::transaction_uuid, "authority transaction_uuid"},
          {&Authority::statement_snapshot_uuid,
           "authority statement_snapshot_uuid"},
          {&Authority::statement_metadata_snapshot_uuid,
           "authority statement_metadata_snapshot_uuid"},
          {&Authority::catalog_epoch_uuid, "authority catalog_epoch_uuid"},
      }};
  for (const auto& [member, field_name] : authority_uuid_fields) {
    context = ValuesBindingContext();
    (context.engine_statement_authority.*member).clear();
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " missing UUID did not refuse atomically");

    context = ValuesBindingContext();
    context.engine_statement_authority.*member = "not-a-uuid";
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " malformed UUID did not refuse atomically");

    context = ValuesBindingContext();
    context.engine_statement_authority.*member =
        "00000000-0000-0000-0000-000000000000";
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " nil UUID did not refuse atomically");

    context = ValuesBindingContext();
    context.engine_statement_authority.*member =
        "019f0000-0000-7999-8000-000000009999";
    passed &= require_atomic_refusal(
        context, std::string(field_name) +
                     " mismatch did not refuse atomically");
  }

  context = ValuesBindingContext();
  std::swap(context.statement_snapshot_uuid,
            context.statement_metadata_snapshot_uuid);
  passed &= require_atomic_refusal(
      context, "swapped data and metadata snapshot UUIDs did not refuse atomically");

  context = ValuesBindingContext();
  context.catalog_epoch_uuid =
      "019f0000-0000-7998-8000-000000009998";
  passed &= require_atomic_refusal(
      context, "stale catalog epoch UUID did not refuse atomically");

  for (const std::string_view invalid_catalog_epoch : {
           std::string_view{}, std::string_view{"not-a-uuid"},
           std::string_view{"00000000-0000-0000-0000-000000000000"}}) {
    context = ValuesBindingContext();
    context.catalog_epoch_uuid = invalid_catalog_epoch;
    passed &= require_atomic_refusal(
        context, "missing, malformed, or nil catalog epoch UUID did not refuse atomically");
  }

  context = ValuesBindingContext();
  std::swap(context.catalog_epoch_uuid,
            context.statement_metadata_snapshot_uuid);
  passed &= require_atomic_refusal(
      context, "swapped catalog and metadata snapshot UUIDs did not refuse atomically");

  context = ValuesBindingContext();
  ++context.engine_statement_authority.local_transaction_id;
  passed &= require_atomic_refusal(
      context, "local transaction number mismatch did not refuse atomically");

  context = ValuesBindingContext();
  ++context.engine_statement_authority
        .snapshot_visible_through_local_transaction_id;
  passed &= require_atomic_refusal(
      context, "snapshot visibility high-water mismatch did not refuse atomically");

  context = ValuesBindingContext();
  context.local_transaction_id = 0;
  context.snapshot_visible_through_local_transaction_id = 0;
  passed &= require_atomic_refusal(
      context, "zero local transaction number with zero visibility boundary did not refuse atomically");

  context = ValuesBindingContext();
  context.snapshot_visible_through_local_transaction_id = 0;
  context.engine_statement_authority
      .snapshot_visible_through_local_transaction_id = 0;
  auto bound = sbsql::BindNativeRelationalAst(ast, context);
  passed &= Require(
      bound.bound && !bound.messages.has_errors() &&
          HasExactMgaStatementContext(bound, context) &&
          context.catalog_epoch_uuid !=
              context.statement_metadata_snapshot_uuid &&
          bound.snapshot_visible_through_local_transaction_id == 0,
      "zero high-water or independent catalog identity was not preserved");

  context = ValuesBindingContext();
  context.snapshot_visible_through_local_transaction_id =
      std::numeric_limits<std::uint64_t>::max();
  context.engine_statement_authority
      .snapshot_visible_through_local_transaction_id =
      std::numeric_limits<std::uint64_t>::max();
  bound = sbsql::BindNativeRelationalAst(ast, context);
  passed &= Require(
      bound.bound && !bound.messages.has_errors() &&
          HasExactMgaStatementContext(bound, context) &&
          bound.snapshot_visible_through_local_transaction_id ==
              std::numeric_limits<std::uint64_t>::max(),
      "UINT64_MAX subordinate snapshot visibility high-water value was narrowed");
  return passed;
}

bool ValidateTypedWindowBinding() {
  constexpr std::string_view sql =
      "SELECT ROW_NUMBER() OVER (PARTITION BY account_id ORDER BY created_at "
      "DESC NULLS FIRST GROUPS BETWEEN 2 PRECEDING AND CURRENT ROW EXCLUDE "
      "TIES) AS sequence_no FROM app.events AS e;";
  const auto cst = sbsql::BuildCst(sql);
  const auto ast_document = sbsql::BuildAst(cst);
  const auto& ast = ast_document.native_relational;
  auto context = WindowBindingContext();
  auto bound = sbsql::BindNativeRelationalAst(ast, context);
  bool passed = true;
  passed &= Require(bound.bound && !bound.messages.has_errors(),
                    "typed ROW_NUMBER binding was refused");
  passed &= Require(HasExactMgaStatementContext(bound, context),
                    "typed window binding changed MGA statement authority");
  passed &= Require(bound.root_relation_id == 2 &&
                        bound.relations.size() == 2 &&
                        bound.relations[1].relation_kind ==
                            sbsql::NativeRelationAstKind::kWindow &&
                        bound.relations[1].semantic_variant_id ==
                            "window.row-number.v1" &&
                        bound.relations[1].input_relation_ids ==
                            std::vector<std::uint32_t>({1}) &&
                        bound.relations[1].window_invocation_ids ==
                            std::vector<std::uint32_t>({1}),
                    "typed bound window relation differs");
  passed &= Require(bound.expressions.size() == 4 &&
                        bound.expressions[2].literal_kind ==
                            sbsql::NativeLiteralAstKind::kNumeric &&
                        bound.expressions[2].literal_or_parameter_ref == "2" &&
                        bound.expressions[3].bound_function_uuid ==
                            context.expressions[3].function_uuid,
                    "typed window expression binding differs");
  passed &= Require(bound.window_definitions.size() == 1 &&
                        bound.window_invocations.size() == 1,
                    "bound window carrier cardinality differs");
  if (bound.window_invocations.size() == 1) {
    const auto& invocation = bound.window_invocations.front();
    passed &= Require(
        invocation.function_abi_version == 1 &&
            invocation.builtin_id == "sb.window.row_number" &&
            invocation.bound_function_uuid ==
                "019de5fc-2400-7539-bcce-00eef3ae7220" &&
            invocation.result_descriptor_id == 4 &&
            invocation.argument_expression_ids.empty(),
        "engine-issued window registry identity was not preserved");
  }
  if (bound.window_definitions.size() == 1) {
    const auto& definition = bound.window_definitions.front();
    passed &= Require(
        definition.partition_expression_ids ==
                std::vector<std::uint32_t>({1}) &&
            definition.ordering_terms.size() == 1 &&
            definition.ordering_terms.front().expression_id == 2 &&
            definition.frame_unit == sbsql::NativeWindowFrameUnit::kGroups &&
            definition.frame_start.has_value() &&
            definition.frame_start->offset_expression_id == 3 &&
            definition.frame_end.has_value() &&
            definition.frame_end->bound_kind ==
                sbsql::NativeWindowFrameBoundKind::kCurrentRow &&
            definition.exclusion ==
                sbsql::NativeWindowFrameExclusion::kTies,
        "bound window partition/order/frame state differs");
  }
  passed &= Require(bound.outputs.size() == 3 &&
                        !bound.outputs[0].visible &&
                        !bound.outputs[1].visible &&
                        bound.outputs[2].visible &&
                        bound.outputs[2].output_name_utf8 == "sequence_no" &&
                        bound.scopes.front().visible_projection_ids ==
                            std::vector<std::uint32_t>({3}),
                    "typed window hidden/source or visible/result output differs");

  const auto session = SessionForTest();
  const auto bound_statement =
      sbsql::BindAst(ast_document, cst, ParserConfigForTest(), session, {},
                     &context);
  passed &= Require(bound_statement.bound &&
                        bound_statement.native_relational.bound &&
                        !bound_statement.messages.has_errors(),
                    "typed window BindAst route was refused");
  const auto lowered = sbsql::LowerToSblr(bound_statement, cst, session);
  const auto definition_operand = std::ranges::find_if(
      lowered.operands, [](const auto& operand) {
        return operand.type == "relational_window_definition_v1";
      });
  const auto invocation_operand = std::ranges::find_if(
      lowered.operands, [](const auto& operand) {
        return operand.type == "relational_window_invocation_v1";
      });
  passed &= Require(
      !lowered.messages.has_errors() && !lowered.payload.empty() &&
          definition_operand != lowered.operands.end() &&
          definition_operand->name == "1" &&
          definition_operand->value == "2|-|-|1|2:2:1:-|3|2:3|3:-|4" &&
          invocation_operand != lowered.operands.end() &&
          invocation_operand->name == "1" &&
          invocation_operand->value.find(
              "2|4|1|1|73622e77696e646f772e726f775f6e756d626572|"
              "019de5fc-2400-7539-bcce-00eef3ae7220|4|"
              "73657175656e63655f6e6f|-") == 0,
      "canonical typed window definition or invocation operand differs");
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  if (!verified.admitted) {
    for (const auto& diagnostic : verified.messages.diagnostics) {
      std::cerr << "window verifier diagnostic: " << diagnostic.code << " "
                << diagnostic.message << '\n';
    }
  }
  passed &= Require(verified.admitted && !verified.messages.has_errors() &&
                        verified.validated_relational_node_count == 2 &&
                        verified.validated_relational_expression_count == 4,
                    "canonical typed window envelope was not verifier-admitted");
  auto missing_definition = lowered;
  std::erase_if(missing_definition.operands, [](const auto& operand) {
    return operand.type == "relational_window_definition_v1";
  });
  const auto missing_definition_result =
      sbsql::VerifySblrEnvelope(missing_definition);
  passed &= Require(
      !missing_definition_result.admitted &&
          HasDiagnostic(missing_definition_result.messages,
                        "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "window invocation without its canonical definition was admitted");

  constexpr std::string_view reused_sql =
      "SELECT ROW_NUMBER() OVER (PARTITION BY account_id ORDER BY account_id) "
      "AS sequence_no FROM app.events AS e;";
  const auto reused_ast =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(reused_sql));
  auto reused_context = WindowBindingContext();
  reused_context.catalog_relations.front().columns.resize(1);
  auto reused_result_descriptor = reused_context.descriptors.back();
  reused_result_descriptor.descriptor_id = 2;
  reused_context.descriptors = {reused_context.descriptors.front(),
                                reused_result_descriptor};
  reused_context.expressions = {
      {1, 1, std::nullopt,
       reused_context.catalog_relations.front().columns.front().column_uuid},
      {2, 2, "019de5fc-2400-7539-bcce-00eef3ae7220", std::nullopt},
  };
  reused_context.outputs = {
      {1, 1, "account_id", 1, false, 0, 1},
      {2, 2, "sequence_no", 2, true, 0, 2},
  };
  reused_context.window_functions = {
      {1, 2, 1, "sb.window.row_number",
       "019de5fc-2400-7539-bcce-00eef3ae7220", true, 2},
  };
  const auto reused_bound =
      sbsql::BindNativeRelationalAst(reused_ast, reused_context);
  passed &= Require(
      reused_bound.bound && reused_bound.relations.size() == 2 &&
          reused_bound.relations.back().bound_expression_ids ==
              std::vector<std::uint32_t>({1, 2}) &&
          reused_bound.window_definitions.front().partition_expression_ids ==
              std::vector<std::uint32_t>({1}) &&
          reused_bound.window_definitions.front()
                  .ordering_terms.front()
                  .expression_id == 1,
      "reused partition/order expression was not interned exactly once");

  constexpr std::string_view named_sql =
      "SELECT ROW_NUMBER() OVER framed AS sequence_no FROM app.events AS e "
      "WINDOW partitioned AS (PARTITION BY account_id), "
      "ordered AS (partitioned ORDER BY created_at DESC NULLS FIRST), "
      "framed AS (ordered GROUPS BETWEEN 2 PRECEDING AND CURRENT ROW "
      "EXCLUDE TIES);";
  const auto named_cst = sbsql::BuildCst(named_sql);
  const auto named_ast_document = sbsql::BuildAst(named_cst);
  auto named_context = WindowBindingContext();
  const auto named_bound = sbsql::BindNativeRelationalAst(
      named_ast_document.native_relational, named_context);
  passed &= Require(
      named_bound.bound && named_bound.window_definitions.size() == 3 &&
          named_bound.window_definitions[0].canonical_name_key ==
              std::optional<std::string>("partitioned") &&
          !named_bound.window_definitions[0].inherited_window_id.has_value() &&
          named_bound.window_definitions[0].partition_expression_ids ==
              std::vector<std::uint32_t>({1}) &&
          named_bound.window_definitions[1].canonical_name_key ==
              std::optional<std::string>("ordered") &&
          named_bound.window_definitions[1].inherited_window_id == 1 &&
          named_bound.window_definitions[1].ordering_terms.front().expression_id ==
              2 &&
          named_bound.window_definitions[2].canonical_name_key ==
              std::optional<std::string>("framed") &&
          named_bound.window_definitions[2].inherited_window_id == 2 &&
          named_bound.window_definitions[2].frame_start->offset_expression_id ==
              3 &&
          named_bound.window_invocations.front().window_definition_id == 3 &&
          named_bound.relations.back().bound_expression_ids ==
              std::vector<std::uint32_t>({1, 2, 3, 4}),
      "named-window binding or inherited identities differ");

  const auto named_bound_statement =
      sbsql::BindAst(named_ast_document, named_cst, ParserConfigForTest(),
                     session, {}, &named_context);
  const auto named_lowered =
      sbsql::LowerToSblr(named_bound_statement, named_cst, session);
  std::vector<std::string> named_definition_operands;
  for (const auto& operand : named_lowered.operands) {
    if (operand.type == "relational_window_definition_v1") {
      named_definition_operands.push_back(operand.value);
    }
  }
  passed &= Require(
      !named_lowered.messages.has_errors() &&
          named_definition_operands ==
              std::vector<std::string>{
                  "2|706172746974696f6e6564|-|1|-|-|-|-|1",
                  "2|6f726465726564|1|-|2:2:1:-|-|-|-|1",
                  "2|6672616d6564|2|-|-|3|2:3|3:-|4"} &&
          sbsql::VerifySblrEnvelope(named_lowered).admitted,
      "named-window canonical SBLR inheritance records differ");

  auto forward_envelope = named_lowered;
  const auto forward_definition = std::ranges::find_if(
      forward_envelope.operands, [](const auto& operand) {
        return operand.type == "relational_window_definition_v1" &&
               operand.name == "1";
      });
  forward_definition->value =
      "2|706172746974696f6e6564|3|1|-|-|-|-|1";
  const auto forward_envelope_result =
      sbsql::VerifySblrEnvelope(forward_envelope);
  passed &= Require(
      !forward_envelope_result.admitted &&
          HasDiagnostic(forward_envelope_result.messages,
                        "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "forward canonical window inheritance was verifier-admitted");

  auto override_envelope = named_lowered;
  const auto overriding_definition = std::ranges::find_if(
      override_envelope.operands, [](const auto& operand) {
        return operand.type == "relational_window_definition_v1" &&
               operand.name == "2";
      });
  overriding_definition->value =
      "2|6f726465726564|1|1|2:2:1:-|-|-|-|1";
  const auto override_envelope_result =
      sbsql::VerifySblrEnvelope(override_envelope);
  passed &= Require(
      !override_envelope_result.admitted &&
          HasDiagnostic(override_envelope_result.messages,
                        "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "canonical window inherited-state override was verifier-admitted");

  auto forward_ast = named_ast_document.native_relational;
  forward_ast.window_definitions[1].base_name =
      sbsql::NativeIdentifierAstNode{"framed", false, {}};
  const auto forward_bound =
      sbsql::BindNativeRelationalAst(forward_ast, named_context);
  passed &= Require(
      !forward_bound.bound && forward_bound.window_definitions.empty() &&
          forward_bound.window_invocations.empty() &&
          HasScrubbedMgaStatementContext(forward_bound),
      "forward named-window base did not refuse atomically in binding");

  context.expressions.back().function_uuid = std::nullopt;
  bound = sbsql::BindNativeRelationalAst(ast, context);
  passed &= Require(
      !bound.bound && bound.relations.empty() && bound.expressions.empty() &&
          bound.window_definitions.empty() && bound.window_invocations.empty() &&
          HasScrubbedMgaStatementContext(bound),
      "missing engine-issued window UUID did not refuse atomically");
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
  passed &= ValidateMgaStatementContext();
  passed &= ValidateTypedWindowBinding();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
