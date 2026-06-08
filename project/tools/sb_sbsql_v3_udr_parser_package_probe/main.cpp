// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/parser_package_api.hpp"
#include "extensibility/udr_api.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else {
      return false;
    }
  }
  return !args->path.empty();
}

EngineRequestContext Context(const Args& args, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "sbsql-v3-udr-parser-package-probe-secure" : "sbsql-v3-udr-parser-package-probe-open";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001501";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001502";
  return context;
}

EngineRequestContext BeginContext(const Args& args, bool secure) {
  auto context = Context(args, secure);
  EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto tx = EngineBeginTransaction(begin);
  if (tx.ok) {
    context.local_transaction_id = tx.local_transaction_id;
    context.transaction_uuid = tx.transaction_uuid;
  }
  return context;
}

bool CommitContext(const EngineRequestContext& context) {
  EngineCommitTransactionRequest commit;
  commit.context = context;
  return EngineCommitTransaction(commit).ok;
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

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

scratchbird::core::platform::TypedUuid Generate(scratchbird::core::platform::UuidKind kind, std::uint64_t millis) {
  auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

bool CreateDatabase(const std::string& path, bool overwrite) {
  if (overwrite) {
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".sb.owner.lock");
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  const auto seed = NowMillis();
  create.path = path;
  create.database_uuid = Generate(scratchbird::core::platform::UuidKind::database, seed);
  create.filespace_uuid = Generate(scratchbird::core::platform::UuidKind::filespace, seed + 1);
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = overwrite;
  return create.database_uuid.valid() && create.filespace_uuid.valid() &&
         scratchbird::storage::database::CreateDatabaseFile(create).ok();
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_udr_parser_package_probe --path PATH [--overwrite]\n";
    return 2;
  }
  const bool database_created = CreateDatabase(args.path, args.overwrite);

  auto open_context = BeginContext(args, false);
  EngineRegisterUdrPackageRequest open_udr;
  open_udr.context = open_context;
  open_udr.localized_names.push_back({"en", "default", "/extensions/udr", "unsafe_udr", true});
  const auto open_udr_result = EngineRegisterUdrPackage(open_udr);

  auto secure_context = BeginContext(args, true);
  const bool tx_started = secure_context.local_transaction_id != 0;

  EngineRegisterUdrPackageRequest udr;
  udr.context = secure_context;
  udr.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001511";
  udr.localized_names.push_back({"en", "default", "/extensions/udr", "math128_udr", true});
  udr.option_envelopes.push_back("permission:manage_udr");
  udr.option_envelopes.push_back("abi:sb_udr_1");
  udr.option_envelopes.push_back("trust:trusted_cpp");
  udr.option_envelopes.push_back("register:trusted_cpp_udr");
  const auto udr_result = EngineRegisterUdrPackage(udr);

  EngineRegisterParserPackageRequest parser;
  parser.context = secure_context;
  parser.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001512";
  parser.localized_names.push_back({"en", "default", "/extensions/parser", "native_v3_parser_package", true});
  parser.option_envelopes.push_back("register:parser_package");
  const auto parser_result = EngineRegisterParserPackage(parser);

  EngineRegisterParserPackageRequest cluster_parser;
  cluster_parser.context = secure_context;
  cluster_parser.option_envelopes.push_back("global_deploy:parser_package");
  const auto cluster_parser_result = EngineRegisterParserPackage(cluster_parser);

  const bool committed = tx_started && CommitContext(secure_context);

  const bool udr_security_denied = !open_udr_result.ok &&
                                   HasDiagnosticCode(open_udr_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool udr_ok = udr_result.ok &&
                      HasEvidence(udr_result, "extension_family", "udr") &&
                      HasEvidence(udr_result, "security_scope", "permission_checked") &&
                      HasEvidence(udr_result, "execution_boundary", "engine_owned_no_bypass");
  const bool parser_ok = parser_result.ok &&
                         HasEvidence(parser_result, "extension_family", "parser_package") &&
                         HasEvidence(parser_result, "parser_trust_boundary", "untrusted_per_connection") &&
                         HasEvidence(parser_result, "engine_mutation_authority", "false");
  const bool cluster_denied = !cluster_parser_result.ok && cluster_parser_result.cluster_authority_required;
  const bool ok = database_created && udr_security_denied && udr_ok && parser_ok && cluster_denied && committed;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("database_created", database_created, true);
  PrintBool("udr_security_denied", udr_security_denied, true);
  PrintBool("udr_ok", udr_ok, true);
  PrintBool("parser_ok", parser_ok, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
