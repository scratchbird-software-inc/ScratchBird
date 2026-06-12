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

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

sbsql::ParserConfig TestConfig() {
  sbsql::ParserConfig config;
  config.parser_uuid = "parser.sml.language-control";
  config.bundle_contract_id = "sbp_sbsql@sml-language-control";
  config.build_id = "sml-language-control-test";
  return config;
}

sbsql::SessionContext TestSession() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "session.sml.language-control";
  session.connection_uuid = "connection.sml.language-control";
  session.database_uuid = "database.sml.language-control";
  session.language_profile = "sbsql.builtin.recovery.en";
  session.language_tag = "en";
  session.language_resource_epoch = 17;
  session.catalog_epoch = 11;
  session.security_policy_epoch = 13;
  session.descriptor_epoch = 19;
  return session;
}

struct LoweredRoute {
  sbsql::AstDocument ast;
  sbsql::BoundStatement bound;
  sbsql::SblrEnvelope envelope;
  sbsql::SblrVerifierResult verifier;
};

LoweredRoute Lower(std::string_view sql) {
  const auto cst = sbsql::BuildCst(sql);
  auto ast = sbsql::BuildAst(cst);
  const auto config = TestConfig();
  const auto session = TestSession();
  auto bound = sbsql::BindAst(ast, cst, config, session);
  auto envelope = sbsql::LowerToSblr(bound, cst, session);
  auto verifier = sbsql::VerifySblrEnvelope(envelope);
  return LoweredRoute{std::move(ast), std::move(bound),
                      std::move(envelope), std::move(verifier)};
}

void RequireLanguageEnvelope(const LoweredRoute& route,
                             std::string_view operation_id,
                             std::string_view opcode) {
  Require(route.ast.statement_parser_category == "language_resource",
          "SML-008/SML-009 language command did not classify as language_resource");
  Require(route.bound.bound, "SML-008/SML-009 language command did not bind");
  Require(route.envelope.operation_family == "sblr.language.resource_control.v3",
          "SML-008/SML-009 language command operation family mismatch");
  Require(route.envelope.operation_id == operation_id,
          "SML-008/SML-009 language command operation id mismatch");
  Require(route.envelope.sblr_opcode == opcode,
          "SML-008/SML-009 language command opcode mismatch");
  Require(route.envelope.payload.find("\"language_control\":true") !=
              std::string::npos,
          "SML-008/SML-009 language command payload marker missing");
  Require(route.envelope.payload.find("\"parser_executes_sql\":false") !=
              std::string::npos,
          "SML-008/SML-009 language command claimed parser SQL execution");
  Require(route.envelope.payload.find("\"row_storage_touched\":false") !=
              std::string::npos,
          "SML-008/SML-009 language command claimed row storage effects");
}

void VerifySessionLanguageControls() {
  auto route = Lower("SET LANGUAGE PROFILE fr_ca;");
  RequireLanguageEnvelope(route, "language.session.set",
                          "SBLR_LANGUAGE_SESSION_SET");
  Require(route.verifier.admitted, "SML-008 SET LANGUAGE was not verifier-admitted");
  Require(route.envelope.payload.find("\"target_language_profile\":\"fr_ca\"") !=
              std::string::npos,
          "SML-008 SET LANGUAGE target profile missing");
  Require(route.envelope.payload.find("\"parser_updates_session_language\":false") !=
              std::string::npos,
          "SML-008 SET LANGUAGE allowed parser-side session mutation");
  Require(route.envelope.payload.find("\"prepared_statement_reinterpretation\":false") !=
              std::string::npos,
          "SML-008 SET LANGUAGE did not preserve prepared statement safety");
  Require(Contains(route.envelope.required_authority_steps,
                   "authority.server.session_language_context_required"),
          "SML-008 SET LANGUAGE missing server session language authority");

  route = Lower("RESET LANGUAGE;");
  RequireLanguageEnvelope(route, "language.session.reset",
                          "SBLR_LANGUAGE_SESSION_RESET");
  Require(route.verifier.admitted, "SML-008 RESET LANGUAGE was not verifier-admitted");
  Require(route.envelope.payload.find("\"mutates_session_language\":true") !=
              std::string::npos,
          "SML-008 RESET LANGUAGE did not declare session-language mutation");

  route = Lower("SHOW LANGUAGE;");
  RequireLanguageEnvelope(route, "language.session.show",
                          "SBLR_LANGUAGE_SESSION_SHOW");
  Require(route.verifier.admitted, "SML-008 SHOW LANGUAGE was not verifier-admitted");
  Require(Contains(route.envelope.required_rights, "right.observe"),
          "SML-008 SHOW LANGUAGE did not use observe right");
}

void VerifyBundleControlsFailClosedWithoutManifest() {
  auto route = Lower("LOAD LANGUAGE BUNDLE fr_ca_bundle;");
  RequireLanguageEnvelope(route, "language.bundle.load",
                          "SBLR_LANGUAGE_BUNDLE_LOAD");
  Require(!route.verifier.admitted,
          "SML-009 LOAD LANGUAGE BUNDLE admitted without bundle manifest");
  Require(HasDiagnostic(route.verifier.messages,
                        "SBSQL.SBLR.LANGUAGE_BUNDLE_MANIFEST_REQUIRED"),
          "SML-009 LOAD LANGUAGE BUNDLE did not fail closed on missing manifest");
  Require(route.envelope.payload.find("\"admitted_bundle_manifest_attached\":false") !=
              std::string::npos,
          "SML-009 LOAD LANGUAGE BUNDLE did not expose missing manifest evidence");
  Require(route.envelope.payload.find("\"load_or_unload_effects_executed_by_parser\":false") !=
              std::string::npos,
          "SML-009 LOAD LANGUAGE BUNDLE claimed parser load effects");

  route = Lower("UNLOAD LANGUAGE BUNDLE fr_ca_bundle;");
  RequireLanguageEnvelope(route, "language.bundle.unload",
                          "SBLR_LANGUAGE_BUNDLE_UNLOAD");
  Require(!route.verifier.admitted,
          "SML-009 UNLOAD LANGUAGE BUNDLE admitted without bundle manifest");
  Require(HasDiagnostic(route.verifier.messages,
                        "SBSQL.SBLR.LANGUAGE_BUNDLE_MANIFEST_REQUIRED"),
          "SML-009 UNLOAD LANGUAGE BUNDLE did not fail closed on missing manifest");

  route = Lower("VALIDATE LANGUAGE BUNDLE fr_ca_bundle;");
  RequireLanguageEnvelope(route, "language.bundle.validate",
                          "SBLR_LANGUAGE_BUNDLE_VALIDATE");
  Require(!route.verifier.admitted,
          "SML-009 VALIDATE LANGUAGE BUNDLE admitted without bundle manifest");
  Require(HasDiagnostic(route.verifier.messages,
                        "SBSQL.SBLR.LANGUAGE_BUNDLE_MANIFEST_REQUIRED"),
          "SML-009 VALIDATE LANGUAGE BUNDLE did not fail closed on missing manifest");
}

void VerifyMalformedLanguageControlsFailClosed() {
  const auto route = Lower("LOAD LANGUAGE BUNDLE;");
  Require(!route.envelope.messages.diagnostics.empty(),
          "SML-009 malformed LOAD LANGUAGE BUNDLE did not emit diagnostics");
  Require(HasDiagnostic(route.envelope.messages,
                        "SBSQL.LANGUAGE_CONTROL.UNSUPPORTED_SHAPE"),
          "SML-009 malformed LOAD LANGUAGE BUNDLE did not fail closed");
}

} // namespace

int main() {
  VerifySessionLanguageControls();
  VerifyBundleControlsFailClosedWithoutManifest();
  VerifyMalformedLanguageControlsFailClosed();
  std::cout << "sbsql_language_control_surface_conformance=passed\n";
  return EXIT_SUCCESS;
}
