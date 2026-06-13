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
#include "rendering/rendering.hpp"
#include "resources/language_resource_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
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

std::vector<std::string> CanonicalIds(const sbsql::CanonicalElementStream& stream) {
  std::vector<std::string> ids;
  for (const auto& element : stream.elements) ids.push_back(element.canonical_id);
  return ids;
}

sbsql::LanguageNormalizationOptions FrCaSovProfile() {
  sbsql::LanguageNormalizationOptions profile;
  profile.language_profile_uuid = "sbsql.lang.fr-ca.release-test";
  profile.exact_language_tag = "fr-CA";
  profile.topology_profile_uuid = "topology.sbsql.test.sov.v1";
  profile.input_syntax_profile_uuid = "sbsql.syntax.fr-ca.test.sov";
  profile.common_resource_hash = "common.hash.fr-ca.test";
  profile.aliases.push_back(sbsql::LanguageTokenAlias{
      "selectionner", "SELECT", "SBSQL.TOKEN.SELECT", "alias.fr-ca.selectionner"});
  profile.aliases.push_back(sbsql::LanguageTokenAlias{
      "depuis", "FROM", "SBSQL.TOKEN.FROM", "alias.fr-ca.depuis"});
  return profile;
}

sbsql::LanguageNormalizationOptions FrCaGrammarAliasProfile() {
  sbsql::LanguageNormalizationOptions profile;
  profile.language_profile_uuid = "sbsql.lang.fr-ca.grammar-alias-test";
  profile.exact_language_tag = "fr-CA";
  profile.topology_profile_uuid = "topology.sbsql.test.statement-order";
  profile.input_syntax_profile_uuid = "sbsql.syntax.fr-ca.test.grammar-alias";
  profile.common_resource_hash = "common.hash.fr-ca.grammar-alias-test";
  profile.aliases.push_back(sbsql::LanguageTokenAlias{
      "jointure", "JT", "SBSQL.TOKEN.JT", "alias.fr-ca.jointure"});
  return profile;
}

sbsql::SessionContext ParserSession() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000005101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000005102";
  session.database_uuid = "019f0000-0000-7000-8000-000000005103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.default_language = "en";
  session.language_tag = "fr-CA";
  session.language_profile = "sbsql.lang.fr-ca.grammar-alias-test";
  session.input_syntax_profile = "sbsql.syntax.fr-ca.test.grammar-alias";
  session.common_resource_hash = "common.hash.fr-ca.grammar-alias-test";
  session.catalog_epoch = 5;
  session.security_policy_epoch = 6;
  session.descriptor_epoch = 7;
  session.language_resource_epoch = 8;
  return session;
}

sbsql::ParserConfig ParserConfigForTest() {
  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sml_canonical_runtime";
  config.parser_uuid = "019f0000-0000-7000-8000-000000005104";
  config.bundle_contract_id = "sbp_sbsql@sml-canonical-runtime";
  config.build_id = "sbsql-sml-canonical-runtime";
  config.profile_id = "sbsql.lang.fr-ca.grammar-alias-test";
  return config;
}

void VerifyEnglishAndLocalizedStreamsMatch() {
  const auto canonical = sbsql::BuildCst("select name from users");
  const auto localized = sbsql::BuildCst("depuis users selectionner name", FrCaSovProfile());

  Require(sbsql::ReconstructSourceFromTokens(localized) ==
              "depuis users selectionner name",
          "SML-005 localized source reconstruction changed");
  Require(localized.language_profile_uuid == "sbsql.lang.fr-ca.release-test",
          "SML-005 language profile identity was not retained");
  Require(localized.topology_profile_uuid == "topology.sbsql.test.sov.v1",
          "SML-082 topology profile identity was not retained");
  Require(!localized.canonical_element_stream.elements.empty(),
          "SML-005 canonical element stream was not emitted");

  const auto canonical_ids = CanonicalIds(canonical.canonical_element_stream);
  const auto localized_ids = CanonicalIds(localized.canonical_element_stream);
  Require(canonical_ids == localized_ids,
          "SML-005/SML-082 localized topology did not normalize to canonical English stream");
  Require(localized_ids.size() == 4, "SML-005 unexpected canonical stream size");
  Require(localized_ids[0] == "SBSQL.TOKEN.SELECT" &&
              localized_ids[1] == "SBSQL.TOKEN.IDENTIFIER" &&
              localized_ids[2] == "SBSQL.TOKEN.FROM" &&
              localized_ids[3] == "SBSQL.TOKEN.IDENTIFIER",
          "SML-005 canonical stream order is not SELECT projection FROM source");

  const auto validation = sbsql::ValidateCanonicalElementStream(localized.canonical_element_stream);
  Require(validation.accepted, "SML-005 runtime canonical element stream failed validation");
  Require(localized.canonical_element_stream.normalized_before_uuid_resolution,
          "SML-082 canonical stream does not record pre-UUID normalization");
  Require(localized.canonical_element_stream.server_revalidation_required,
          "SML-083 parser canonical stream did not require server revalidation");
}

void VerifyAstConsumesCanonicalStream() {
  const auto localized = sbsql::BuildCst("depuis users selectionner name", FrCaSovProfile());
  const auto ast = sbsql::BuildAst(localized);
  Require(!ast.messages.has_errors(), "SML-006 localized canonical AST raised diagnostics");
  Require(ast.family == sbsql::StatementFamily::kQuery,
          "SML-006 AST did not classify localized canonical stream as query");
  Require(ast.operation_family == "sblr.query.relational.v3",
          "SML-006 localized canonical stream did not select query SBLR family");
  Require(ast.source_text == "depuis users selectionner name",
          "SML-005 AST source artifact was not preserved");
}

void VerifyCanonicalStreamFeedsLowering() {
  const auto cst = sbsql::BuildCst("jointure COLUMN doc_path;",
                                   FrCaGrammarAliasProfile());
  const auto ast = sbsql::BuildAst(cst);
  const auto session = ParserSession();
  const auto bound = sbsql::BindAst(ast, cst, ParserConfigForTest(), session, {});
  const auto envelope = sbsql::LowerToSblr(bound, cst, session);
  const auto verifier = sbsql::VerifySblrEnvelope(envelope);

  if (cst.messages.has_errors()) {
    std::cerr << sbsql::RenderMessageVectorSet(cst.messages);
  }
  if (ast.messages.has_errors()) {
    std::cerr << sbsql::RenderMessageVectorSet(ast.messages);
  }
  if (!bound.bound) {
    std::cerr << sbsql::RenderMessageVectorSet(bound.messages);
  }
  if (!verifier.admitted) {
    std::cerr << sbsql::RenderMessageVectorSet(verifier.messages);
  }

  Require(!cst.messages.has_errors(), "SML-006 localized grammar CST failed");
  Require(!ast.messages.has_errors(), "SML-006 localized grammar AST failed");
  Require(ast.statement_surface_id == "SBSQL-9CB80D4097E7",
          "SML-006 localized grammar AST did not resolve canonical surface id");
  Require(bound.bound, "SML-006 localized grammar bind failed");
  Require(verifier.admitted, "SML-006 localized grammar SBLR verification failed");
  Require(envelope.operation_family == "sblr.query.relational.v3",
          "SML-006 localized grammar lowering operation family mismatch");
  Require(envelope.operation_id == "query.plan_operation",
          "SML-006 localized grammar lowering operation id mismatch");
  Require(envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SML-006 localized grammar lowering opcode mismatch");
  Require(envelope.source_artifact_policy == "span_metadata_only",
          "SML-006 localized grammar source artifact policy drifted");
  Require(!envelope.parser_executes_sql,
          "SML-006 localized grammar lowering allowed parser SQL execution");
  Require(envelope.payload.find("SBSQL-9CB80D4097E7") != std::string::npos,
          "SML-006 localized grammar payload missing canonical surface id");
  Require(envelope.payload.find("jointure COLUMN doc_path") == std::string::npos,
          "SML-006 localized grammar payload embedded source SQL text");
}

void VerifyNonSelectCanonicalStreamKeepsStatementOrder() {
  const auto cst = sbsql::BuildCst("THROTTLE ASSIGN limit;");
  const auto ids = CanonicalIds(cst.canonical_element_stream);
  Require(ids.size() >= 3, "SML-006 descriptor canonical stream missing tokens");
  Require(cst.canonical_element_stream.elements[0].canonical_text == "THROTTLE" &&
              cst.canonical_element_stream.elements[1].canonical_text == "ASSIGN" &&
              cst.canonical_element_stream.elements[2].canonical_text == "LIMIT",
          "SML-006 non-SELECT descriptor canonical stream was reordered");
}

void VerifyTokenCanonicalMetadata() {
  const auto cst = sbsql::BuildCst("select 1");
  const auto token = std::find_if(cst.tokens.begin(), cst.tokens.end(),
                                  [](const sbsql::Token& candidate) {
                                    return candidate.text == "select";
                                  });
  Require(token != cst.tokens.end(), "SML-005 SELECT token not found");
  Require(token->canonical_text == "SELECT", "SML-005 canonical token text missing");
  Require(token->canonical_token_id == "SBSQL.TOKEN.SELECT",
          "SML-005 canonical token id missing");
}

} // namespace

int main() {
  VerifyEnglishAndLocalizedStreamsMatch();
  VerifyAstConsumesCanonicalStream();
  VerifyCanonicalStreamFeedsLowering();
  VerifyNonSelectCanonicalStreamKeepsStatementOrder();
  VerifyTokenCanonicalMetadata();
  std::cout << "sbsql_canonical_element_stream_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
