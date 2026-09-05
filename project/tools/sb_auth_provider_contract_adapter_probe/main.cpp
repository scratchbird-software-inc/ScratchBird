// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_plugin_api.hpp"
#include "security/auth_provider_trusted_result.hpp"

#include <iostream>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>

namespace api = scratchbird::engine::internal_api;

namespace {

static_assert(
    !std::is_default_constructible_v<api::AuthProviderTrustedResult>,
    "trusted provider results must not be caller-constructible");

constexpr const char* kFuture = "4102444800000";
constexpr const char* kProof =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_auth_provider_contract_adapter_probe.sbdb";
  context.local_transaction_id = 700;
  context.security_context_present = true;
  context.database_uuid.canonical =
      "018f0000-0000-7000-8000-0000000c0001";
  context.session_uuid.canonical =
      "018f0000-0000-7000-8000-0000000c0002";
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-0000000c0003";
  context.trace_tags = {"right:AUTH_PROVIDER_ADMIN", "security.bootstrap"};
  return context;
}

api::EngineAuthenticateProviderRequest Request(const std::string& provider,
                                               const std::string& payload) {
  api::EngineAuthenticateProviderRequest request;
  request.context = Context();
  request.option_envelopes = {
      "provider:" + provider,
      "principal:alice",
      "auth_flow:login",
      "provider_enabled:true",
      "adapter_mode:contract",
      "provider_payload:" + payload,
  };
  return request;
}

bool HasEvidence(const api::EngineAuthenticateProviderResult& result,
                 const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   const std::string& code,
                   const std::string& detail = {}) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code &&
        (detail.empty() || diagnostic.detail == detail)) {
      return true;
    }
  }
  return false;
}

bool Denied(const api::EngineAuthenticateProviderResult& result,
            const std::string& detail) {
  return !result.ok && !result.authenticated &&
         HasDiagnostic(result, "SECURITY.AUTH_SOURCE_UNAVAILABLE", detail) &&
         HasEvidence(result, "auth_provider_contract_adapter");
}

int Finish(std::initializer_list<std::pair<std::string, bool>> checks) {
  bool ok = true;
  std::cout << "{";
  bool first = true;
  for (const auto& check : checks) {
    ok = ok && check.second;
    if (!first) { std::cout << ","; }
    std::cout << "\"" << check.first << "\":"
              << (check.second ? "true" : "false");
    first = false;
  }
  if (!first) { std::cout << ","; }
  std::cout << "\"ok\":" << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

}  // namespace

int main() {
  auto dependency = Request(
      "ldap_ad",
      "user=alice;endpoint=ldap.example;starttls=true;bind=allow;groups=DBA");
  dependency.option_envelopes.push_back("dependency:ldap_client:available");

  auto client_success = Request(
      "ldap_ad",
      "user=alice;endpoint=ldap.example;starttls=true;bind=allow;groups=DBA;client_result=success");
  client_success.option_envelopes.push_back(
      "dependency:ldap_client:available");
  client_success.option_envelopes.push_back("client_mode:external");
  client_success.option_envelopes.push_back("provider_client:real");
  client_success.option_envelopes.push_back("real_external_client:true");
  client_success.option_envelopes.push_back("client_result:success");

  auto signature = Request(
      "oidc_jwt",
      std::string("iss=issuer;aud=scratchbird;sub=alice;alg=rs256;exp=") +
          kFuture + ";sig=" + kProof + ";groups=DBA;validator=jwks");
  signature.option_envelopes.push_back(
      "dependency:oidc_jwt_client:available");

  auto groups = dependency;
  groups.option_envelopes.push_back("groups:materialized");
  groups.option_envelopes.push_back("groups_materialized:true");

  auto freshness = signature;
  freshness.option_envelopes.push_back("freshness:verified");
  freshness.option_envelopes.push_back("provider_result:fresh");

  auto provider_result = dependency;
  provider_result.option_envelopes.push_back("provider_result:allow");
  provider_result.option_envelopes.push_back("provider_authenticated:true");

  auto fixture = dependency;
  fixture.option_envelopes.push_back("allow_fixture:true");
  fixture.option_envelopes.push_back("fixture:success");
  fixture.option_envelopes.push_back("credential:valid");

  auto malformed = Request("ldap_ad", "user=alice;user=mallory");
  malformed.option_envelopes.push_back("dependency:ldap_client:available");

  const auto dependency_result = api::EngineAuthenticateProvider(dependency);
  const auto client_success_result =
      api::EngineAuthenticateProvider(client_success);
  const auto signature_result = api::EngineAuthenticateProvider(signature);
  const auto groups_result = api::EngineAuthenticateProvider(groups);
  const auto freshness_result = api::EngineAuthenticateProvider(freshness);
  const auto provider_result_result =
      api::EngineAuthenticateProvider(provider_result);
  const auto fixture_result = api::EngineAuthenticateProvider(fixture);
  const auto malformed_result = api::EngineAuthenticateProvider(malformed);

  return Finish({
      {"dependency_claim_denied",
       Denied(dependency_result, "ldap_starttls_verifier_required")},
      {"client_success_claim_denied",
       Denied(client_success_result, "ldap_starttls_verifier_required")},
      {"signature_claim_denied",
       Denied(signature_result, "oidc_jwt_validator_required")},
      {"group_claim_denied",
       Denied(groups_result, "ldap_starttls_verifier_required")},
      {"freshness_claim_denied",
       Denied(freshness_result, "oidc_jwt_validator_required")},
      {"provider_result_claim_denied",
       Denied(provider_result_result, "ldap_starttls_verifier_required")},
      {"fixture_claim_denied",
       Denied(fixture_result, "ldap_starttls_verifier_required")},
      {"malformed_payload_denied",
       !malformed_result.ok && !malformed_result.authenticated &&
           HasDiagnostic(malformed_result,
                         "SECURITY.AUTHENTICATION.REQUEST_INVALID",
                         "provider_payload_duplicate_key")},
      {"no_authenticated_evidence",
       !HasEvidence(client_success_result, "auth_provider_authenticated")},
  });
}
