// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "security/auth_provider_model.hpp"
#include "security/auth_provider_policy_api.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_evidence_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>

namespace api = scratchbird::engine::internal_api;

void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::cerr << "native auth policy failure at " << at.line() << '\n';
    std::abort();
  }
}

void Refuses(const api::EngineApiRequest& request) {
  Check(!api::AuthProviderPolicyFromRequest(request));
  const auto decision = api::EvaluateAuthProviderPolicy(request);
  Check(!decision.ok && !decision.admitted && !decision.authenticated);
  Check(!decision.evaluated_policy);
  Check(decision.diagnostic.code == "SECURITY.AUTHORITY.INVALID");
  Check(decision.diagnostic.detail == "auth_provider_binary_identity_required");
  api::EngineReloadAuthProviderPolicyRequest reload;
  static_cast<api::EngineApiRequest&>(reload) = request;
  const auto result = api::EngineReloadAuthProviderPolicy(reload);
  Check(!result.ok && !result.reloaded);
  Check(result.primary_object.uuid.is_nil() && result.policy.policy_uuid.is_nil());
  Check(result.diagnostics.size() == 1);
  Check(result.diagnostics.front().code == decision.diagnostic.code);
}

int main() {
  using scratchbird::tests::FixtureUuid;
  auto provider = FixtureUuid(2161, 1);
  auto policy = FixtureUuid(2161, 2);
  provider.bytes[9] = 0; provider.bytes[10] = '\n'; provider.bytes[11] = 0xff;
  policy.bytes[9] = ':'; policy.bytes[10] = '\0'; policy.bytes[11] = 0xff;
  const auto provider_option = "provider_uuid:" + api::MetadataUuidBytes(provider);
  const auto policy_option = "policy_uuid:" + api::MetadataUuidBytes(policy);
  api::EngineApiRequest request;
  request.target_object.uuid = provider;
  request.option_envelopes = {"provider:local_password", provider_option, policy_option};
  const auto parsed = api::AuthProviderPolicyFromRequest(request);
  Check(parsed.has_value());
  Check(parsed->provider_uuid == provider && parsed->policy_uuid == policy);
  Check(api::AuthProviderPolicyFromRequest(request)->policy_uuid == policy);
  auto no_target = request;
  no_target.target_object.uuid = {};
  Check(api::AuthProviderPolicyFromRequest(no_target)->provider_uuid == provider);
  // Every unrestricted octet must survive the bounded raw16 carrier, including
  // NUL, delimiters and non-UTF8. System version/variant bits remain UUIDv7.
  for (std::size_t offset = 0; offset < 16; ++offset) {
    for (unsigned value = 0; value < 256; ++value) {
      auto identity = provider;
      identity.bytes[offset] = static_cast<std::uint8_t>(value);
      identity.bytes[6] = (identity.bytes[6] & 0x0f) | 0x70;
      identity.bytes[8] = (identity.bytes[8] & 0x3f) | 0x80;
      auto exact = request;
      exact.target_object.uuid = identity;
      exact.option_envelopes[1] = "provider_uuid:" + api::MetadataUuidBytes(identity);
      exact.option_envelopes[2] = "policy_uuid:" + api::MetadataUuidBytes(identity);
      auto decoded = api::AuthProviderPolicyFromRequest(exact);
      Check(decoded && decoded->provider_uuid == identity && decoded->policy_uuid == identity);
    }
  }
  // Structural binary decoding grants no permission to evaluate or reload.
  Check(!api::EvaluateAuthProviderPolicy(request).ok);

  for (const auto& key : {"provider_uuid:", "policy_uuid:"}) {
    for (const auto& bytes : {std::string{}, std::string(15, '\0'),
                             std::string(16, '\0'), std::string(17, '\0'),
                             std::string("019d0000-0000-7000-8000-000000000001")}) {
      auto bad = request;
      for (auto& option : bad.option_envelopes)
        if (option.starts_with(key)) option = std::string(key) + bytes;
      Refuses(bad);
    }
    auto duplicate = request;
    duplicate.option_envelopes.push_back(
        std::string(key) + api::MetadataUuidBytes(
            std::string_view(key) == "provider_uuid:" ? provider : policy));
    Refuses(duplicate);
    // A valid first occurrence cannot hide an empty or malformed shadow.
    duplicate.option_envelopes.back() = key;
    Refuses(duplicate);
    auto bad = request;
    auto non_v7 = provider; non_v7.bytes[6] = 0x40;
    for (auto& option : bad.option_envelopes)
      if (option.starts_with(key)) option = std::string(key) + api::MetadataUuidBytes(non_v7);
    Refuses(bad);
    non_v7 = provider; non_v7.bytes[8] = 0;
    for (auto& option : bad.option_envelopes)
      if (option.starts_with(key)) option = std::string(key) + api::MetadataUuidBytes(non_v7);
    Refuses(bad);
  }
  auto conflicting = request;
  conflicting.target_object.uuid = FixtureUuid(2161, 3);
  Refuses(conflicting);
  conflicting.target_object.uuid.bytes[6] = 0x40;
  Refuses(conflicting);

  // Exercise evaluation's retained identity. This is the existing policy-only
  // login path, not credential verification, principal binding or SQL E2E.
  request.option_envelopes.push_back("auth_flow:login");
  auto decision = api::EvaluateAuthProviderPolicy(request);
  Check(decision.ok && !decision.authenticated && decision.evaluated_policy);
  Check(decision.evaluated_policy->policy_uuid == policy);
  Check(decision.evaluated_policy->provider_uuid == provider);
  bool found = false;
  for (const auto& evidence : decision.evidence)
    if (evidence.evidence_kind == "auth_provider_policy") {
      Check(scratchbird::tests::EvidenceIdentityEquals(evidence.evidence_id, policy));
      found = true;
    }
  Check(found);
  // The legacy implicit-allocation path must also retain its one evaluated
  // identity; callers must not reconstruct the policy after evaluation.
  request.option_envelopes.erase(request.option_envelopes.begin() + 2);
  decision = api::EvaluateAuthProviderPolicy(request);
  Check(decision.ok && decision.evaluated_policy);
  const auto issued = decision.evaluated_policy->policy_uuid;
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(issued));
  found = false;
  for (const auto& evidence : decision.evidence)
    if (evidence.evidence_kind == "auth_provider_policy") {
      Check(scratchbird::tests::EvidenceIdentityEquals(evidence.evidence_id, issued));
      found = true;
    }
  Check(found);
  std::cout << "native policy identities and typed malformed-input refusals PASS\n";
}
