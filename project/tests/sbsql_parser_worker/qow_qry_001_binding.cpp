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

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

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

sbsql::NativeRelationalBindingContext ValuesBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000101";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000102";

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

} // namespace

// QOW-TEST-QRY-001-BINDING-V1
int main() {
  bool passed = true;
  passed &= ValidateTypedBinding();
  passed &= ValidateMissingContextRefusal();
  passed &= ValidateDescriptorRefusal();
  passed &= ValidateExpressionRefusal();
  passed &= ValidateExpressionCycleRefusal();
  passed &= ValidateOutputRefusal();
  passed &= ValidateScopeRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
