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
#include "statement/statement_catalog.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace scratchbird::parser::sbsql;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << "\n";
  return condition;
}

bool HasStep(const BoundStatement& bound, std::string_view step) {
  return std::find(bound.required_authority_steps.begin(),
                   bound.required_authority_steps.end(),
                   step) != bound.required_authority_steps.end();
}

SessionContext AuthenticatedSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000001";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

BoundStatement BindSql(std::string_view sql,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids = {}) {
  const auto cst = BuildCst(sql);
  const auto ast = BuildAst(cst);
  return BindAst(ast, cst, config, session, resolved_object_uuids);
}

bool ValidateMetadataPreservation() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "00000000-0000-7000-8000-00000000b006";
  config.bundle_contract_id = "sbp_sbsql@binder-test";
  config.build_id = "binder-authority-test";
  const auto bound = BindSql("SELECT 1", config, AuthenticatedSession());
  bool ok = true;
  const auto* select = FindStatementSurfaceByName("select");
  ok &= Require(select != nullptr, "missing select descriptor");
  ok &= Require(bound.bound, "SELECT 1 should bind without external name resolution");
  ok &= Require(!bound.messages.has_errors(), "SELECT 1 produced binder diagnostics");
  ok &= Require(bound.bound_ast_format_version == 1, "BoundAST format version mismatch");
  ok &= Require(bound.parser_api_major == config.parser_api_major, "parser API major not preserved");
  ok &= Require(bound.protocol_version == config.protocol_version, "protocol version not preserved");
  ok &= Require(bound.parser_package_uuid == config.parser_uuid, "parser package UUID not preserved");
  ok &= Require(bound.parser_package_version == config.bundle_contract_id,
                "parser package version not preserved");
  ok &= Require(bound.parser_build_id == config.build_id, "parser build ID not preserved");
  ok &= Require(bound.command_registry_snapshot_uuid == "sbsql-generated-registry.v1",
                "command registry snapshot UUID mismatch");
  ok &= Require(bound.catalog_epoch == 7, "catalog epoch not preserved");
  ok &= Require(bound.security_policy_epoch == 11, "security policy epoch not preserved");
  ok &= Require(bound.descriptor_epoch == 13, "descriptor epoch not preserved");
  if (select != nullptr) {
    ok &= Require(bound.statement_surface_id == select->surface_id, "select surface ID not preserved");
    ok &= Require(bound.statement_surface_name == "select", "select surface name not preserved");
    ok &= Require(bound.statement_parser_category == "query", "select parser category mismatch");
    ok &= Require(bound.parser_handler_key == select->parser_handler_key,
                  "select parser handler not preserved");
    ok &= Require(bound.binding_contract_key == select->binding_contract_key,
                  "select binding contract not preserved");
    ok &= Require(bound.admission_contract_key == select->admission_contract_key,
                  "select admission contract not preserved");
    ok &= Require(bound.behavior_descriptor_key == select->behavior_descriptor_key,
                  "select behavior descriptor not preserved");
    ok &= Require(bound.diagnostic_key == select->diagnostic_key,
                  "select diagnostic key not preserved");
  }
  ok &= Require(bound.name_resolution_authority_key ==
                    "authority.not_required.parser_syntax_only",
                "SELECT 1 should not require public name resolver");
  ok &= Require(bound.descriptor_authority_key ==
                    "authority.engine.descriptor_context_required",
                "query descriptor authority key mismatch");
  ok &= Require(bound.command_family == "query", "command family mismatch");
  ok &= Require(bound.surface_key == bound.statement_surface_id, "surface key mismatch");
  ok &= Require(bound.sblr_operation_key == bound.operation_family, "SBLR operation key mismatch");
  ok &= Require(bound.result_shape_key == "result.shape.rowset", "result shape mismatch");
  ok &= Require(bound.diagnostic_shape_key == bound.diagnostic_key,
                "diagnostic shape mismatch");
  ok &= Require(bound.resource_contract_key == "resource.contract.query_read",
                "resource contract mismatch");
  ok &= Require(!bound.conformance_case_key.empty(), "conformance case key missing");
  ok &= Require(!bound.trace_key.empty(), "trace key missing");
  ok &= Require(bound.edition_gate_result == "edition_gate.not_evaluated_parser_binder",
                "edition gate result mismatch");
  ok &= Require(bound.profile_gate_result == "profile_gate.public_or_default",
                "profile gate result mismatch");
  ok &= Require(bound.granted_scope == "granted_scope.pending_server_authority",
                "granted scope mismatch");
  ok &= Require(!bound.required_rights.empty() && bound.required_rights[0] == "right.read",
                "required rights mismatch");
  ok &= Require(!bound.descriptor_refs.empty(), "descriptor refs missing");
  ok &= Require(HasStep(bound, "authority.parser.syntax_evidence_only"),
                "syntax evidence authority step missing");
  ok &= Require(HasStep(bound, "authority.parser.surface_descriptor_candidate"),
                "surface descriptor candidate authority step missing");
  ok &= Require(HasStep(bound, "authority.engine.descriptor_context_required"),
                "descriptor authority step missing");
  return ok;
}

bool ValidateAuthenticationGate() {
  ParserConfig config;
  config.probe_mode = true;
  SessionContext unauthenticated;
  const auto bound = BindSql("SELECT 1", config, unauthenticated);
  bool ok = true;
  ok &= Require(!bound.bound, "unauthenticated statement unexpectedly bound");
  ok &= Require(bound.messages.has_errors(), "unauthenticated statement lacked diagnostic");
  ok &= Require(bound.statement_surface_name == "select",
                "unauthenticated binder should still preserve syntax metadata");
  ok &= Require(HasStep(bound, "authority.parser.syntax_evidence_only"),
                "unauthenticated binder lost syntax authority step");
  return ok;
}

bool ValidatePublicResolverGate() {
  ParserConfig config;
  config.probe_mode = true;
  const auto session = AuthenticatedSession();
  bool ok = true;

  const auto no_endpoint = BindSql("SELECT * FROM customer", config, session);
  ok &= Require(!no_endpoint.bound, "FROM query unexpectedly bound without server endpoint");
  ok &= Require(no_endpoint.messages.has_errors(), "FROM query lacked resolver diagnostic");
  ok &= Require(no_endpoint.requires_name_resolution,
                "FROM query did not record name-resolution requirement");
  ok &= Require(no_endpoint.name_resolution_authority_key ==
                    "authority.server.resolve_name_registry_public",
                "FROM query resolver authority key mismatch");
  ok &= Require(HasStep(no_endpoint, "authority.server.resolve_name_registry_public"),
                "FROM query resolver authority step missing");

  ParserConfig endpoint_config = config;
  endpoint_config.server_endpoint = "unix:/tmp/sb_server.sbps.sock";
  const auto unresolved = BindSql("SELECT * FROM customer", endpoint_config, session);
  ok &= Require(!unresolved.bound, "unresolved FROM query unexpectedly bound");
  ok &= Require(unresolved.messages.has_errors(), "unresolved FROM query lacked diagnostic");

  const auto resolved = BindSql("SELECT * FROM customer",
                                endpoint_config,
                                session,
                                {"00000000-0000-7000-8000-00000000c001"});
  ok &= Require(resolved.bound, "resolved FROM query did not bind");
  ok &= Require(!resolved.messages.has_errors(), "resolved FROM query produced diagnostics");
  ok &= Require(resolved.resolved_object_uuids.size() == 1,
                "resolved UUID list size mismatch");
  ok &= Require(resolved.resolved_object_uuids[0] ==
                    "00000000-0000-7000-8000-00000000c001",
                "resolved UUID value mismatch");
  return ok;
}

bool ValidateSecurityAndTransactionAuthorityMetadata() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "unix:/tmp/sb_server.sbps.sock";
  const auto session = AuthenticatedSession();
  bool ok = true;

  const auto grant = BindSql("GRANT SELECT ON customer TO app_role",
                             config,
                             session,
                             {"00000000-0000-7000-8000-00000000c002"});
  ok &= Require(grant.bound, "GRANT did not bind with server-resolved UUID evidence");
  ok &= Require(grant.requires_security_authority, "GRANT lacked security authority flag");
  ok &= Require(grant.security_authority_key ==
                    "authority.server.security_policy_context_required",
                "GRANT security authority key mismatch");
  ok &= Require(HasStep(grant, "authority.server.security_policy_context_required"),
                "GRANT security authority step missing");

  const auto set_transaction = BindSql("SET TRANSACTION READ WRITE", config, session);
  ok &= Require(set_transaction.bound, "SET TRANSACTION did not bind");
  ok &= Require(set_transaction.requires_transaction_authority,
                "SET TRANSACTION lacked transaction authority flag");
  ok &= Require(set_transaction.transaction_authority_key ==
                    "authority.server.transaction_context_required",
                "SET TRANSACTION authority key mismatch");
  ok &= Require(HasStep(set_transaction, "authority.server.transaction_context_required"),
                "SET TRANSACTION authority step missing");
  return ok;
}

bool ValidateClusterPrivateFailClosed() {
  const auto* descriptor = FindStatementSurfaceByName("cluster_setting_stmt");
  bool ok = Require(descriptor != nullptr, "missing cluster_setting_stmt descriptor");
  if (descriptor == nullptr) return ok;

  AstDocument ast;
  ast.family = StatementFamily::kClusterPrivate;
  ast.statement_surface_id = std::string(descriptor->surface_id);
  ast.statement_surface_name = std::string(descriptor->canonical_name);
  ast.statement_parser_category = "cluster_private";
  ast.parser_handler_key = std::string(descriptor->parser_handler_key);
  ast.statement_binding_contract_key = std::string(descriptor->binding_contract_key);
  ast.statement_admission_contract_key = std::string(descriptor->admission_contract_key);
  ast.statement_behavior_descriptor_key = std::string(descriptor->behavior_descriptor_key);
  ast.diagnostic_key = std::string(descriptor->diagnostic_key);
  ast.operation_family = std::string(descriptor->sblr_operation_family);
  ast.produces_sblr = true;
  ast.requires_cluster_profile = true;
  ast.exact_refusal_required = true;

  CstDocument cst;
  cst.source = "CLUSTER SETTING x = y";
  ParserConfig config;
  config.probe_mode = true;
  const auto bound = BindAst(ast, cst, config, AuthenticatedSession());
  ok &= Require(!bound.bound, "cluster-private statement unexpectedly bound");
  ok &= Require(bound.messages.has_errors(), "cluster-private statement lacked diagnostic");
  ok &= Require(bound.requires_cluster_profile, "cluster profile flag missing");
  ok &= Require(bound.exact_refusal_required, "cluster exact-refusal flag missing");
  ok &= Require(HasStep(bound, "authority.cluster.profile_gate_required"),
                "cluster authority step missing");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateMetadataPreservation();
  ok &= ValidateAuthenticationGate();
  ok &= ValidatePublicResolverGate();
  ok &= ValidateSecurityAndTransactionAuthorityMetadata();
  ok &= ValidateClusterPrivateFailClosed();
  return ok ? 0 : 1;
}
