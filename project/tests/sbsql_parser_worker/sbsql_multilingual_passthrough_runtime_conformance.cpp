// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
#include "lowering/lowering.hpp"
#include "metrics/parser_metrics.hpp"
#include "rendering/rendering.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool HasDiagnosticCode(const sbsql::MessageVectorSet& messages,
                       std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticCodes(const sbsql::MessageVectorSet& messages) {
  std::string out;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!out.empty()) out += ',';
    out += diagnostic.code;
  }
  return out.empty() ? "none" : out;
}

sbsql::ParserConfig Config() {
  sbsql::ParserConfig config;
  config.parser_uuid = "sml018-sml020-sml090-parser";
  config.dialect = "sbsql";
  config.profile_id = "default";
  config.registry_version = sbsql::kSbsqlWorkerRegistryCurrentVersion;
  config.protocol_version = sbsql::kSbsqlWorkerProtocolCurrentVersion;
  config.bundle_contract_id = "sbp_sbsql@1";
  config.build_id = "sml018-sml020-sml090-build";
  return config;
}

void SeedAuthenticatedContext(sbsql::SbsqlTestWireSession* session,
                              std::string_view requested_language_tag) {
  auto& context = const_cast<sbsql::SessionContext&>(session->session());
  context.authenticated = true;
  context.session_uuid = "00000000-0000-7000-8000-000000000901";
  context.connection_uuid = "00000000-0000-7000-8000-000000000902";
  context.database_uuid = "00000000-0000-7000-8000-000000000903";
  context.authenticated_user_uuid = "00000000-0000-7000-8000-000000000904";
  context.principal_claim = "sml-runtime-user";
  context.auth_provider_family = "test";
  context.effective_role_uuids = {"00000000-0000-7000-8000-000000000905"};
  context.effective_group_uuids = {"00000000-0000-7000-8000-000000000906"};
  context.search_path = {"public"};
  context.default_language = "en";
  context.language_tag = requested_language_tag.empty()
                             ? "en"
                             : std::string(requested_language_tag);
  context.language_profile = context.language_tag == "en"
                                 ? "sbsql.builtin.recovery.en"
                                 : "sbsql.language-profile." + context.language_tag;
  context.input_syntax_profile = "sbsql.syntax.standard";
  context.input_language_fallback_tag =
      context.language_tag == "en" ? "" : "en";
  context.common_resource_hash = "builtin.common.sbsql.v1";
  context.dialect_profile_uuid = "sbsql/default";
  context.policy_profile_uuid = "policy/default";
  context.resource_compatibility_identity = "sbsql.resource.compat.v1";
  context.resource_version_identity = "sbsql.resource-pack.v1";
  context.language_resource_epoch = 901;
  context.localized_name_epoch = 902;
  context.message_resource_epoch = 903;
  context.transaction_context = "read_only_prepare";
  context.catalog_epoch = 904;
  context.security_policy_epoch = 905;
  context.grant_epoch = 906;
  context.descriptor_epoch = 907;
  context.udr_epoch = 908;
  context.result_rendering_policy = "result/default";
  context.metric_redaction_policy = "redaction/default";
}

void VerifyStandardEnglishFallbackPassThrough() {
  constexpr std::string_view kParserOnlyQueryCarrier =
      "ENGINE QUERY BIND EXPRESSION";
  const auto is_exact_query_carrier = [](const sbsql::PipelineResult& result) {
    return Contains(result.sblr_payload,
                    "\"operation_id\":\"query.bind_expression\"") &&
           Contains(result.sblr_payload,
                    "\"sblr_operation\":\"SBLR_QUERY_BIND_EXPRESSION\"");
  };
  sbsql::SblrTemplateCache cache(16);
  sbsql::ParserMetrics metrics;
  const auto config = Config();
  sbsql::SbsqlTestWireSession preferred_session(config, &metrics, &cache);
  SeedAuthenticatedContext(&preferred_session, "fr-CA");

  const auto first =
      preferred_session.RunPipeline(kParserOnlyQueryCarrier, false);
  Require(first.accepted,
          std::string("standard English SBsql was not accepted under fr-CA "
                      "preferred language diagnostics=") +
              DiagnosticCodes(first.messages));
  Require(!first.frontdoor_cache_hit,
          "first fr-CA standard English fallback parse unexpectedly hit cache");
  Require(!first.parser_executes_sql,
          "pass-through parse claimed parser-side SQL execution");
  Require(!first.cached_storage_authority &&
              !first.cached_authorization_authority &&
              !first.cached_finality_authority,
          "pass-through parse cached engine authority");
  Require(first.statement_family == "query",
          "pass-through parse did not retain query statement family");
  Require(!first.operation_family.empty(),
          "pass-through parse lost operation family evidence");
  Require(is_exact_query_carrier(first),
          "pass-through payload lost exact query.bind_expression carrier identity");
  Require(Contains(first.sblr_payload, "\"parser_executes_sql\":false"),
          "pass-through SBLR payload lost parser authority boundary");

  const auto& session = preferred_session.session();
  Require(session.default_language == "en",
          "preferred-language session did not retain standard English default");
  Require(session.language_tag == "fr-CA",
          "preferred-language session did not preserve requested language tag");
  Require(session.language_profile == "sbsql.language-profile.fr-CA",
          "preferred-language session did not preserve requested profile");
  Require(session.input_syntax_profile == "sbsql.syntax.standard",
          "preferred-language session changed standard SBsql syntax profile");
  Require(session.input_language_fallback_tag == "en",
          "preferred-language session did not advertise standard English fallback");

  const auto second =
      preferred_session.RunPipeline(kParserOnlyQueryCarrier, false);
  Require(second.accepted, "cached fallback parse was not accepted");
  Require(second.frontdoor_cache_hit,
          "second fr-CA standard English fallback parse did not hit cache");
  Require(second.sblr_payload == first.sblr_payload,
          "cached fallback payload changed");

  sbsql::ParserMetrics english_metrics;
  sbsql::SbsqlTestWireSession english_session(config, &english_metrics, &cache);
  SeedAuthenticatedContext(&english_session, "en");
  const auto english = english_session.RunPipeline(kParserOnlyQueryCarrier, false);
  Require(english.accepted,
          std::string("standard English session was not accepted diagnostics=") +
              DiagnosticCodes(english.messages));
  Require(!english.frontdoor_cache_hit,
          "canonical English session reused preferred-language fallback cache entry");
  Require(is_exact_query_carrier(english),
          "canonical English payload lost exact query.bind_expression carrier identity");

  const auto snapshot = cache.SnapshotJson();
  Require(Contains(snapshot, "\"language_profile\"") &&
              Contains(snapshot, "\"language_tag\"") &&
              Contains(snapshot, "\"input_language_fallback_tag\"") &&
              Contains(snapshot, "\"language_resource_epoch\"") &&
              Contains(snapshot, "\"resource_compatibility_identity\"") &&
              Contains(snapshot, "\"resource_version_identity\""),
          "cache snapshot does not disclose language/resource dimensions");
  Require(Contains(snapshot, "sbsql.language-profile.fr-CA") &&
              Contains(snapshot, ":fr-CA:sbsql.syntax.standard:en:"),
          "cache snapshot did not retain preferred language fallback key evidence");
  Require(Contains(snapshot, "sbsql.builtin.recovery.en") &&
              Contains(snapshot, ":en:sbsql.syntax.standard::"),
          "cache snapshot did not retain canonical English key evidence");
}

sbsql::SblrEnvelope MinimalEnvelope() {
  sbsql::SblrEnvelope envelope;
  envelope.operation_family = "sblr.query.relational.v3";
  envelope.sblr_operation_key = "sblr.query.relational.v3";
  envelope.statement_hash = 42;
  envelope.surface_key = "sbsql.test.family";
  envelope.command_family = "query";
  envelope.operation_id = "query.evaluate_projection";
  envelope.sblr_opcode = "SBLR_QUERY_EVALUATE_PROJECTION";
  envelope.engine_api_function = "EnginePlanOperation";
  envelope.result_shape_key = "result.shape.test";
  envelope.diagnostic_shape_key = "diagnostic.canonical_message_vector";
  envelope.resource_contract_key = "resource.contract.test";
  envelope.required_authority_steps = {
      "authority.engine.query_projection_api_required",
      "authority.server.transaction_context_required",
      "authority.parser.no_security_authorization",
      "authority.parser.no_storage_or_finality",
      "authority.parser.no_sql_text_execution"};
  envelope.payload =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"query_envelope_kind\":\"scalar_projection\","
      "\"projection_count\":1,"
      "\"source_relation_required\":false,"
      "\"row_storage_touched\":false,"
      "\"sql_text_included\":false,"
      "\"contains_sql_text\":false,"
      "\"source_payload_embedded\":false}";
  return envelope;
}

void VerifyParserFamilyNegativesFailClosed() {
  const auto valid = sbsql::VerifySblrEnvelope(MinimalEnvelope());
  Require(valid.admitted,
          std::string("baseline parser family envelope was refused diagnostics=") +
              DiagnosticCodes(valid.messages));

  auto mismatched = MinimalEnvelope();
  mismatched.sblr_operation_key = "sblr.catalog.mutation.v3";
  const auto mismatch = sbsql::VerifySblrEnvelope(mismatched);
  Require(!mismatch.admitted,
          "mismatched parser operation family was admitted");
  Require(HasDiagnosticCode(mismatch.messages,
                            "SBSQL.SBLR.OPERATION_FAMILY_MISMATCH"),
          "mismatched parser operation family did not emit fail-closed diagnostic");

  auto missing = MinimalEnvelope();
  missing.operation_family.clear();
  const auto missing_family = sbsql::VerifySblrEnvelope(missing);
  Require(!missing_family.admitted,
          "missing parser operation family was admitted");
  Require(HasDiagnosticCode(missing_family.messages,
                            "SBSQL.SBLR.OPERATION_FAMILY_MISSING"),
          "missing parser operation family did not emit fail-closed diagnostic");
}

void VerifyPreferredAndFallbackSblrRendering() {
  const auto envelope = MinimalEnvelope();

  sbsql::SblrRenderRequest preferred;
  preferred.sblr_uuid_authority_valid = true;
  preferred.preferred_renderer_available = true;
  preferred.canonical_english_renderer_available = true;
  preferred.preferred_language_profile = "fr-CA";
  const auto preferred_result =
      sbsql::RenderSblrEnvelopeWithProfileSelection(envelope, preferred);
  Require(preferred_result.ok,
          "SML-084 preferred-language SBLR renderer did not accept admitted envelope");
  Require(preferred_result.selection.decision ==
              sbsql::SblrRenderDecision::kPreferredLanguage,
          "SML-084 preferred-language renderer did not preserve preferred decision");
  Require(preferred_result.selection.selected_language_profile == "fr-CA",
          "SML-084 preferred-language renderer lost selected profile evidence");
  Require(preferred_result.selection.server_revalidation_required,
          "SML-084 preferred-language renderer lost server revalidation requirement");
  Require(Contains(preferred_result.text, "SBLR sblr.query.relational.v3 42"),
          "SML-084 preferred-language renderer lost SBLR output");

  sbsql::SblrRenderRequest fallback;
  fallback.sblr_uuid_authority_valid = true;
  fallback.preferred_renderer_available = false;
  fallback.canonical_english_renderer_available = true;
  fallback.preferred_language_profile = "fr-CA";
  fallback.canonical_english_profile = "sbsql.builtin.recovery.en";
  const auto fallback_result =
      sbsql::RenderSblrEnvelopeWithProfileSelection(envelope, fallback);
  Require(fallback_result.ok,
          "SML-084 canonical English fallback renderer did not accept admitted envelope");
  Require(fallback_result.selection.decision ==
              sbsql::SblrRenderDecision::kCanonicalEnglishFallback,
          "SML-084 fallback renderer did not preserve fallback decision");
  Require(fallback_result.selection.used_canonical_english_fallback,
          "SML-084 fallback renderer did not record canonical English fallback");
  Require(fallback_result.selection.fallback_language_profile ==
              "sbsql.builtin.recovery.en",
          "SML-084 fallback renderer lost canonical English profile evidence");
  Require(fallback_result.selection.server_revalidation_required,
          "SML-084 fallback renderer lost server revalidation requirement");
  Require(HasDiagnosticCode(fallback_result.messages,
                            "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH"),
          "SML-084 fallback renderer did not emit fallback diagnostic");
  Require(Contains(fallback_result.text, "SBLR sblr.query.relational.v3 42"),
          "SML-084 fallback renderer lost SBLR output");
}

} // namespace

int main() {
  VerifyStandardEnglishFallbackPassThrough();
  VerifyParserFamilyNegativesFailClosed();
  VerifyPreferredAndFallbackSblrRendering();
  std::cout << "sbsql_multilingual_passthrough_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
