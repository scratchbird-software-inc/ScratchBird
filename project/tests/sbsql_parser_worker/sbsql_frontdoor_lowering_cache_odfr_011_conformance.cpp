// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
#include "metrics/parser_metrics.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

std::string DiagnosticCodes(const sbsql::MessageVectorSet& messages) {
  std::string out;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!out.empty()) out += ',';
    out += diagnostic.code;
  }
  return out.empty() ? "none" : out;
}

sbsql::ParserConfig Config(std::string profile = "default",
                           std::uint32_t registry_version =
                               sbsql::kSbsqlWorkerRegistryCurrentVersion) {
  sbsql::ParserConfig config;
  config.parser_uuid = "odfr011-parser";
  config.dialect = "sbsql";
  config.profile_id = std::move(profile);
  config.registry_version = registry_version;
  config.protocol_version = sbsql::kSbsqlWorkerProtocolCurrentVersion;
  config.bundle_contract_id = "sbp_sbsql@1";
  config.build_id = "odfr011-build";
  return config;
}

void SeedEngineAuthenticatedContext(sbsql::SbsqlTestWireSession* session) {
  auto& context = const_cast<sbsql::SessionContext&>(session->session());
  context.authenticated = true;
  context.session_uuid = "00000000-0000-7000-8000-000000000011";
  context.connection_uuid = "00000000-0000-7000-8000-000000000012";
  context.database_uuid = "00000000-0000-7000-8000-000000000013";
  context.authenticated_user_uuid = "00000000-0000-7000-8000-000000000014";
  context.effective_role_uuids = {"00000000-0000-7000-8000-000000000015"};
  context.effective_group_uuids = {"00000000-0000-7000-8000-000000000016"};
  context.search_path = {"public"};
  context.dialect_profile_uuid = "sbsql/default";
  context.policy_profile_uuid = "policy/default";
  context.transaction_context = "read_only_prepare";
  context.catalog_epoch = 7;
  context.security_policy_epoch = 11;
  context.grant_epoch = 13;
  context.descriptor_epoch = 17;
  context.udr_epoch = 19;
  context.result_rendering_policy = "result/default";
  context.metric_redaction_policy = "redaction/default";
}

void RunFrontdoorCacheHitPath() {
  sbsql::SblrTemplateCache cache(16);
  sbsql::ParserMetrics metrics;
  const auto config = Config();
  sbsql::SbsqlTestWireSession session(config, &metrics, &cache);
  SeedEngineAuthenticatedContext(&session);

  const auto first = session.RunPipeline("select 1", false);
  Require(first.accepted,
          std::string("first lowering was not accepted diagnostics=") +
              DiagnosticCodes(first.messages));
  Require(!first.frontdoor_cache_hit, "first lowering unexpectedly hit cache");
  Require(!first.parser_executes_sql, "first lowering claimed parser SQL execution");
  Require(!first.cached_storage_authority, "first lowering cached storage authority");
  Require(!first.cached_authorization_authority,
          "first lowering cached authorization authority");
  Require(!first.cached_finality_authority, "first lowering cached finality authority");
  Require(Contains(first.sblr_payload, "\"parser_executes_sql\":false"),
          "lowered payload did not carry parser_executes_sql=false");

  const auto second = session.RunPipeline("select 1", false);
  Require(second.accepted, "cached lowering was not accepted");
  Require(second.frontdoor_cache_hit, "second lowering did not hit front-door cache");
  Require(second.sblr_payload == first.sblr_payload,
          "cached lowering payload changed");
  Require(second.statement_family == first.statement_family,
          "cached lowering lost statement family");
  Require(second.operation_family == first.operation_family,
          "cached lowering lost operation family");
  Require(second.statement_hash == first.statement_hash,
          "cached lowering lost statement hash");
  Require(!second.parser_executes_sql, "cached lowering claimed parser SQL execution");
  Require(!second.cached_storage_authority, "cached lowering cached storage authority");
  Require(!second.cached_authorization_authority,
          "cached lowering cached authorization authority");
  Require(!second.cached_finality_authority, "cached lowering cached finality authority");

  const auto snapshot = metrics.SnapshotJson(config, session.session(), cache);
  Require(Contains(snapshot, "\"sys.metrics.parsers.frontdoor_cache.attempts_total\":2"),
          "front-door cache attempts counter missing");
  Require(Contains(snapshot, "\"sys.metrics.parsers.frontdoor_cache.hits_total\":1"),
          "front-door cache hit counter missing");
  Require(Contains(snapshot, "\"sys.metrics.parsers.frontdoor_cache.misses_total\":1"),
          "front-door cache miss counter missing");
  Require(Contains(snapshot, "\"sys.metrics.parsers.frontdoor_cache.stores_total\":1"),
          "front-door cache store counter missing");
  Require(Contains(snapshot,
                   "\"sys.metrics.parsers.frontdoor_cache.parse_lower_skips_total\":1"),
          "front-door cache parse/lower skip counter missing");
  Require(Contains(snapshot, "\"storage\":false") &&
              Contains(snapshot, "\"authorization\":false") &&
              Contains(snapshot, "\"finality\":false"),
          "cache snapshot did not prove authority is not cached");
}

void RunDimensionMisses() {
  sbsql::SblrTemplateCache cache(16);
  sbsql::ParserMetrics base_metrics;
  const auto base_config = Config();
  sbsql::SbsqlTestWireSession base_session(base_config, &base_metrics, &cache);
  SeedEngineAuthenticatedContext(&base_session);
  const auto first = base_session.RunPipeline("select 1", false);
  Require(first.accepted,
          std::string("base cache seed was not accepted diagnostics=") +
              DiagnosticCodes(first.messages));
  const auto second = base_session.RunPipeline("select 1", false);
  Require(second.frontdoor_cache_hit, "base cache seed did not hit");

  sbsql::ParserMetrics registry_metrics;
  const auto registry_config = Config("default", base_config.registry_version + 1);
  sbsql::SbsqlTestWireSession registry_session(registry_config, &registry_metrics, &cache);
  SeedEngineAuthenticatedContext(&registry_session);
  const auto registry_changed = registry_session.RunPipeline("select 1", false);
  Require(registry_changed.accepted,
          std::string("registry-version changed lowering was not accepted diagnostics=") +
              DiagnosticCodes(registry_changed.messages));
  Require(!registry_changed.frontdoor_cache_hit,
          "registry version change reused stale front-door cache entry");

  sbsql::ParserMetrics profile_metrics;
  const auto profile_config = Config("profile.changed");
  sbsql::SbsqlTestWireSession profile_session(profile_config, &profile_metrics, &cache);
  SeedEngineAuthenticatedContext(&profile_session);
  const auto profile_changed = profile_session.RunPipeline("select 1", false);
  Require(profile_changed.accepted,
          std::string("profile changed lowering was not accepted diagnostics=") +
              DiagnosticCodes(profile_changed.messages));
  Require(!profile_changed.frontdoor_cache_hit,
          "parser profile change reused stale front-door cache entry");

  sbsql::ParserMetrics parameter_metrics;
  sbsql::SbsqlTestWireSession parameter_session(base_config, &parameter_metrics, &cache);
  SeedEngineAuthenticatedContext(&parameter_session);
  const auto parameter_changed = parameter_session.RunPipeline("select '1'", false);
  Require(parameter_changed.accepted,
          std::string("parameter-shape changed lowering was not accepted diagnostics=") +
              DiagnosticCodes(parameter_changed.messages));
  Require(!parameter_changed.frontdoor_cache_hit,
          "parameter shape change reused stale front-door cache entry");
}

void RunNormalizationSafety() {
  sbsql::SblrTemplateCache cache(16);
  sbsql::ParserMetrics metrics;
  const auto config = Config();
  sbsql::SbsqlTestWireSession session(config, &metrics, &cache);
  SeedEngineAuthenticatedContext(&session);

  const auto canonical = session.RunPipeline("select 1", false);
  Require(canonical.accepted,
          std::string("normalization seed was not accepted diagnostics=") +
              DiagnosticCodes(canonical.messages));
  Require(!canonical.frontdoor_cache_hit,
          "normalization seed unexpectedly hit front-door cache");

  const auto outside_case_whitespace = session.RunPipeline(" SELECT   1 ", false);
  Require(outside_case_whitespace.accepted,
          std::string("outside-token normalization query was not accepted diagnostics=") +
              DiagnosticCodes(outside_case_whitespace.messages));
  Require(outside_case_whitespace.frontdoor_cache_hit,
          "outside-token casing/whitespace normalization did not hit cache");

  sbsql::SblrTemplateCache literal_cache(16);
  sbsql::ParserMetrics literal_metrics;
  sbsql::SbsqlTestWireSession literal_session(config, &literal_metrics, &literal_cache);
  SeedEngineAuthenticatedContext(&literal_session);

  const auto lower_literal = literal_session.RunPipeline("select 'abc'", false);
  Require(lower_literal.accepted,
          std::string("lowercase literal query was not accepted diagnostics=") +
              DiagnosticCodes(lower_literal.messages));
  Require(!lower_literal.frontdoor_cache_hit,
          "lowercase literal query unexpectedly hit front-door cache");

  const auto upper_literal = literal_session.RunPipeline("select 'ABC'", false);
  Require(upper_literal.accepted,
          std::string("uppercase literal query was not accepted diagnostics=") +
              DiagnosticCodes(upper_literal.messages));
  Require(!upper_literal.frontdoor_cache_hit,
          "string literal case reused stale front-door cache entry");
  Require(upper_literal.statement_hash != lower_literal.statement_hash ||
              upper_literal.sblr_payload != lower_literal.sblr_payload,
          "string literal case produced indistinguishable lowering evidence");

  sbsql::SblrTemplateCache identifier_cache(16);
  sbsql::ParserMetrics identifier_metrics;
  sbsql::SbsqlTestWireSession identifier_session(config, &identifier_metrics,
                                                 &identifier_cache);
  SeedEngineAuthenticatedContext(&identifier_session);

  const auto lower_identifier =
      identifier_session.RunPipeline("select 1 as \"abc\"", false);
  Require(lower_identifier.accepted,
          std::string("lowercase quoted identifier query was not accepted diagnostics=") +
              DiagnosticCodes(lower_identifier.messages));
  Require(!lower_identifier.frontdoor_cache_hit,
          "lowercase quoted identifier query unexpectedly hit front-door cache");

  const auto upper_identifier =
      identifier_session.RunPipeline("select 1 as \"ABC\"", false);
  Require(upper_identifier.accepted,
          std::string("uppercase quoted identifier query was not accepted diagnostics=") +
              DiagnosticCodes(upper_identifier.messages));
  Require(!upper_identifier.frontdoor_cache_hit,
          "quoted identifier case reused stale front-door cache entry");
  Require(upper_identifier.statement_hash != lower_identifier.statement_hash ||
              upper_identifier.sblr_payload != lower_identifier.sblr_payload,
          "quoted identifier case produced indistinguishable lowering evidence");
}

}  // namespace

int main() {
  RunFrontdoorCacheHitPath();
  RunDimensionMisses();
  RunNormalizationSafety();
  std::cout << "sbsql_frontdoor_lowering_cache_odfr_011_conformance=passed\n";
  return EXIT_SUCCESS;
}
