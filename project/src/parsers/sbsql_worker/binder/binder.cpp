// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "binder/binder.hpp"

namespace scratchbird::parser::sbsql {
namespace {

bool RequiresDescriptorAuthority(const AstDocument& ast) {
  return ast.statement_binding_contract_key == "binder.statement.public_authority_required" ||
         ast.requires_name_resolution;
}

bool RequiresSecurityAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "security" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_parser_category == "bridge" ||
         ast.family == StatementFamily::kBridge ||
         ast.statement_binding_contract_key == "binder.statement.cluster_profile_gate";
}

bool RequiresTransactionAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "transaction" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_binding_contract_key == "binder.statement.transaction_context";
}

bool TokenTextIs(const Token& token, std::string_view expected) {
  return ToUpperAscii(token.text) == expected;
}

bool IsSourceFreeCteRoute(const CstDocument& cst) {
  std::vector<const Token*> tokens;
  for (const auto& token : cst.tokens) {
    if (IsTriviaToken(token)) continue;
    tokens.push_back(&token);
  }
  if (tokens.empty()) return false;
  std::size_t first = 0;
  if (TokenTextIs(*tokens[first], "EXPLAIN")) {
    ++first;
    if (first >= tokens.size()) return false;
  }
  if (TokenTextIs(*tokens[first], "WITH")) return true;
  if (!TokenTextIs(*tokens[first], "SELECT")) return false;
  for (std::size_t index = first + 1; index + 2 < tokens.size(); ++index) {
    if (TokenTextIs(*tokens[index], "FROM") &&
        tokens[index + 1]->text == "(" &&
        TokenTextIs(*tokens[index + 2], "WITH")) {
      return true;
    }
  }
  return false;
}

std::string ResultShapeFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "result.shape.rowset";
  }
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "result.shape.management_report";
  }
  if (ast.family == StatementFamily::kCall) return "result.shape.routine_result";
  return "result.shape.command_status";
}

std::string ResourceContractFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "resource.contract.query_read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "resource.contract.dml_write";
  }
  if (ast.family == StatementFamily::kCatalog || ast.family == StatementFamily::kSecurity ||
      ast.family == StatementFamily::kStorageManagement ||
      ast.family == StatementFamily::kMigration ||
      ast.family == StatementFamily::kBridge) {
    return "resource.contract.metadata_mutation";
  }
  return "resource.contract.control";
}

std::string RequiredRightFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "right.read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "right.write";
  }
  if (ast.family == StatementFamily::kCatalog) return "right.catalog_mutate";
  if (ast.family == StatementFamily::kSecurity) return "right.security_admin";
  if (ast.family == StatementFamily::kTransaction) return "right.transaction_control";
  if (ast.family == StatementFamily::kMigration) return "right.migrate_database";
  if (ast.family == StatementFamily::kBridge) return "right.bridge.use";
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "right.observe";
  }
  return "right.execute";
}

void PopulateAuthorityMetadata(BoundStatement* bound, const AstDocument& ast) {
  bound->statement_surface_id = ast.statement_surface_id;
  bound->statement_surface_name = ast.statement_surface_name;
  bound->statement_parser_category = ast.statement_parser_category;
  bound->parser_handler_key = ast.parser_handler_key;
  bound->binding_contract_key = ast.statement_binding_contract_key;
  bound->admission_contract_key = ast.statement_admission_contract_key;
  bound->behavior_descriptor_key = ast.statement_behavior_descriptor_key;
  bound->diagnostic_key = ast.diagnostic_key;
  bound->requires_name_resolution = ast.requires_name_resolution;
  bound->requires_descriptor_authority = RequiresDescriptorAuthority(ast);
  bound->requires_security_authority = RequiresSecurityAuthority(ast);
  bound->requires_transaction_authority = RequiresTransactionAuthority(ast);
  bound->requires_cluster_profile = ast.requires_cluster_profile;
  bound->exact_refusal_required = ast.exact_refusal_required;
  bound->command_family = ast.statement_kind;
  bound->surface_key = ast.statement_surface_id.empty() ? ast.registry_family
                                                        : ast.statement_surface_id;
  bound->sblr_operation_key = ast.operation_family;
  bound->result_shape_key = ResultShapeFor(ast);
  bound->diagnostic_shape_key = ast.diagnostic_key.empty() ? "diagnostic.canonical_message_vector"
                                                           : ast.diagnostic_key;
  bound->resource_contract_key = ResourceContractFor(ast);
  bound->conformance_case_key = ast.statement_surface_id.empty()
                                    ? "conformance.unclassified_statement"
                                    : "conformance." + ast.statement_surface_id;
  bound->trace_key = "trace.bound_ast." + std::to_string(ast.source_hash);
  bound->edition_gate_result = "edition_gate.not_evaluated_parser_binder";
  bound->profile_gate_result = bound->requires_cluster_profile
                                   ? "profile_gate.cluster_required"
                                   : "profile_gate.public_or_default";
  bound->granted_scope = "granted_scope.pending_server_authority";
  bound->required_rights.push_back(RequiredRightFor(ast));
  if (bound->requires_descriptor_authority) {
    bound->descriptor_refs.push_back("descriptor.pending_server_or_engine_authority");
  }
  if (bound->requires_security_authority) {
    bound->policy_refs.push_back("policy.pending_server_security_authority");
  }

  bound->name_resolution_authority_key =
      bound->requires_name_resolution ? "authority.server.resolve_name_registry_public"
                                      : "authority.not_required.parser_syntax_only";
  bound->descriptor_authority_key =
      bound->requires_descriptor_authority ? "authority.engine.descriptor_context_required"
                                           : "authority.not_required.parser_syntax_only";
  bound->security_authority_key =
      bound->requires_security_authority ? "authority.server.security_policy_context_required"
                                         : "authority.not_required.parser_syntax_only";
  bound->transaction_authority_key =
      bound->requires_transaction_authority ? "authority.server.transaction_context_required"
                                            : "authority.not_required.parser_syntax_only";

  bound->required_authority_steps.push_back("authority.parser.syntax_evidence_only");
  if (!bound->statement_surface_id.empty()) {
    bound->required_authority_steps.push_back("authority.parser.surface_descriptor_candidate");
  }
  if (bound->requires_name_resolution) {
    bound->required_authority_steps.push_back(bound->name_resolution_authority_key);
  }
  if (bound->requires_descriptor_authority) {
    bound->required_authority_steps.push_back(bound->descriptor_authority_key);
  }
  if (bound->requires_security_authority) {
    bound->required_authority_steps.push_back(bound->security_authority_key);
  }
  if (bound->requires_transaction_authority) {
    bound->required_authority_steps.push_back(bound->transaction_authority_key);
  }
  if (bound->requires_cluster_profile) {
    bound->required_authority_steps.push_back("authority.cluster.profile_gate_required");
  }
}

} // namespace

BoundStatement BindAst(const AstDocument& ast,
                       const CstDocument& cst,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids) {
  BoundStatement bound;
  bound.parser_api_major = config.parser_api_major;
  bound.protocol_version = config.protocol_version;
  bound.parser_package_uuid = config.parser_uuid;
  bound.parser_package_version = config.bundle_contract_id;
  bound.parser_build_id = config.build_id;
  bound.command_registry_snapshot_uuid = "sbsql-generated-registry.v1";
  bound.session_uuid = session.session_uuid;
  bound.connection_uuid = session.connection_uuid;
  bound.database_uuid = session.database_uuid;
  bound.dialect_profile_uuid = session.dialect_profile_uuid;
  bound.catalog_epoch = session.catalog_epoch;
  bound.security_policy_epoch = session.security_policy_epoch;
  bound.descriptor_epoch = session.descriptor_epoch;
  bound.transaction_context = session.transaction_context;
  bound.registry_family = ast.registry_family;
  bound.operation_family = ast.operation_family;
  bound.statement_hash = Fnv1a64(cst.source);
  bound.messages = ast.messages;
  PopulateAuthorityMetadata(&bound, ast);
  if (bound.messages.has_errors()) return bound;
  if (!session.authenticated && ast.family != StatementFamily::kUnknown) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "statement binding requires an authenticated server session",
        "sbp_sbsql.binder"));
    return bound;
  }
  if (bound.requires_cluster_profile) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.CLUSTER.AUTHORITY_REQUIRED", "ERROR",
        "cluster-private statement binding requires a cluster profile authority context",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"authority", "authority.cluster.profile_gate_required"}}));
    return bound;
  }
  if (bound.exact_refusal_required && bound.behavior_descriptor_key.find("fail_closed") != std::string::npos) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.STATEMENT.EXACT_REFUSAL_REQUIRED", "ERROR",
        "statement binding requires exact refusal before SBLR lowering",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"diagnostic_key", bound.diagnostic_key}}));
    return bound;
  }
  if (ast.requires_name_resolution) {
    if (!resolved_object_uuids.empty()) {
      bound.resolved_object_uuids = resolved_object_uuids;
      bound.bound = true;
      return bound;
    }
    if (IsSourceFreeCteRoute(cst)) {
      bound.bound = true;
      return bound;
    }
    if (config.server_endpoint.empty()) {
      bound.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NAME_RESOLUTION.SERVER_ENDPOINT_REQUIRED", "ERROR",
          "object-name binding requires ResolveNameRegistryPublic through sb_server IPC",
          "sbp_sbsql.binder"));
      return bound;
    }
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NAME_RESOLUTION.PUBLIC_RESOLVER_REQUIRED", "ERROR",
        "public name resolution must be performed by sb_server before this statement can lower to final SBLR",
        "sbp_sbsql.binder"));
    return bound;
  }
  bound.bound = true;
  return bound;
}

} // namespace scratchbird::parser::sbsql
