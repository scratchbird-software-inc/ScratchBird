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

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

SessionContext Session() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000007";
  session.connection_uuid = "00000000-0000-7000-8000-000000000107";
  session.database_uuid = "00000000-0000-7000-8000-000000000207";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ConfigWithResolver() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "00000000-0000-7000-8000-00000000b007";
  config.bundle_contract_id = "sbp_sbsql@lowering-test";
  config.build_id = "sblr-lowering-test";
  config.server_endpoint = "unix:/tmp/sb_server.sbps.sock";
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
                              const std::vector<std::string>& resolved_object_uuids = {}) {
  PipelineArtifacts artifacts;
  const auto session = Session();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ConfigWithResolver(), session,
                            resolved_object_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

bool ValidateAdmittedSelectEnvelope() {
  const auto artifacts = RunPipeline("SELECT 1");
  bool ok = true;
  ok &= Require(artifacts.bound.bound, "SELECT 1 did not bind");
  ok &= Require(!artifacts.envelope.payload.empty(), "SELECT 1 produced empty SBLR payload");
  ok &= Require(artifacts.verifier.admitted, "SELECT 1 SBLR envelope was not verifier-admitted");
  ok &= Require(!artifacts.verifier.messages.has_errors(), "SELECT 1 verifier emitted diagnostics");
  ok &= Require(artifacts.envelope.envelope_version == 3, "SBLR envelope version mismatch");
  ok &= Require(artifacts.envelope.operation_family == artifacts.bound.operation_family,
                "operation family not preserved");
  ok &= Require(artifacts.envelope.sblr_operation_key == artifacts.bound.sblr_operation_key,
                "SBLR operation key not preserved");
  ok &= Require(artifacts.envelope.surface_key == artifacts.bound.surface_key,
                "surface key not preserved");
  ok &= Require(artifacts.envelope.command_family == "query", "command family mismatch");
  ok &= Require(artifacts.envelope.result_shape_key == "result.shape.rowset",
                "result shape mismatch");
  ok &= Require(artifacts.envelope.resource_contract_key == "resource.contract.query_read",
                "resource contract mismatch");
  ok &= Require(artifacts.envelope.catalog_epoch == 7, "catalog epoch mismatch");
  ok &= Require(artifacts.envelope.security_policy_epoch == 11,
                "security policy epoch mismatch");
  ok &= Require(artifacts.envelope.descriptor_epoch == 13, "descriptor epoch mismatch");
  ok &= Require(HasValue(artifacts.envelope.required_rights, "right.read"),
                "required right missing");
  ok &= Require(HasValue(artifacts.envelope.required_authority_steps,
                         "authority.parser.syntax_evidence_only"),
                "syntax authority step missing");
  ok &= Require(artifacts.envelope.payload.find("SELECT 1") == std::string::npos,
                "SBLR payload embedded SQL text");
  ok &= Require(artifacts.envelope.payload.find("\"source_text\"") == std::string::npos,
                "SBLR payload embedded source_text field");
  return ok;
}

bool ValidateResolvedNameEnvelope() {
  const auto artifacts = RunPipeline("SELECT * FROM customer",
                                     {"00000000-0000-7000-8000-00000000c007"});
  bool ok = true;
  ok &= Require(artifacts.bound.bound, "resolved SELECT FROM did not bind");
  ok &= Require(artifacts.verifier.admitted, "resolved SELECT FROM was not verifier-admitted");
  ok &= Require(artifacts.envelope.resolved_object_uuids.size() == 1,
                "resolved UUID count mismatch");
  ok &= Require(artifacts.envelope.resolved_object_uuids[0] ==
                    "00000000-0000-7000-8000-00000000c007",
                "resolved UUID mismatch");
  ok &= Require(HasValue(artifacts.envelope.required_authority_steps,
                         "authority.server.resolve_name_registry_public"),
                "resolver authority step missing");
  ok &= Require(!artifacts.envelope.descriptor_refs.empty(), "descriptor refs missing");
  return ok;
}

bool ValidateSecurityEnvelope() {
  const auto artifacts = RunPipeline("GRANT SELECT ON customer TO app_role",
                                     {"00000000-0000-7000-8000-00000000c107",
                                      "00000000-0000-7000-8000-00000000c207"});
  bool ok = true;
  ok &= Require(artifacts.bound.bound, "GRANT did not bind");
  ok &= Require(artifacts.verifier.admitted, "GRANT envelope not verifier-admitted");
  ok &= Require(artifacts.envelope.command_family == "security", "GRANT command family mismatch");
  ok &= Require(HasValue(artifacts.envelope.required_rights, "right.security_admin"),
                "GRANT required right missing");
  ok &= Require(HasValue(artifacts.envelope.required_authority_steps,
                         "authority.server.security_policy_context_required"),
                "GRANT security authority step missing");
  ok &= Require(!artifacts.envelope.policy_refs.empty(), "GRANT policy refs missing");
  return ok;
}

bool ValidateUnboundRefusal() {
  ParserConfig config = ConfigWithResolver();
  config.server_endpoint.clear();
  const auto session = Session();
  const auto cst = BuildCst("SELECT * FROM customer");
  const auto ast = BuildAst(cst);
  const auto bound = BindAst(ast, cst, config, session);
  const auto envelope = LowerToSblr(bound, cst, session);
  const auto verifier = VerifySblrEnvelope(envelope);
  bool ok = true;
  ok &= Require(!bound.bound, "unresolved SELECT FROM unexpectedly bound");
  ok &= Require(envelope.payload.empty(), "unbound statement produced SBLR payload");
  ok &= Require(!verifier.admitted, "unbound statement was verifier-admitted");
  ok &= Require(verifier.messages.has_errors(), "unbound statement lacked verifier diagnostics");
  return ok;
}

bool ValidateMalformedEnvelopeRejected() {
  SblrEnvelope envelope;
  envelope.envelope_version = 3;
  envelope.operation_family = "sblr.query.relational.v3";
  envelope.sblr_operation_key = "sblr.query.relational.v3";
  envelope.statement_hash = 1;
  envelope.surface_key = "SBSQL-INVALID";
  envelope.command_family = "query";
  envelope.result_shape_key = "result.shape.rowset";
  envelope.diagnostic_shape_key = "diagnostic.canonical_message_vector";
  envelope.resource_contract_key = "resource.contract.query_read";
  envelope.required_authority_steps.push_back("authority.parser.syntax_evidence_only");
  envelope.payload = "{\"sql\":\"SELECT 1\"}";
  const auto verifier = VerifySblrEnvelope(envelope);
  bool ok = true;
  ok &= Require(!verifier.admitted, "malformed envelope was verifier-admitted");
  ok &= Require(verifier.messages.has_errors(), "malformed envelope lacked diagnostics");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateAdmittedSelectEnvelope();
  ok &= ValidateResolvedNameEnvelope();
  ok &= ValidateSecurityEnvelope();
  ok &= ValidateUnboundRefusal();
  ok &= ValidateMalformedEnvelopeRejected();
  return ok ? 0 : 1;
}
