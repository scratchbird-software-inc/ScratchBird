// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "security/auth_challenge_api.hpp"
#include "security/auth_credential_api.hpp"
#include "security/auth_provider_observability_api.hpp"
#include "security/auth_provider_plugin_api.hpp"
#include "security/auth_provider_policy_api.hpp"
#include "security/auth_provider_trusted_result.hpp"
#include "security/auth_token_api.hpp"
#include "security/external_group_api.hpp"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <type_traits>

namespace sb_auth_probe {
using namespace scratchbird::engine::internal_api;

inline EngineUuid Uuid(const char* value) {
  EngineUuid uuid;
  uuid.canonical = value;
  return uuid;
}

inline void AddMaterializedGrant(EngineRequestContext* context,
                                 const char* grant_uuid,
                                 const std::string& right,
                                 bool deny = false) {
  context->authorization_context.grants.push_back(
      {Uuid(grant_uuid), context->principal_uuid, "principal", {}, right, deny,
       context->security_epoch});
}

inline void MaterializeAuthorization(EngineRequestContext* context,
                                     bool include_rights) {
  context->security_epoch = 1;
  context->catalog_generation_id = 1;
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid =
      Uuid("018f0000-0000-7000-8000-0000000a0010");
  context->authorization_context.security_context_generation = 1;
  context->authorization_context.principal_uuid = context->principal_uuid;
  context->authorization_context.security_epoch = context->security_epoch;
  context->authorization_context.policy_epoch = 1;
  context->authorization_context.catalog_generation_id =
      context->catalog_generation_id;
  context->authorization_context.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  context->authorization_context.evidence_tags.push_back(
      "durable_authorization_context");
  if (!include_rights) { return; }
  AddMaterializedGrant(context,
                       "018f0000-0000-7000-8000-0000000a0011",
                       "AUTH_PROVIDER_ADMIN");
  AddMaterializedGrant(context,
                       "018f0000-0000-7000-8000-0000000a0012",
                       "CONNECT");
  AddMaterializedGrant(context,
                       "018f0000-0000-7000-8000-0000000a0013",
                       "OBS_CONFIG_INSPECT");
  AddMaterializedGrant(context,
                       "018f0000-0000-7000-8000-0000000a0014",
                       "OBS_METRICS_READ_FAMILY");
  AddMaterializedGrant(context,
                       "018f0000-0000-7000-8000-0000000a0015",
                       "PROTECTED_MATERIAL_RELEASE");
}

inline EngineRequestContext Context(std::initializer_list<const char*> extra_tags = {}) {
  static bool cleaned = false;
  const std::string path = "/tmp/sb_auth_provider_plugin_probe.sbdb";
  if (!cleaned) { std::filesystem::remove(path); cleaned = true; }
  EngineRequestContext context;
  context.database_path = path;
  context.local_transaction_id = 100;
  context.security_context_present = true;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-0000000a0001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-0000000a0002";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-0000000a0003";
  context.trace_tags = {"security.bootstrap"};
  for (const char* tag : extra_tags) { context.trace_tags.emplace_back(tag); }
  MaterializeAuthorization(&context, true);
  return context;
}

inline EngineRequestContext ContextWithoutRights(std::initializer_list<const char*> extra_tags = {}) {
  EngineRequestContext context;
  context.database_path = "/tmp/sb_auth_provider_plugin_probe.sbdb";
  context.local_transaction_id = 100;
  context.security_context_present = true;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-0000000a0001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-0000000a0002";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-0000000a0003";
  MaterializeAuthorization(&context, false);
  for (const char* tag : extra_tags) {
    const std::string value(tag);
    constexpr const char* kDenyPrefix = "deny:";
    if (value.rfind(kDenyPrefix, 0) == 0) {
      AddMaterializedGrant(&context,
                           "018f0000-0000-7000-8000-0000000a0016",
                           value.substr(std::char_traits<char>::length(
                               kDenyPrefix)),
                           true);
    } else {
      context.trace_tags.emplace_back(value);
    }
  }
  return context;
}

template <typename TRequest>
TRequest Request(const std::string& provider) {
  TRequest request;
  request.context = Context();
  request.target_object.uuid.canonical = "018f0000-0000-7000-8000-0000000a0100";
  request.option_envelopes.push_back("provider:" + provider);
  request.option_envelopes.push_back("credential:valid");
  request.option_envelopes.push_back("fixture:success");
  request.option_envelopes.push_back("principal:alice");
  request.option_envelopes.push_back("groups:materialized");
  request.option_envelopes.push_back("external_group:CN=DBA,DC=example,DC=org");
  request.option_envelopes.push_back("internal_group_uuid:018f0000-0000-7000-8000-0000000a0200");
  request.option_envelopes.push_back("protected_material:available");
  request.option_envelopes.push_back("token_uuid:018f0000-0000-7000-8000-0000000a0300");
  request.option_envelopes.push_back("challenge_uuid:018f0000-0000-7000-8000-0000000a0400");
  if constexpr (std::is_same_v<TRequest,
                               EngineAuthenticateProviderRequest>) {
    request.option_envelopes.push_back("auth_flow:login");
  }
  return request;
}

inline bool HasDiagnostic(const EngineApiResult& result,
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

inline void RemoveOption(EngineApiRequest* request,
                         const std::string& option) {
  request->option_envelopes.erase(
      std::remove(request->option_envelopes.begin(),
                  request->option_envelopes.end(), option),
      request->option_envelopes.end());
}

#if defined(SCRATCHBIRD_AUTH_PROVIDER_TESTING)
inline std::optional<AuthProviderTrustedResult> TrustedResultFromTestRequest(
    const EngineApiRequest& request) {
  const std::string family = CanonicalAuthProviderFamily(
      AuthProviderOptionValue(request, "provider:"));
  std::string principal = AuthProviderOptionValue(request, "principal:");
  if (principal.empty()) { principal = "test_principal"; }
  return AuthProviderTrustedResultTestFactory::Authenticated(
      family, principal,
      AuthProviderOptionBool(request, "test_trusted_groups_materialized:",
                             false),
      AuthProviderOptionBool(request, "test_trusted_membership_explainable:",
                             false),
      AuthProviderOptionBool(request, "test_trusted_mfa_verified:", false));
}

class ScopedTrustedProviderResult final {
 public:
  ScopedTrustedProviderResult() {
    InstallAuthProviderTrustedResultTestHook(TrustedResultFromTestRequest);
  }
  ~ScopedTrustedProviderResult() { ClearAuthProviderTrustedResultTestHook(); }

  ScopedTrustedProviderResult(const ScopedTrustedProviderResult&) = delete;
  ScopedTrustedProviderResult& operator=(
      const ScopedTrustedProviderResult&) = delete;
};
#endif

inline void PrintResult(const std::string& name, bool ok) {
  std::cout << "\"" << name << "\":" << (ok ? "true" : "false");
}

inline int Finish(std::initializer_list<std::pair<std::string, bool>> values) {
  bool all = true;
  std::cout << "{";
  bool first = true;
  for (const auto& value : values) {
    all = all && value.second;
    if (!first) { std::cout << ","; }
    PrintResult(value.first, value.second);
    first = false;
  }
  if (!first) { std::cout << ","; }
  PrintResult("ok", all);
  std::cout << "}\n";
  return all ? 0 : 1;
}

}  // namespace sb_auth_probe
