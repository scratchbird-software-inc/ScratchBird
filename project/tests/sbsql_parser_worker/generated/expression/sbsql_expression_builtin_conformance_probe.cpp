// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "expression/expression_catalog.hpp"
#include "expression/donor_variable_compatibility.hpp"
#include "lexer/lexer.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>

using namespace scratchbird::parser::sbsql;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << "\n";
  return condition;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool IsExpressionRow(const GeneratedSurfaceRegistryRow& row) {
  return row.family == "expression_runtime";
}

bool IsDonorCompatibilityRow(const GeneratedSurfaceRegistryRow& row) {
  return IsDonorVariableCompatibilitySurface(row.surface_id) ||
         IsDonorVariableCompatibilitySpelling(row.canonical_name);
}

bool ValidateGeneratedRegistryCoverage() {
  bool ok = true;
  std::size_t native_registry_count = 0;
  std::size_t total_registry_count = 0;
  std::size_t donor_compatibility_count = 0;
  std::size_t function_count = 0;
  std::size_t operator_count = 0;
  std::size_t variable_count = 0;
  std::size_t native_now_count = 0;
  std::size_t native_future_count = 0;
  std::size_t cluster_private_status_count = 0;
  std::size_t exact_refusal_count = 0;
  std::set<std::string_view> descriptor_ids;

  for (const auto& descriptor : BuiltinExpressionSurfaceDescriptors()) {
    ok &= Require(!descriptor.surface_id.empty(), "descriptor has empty surface_id");
    ok &= Require(!descriptor.fixed_uuid_v7.empty(), "descriptor has empty fixed UUID");
    ok &= Require(!descriptor.canonical_name.empty(), "descriptor has empty canonical name");
    ok &= Require(!descriptor.parser_handler_key.empty(),
                  std::string(descriptor.surface_id) + " missing parser handler");
    ok &= Require(!descriptor.binder_descriptor_key.empty(),
                  std::string(descriptor.surface_id) + " missing binder descriptor");
    ok &= Require(!descriptor.lowering_descriptor_key.empty(),
                  std::string(descriptor.surface_id) + " missing lowering descriptor");
    ok &= Require(!descriptor.engine_rule_key.empty(),
                  std::string(descriptor.surface_id) + " missing engine rule");
    ok &= Require(!descriptor.diagnostic_key.empty(),
                  std::string(descriptor.surface_id) + " missing diagnostic key");
    ok &= Require(!descriptor.behavior_descriptor_key.empty(),
                  std::string(descriptor.surface_id) + " missing behavior descriptor");
    ok &= Require(!descriptor.expression_class.empty(),
                  std::string(descriptor.surface_id) + " missing expression class");
    ok &= Require(!descriptor.arity_class.empty(),
                  std::string(descriptor.surface_id) + " missing arity class");
    ok &= Require(descriptor_ids.insert(descriptor.surface_id).second,
                  std::string(descriptor.surface_id) + " appears more than once");
  }

  for (const auto& row : GeneratedSurfaceRegistryRows()) {
    if (!IsExpressionRow(row)) continue;
    ++total_registry_count;
    if (IsDonorCompatibilityRow(row)) {
      ++donor_compatibility_count;
      ok &= Require(FindExpressionSurfaceById(row.surface_id) == nullptr,
                    std::string(row.surface_id) +
                        " donor compatibility row leaked into native expression descriptor catalog");
      continue;
    }
    ++native_registry_count;
    const auto* descriptor = FindExpressionSurfaceById(row.surface_id);
    ok &= Require(descriptor != nullptr, std::string(row.surface_id) + " missing expression descriptor");
    if (descriptor == nullptr) continue;

    const auto parsed_kind = ParseExpressionSurfaceKind(row.surface_kind);
    ok &= Require(parsed_kind.has_value(), std::string(row.surface_id) + " has invalid surface_kind");
    ok &= Require(descriptor->kind == parsed_kind.value(),
                  std::string(row.surface_id) + " descriptor kind mismatch");
    ok &= Require(descriptor->fixed_uuid_v7 == row.fixed_uuid_v7,
                  std::string(row.surface_id) + " fixed UUID mismatch");
    ok &= Require(descriptor->canonical_name == row.canonical_name,
                  std::string(row.surface_id) + " canonical name mismatch");
    ok &= Require(descriptor->source_status == row.source_status,
                  std::string(row.surface_id) + " source status mismatch");
    ok &= Require(descriptor->cluster_scope == row.cluster_scope,
                  std::string(row.surface_id) + " cluster scope mismatch");
    ok &= Require(descriptor->parser_handler_key == row.parser_handler_key,
                  std::string(row.surface_id) + " parser handler mismatch");
    ok &= Require(descriptor->lowering_descriptor_key == row.lowering_handler_key,
                  std::string(row.surface_id) + " lowering handler mismatch");
    ok &= Require(descriptor->engine_rule_key == row.engine_rule_key,
                  std::string(row.surface_id) + " engine rule mismatch");
    ok &= Require(descriptor->diagnostic_key == row.diagnostic_key,
                  std::string(row.surface_id) + " diagnostic key mismatch");
    ok &= Require(descriptor->final_acceptance_rule == row.final_acceptance_rule,
                  std::string(row.surface_id) + " final acceptance rule mismatch");

    if (row.surface_kind == "function") ++function_count;
    if (row.surface_kind == "operator") ++operator_count;
    if (row.surface_kind == "variable") ++variable_count;
    if (row.source_status == "native_now") ++native_now_count;
    if (row.source_status == "native_future") ++native_future_count;
    if (row.source_status == "cluster_private") ++cluster_private_status_count;
    if (descriptor->exact_refusal_required) ++exact_refusal_count;

    if (row.source_status == "native_future" && row.cluster_scope != "cluster_private") {
      ok &= Require(descriptor->exact_refusal_required,
                    std::string(row.surface_id) + " native_future row lacks exact refusal flag");
      ok &= Require(descriptor->behavior_descriptor_key ==
                        "behavior.native_future.exact_refusal_or_promoted_builtin",
                    std::string(row.surface_id) + " native_future behavior mismatch");
    }
    if (row.cluster_scope == "cluster_private") {
      ok &= Require(descriptor->exact_refusal_required,
                    std::string(row.surface_id) + " cluster-private row lacks exact refusal flag");
      ok &= Require(descriptor->behavior_descriptor_key ==
                        "behavior.cluster_private.fail_closed_or_profile_gate",
                    std::string(row.surface_id) + " cluster-private behavior mismatch");
    }
    if (row.source_status == "native_now" && row.cluster_scope != "cluster_private" &&
        !Contains(row.final_acceptance_rule, "refusal") &&
        !Contains(row.engine_rule_key, "fail_closed")) {
      ok &= Require(!descriptor->exact_refusal_required,
                    std::string(row.surface_id) + " native_now row unexpectedly requires refusal");
      ok &= Require(descriptor->behavior_descriptor_key ==
                        "behavior.native_now.parse_bind_lower_engine_rule",
                    std::string(row.surface_id) + " native_now behavior mismatch");
    }
  }

  ok &= Require(BuiltinExpressionSurfaceDescriptors().size() == native_registry_count,
                "expression descriptor count does not match native registry row count");
  ok &= Require(total_registry_count == 1534, "expected 1534 expression_runtime rows");
  ok &= Require(donor_compatibility_count == 27,
                "expected 27 generated donor variable compatibility expression rows");
  ok &= Require(BuiltinDonorVariableCompatibilityDescriptors().size() == 29,
                "expected 29 donor variable compatibility descriptors");
  ok &= Require(native_registry_count == 1507, "expected 1507 native expression_runtime rows");
  ok &= Require(function_count == 1505, "expected 1505 native expression function rows");
  ok &= Require(operator_count == 1, "expected 1 native expression operator row");
  ok &= Require(variable_count == 1, "expected 1 expression variable row");
  ok &= Require(native_now_count + native_future_count + cluster_private_status_count ==
                    native_registry_count,
                "expression status counts must cover registry row count");
  ok &= Require(exact_refusal_count >= native_future_count,
                "exact refusal count must cover native_future rows");
  return ok;
}

bool ValidateLookupAndBehaviorDescriptors() {
  bool ok = true;

  const auto* variable = FindExpressionSurfaceByName("@");
  ok &= Require(variable != nullptr, "missing @ variable descriptor");
  if (variable != nullptr) {
    ok &= Require(variable->kind == ExpressionSurfaceKind::kVariable, "@ should be a variable descriptor");
    ok &= Require(variable->precedence == 100, "@ should have prefix precedence");
    ok &= Require(variable->associativity == ExpressionAssociativity::kPrefix,
                  "@ should be prefix associative");
  }

  const auto* rowcount = FindExpressionSurfaceByName("@@rowcount");
  ok &= Require(rowcount == nullptr, "@@ROWCOUNT must not be a native SBsql descriptor");
  const auto* rowcount_compat = FindDonorVariableCompatibilityBySpelling("@@rowcount");
  ok &= Require(rowcount_compat != nullptr, "missing @@ROWCOUNT donor compatibility descriptor");
  if (rowcount_compat != nullptr) {
    ok &= Require(rowcount_compat->canonical_variable_id == "ctx_last_row_count",
                  "@@ROWCOUNT canonical variable mismatch");
    ok &= Require(rowcount_compat->sblr_operation_id == "expression.system_variable_read",
                  "@@ROWCOUNT SBLR operation mismatch");
    ok &= Require(rowcount_compat->sblr_opcode == "SBLR_SYSTEM_VARIABLE_READ",
                  "@@ROWCOUNT SBLR opcode mismatch");
    ok &= Require(!rowcount_compat->native_sbsql_surface,
                  "@@ROWCOUNT must not be marked native SBsql");
    ok &= Require(rowcount_compat->donor_parser_only,
                  "@@ROWCOUNT must be donor-parser-only metadata");
    ok &= Require(!rowcount_compat->exact_refusal,
                  "@@ROWCOUNT should lower to a canonical variable read");
  }

  const auto* system_var = FindExpressionSurfaceByName("SYSTEM_VAR('var')");
  ok &= Require(system_var != nullptr,
                "SYSTEM_VAR('var') must remain a native SBsql descriptor");
  if (system_var != nullptr) {
    ok &= Require(system_var->surface_id == "SBSQL-6BD2088A414A",
                  "SYSTEM_VAR('var') native surface id mismatch");
    ok &= Require(system_var->kind == ExpressionSurfaceKind::kFunction,
                  "SYSTEM_VAR('var') should remain a native function surface");
  }

  const auto* accept = FindExpressionSurfaceByName("accept");
  ok &= Require(accept != nullptr, "missing Accept descriptor");
  if (accept != nullptr) {
    ok &= Require(accept->kind == ExpressionSurfaceKind::kFunction,
                  "Accept should be a function descriptor");
    ok &= Require(accept->source_status == "native_now", "Accept source status mismatch");
    ok &= Require(!accept->exact_refusal_required, "Accept must not require exact refusal");
    ok &= Require(accept->behavior_descriptor_key ==
                      "behavior.native_now.parse_bind_lower_engine_rule",
                  "Accept behavior mismatch after native implementation");
  }

  const auto* coalesce = FindExpressionSurfaceByName("coalesce(a,b,...)");
  ok &= Require(coalesce != nullptr, "missing COALESCE variadic descriptor");
  if (coalesce != nullptr) {
    ok &= Require(coalesce->arity_class == "variadic", "COALESCE arity class mismatch");
    ok &= Require(coalesce->source_status == "native_now", "COALESCE variadic should be promoted");
    ok &= Require(!coalesce->exact_refusal_required, "COALESCE variadic should not require exact refusal after promotion");
    ok &= Require(coalesce->behavior_descriptor_key == "behavior.native_now.parse_bind_lower_engine_rule",
                  "COALESCE variadic behavior mismatch after promotion");
  }

  const auto* named_argument = FindExpressionSurfaceByName("F(NAME=>VALUE)");
  ok &= Require(named_argument != nullptr, "missing named-argument operator descriptor");
  if (named_argument != nullptr) {
    ok &= Require(named_argument->arity_class == "named_argument_pair",
                  "named-argument arity class mismatch");
    ok &= Require(named_argument->associativity == ExpressionAssociativity::kRight,
                  "named-argument associativity mismatch");
  }

  ok &= Require(ExpressionSurfaceKindName(ExpressionSurfaceKind::kFunction) == "function",
                "function kind name mismatch");
  ok &= Require(ExpressionAssociativityName(ExpressionAssociativity::kPrefix) == "prefix",
                "prefix associativity name mismatch");

  return ok;
}

bool ValidateLexerToCatalogBridge() {
  const auto lexed = Lex("SELECT @@ROWCOUNT, @session_var, COALESCE(a,b);");
  bool ok = Require(!lexed.messages.has_errors(), "lexer bridge sample emitted diagnostics");
  bool saw_system_variable = false;
  bool saw_session_variable = false;
  bool saw_function_identifier = false;
  for (const auto& token : lexed.tokens) {
    if (token.kind == TokenKind::kVariable && token.text == "@@ROWCOUNT") {
      saw_system_variable =
          FindExpressionSurfaceByName(token.text) == nullptr &&
          FindDonorVariableCompatibilityBySpelling(token.text) != nullptr;
    }
    if (token.kind == TokenKind::kVariable && token.text == "@session_var") {
      saw_session_variable = FindExpressionSurfaceByName("@") != nullptr;
    }
    if ((token.kind == TokenKind::kKeyword || token.kind == TokenKind::kIdentifier) &&
        token.text == "COALESCE") {
      saw_function_identifier = FindExpressionSurfaceByName("COALESCE(a,b,...)") != nullptr;
    }
  }
  ok &= Require(saw_system_variable,
                "lexer token @@ROWCOUNT did not bridge to donor compatibility descriptor");
  ok &= Require(saw_session_variable, "lexer token @session_var did not bridge to variable marker descriptor");
  ok &= Require(saw_function_identifier, "lexer token COALESCE did not bridge to function descriptor");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateGeneratedRegistryCoverage();
  ok &= ValidateLookupAndBehaviorDescriptors();
  ok &= ValidateLexerToCatalogBridge();
  return ok ? 0 : 1;
}
