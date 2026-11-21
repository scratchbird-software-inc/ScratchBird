// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_LOGIN_REFUSAL_TEST (S3f0, owner Q301 = yes, 2026-11-19; EXPECTATIONS FLIPPED by S4c, owner Q312,
// 2026-11-22). WAS: native login did not exist until S4, so a login handoff on a NATIVE database refused, specifically
// and first, with SECURITY.AUTHENTICATION.NATIVE_LOGIN_UNAVAILABLE (reason=native_login_s4) instead of reaching the legacy
// security loader. Q312: "keep SECURITY.AUTHENTICATION.NATIVE_LOGIN_UNAVAILABLE until S4c replaces it". S4c replaced it:
//   * a database a SERVER SERVES (the marker, mode `served`): a login with a claim that names nobody now returns the ONE
//     generic credential failure SECURITY.AUTHENTICATION.LOGIN_FAILED (reason=login_failed; the binary database
//     identity; no prose, no principal text) through HandleAuthHandoff and EngineAuthenticate; the legacy loader is still
//     never reached. (The accepted / every-cause / throttle / strace cases are native_login_test; this file keeps the S3f0
//     test's structure and its no-echo, no-session, unchanged-bytes checks.)
//   * a native file NOBODY serves in this process (mode `path`: a path with no marker): there is no loop to read through,
//     so it still refuses specifically, with its own code SECURITY.AUTHENTICATION.NATIVE_DATABASE_NOT_SERVED
//     (reason=native_database_not_served) - not a credential verdict.
// Modes (a child process each; the strace half is native_login_strace.py):
//   create <path> <page_size>
//   served <path>   the real StartHostedEngine, then the REAL login handoff (HandleAuthHandoff, the function the
//                   server's attach path runs) with local-password evidence for a principal claim, and
//                   EngineAuthenticate with the server's marker; prints "LOGIN_OK checks=N"
//   path <path>     the file is NOT served: EngineAuthenticate with only a database path (the engine's own
//                   read-only discovery decides); prints "LOGIN_OK checks=N"
// With no argument the driver runs both modes at 8K and 128K and, for each, checks the directory listing and the
// file's bytes are unchanged and no .sb.* name exists.

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "security/authentication_api.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/process_crypto_owner.hpp"
#include "session_registry.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/native_database_create_fixture.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
namespace sbps = scratchbird::server::sbps;
namespace api = scratchbird::engine::internal_api;
namespace fs = std::filesystem;

constexpr std::string_view kPrincipalClaim = "claim_not_to_be_echoed_6f1d";

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
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
  request.database_name = "native_login";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  return 0;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}
void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

// The same wire layout the server's attach conformance test builds (a local-password login handoff).
std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid) {
  std::vector<std::uint8_t> out(connection_uuid.begin(), connection_uuid.end());
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, kPrincipalClaim);
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  PutString(&out, "native_login_refusal_test");
  PutString(&out, "");
  return out;
}

bool HasField(const api::EngineApiDiagnostic& diagnostic, std::string_view key, std::string_view value) {
  return std::any_of(diagnostic.fields.begin(), diagnostic.fields.end(),
                     [&](const auto& field) { return field.key == key && field.value == value; });
}

// A refusal vector must be structured only: no prose, no echo of the principal claim anywhere.
void CheckRefusalVector(const api::EngineAuthenticateResult& result, const p::Uuid& database, const std::string& what,
                        const std::string& expected_code, const std::string& expected_reason) {
  Check(!result.ok && !result.authenticated, what + ": not authenticated");
  Check(!result.diagnostics.empty(), what + ": a diagnostic exists");
  const auto& diagnostic = result.diagnostics.front();
  Check(diagnostic.code == expected_code, what + ": specific code " + expected_code + ", got " + diagnostic.code);
  Check(diagnostic.detail.empty(), what + ": no prose detail");
  Check(HasField(diagnostic, "reason", expected_reason), what + ": reason=" + expected_reason);
  Check(std::any_of(diagnostic.identity_fields.begin(), diagnostic.identity_fields.end(),
                    [&](const auto& field) { return field.first == "database_uuid" && field.second == database; }),
        what + ": the binary database identity");
  for (const auto& d : result.diagnostics) {
    Check(d.detail.find(kPrincipalClaim) == std::string::npos && d.code.find(kPrincipalClaim) == std::string::npos &&
              std::none_of(d.fields.begin(), d.fields.end(),
                           [&](const auto& field) { return field.value.find(kPrincipalClaim) != std::string::npos; }),
          what + ": no principal text in the vector");
  }
}

api::EngineAuthenticateRequest LoginRequest(const api::EngineRequestContext& context) {
  api::EngineAuthenticateRequest request;
  request.context = context;
  request.provider_family = "local_password";
  request.principal_claim = std::string(kPrincipalClaim);
  request.credential_evidence = "verifier=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  request.credential_evidence_present = true;
  return request;
}

int Setup(srv::ServerBootstrapConfig* config, srv::ServerProcessCryptoOwner* crypto) {
  config->database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config->memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(*config);
  if (!policy.ok() ||
      !scratchbird::core::memory::ConfigureDefaultMemoryManager(policy.policy, config->memory_policy_provenance).ok() ||
      !crypto->Start(*config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  return 0;
}

int ServedMode(const fs::path& path) {
  srv::ServerBootstrapConfig config;
  srv::ServerProcessCryptoOwner crypto;
  if (const int setup = Setup(&config, &crypto)) return setup;
  config.database_default_path = path;
  const auto directory = path.parent_path();
  const auto before = Snapshot(directory);
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (!started.ok()) {
    std::cout << "REFUSED code=" << started.diagnostics.front().code << std::endl;
    return 2;
  }
  const std::string path_text = path.string();
  const auto facts = srv::ServedNativeFactsFor(started.state, path_text);
  Check(facts != nullptr, "the loop published facts");

  // The real login handoff, as the server's attach path runs it.
  srv::ServerSessionRegistry registry;
  const auto connection = sbps::MakeUuidV7Bytes();
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kAuthHandoff);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection;
  frame.header.payload_schema_id = 3001;
  frame.payload = AuthPayload(connection);
  const auto result = srv::HandleAuthHandoff(&registry, started.state, frame);
  Check(!result.accepted, "the login handoff with a claim that names nobody is not accepted");
  Check(!result.diagnostics.empty(), "the handoff returns a diagnostic");
  bool found = false;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code != "SECURITY.AUTHENTICATION.LOGIN_FAILED") continue;
    found = true;
    Check(std::any_of(diagnostic.fields.begin(), diagnostic.fields.end(),
                      [](const auto& field) { return field.key == "detail" && field.value == "login_failed"; }),
          "the handoff's diagnostic carries the structured reason login_failed");
  }
  Check(found, "the handoff diagnostics hold SECURITY.AUTHENTICATION.LOGIN_FAILED (first: " +
                   (result.diagnostics.empty() ? std::string("none") : result.diagnostics.front().code) + ")");
  for (const auto& diagnostic : result.diagnostics) {
    Check(diagnostic.code.find(kPrincipalClaim) == std::string::npos &&
              diagnostic.safe_message.find(kPrincipalClaim) == std::string::npos,
          "no principal text in the handoff's diagnostics");
  }
  const std::string payload(result.payload.begin(), result.payload.end());
  Check(payload.find(kPrincipalClaim) == std::string::npos, "no principal text in the handoff's payload");
  Check(payload.find("login_failed") != std::string::npos, "the handoff's payload names the reason");
  Check(registry.sessions_by_uuid.empty() && registry.auth_contexts_by_uuid.empty(), "no session and no auth context were created");

  // The engine call with the server's marker, directly.
  api::EngineRequestContext context;
  context.database_path = path_text;
  context.database_uuid = facts->database_uuid;
  context.native_served_facts = facts;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  CheckRefusalVector(api::EngineAuthenticate(LoginRequest(context)), facts->database_uuid, "EngineAuthenticate with the marker",
                     "SECURITY.AUTHENTICATION.LOGIN_FAILED", "login_failed");
  // With a no-credential request the failure is the same generic one (an empty password is inadmissible: a credential cause).
  auto no_credential = LoginRequest(context);
  no_credential.credential_evidence.clear();
  no_credential.credential_evidence_present = false;
  CheckRefusalVector(api::EngineAuthenticate(no_credential), facts->database_uuid, "EngineAuthenticate without credential evidence",
                     "SECURITY.AUTHENTICATION.LOGIN_FAILED", "login_failed");

  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  std::cout << "LOGIN_OK checks=" << checks << " mode=served" << std::endl;
  return 0;
}

int PathMode(const fs::path& path) {
  srv::ServerBootstrapConfig config;
  srv::ServerProcessCryptoOwner crypto;
  if (const int setup = Setup(&config, &crypto)) return setup;
  const auto directory = path.parent_path();
  const auto before = Snapshot(directory);
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.trust_mode = api::EngineTrustMode::server_isolated;
  const auto result = api::EngineAuthenticate(LoginRequest(context));
  Check(!result.diagnostics.empty() &&
            result.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_DATABASE_NOT_SERVED",
        "a path-only context on a native file (WAS NATIVE_LOGIN_UNAVAILABLE, Q301): the engine's own read-only discovery refuses specifically, "
        "with its own code (nobody serves this file: no loop to read through)");
  // The binary identity is page zero's (read-only discovery), whatever the caller's context said.
  p::Uuid page_zero_database;
  {
    // Recover it from the diagnostic itself: identity field database_uuid must be non-nil.
    for (const auto& field : result.diagnostics.front().identity_fields)
      if (field.first == "database_uuid") page_zero_database = field.second;
  }
  Check(!page_zero_database.is_nil(), "the refusal names page zero's database identity");
  CheckRefusalVector(result, page_zero_database, "EngineAuthenticate by path", "SECURITY.AUTHENTICATION.NATIVE_DATABASE_NOT_SERVED",
                     "native_database_not_served");
  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  std::cout << "LOGIN_OK checks=" << checks << " mode=path" << std::endl;
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
    for (const char* mode : {"served", "path"}) {
      const auto directory = artifacts.path() / (std::string(mode) + "_" + std::to_string(page_size));
      fs::create_directories(directory);
      const auto path = (directory / "login.sdb").string();
      Check(Run("'" + self + "' create '" + path + "' " + std::to_string(page_size)) == 0, std::string("create ") + mode);
      Check(Run("'" + self + "' " + mode + " '" + path + "'") == 0, std::string("child ") + mode + " at " + std::to_string(page_size));
    }
  }
  artifacts.Cleanup();
  std::printf("native_login_refusal: %d driver checks passed\n", checks);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 3 && std::string(argv[1]) == "served") return ServedMode(argv[2]);
  if (argc == 3 && std::string(argv[1]) == "path") return PathMode(argv[2]);
  if (argc != 1) return 2;
  Driver();
  return 0;
}
