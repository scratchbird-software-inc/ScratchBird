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
#include "security/auth_token_api.hpp"
#include "security/external_group_api.hpp"

#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>

namespace sb_auth_probe {
using namespace scratchbird::engine::internal_api;

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
  context.trace_tags = {"right:AUTH_PROVIDER_ADMIN", "right:OBS_CONFIG_INSPECT", "right:OBS_METRICS_READ_FAMILY", "right:PROTECTED_MATERIAL_RELEASE", "security.bootstrap"};
  for (const char* tag : extra_tags) { context.trace_tags.emplace_back(tag); }
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
  for (const char* tag : extra_tags) { context.trace_tags.emplace_back(tag); }
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
  return request;
}

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
