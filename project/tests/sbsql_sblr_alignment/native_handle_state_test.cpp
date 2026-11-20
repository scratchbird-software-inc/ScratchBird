// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_HANDLE_STATE_TEST (S3f0, owner Q299 = a, 2026-11-19). The serving state of a native
// database decides what a handle on it may do:
//   serving     the handle opens; transaction begin / SBLR dispatch / parameter coordination refuse the generic
//               ENGINE.NATIVE.OPERATION_NOT_BUILT (unchanged from S3e);
//   restricted  (a committing slot was left by the writer: the real fixture, the real StartHostedEngine) the handle
//               opens READ-ONLY, the facts it carries say restricted and name the loop's diagnostic code, and every
//               operation that could write refuses ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY;
//   failed      (forged from the real facts: a real failed restart exits the server) the open refuses
//               ENGINE.OPEN.NATIVE_DATABASE_FAILED before any handle exists.
// Modes (a child process each: the process gate binds after one open):
//   create <path> <page_size>     native create
//   state <path> <serving|restricted>   one StartHostedEngine, then the cases for that state;
//                                       prints "STATE_OK checks=N state=..."
// With no argument the driver runs both states at 8K and 128K (the restricted fixture is written by the
// native_serving binary, "fixture <path> committing", the file's own writer) and checks that the served file's
// directory listing and bytes are unchanged and hold no .sb.* name.

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "scratchbird/engine/engine.h"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/process_crypto_owner.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/native_database_create_fixture.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace srv = scratchbird::server;
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace fs = std::filesystem;

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
}

std::string_view View(sb_engine_string_view_t value) {
  return {value.data ? value.data : "", static_cast<std::size_t>(value.size_bytes)};
}

// Releases the result and returns the first diagnostic's symbolic code ("" when none); the safe detail is
// returned through `detail` when asked (a structured refusal carries none).
std::string Code(sb_engine_result_t& result, std::string* detail = nullptr) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  diagnostics.struct_size = sizeof(diagnostics);
  diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::string code;
  if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK && diagnostics.diagnostic_count != 0) {
    code = std::string(View(diagnostics.diagnostics[0].symbolic_code));
    if (detail) *detail = std::string(View(diagnostics.diagnostics[0].safe_detail));
  }
  (void)sb_engine_result_release(result);
  result = nullptr;
  return code;
}

std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    std::string value = "dir";
    if (entry.is_regular_file()) {
      std::ifstream in(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), {});
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          reinterpret_cast<const p::byte*>(bytes.data()), bytes.size());
      Check(digest.ok(), "sha256");
      value = std::to_string(bytes.size()) + ":" + scratchbird::core::hash::HexLower(digest.digest);
    }
    out[entry.path().filename().string()] = value;
  }
  return out;
}

int CreateMode(const fs::path& path, std::uint32_t page_size) {
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_handle_state";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  return 0;
}

sb_engine_open_params_v1_t Params(const std::string& path, sb_engine_open_mode_t mode) {
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = path.data();
  params.database_path_size = path.size();
  params.mode = mode;
  return params;
}

int StateMode(const fs::path& path, const std::string& expected) {
  const bool restricted = expected == "restricted";
  const auto directory = path.parent_path();
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !scratchbird::core::memory::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  const auto before = Snapshot(directory);
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (!started.ok()) {
    std::cout << "REFUSED code=" << started.diagnostics.front().code << std::endl;
    return 2;
  }
  const std::string path_text = path.string();
  const auto facts = srv::ServedNativeFactsFor(started.state, path_text);
  const auto owner = srv::ServedRouteOwnerFor(started.state, path_text);
  Check(facts != nullptr && owner != nullptr, "the loop published facts and a served owner");
  Check(facts->serving_state == (restricted ? api::NativeServedServingState::restricted : api::NativeServedServingState::serving),
        "the facts carry the serving state the database opened in: " + expected);
  Check(restricted == (facts->serving_diagnostic_code == "DATABASE.OPEN_RESTRICTED_UNRESOLVED_TRANSACTIONS"),
        "the facts name the loop's diagnostic code behind the state (restricted only)");
  Check(restricted || facts->serving_diagnostic_code.empty(), "a serving database names no diagnostic");

  // ---- the open: serving and restricted open, in both modes ----
  const auto normal = Params(path_text, SB_ENGINE_OPEN_NORMAL);
  const auto validation = Params(path_text, SB_ENGINE_OPEN_VALIDATION_ONLY);
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  auto status = bridge::OpenServedEngine(&validation, owner, "", facts, &engine, &result);
  Check(status == SB_ENGINE_STATUS_OK && engine != nullptr, expected + " database: the handle opens (validation-only): " + Code(result));
  sb_engine_handle_t second = nullptr;
  status = bridge::OpenServedEngine(&normal, owner, "", facts, &second, &result);
  Check(status == SB_ENGINE_STATUS_OK && second != nullptr, expected + " database: the handle opens (normal): " + Code(result));
  Check(sb_engine_close(second, nullptr) == SB_ENGINE_STATUS_OK, "close the second handle");

  // ---- a session, then the write-capable entry points ----
  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  const auto user_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  const auto session_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(user_id && session_id, "runtime identities issued");
  std::copy(user_id->bytes.begin(), user_id->bytes.end(), session_params.effective_user_uuid.bytes);
  std::copy(session_id->bytes.begin(), session_id->bytes.end(), session_params.session_uuid.bytes);
  session_params.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  sb_engine_session_t session = nullptr;
  status = sb_engine_session_begin(engine, &session_params, &session, &result);
  Check(status == SB_ENGINE_STATUS_OK && session != nullptr, expected + " database: a session begins: " + Code(result));

  const std::string want = restricted ? "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY" : "ENGINE.NATIVE.OPERATION_NOT_BUILT";
  std::string detail;
  {
    sb_engine_transaction_params_v1_t transaction_params{};
    transaction_params.struct_size = sizeof(transaction_params);
    transaction_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_transaction_t transaction = nullptr;
    status = sb_engine_transaction_begin(session, &transaction_params, &transaction, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && transaction == nullptr, expected + ": transaction_begin refuses");
    Check(Code(result, &detail) == want && detail.empty(), expected + ": transaction_begin: " + want + " (no prose detail)");

    sb_engine_request_context_v1_t request_context{};
    request_context.struct_size = sizeof(request_context);
    request_context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_sblr_dispatch_params_v1_t dispatch_params{};
    dispatch_params.struct_size = sizeof(dispatch_params);
    dispatch_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    status = sb_engine_dispatch_sblr(session, nullptr, &request_context, &dispatch_params, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED, expected + ": dispatch_sblr refuses");
    Check(Code(result, &detail) == want && detail.empty(), expected + ": dispatch_sblr: " + want + " (no prose detail)");

    api::EngineRequestContext context;
    context.database_path = path_text;
    context.database_uuid = facts->database_uuid;
    std::copy_n(session_params.effective_user_uuid.bytes, 16, context.principal_uuid.bytes.begin());
    std::copy_n(session_params.session_uuid.bytes, 16, context.session_uuid.bytes.begin());
    context.trust_mode = api::EngineTrustMode::server_isolated;
    bridge::StatementParameterCoordinationBeginRequestV1 begin{};
    begin.engine_session = session;
    begin.engine_context = &context;
    begin.mode = bridge::StatementParameterExecutionMode::kPrepared;
    begin.operation_uuid = *scratchbird::core::uuid::IssueRuntimeIdentityV7();
    bridge::StatementParameterCoordinationViewV1 coordination{};
    status = bridge::BeginStatementParameterExecutionCoordinationV1(&begin, &coordination, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED, expected + ": parameter coordination begin refuses");
    Check(Code(result, &detail) == want && detail.empty(), expected + ": parameter coordination begin: " + want + " (no prose detail)");
  }
  status = sb_engine_session_end(session, nullptr, &result);
  Check(status == SB_ENGINE_STATUS_OK, expected + ": the session ends: " + Code(result));
  Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close the handle");

  // ---- failed (forged from the real facts: a real failed restart exits the server, Q292) ----
  for (const char* diagnostic : {"DATABASE.RESTART_WRITE_FAILED", ""}) {
    auto copy = std::make_shared<api::NativeServedDatabaseFacts>(*facts);
    copy->serving_state = api::NativeServedServingState::failed;
    copy->serving_diagnostic_code = diagnostic;
    const std::shared_ptr<const api::NativeServedDatabaseFacts> failed = copy;
    for (const auto* params : {&normal, &validation}) {
      sb_engine_handle_t none = nullptr;
      status = bridge::OpenServedEngine(params, owner, "", failed, &none, &result);
      Check(status == SB_ENGINE_STATUS_CONFLICT && none == nullptr, "failed facts: the open refuses and issues no handle");
      Check(Code(result, &detail) == "ENGINE.OPEN.NATIVE_DATABASE_FAILED" && detail.empty(),
            "failed facts: ENGINE.OPEN.NATIVE_DATABASE_FAILED (no prose detail)");
    }
  }
  // The refusal is first: it needs neither the owner nor a matching path (no I/O decides it).
  {
    auto copy = std::make_shared<api::NativeServedDatabaseFacts>(*facts);
    copy->serving_state = api::NativeServedServingState::failed;
    const std::shared_ptr<const api::NativeServedDatabaseFacts> failed = copy;
    sb_engine_handle_t none = nullptr;
    status = bridge::OpenServedEngine(&normal, nullptr, "", failed, &none, &result);
    Check(none == nullptr && Code(result) == "ENGINE.OPEN.NATIVE_DATABASE_FAILED",
          "failed facts without an owner: still ENGINE.OPEN.NATIVE_DATABASE_FAILED (decided before any I/O)");
  }

  // ---- nothing was written or created ----
  Check(Snapshot(directory) == before, "the served directory and every file's bytes are unchanged");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  std::cout << "STATE_OK checks=" << checks << " state=" << expected << std::endl;
  return 0;
}

int Run(const std::string& command) {
  const int raw = std::system(command.c_str());
  return raw == -1 ? -1 : (WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw));
}

void Driver() {
  const std::string self = fs::read_symlink("/proc/self/exe").string();
  scratchbird::tests::OwnedTempDirectory artifacts;
  for (const std::uint32_t page_size : {8192u, 131072u}) {
    for (const char* state : {"serving", "restricted"}) {
      const auto directory = artifacts.path() / (std::string(state) + "_" + std::to_string(page_size));
      fs::create_directories(directory);
      const auto path = (directory / "state.sdb").string();
      Check(Run("'" + self + "' create '" + path + "' " + std::to_string(page_size)) == 0, std::string("create ") + state);
      if (std::string_view(state) == "restricted")
        Check(Run(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path + "' committing") == 0,
              "the native_serving fixture writes a committing slot");
      Check(Run("'" + self + "' state '" + path + "' " + state) == 0,
            std::string("state child: ") + state + " at " + std::to_string(page_size));
    }
  }
  artifacts.Cleanup();
  std::printf("native_handle_state: %d driver checks passed\n", checks);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 4 && std::string(argv[1]) == "state") return StateMode(argv[2], argv[3]);
  if (argc != 1) return 2;
  Driver();
  return 0;
}
