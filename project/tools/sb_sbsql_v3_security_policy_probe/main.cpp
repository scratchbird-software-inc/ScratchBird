// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/grant_api.hpp"
#include "security/identity_api.hpp"
#include "security/policy_api.hpp"
#include "security/visibility_api.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args { std::string path; bool overwrite = false; };

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; } else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext Base(const Args& args, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "sbsql-v3-security-policy-probe-secure" : "sbsql-v3-security-policy-probe-open";
  context.database_path = args.path;
  return context;
}

EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_security_policy_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto open_base = Base(args, false);
  EngineGrantRightRequest denied_grant;
  denied_grant.context = open_base;
  const auto denied_grant_result = EngineGrantRight(denied_grant);
  const bool security_context_required = !denied_grant_result.ok &&
                                         HasDiagnosticCode(denied_grant_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");

  EngineEvaluateVisibilityRequest visibility;
  visibility.context = open_base;
  visibility.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001001";
  visibility.target_object.object_kind = "table";
  const auto visibility_result = EngineEvaluateVisibility(visibility);
  const bool visibility_denied_without_context = !visibility_result.ok &&
                                                 HasEvidence(visibility_result, "visibility_decision", "deny");

  EngineEvaluatePolicyRequest invalid_policy;
  invalid_policy.context = open_base;
  invalid_policy.policy_profile.encoded_profiles.push_back("invalid:unsafe");
  const auto invalid_policy_result = EngineEvaluatePolicy(invalid_policy);
  const bool invalid_policy_denied = !invalid_policy_result.ok &&
                                     HasEvidence(invalid_policy_result, "policy_decision", "deny_invalid_profile");

  const auto secure_base = Base(args, true);
  const auto tx = Begin(secure_base);
  const auto secure_context = Tx(secure_base, tx);

  EngineCreateIdentityRequest identity;
  identity.context = secure_context;
  identity.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001002";
  identity.target_object.object_kind = "identity";
  identity.localized_names.push_back({"en", "default", "dev_user", "dev_user", true});
  const auto identity_result = EngineCreateIdentity(identity);

  EngineGrantRightRequest grant;
  grant.context = secure_context;
  grant.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001003";
  grant.target_object.object_kind = "grant";
  grant.localized_names.push_back({"en", "default", "dev_index_profile", "dev_index_profile", true});
  grant.option_envelopes.push_back("group:DEV");
  grant.option_envelopes.push_back("right:OBS_INDEX_PROFILE_READ");
  const auto grant_result = EngineGrantRight(grant);
  const bool dev_warning = grant_result.ok &&
                           HasDiagnosticCode(grant_result, "SB_ENGINE_API_DEV_ONLY_RIGHT_WARNING");

  EngineRevokeRightRequest revoke;
  revoke.context = secure_context;
  revoke.target_object = grant_result.primary_object;
  const auto revoke_result = EngineRevokeRight(revoke);

  EngineEvaluatePolicyRequest allow_policy;
  allow_policy.context = secure_context;
  allow_policy.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001004";
  allow_policy.target_object.object_kind = "table";
  const auto allow_policy_result = EngineEvaluatePolicy(allow_policy);
  const bool policy_allow = allow_policy_result.ok && HasEvidence(allow_policy_result, "policy_decision", "allow_local_default");
  const bool committed = Commit(secure_context);

  const bool ok = security_context_required && visibility_denied_without_context && invalid_policy_denied &&
                  tx.ok && identity_result.ok && grant_result.ok && dev_warning && revoke_result.ok &&
                  policy_allow && committed;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("security_context_required", security_context_required, true);
  PrintBool("visibility_denied_without_context", visibility_denied_without_context, true);
  PrintBool("invalid_policy_denied", invalid_policy_denied, true);
  PrintBool("dev_warning", dev_warning, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

