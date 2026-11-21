// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_LOGIN_TEST (thin-slice S4c, plan 40; owner Q306-Q312, the 2026-11-20 name-model rulings, the
// 2026-11-21 snapshot-point ruling, the Firebird login review). NATIVE LOGIN through the real handoff:
// HandleAuthHandoff (the function the server's attach path runs) and EngineAuthenticate, on a PRODUCTION-created
// database served by StartHostedEngine. The presented login text resolves to a principal UUID through the existing
// resolver, then the S4a credential read runs, both on the loop thread inside ONE snapshot-point scope. REPLACES the
// S3f0 expectation (SECURITY.AUTHENTICATION.NATIVE_LOGIN_UNAVAILABLE, owner Q312): a native database now logs in.
//
// What is asserted, per page size (a child process each, `serve <path> <state> ...`):
//   * the right name + password is accepted with the BINARY bootstrap principal UUID and the role UUIDs the S4a read
//     reports; an alias name logs in too; no session record and no transaction exists afterwards;
//   * EVERY credential cause - wrong password, unknown name, hidden-class name, a name two different objects match,
//     an empty name, a role's name, an inadmissible password, L5 with its correct derived password, a named agent with
//     its correct derived password - returns the SAME client-visible failure: the full serialized result (diagnostics,
//     payload, flags) is compared byte for byte; the SPECIFIC internal code (result.native_login_audit) differs;
//   * a disabled principal and a credential-less principal (forged by the engine's own writers) fail the same way;
//   * infrastructure causes keep their own codes (failed database, closed loop, an unsupported provider);
//   * a restricted database logs in (Q309);
//   * no field of the request can inject a principal, a role or a "trusted" flag: a claimed principal UUID is refused,
//     option envelopes and evidence fields are ignored;
//   * nothing is written: the file bytes and the directory are unchanged, no .sb.* file exists, the TIP's Next is the
//     same before and after (a failed login burns no number);
//   * no password or name text appears in any output the test captured;
//   * the unknown-name path costs the same PBKDF2 as a wrong password (timing equaliser; timings are printed).
// State `throttle` (page-size independent, run once) asserts the in-memory throttle that sits BEFORE the loop.
// Modes (a child process each):
//   serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>   states: serving restricted failed disabled
//                                  nocredential throttle; prints "LOGIN_OK checks=N"
// With no argument the driver runs everything; "size <bytes>" one page size; "common" the throttle state once. The
// create, the forges and the offline derivation are the sibling binaries' own modes (native_login_material,
// native_name_resolve, native_serving); the strace half is native_login_strace.py (SB_NATIVE_LOGIN_STRACE).

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_login_material.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "security/authentication_api.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "catalog_language.hpp"
#include "catalog_records.hpp"
#include "hash_digest.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "uuid.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/login_throttle.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "session_registry.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace srv = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace api = scratchbird::engine::internal_api;
namespace mem = scratchbird::core::memory;
namespace cat = scratchbird::core::catalog;
namespace fs = std::filesystem;

// The same constants as native_login_material_test.cpp (create's bootstrap password) and the names native_name_resolve's
// `login` forge writes. Test passwords: distinctive, so a leak is a byte-exact search hit. Never in an argv.
constexpr std::string_view kBootstrapPassword = "S4a-Bootstrap-Pw-7f3a91c2d8";
constexpr std::string_view kWrongPassword = "S4c-Wrong-Pw-4d9e1a77c0b2";
constexpr std::string_view kBootstrapName = "fixture_owner";
constexpr std::string_view kServiceName = "fixture_service";
constexpr std::string_view kLoginAlias = "owner_login_alias";
constexpr std::string_view kLoginHidden = "owner_hidden_login";
constexpr std::string_view kLoginHiddenOnly = "hidden_only_login";
constexpr std::string_view kUnknownName = "nobody_by_this_name_c81f";
constexpr std::string_view kGeneric = "SECURITY.AUTHENTICATION.LOGIN_FAILED";

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
}

std::string Hex(const p::Uuid& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : id.bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}
std::string HexBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}
p::Uuid FromHex(std::string_view hex) {
  p::Uuid id;
  Check(hex.size() == 32, "a 32-digit hex identity");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<p::byte>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return id;
}
std::string FromHexText(std::string_view hex) {
  std::string out;
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return out;
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

// ---- the wire layout the server's handoff and attach conformance tests build ------------------------------------
void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}
void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
struct LoginAsk {
  std::string name;
  std::string password;
  std::string provider = "local_password";
  std::string role;       // requested role (a name)
  std::string language = "en";
};
std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid, const LoginAsk& ask) {
  std::vector<std::uint8_t> out(connection_uuid.begin(), connection_uuid.end());
  out.push_back(1);  // credential evidence present
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, ask.provider);
  PutString(&out, ask.name);
  PutString(&out, "default");
  PutString(&out, ask.language);
  PutString(&out, ask.password);
  PutString(&out, "native_login_test");
  PutString(&out, ask.role);
  return out;
}
std::vector<std::uint8_t> AttachPayload(const std::array<std::uint8_t, 16>& connection, const std::array<std::uint8_t, 16>& auth_context) {
  std::vector<std::uint8_t> out(connection.begin(), connection.end());
  out.insert(out.end(), auth_context.begin(), auth_context.end());
  PutString(&out, "default");
  PutString(&out, "read_write");
  return out;
}
sbps::Frame MakeFrame(sbps::MessageType type, std::vector<std::uint8_t> payload, const std::array<std::uint8_t, 16>& connection) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection;
  frame.header.payload_schema_id = 3001;
  frame.payload = std::move(payload);
  return frame;
}

// The full client-visible serialization of a handoff result: everything a client could receive (flags, types, the
// payload bytes, every diagnostic with its fields and identity fields). Per-occurrence random identities (the
// diagnostic occurrence UUID) and the server-only audit key are not client-visible and are left out.
std::string Wire(const srv::SessionOperationResult& r) {
  std::ostringstream out;
  out << "accepted=" << r.accepted << " flags=" << r.frame_flags << " type=" << r.response_message_type << " schema=" << r.response_schema_id
      << " payload=" << HexBytes(r.payload) << " diagnostics=" << r.diagnostics.size();
  for (const auto& d : r.diagnostics) {
    out << " [" << d.code << "|" << d.message_key << "|" << static_cast<int>(d.severity) << "|" << d.safe_message << "|" << d.diagnostic_shape_id
        << "|" << d.retryable;
    for (const auto& f : d.fields) out << "|" << f.key << "=" << f.value;
    for (const auto& f : d.identity_fields) out << "|" << f.first << "#" << Hex(f.second);
    out << "]";
  }
  return out.str();
}
std::string EngineWire(const api::EngineAuthenticateResult& r) {
  std::ostringstream out;
  out << "ok=" << r.ok << " authenticated=" << r.authenticated << " diagnostics=" << r.diagnostics.size();
  for (const auto& d : r.diagnostics) {
    out << " [" << d.code << "|" << d.message_key << "|" << d.detail << "|" << d.error;
    for (const auto& f : d.fields) out << "|" << f.key << "=" << f.value;
    for (const auto& f : d.identity_fields) out << "|" << f.first << "#" << Hex(f.second);
    out << "]";
  }
  return out.str();
}

// ---- the protected password for the S4a cross-check --------------------------------------------------------------
struct PasswordFactory {
  mem::MemoryManager manager{mem::DefaultLocalEngineMemoryPolicy()};
  mem::MemoryTag tag;
  explicit PasswordFactory(const p::Uuid& database) {
    tag.category = mem::MemoryCategory::core_runtime;
    tag.purpose = "native login test password";
    tag.callsite = "native_login_test";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = database.bytes;
  }
  mem::ScopedProtectedBuffer Make(std::string_view bytes) {
    mem::ProtectedMemoryRequest request;
    request.bytes = std::max<std::size_t>(bytes.size(), 1);
    request.tag = tag;
    request.material_class = "native login test password";
    request.platform_policy = mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto allocated = manager.AllocateProtected(request);
    Check(allocated.ok(), "protected memory for the test password");
    std::memcpy(allocated.buffer.data(), bytes.data(), bytes.size());
    return std::move(allocated.buffer);
  }
};

// ---- the served child --------------------------------------------------------------------------------------------
class FailFirstWriteSink final : public scratchbird::storage::disk::CarefulWriteSink {
 public:
  using D = scratchbird::storage::disk::BoundedIoResult;
  explicit FailFirstWriteSink(scratchbird::storage::disk::CarefulWriteSink& inner) : inner_(inner) {}
  D WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    D r;
    r.operation = scratchbird::storage::disk::BoundedIoOperation::write;
    r.error = scratchbird::storage::disk::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  D ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  D Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  scratchbird::storage::disk::CarefulWriteSink& inner_;
};

struct Served {
  srv::HostedEngineResult started;
  srv::NativeDatabaseLoop* loop = nullptr;
  std::shared_ptr<const api::NativeServedDatabaseFacts> facts;
  fs::path path, directory;
  p::Uuid database, bootstrap, service;
  std::map<std::string, std::string> derived;   // uuid hex -> password bytes (test secrets; from the environment)
} served;

struct Attempt {
  std::unique_ptr<srv::ServerSessionRegistry> registry = std::make_unique<srv::ServerSessionRegistry>();
  srv::SessionOperationResult result;
  std::string wire;
  double milliseconds = 0;
  std::array<std::uint8_t, 16> connection{};
};
std::vector<std::string> all_wires;       // every client-visible serialization the test saw
std::vector<std::string> all_internal;    // every internal code the test saw

Attempt Handoff(const LoginAsk& ask, srv::ServerSessionRegistry* shared_registry = nullptr, const std::string& source = {}) {
  Attempt attempt;
  auto* registry = shared_registry ? shared_registry : attempt.registry.get();
  registry->login_source_key = source;
  attempt.connection = sbps::MakeUuidV7Bytes();
  const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(attempt.connection, ask), attempt.connection);
  const auto started = std::chrono::steady_clock::now();
  attempt.result = srv::HandleAuthHandoff(registry, served.started.state, frame);
  attempt.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  attempt.wire = Wire(attempt.result);
  all_wires.push_back(attempt.wire);
  all_internal.push_back(attempt.result.native_login_audit.internal_code);
  return attempt;
}

api::EngineAuthenticateRequest EngineRequest(const LoginAsk& ask) {
  api::EngineAuthenticateRequest request;
  request.context.database_path = served.path.string();
  request.context.database_uuid = served.database;
  request.context.native_served_facts = served.facts;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.provider_family = ask.provider;
  request.principal_claim = ask.name;
  request.credential_evidence = ask.password;
  request.credential_evidence_present = true;
  return request;
}
api::EngineAuthenticateResult Engine(const LoginAsk& ask) {
  const auto result = api::EngineAuthenticate(EngineRequest(ask));
  all_wires.push_back(EngineWire(result));
  all_internal.push_back(result.native_login_audit.internal_code);
  return result;
}

constexpr const char* kPrefix = "SECURITY.AUTHENTICATION.NATIVE_LOGIN_";
std::string Internal(std::string_view tail) { return std::string(kPrefix) + std::string(tail); }

// One credential-cause case: both entry points refuse with the SAME client-visible vector, the internal code is the
// expected specific one, and the throttle (cleared first: each case is its own name) never sees a non-credential cause.
struct Collected {
  std::string handoff_wire, engine_wire, internal;
};
std::vector<std::pair<std::string, Collected>> credential_cases;
Collected CredentialCase(const std::string& what, const LoginAsk& ask, const std::vector<std::string>& expected_internal) {
  srv::NativeLoginThrottle().Clear();
  const auto handoff = Handoff(ask);
  Check(!handoff.result.accepted, what + ": the handoff refuses");
  Check(handoff.result.native_login_audit.present && handoff.result.native_login_audit.reached_loop && !handoff.result.native_login_audit.throttled,
        what + ": the audit says it reached the loop and was not throttled");
  const auto internal = handoff.result.native_login_audit.internal_code;
  Check(std::find(expected_internal.begin(), expected_internal.end(), internal) != expected_internal.end(),
        what + ": the specific internal code is one of the expected, got '" + internal + "'");
  Check(!handoff.result.diagnostics.empty() && handoff.result.diagnostics.front().code == kGeneric,
        what + ": the client-visible code is the generic LOGIN_FAILED, got " + (handoff.result.diagnostics.empty() ? std::string("none") : handoff.result.diagnostics.front().code));
  srv::NativeLoginThrottle().Clear();
  const auto engine = Engine(ask);
  Check(!engine.ok && !engine.authenticated && engine.diagnostics.size() == 1 && engine.diagnostics.front().code == kGeneric,
        what + ": EngineAuthenticate refuses with the generic code");
  Check(engine.native_login_audit.native && engine.native_login_audit.credential_failure && engine.native_login_audit.internal_code == internal,
        what + ": EngineAuthenticate's audit holds the same specific code and counts as a credential failure");
  Check(handoff.registry->sessions_by_uuid.empty() && handoff.registry->auth_contexts_by_uuid.empty(), what + ": no session and no auth context was created");
  Collected collected{handoff.wire, EngineWire(engine), internal};
  credential_cases.push_back({what, collected});
  return collected;
}

// The TIP's Next, captured by a snapshot-point scope (memory only).
std::uint64_t NextTransaction() {
  const auto scope = served.loop->RunInEngineReadTransaction([](const srv::NativeLoopReadTransaction&) {});
  Check(scope.begin.ok(), "a snapshot-point scope captures the TIP");
  return scope.snapshot_next_transaction;
}

void NoNameOrPasswordInOutput(const std::vector<std::string>& outputs) {
  const std::vector<std::string_view> forbidden{kBootstrapPassword, kWrongPassword, kBootstrapName, kServiceName, kLoginAlias,
                                                kLoginHidden, kLoginHiddenOnly, kUnknownName};
  for (const auto& output : outputs)
    for (const auto& text : forbidden)
      Check(output.find(text) == std::string::npos, "no name or password text in any client-visible or internal output: found " + std::string(text));
  for (const auto& [uuid, password] : served.derived)
    for (const auto& output : outputs)
      Check(password.empty() || output.find(password) == std::string::npos, "no derived agent password in any output");
}

int ServeMode(const fs::path& path, const std::string& state) {
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  served.path = path;
  served.directory = path.parent_path();
  const auto before = Snapshot(served.directory);   // (after the crypto owner: the digest needs OpenSSL)
  if (const char* env = std::getenv("SB_LOGIN_TEST_DERIVED")) {   // "uuid_hex:password_hex uuid_hex:password_hex ..."
    std::istringstream in(env);
    std::string item;
    while (in >> item) {
      const auto colon = item.find(':');
      if (colon == std::string::npos) continue;
      served.derived[item.substr(0, colon)] = FromHexText(std::string_view(item).substr(colon + 1));
    }
    unsetenv("SB_LOGIN_TEST_DERIVED");
  }
  if (state == "failed")
    srv::SetNativeRestartSinkWrapperForTest([](scratchbird::storage::disk::CarefulWriteSink& inner)
                                                -> std::unique_ptr<scratchbird::storage::disk::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
  served.started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (state == "failed")
    Check(!served.started.ok() && served.started.diagnostics.front().code == "DATABASE.RESTART_WRITE_FAILED", "the seam produced a REAL failed Restart");
  else
    Check(served.started.ok(), "the server serves the file: " + (served.started.diagnostics.empty() ? std::string() : served.started.diagnostics.front().code));
  Check(!served.started.state.database_runtimes.empty() && served.started.state.database_runtimes.front() &&
            served.started.state.database_runtimes.front()->native_loop, "the hosted state holds the native loop");
  served.loop = served.started.state.database_runtimes.front()->native_loop.get();
  served.facts = srv::ServedNativeFactsFor(served.started.state, path.string());
  Check(served.facts != nullptr && served.facts->login_source != nullptr && served.facts->names_source != nullptr,
        "the loop published the names source and the login source");
  served.database = served.facts->database_uuid;
  Check(!served.loop->on_loop_thread(), "the test is not on the loop thread");
  srv::NativeLoginThrottle().Clear();
  srv::NativeLoginThrottleClockHook() = nullptr;
  const auto next_before = state == "failed" ? 0 : NextTransaction();
  std::vector<double> right_ms, wrong_ms, unknown_ms;

  const auto principal_of = [](const Attempt& a) { return a.registry->auth_contexts_by_uuid.empty() ? std::array<std::uint8_t, 16>{} : a.registry->auth_contexts_by_uuid.begin()->second.principal_uuid; };

  if (state == "failed") {
    // The database is `failed`: a login is an INFRASTRUCTURE refusal with its own code (a client can tell the server is
    // unavailable from a wrong password), never the generic credential failure, and it never counts toward the throttle.
    for (int i = 0; i < 6; ++i) {
      const auto a = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
      Check(!a.result.accepted, "failed: refused");
      Check(!a.result.diagnostics.empty() && a.result.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_FAILED",
            "failed: the infrastructure code, not the generic one: " + (a.result.diagnostics.empty() ? std::string("none") : a.result.diagnostics.front().code));
      Check(a.result.native_login_audit.present && !a.result.native_login_audit.throttled, "failed: not a throttled attempt");
    }
    Check(srv::NativeLoginThrottle().size() == 0, "failed: infrastructure failures do not count toward the throttle (table empty after 6)");
    const auto e = Engine({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(!e.ok && e.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_FAILED" && !e.native_login_audit.credential_failure,
          "failed: EngineAuthenticate reports the same infrastructure code");
  } else if (state == "disabled" || state == "nocredential") {
    // The bootstrap principal's committed successor row (made by the engine's writers) is disabled / has no verifier. The right and the
    // wrong password fail the SAME generic way; the specific cause is internal.
    const std::string expected = state == "disabled" ? Internal("PRINCIPAL_NOT_ACTIVE") : Internal("CREDENTIAL_ABSENT");
    const auto right = CredentialCase(state + " principal, the password that was right", {std::string(kBootstrapName), std::string(kBootstrapPassword)}, {expected});
    const auto wrong = CredentialCase(state + " principal, a wrong password", {std::string(kBootstrapName), std::string(kWrongPassword)}, {expected});
    Check(right.handoff_wire == wrong.handoff_wire, state + ": the refusal does not depend on the password: identical client-visible bytes");
    const auto unknown = CredentialCase("unknown name after the change", {std::string(kUnknownName), std::string(kBootstrapPassword)}, {Internal("NAME_NOT_FOUND")});
    Check(unknown.handoff_wire == right.handoff_wire, state + ": a disabled/credential-less principal and an unknown name are indistinguishable");
  } else if (state == "throttle") {
    // ---- SB_NATIVE_LOGIN_THROTTLE: in memory, in the server layer, BEFORE the loop --------------------------------
    using Clock = srv::LoginThrottle::Clock;
    auto virtual_now = Clock::now();
    srv::NativeLoginThrottleClockHook() = [&] { return virtual_now; };
    srv::NativeLoginThrottle().Clear();
    const LoginAsk wrong_login{std::string(kBootstrapName), std::string(kWrongPassword)};
    const LoginAsk right_login{std::string(kBootstrapName), std::string(kBootstrapPassword)};
    // 1. four failures from one login (no source): each reaches the loop; the fifth, with the RIGHT password, is rejected early.
    std::string generic_wire;
    for (int i = 0; i < static_cast<int>(srv::LoginThrottle::kMaxFailures); ++i) {
      const auto a = Handoff(wrong_login);
      Check(a.result.native_login_audit.reached_loop && !a.result.native_login_audit.throttled &&
                a.result.native_login_audit.internal_code == Internal("PASSWORD_MISMATCH"),
            "failure " + std::to_string(i + 1) + " reaches the loop and is a password mismatch");
      generic_wire = a.wire;
    }
    // The proof the throttled attempt does not reach the loop: park the loop thread on a work item that waits, then
    // send (a) a throttled attempt - it must return at once, with the loop still parked - and (b) a control attempt for
    // another name - it must BLOCK behind the parked work until the loop is released.
    std::promise<void> parked, release;
    auto released = release.get_future().share();
    std::thread parker([&] { served.loop->RunOnLoopThreadForTest([&] { parked.set_value(); released.wait(); }); });
    parked.get_future().wait();
    auto throttled_future = std::async(std::launch::async, [&] { return Handoff(right_login); });
    const bool returned_while_parked = throttled_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    if (!returned_while_parked) {   // fail cleanly: free the loop so the blocked attempt can end, then report
      release.set_value();
      parker.join();
      throttled_future.get();
    }
    Check(returned_while_parked, "the throttled attempt returns while the loop is parked: it never reached the loop");
    const auto throttled = throttled_future.get();
    Check(throttled.result.native_login_audit.throttled && !throttled.result.native_login_audit.reached_loop &&
              throttled.result.native_login_audit.internal_code == Internal("THROTTLED"),
          "the fifth attempt is rejected early; its internal code is THROTTLED");
    Check(throttled.wire == generic_wire, "the throttled rejection is byte-identical to a wrong-password rejection (the client is not told)");
    Check(throttled.milliseconds < 100, "the throttled rejection costs no PBKDF2 (" + std::to_string(throttled.milliseconds) + " ms)");
    auto control_future = std::async(std::launch::async, [&] { return Handoff({std::string(kUnknownName), std::string(kWrongPassword)}); });
    Check(control_future.wait_for(std::chrono::milliseconds(700)) == std::future_status::timeout,
          "control: an unthrottled attempt BLOCKS while the loop is parked (the parked loop really is not answering)");
    release.set_value();
    parker.join();
    const auto control = control_future.get();
    Check(control.result.native_login_audit.reached_loop, "control: the unthrottled attempt reached the loop once it was released");
    // 2. expiry: after the window passes since the last failure the key is free again; the right password then logs in.
    virtual_now += std::chrono::seconds(9);
    const auto after = Handoff(right_login);
    Check(after.result.accepted && after.result.native_login_audit.reached_loop, "after the 8 s window the right password reaches the loop and is accepted");
    Check(srv::NativeLoginThrottle().size() <= 1, "a success cleared the entry (table holds at most the control's entry)");
    // 3. a success clears: three failures, a success, three failures are not four in a row.
    srv::NativeLoginThrottle().Clear();
    for (int i = 0; i < 3; ++i) Handoff(wrong_login);
    Check(Handoff(right_login).result.accepted, "after three failures the right password is still admitted");
    for (int i = 0; i < 3; ++i) Check(!Handoff(wrong_login).result.native_login_audit.throttled, "after the success three failures are not throttled");
    srv::NativeLoginThrottle().Clear();
    // 4. a source key: four failures from one source with FOUR DIFFERENT names throttle that source (the address-alone
    //    entry); another source is untouched. (A source is server-verified; nothing sets it in production yet.)
    for (int i = 0; i < 4; ++i) Handoff({std::string(kUnknownName) + std::to_string(i), std::string(kWrongPassword)}, nullptr, "source-A");
    const auto from_a = Handoff(right_login, nullptr, "source-A");
    Check(from_a.result.native_login_audit.throttled && !from_a.result.accepted, "source A is throttled whatever the name (the right password too)");
    const auto from_b = Handoff(right_login, nullptr, "source-B");
    Check(from_b.result.accepted && !from_b.result.native_login_audit.throttled, "source B is not throttled");
    // 5. a storm from many names does not grow the table past its bound.
    srv::NativeLoginThrottle().Clear();
    std::size_t largest = 0;
    for (int i = 0; i < 24; ++i) {
      Handoff({std::string(kUnknownName) + "_storm_" + std::to_string(i), std::string(kWrongPassword)});
      largest = std::max(largest, srv::NativeLoginThrottle().size());
    }
    Check(largest <= srv::LoginThrottle::kCapacity, "the table never exceeded " + std::to_string(srv::LoginThrottle::kCapacity) + " entries (largest " + std::to_string(largest) + ")");
    Check(largest == srv::LoginThrottle::kCapacity, "and it filled to the bound (24 distinct names were tracked up to 16)");
    srv::NativeLoginThrottleClockHook() = nullptr;
    srv::NativeLoginThrottle().Clear();
  } else {
    // ---- serving and restricted -------------------------------------------------------------------------------------
    const bool restricted = state == "restricted";
    // 1. The right name + password: accepted, with the BINARY bootstrap principal UUID and the S4a role UUIDs.
    const auto good = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(good.result.accepted, "bootstrap name + right password: accepted by the handoff: " + good.wire);
    Check(good.result.native_login_audit.accepted && good.result.native_login_audit.internal_code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_ACCEPTED",
          "the audit records the acceptance");
    Check(good.registry->auth_contexts_by_uuid.size() == 1 && good.registry->sessions_by_uuid.empty(),
          "one auth context, NO session record (the session and its transaction are S4e's)");
    const auto& session = good.registry->auth_contexts_by_uuid.begin()->second;
    Check(std::equal(session.principal_uuid.begin(), session.principal_uuid.end(), served.bootstrap.bytes.begin()),
          "the session's principal is the bootstrap principal's BINARY UUID, equal to the create's");
    Check(session.native_login_security_state != nullptr && session.native_served_facts != nullptr, "the native adapter state and the served facts are retained on the auth context");
    right_ms.push_back(good.milliseconds);
    // Cross-check with the S4a read itself (the same code, run through its own entry): the role UUIDs match.
    {
      PasswordFactory passwords(served.database);
      srv::NativeLoopLoginRequest request;
      request.principal_uuid = served.bootstrap;
      request.password = passwords.Make(kBootstrapPassword);
      request.password_length = kBootstrapPassword.size();
      const auto s4a = served.loop->SubmitReadLoginMaterial(std::move(request)).get();
      Check(s4a.ok() && s4a.material->principal_uuid == served.bootstrap, "S4a returns the bootstrap principal");
      Check(!s4a.material->role_uuids.empty(), "the bootstrap principal has direct roles");
      std::set<std::array<std::uint8_t, 16>> s4a_roles;
      for (const auto& role : s4a.material->role_uuids) s4a_roles.insert(role.bytes);
      std::set<std::array<std::uint8_t, 16>> session_roles(session.effective_role_uuids.begin(), session.effective_role_uuids.end());
      Check(!session_roles.empty() && std::includes(session_roles.begin(), session_roles.end(), s4a_roles.begin(), s4a_roles.end()),
            "the session's effective roles include every role UUID S4a reports");
      // The engine call: the same principal and the same roles, as the authenticated context.
      const auto direct = Engine({std::string(kBootstrapName), std::string(kBootstrapPassword)});
      Check(direct.ok && direct.authenticated && direct.native_login_audit.principal_uuid == served.bootstrap, "EngineAuthenticate accepts and names the same principal");
      Check(direct.connection_security_context.active_roles.size() == s4a.material->role_uuids.size() &&
                std::equal(direct.connection_security_context.active_roles.begin(), direct.connection_security_context.active_roles.end(),
                           s4a.material->role_uuids.begin()),
            "the authenticated context's roles are S4a's role UUIDs, in order");
      Check(direct.durable_security_state != nullptr && !direct.durable_security_state->principals.empty() &&
                direct.durable_security_state->principals.front().principal_name.empty() &&
                std::all_of(direct.durable_security_state->roles.begin(), direct.durable_security_state->roles.end(),
                            [](const auto& r) { return r.role_name.empty(); }),
            "the adapter state holds binary identities only: no principal or role name");
      Check(direct.diagnostics.empty(), "the success result carries no diagnostic (and AddSecurityRow holds only binary identities: see the native branch)");
    }
    // 2. An alias name logs in as the same principal.
    if (!restricted) {
      const auto alias = Handoff({std::string(kLoginAlias), std::string(kBootstrapPassword)});
      Check(alias.result.accepted && principal_of(alias) == principal_of(good), "an alias-class name authenticates the same principal");
    }
    // 3. Every credential cause: the SAME client-visible bytes; different internal codes.
    std::vector<Collected> causes;
    const auto wrong = CredentialCase("wrong password", {std::string(kBootstrapName), std::string(kWrongPassword)}, {Internal("PASSWORD_MISMATCH")});
    causes.push_back(wrong);
    causes.push_back(CredentialCase("unknown name", {std::string(kUnknownName), std::string(kBootstrapPassword)}, {Internal("NAME_NOT_FOUND")}));
    if (!restricted) {
      causes.push_back(CredentialCase("hidden-class name, right password", {std::string(kLoginHidden), std::string(kBootstrapPassword)}, {Internal("NAME_HIDDEN")}));
      causes.push_back(CredentialCase("hidden-only name (L5's), any password", {std::string(kLoginHiddenOnly), std::string(kWrongPassword)}, {Internal("NAME_HIDDEN")}));
    }
    // (NAME_AMBIGUOUS is unreachable through the production writers: they refuse a second entry whose key lane collides
    // in the same scope and language, which is the only way two objects can match one lookup. The login's handling of it
    // is therefore not exercised here.)
    causes.push_back(CredentialCase("empty name", {"", std::string(kBootstrapPassword)}, {Internal("NAME_NOT_FOUND")}));
    causes.push_back(CredentialCase("empty password", {std::string(kBootstrapName), ""}, {Internal("PASSWORD_INADMISSIBLE")}));
    causes.push_back(CredentialCase("a password over the limit", {std::string(kBootstrapName), std::string(2000, 'q')}, {Internal("PASSWORD_INADMISSIBLE")}));
    causes.push_back(CredentialCase("a password holding a NUL", {std::string(kBootstrapName), std::string("a\0b", 3)}, {Internal("PASSWORD_INADMISSIBLE")}));
    const std::string l5_hex = Hex(served.service);
    const std::string l5_password = served.derived.count(l5_hex) ? served.derived.at(l5_hex) : std::string();
    Check(l5_password.size() == 16, "L5's derived password is known to the test (database UUID XOR the stored seed)");
    causes.push_back(CredentialCase("L5 with its CORRECT derived password", {std::string(kServiceName), l5_password}, {"SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED"}));
    causes.push_back(CredentialCase("L5 with a wrong password", {std::string(kServiceName), std::string(kWrongPassword)}, {"SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED"}));
    // Names of other principals, roles and agents, from the file's own name rows.
    const auto names = served.facts->names_source->Read();
    Check(names.ok(), "the names source reads");
    std::size_t role_names = 0, agent_names = 0;
    for (const auto& row : names.rows) {
      if (row.object_kind == static_cast<std::uint16_t>(cat::CatalogRecordKind::role_account) && role_names < 1 && row.name_class != 5) {
        ++role_names;
        causes.push_back(CredentialCase("a role's name", {row.raw_name_text, std::string(kBootstrapPassword)},
                                        {Internal("PRINCIPAL_WRONG_FAMILY"), Internal("PRINCIPAL_ABSENT")}));
      }
      const auto hex = Hex(row.object_uuid);
      if (row.object_kind == static_cast<std::uint16_t>(cat::CatalogRecordKind::user_account) && row.object_uuid != served.bootstrap &&
          row.object_uuid != served.service && served.derived.count(hex) && agent_names < 2 && row.name_class != 5 && row.default_for_object) {
        ++agent_names;
        causes.push_back(CredentialCase("a named agent with its CORRECT derived password", {row.raw_name_text, served.derived.at(hex)},
                                        {"SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED"}));
      }
    }
    std::cout << "INFO role_names_tested=" << role_names << " agent_names_tested=" << agent_names << " derived_seeded=" << served.derived.size()
              << " name_rows=" << names.rows.size() << std::endl;
    std::set<std::string> client_wires, internal_codes;
    for (const auto& c : causes) {
      client_wires.insert(c.handoff_wire);
      internal_codes.insert(c.internal);
    }
    Check(client_wires.size() == 1, "EVERY credential cause returned the identical client-visible bytes through HandleAuthHandoff (distinct serializations: " + std::to_string(client_wires.size()) + ")");
    std::set<std::string> engine_wires;
    for (const auto& c : causes) engine_wires.insert(c.engine_wire);
    Check(engine_wires.size() == 1, "and through EngineAuthenticate (distinct serializations: " + std::to_string(engine_wires.size()) + ")");
    Check(internal_codes.size() >= (restricted ? 5u : 6u), "while the specific internal codes differ (" + std::to_string(internal_codes.size()) + " distinct)");
    // 4. Injection: no request field can inject a principal, a role or a "trusted" flag.
    {
      LoginAsk ask{std::string(kBootstrapName), std::string(kWrongPassword)};
      auto request = EngineRequest(ask);
      request.durable_principal_uuid = served.bootstrap;  // a claimed principal UUID, with the WRONG password
      auto result = api::EngineAuthenticate(request);
      Check(!result.ok && !result.authenticated && result.diagnostics.front().code == "SECURITY.AUTHENTICATION.REQUEST_INVALID",
            "a claimed principal UUID is refused outright, never believed");
      request = EngineRequest(ask);
      for (const char* option : {"credential:valid", "credential_evidence_present:true", "fixture_fail:false", "credential_verifier_match:true",
                                 "protected_material:available", "auth_authority:engine", "storage_authority:durable_security_catalog",
                                 "principal:fixture_service", "right:CONNECT", "security.fixture_trace_authority"})
        request.option_envelopes.push_back(option);
      request.option_envelopes.push_back("durable_principal_uuid:" + Hex(served.bootstrap));
      result = api::EngineAuthenticate(request);
      Check(!result.ok && !result.authenticated && result.diagnostics.front().code == kGeneric &&
                result.native_login_audit.internal_code == Internal("PASSWORD_MISMATCH"),
            "option envelopes (credential:valid, principal:, storage authority, durable_principal_uuid:...) are ignored: the wrong password still fails");
      // Evidence fields are the password's bytes, not structure: a "verifier" and a "principal_uuid" inside change nothing.
      LoginAsk forged{std::string(kBootstrapName),
                      "scheme=local_password_v1;principal=" + std::string(kBootstrapName) + ";principal_uuid=" + Hex(served.bootstrap) +
                          ";verifier=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;storage_authority=durable_security_catalog"};
      Check(!Engine(forged).authenticated, "structured-looking evidence is just a wrong password");
      // A transport that says the credential is invalid can only refuse: even the right password is then refused.
      request = EngineRequest({std::string(kBootstrapName), std::string(kBootstrapPassword)});
      request.credential_invalid_claim = true;
      result = api::EngineAuthenticate(request);
      Check(!result.authenticated && result.diagnostics.front().code == kGeneric &&
                result.native_login_audit.internal_code == Internal("TRANSPORT_FLAG_INVALID"),
            "credential_invalid_claim with the RIGHT password is refused (it can only refuse)");
      // The name claim is the presented DATA: a name that spells a principal UUID is just a name.
      LoginAsk uuid_text{Hex(served.bootstrap), std::string(kBootstrapPassword)};
      Check(!Engine(uuid_text).authenticated, "a name that spells the bootstrap UUID is only a name: unknown");
    }
    // 5. An unsupported provider is infrastructure-shaped: its own code, not the generic failure, not throttled.
    {
      const auto a = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword), "security_database_temporary_token"});
      Check(!a.result.accepted && a.result.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_PROVIDER_UNSUPPORTED",
            "a provider the native login does not serve has its own code");
      Check(srv::NativeLoginThrottle().size() == 0, "and does not count toward the throttle");
    }
    // 6. A requested role by name cannot be honoured natively (definitions hold no names): specific refusal AFTER the login.
    {
      const auto a = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword), "local_password", "some_role"});
      bool role_refused = false;
      for (const auto& d : a.result.diagnostics)
        if (d.code == "SECURITY.ROLE_INVALID") role_refused = true;
      Check(!a.result.accepted && role_refused, "a requested role by name is refused specifically (SECURITY.ROLE_INVALID) after authentication");
      Check(a.registry->auth_contexts_by_uuid.empty(), "and no auth context is kept for it");
    }
    // 7. Attach and the embedded attach on a native database: refused specifically, before anything is begun.
    {
      const auto auth = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
      Check(auth.result.accepted, "the login for the attach case");
      const auto context = srv::DecodeAuthContextUuidForTest(auth.result.payload);
      Check(context.has_value(), "the auth context UUID decodes");
      const auto attach_codes = [&](const srv::SessionOperationResult& r) {
        std::string codes;
        for (const auto& d : r.diagnostics) {
          codes += d.code + " ";
          for (const auto& f : d.fields)
            if (f.key == "detail") codes += f.value + " ";
        }
        return codes;
      };
      // (a) As the login built it. The first-slice adapter holds only what the S4a material carries: the principal's OWN
      //     rows and its direct memberships; the privileges its ROLES own are not in the material, so CONNECT is not
      //     authorized. The attach refuses at CONNECT, before anything is begun: a finding for the owner (S4e / material).
      const auto attach_frame = MakeFrame(sbps::MessageType::kAttachDatabase, AttachPayload(auth.connection, *context), auth.connection);
      const auto attached = srv::HandleAttachDatabase(auth.registry.get(), served.started.state, attach_frame);
      const auto first_codes = attach_codes(attached);
      Check(!attached.accepted && auth.registry->sessions_by_uuid.empty(), "the native attach is not accepted and no session exists: " + first_codes);
      if (restricted) {
        // A restricted database is read-only: a read_write attach is denied by its existing attach-mode rule before any
        // authorization (Q309: the login was allowed; transactions are the S5 resolver's).
        Check(first_codes.find("read_only_database") != std::string::npos, "restricted: the attach is denied as a read-only database: " + first_codes);
      } else
      Check(first_codes.find("SECURITY.AUTHORIZATION.DENIED") != std::string::npos && first_codes.find("CONNECT") != std::string::npos,
            "as the adapter built it the attach is refused at the CONNECT authorization (no role-owned privilege rows in the material): " + first_codes);
      std::cout << "INFO attach_as_built=" << first_codes << std::endl;
      if (!restricted) {
      // (b) With a CONNECT privilege added to the retained adapter state IN MEMORY (the test's own mutation of the
      //     registry's record; nothing is mocked in the engine): the attach reaches the specific refusal that stands in
      //     for the legacy transaction begin, still before anything is begun or written.
      {
        auto amended = std::make_shared<api::EngineSecurityPrincipalLifecycleState>(*auth.registry->auth_contexts_by_uuid.begin()->second.native_login_security_state);
        api::EngineSecurityPrivilegeGrantRecord grant;
        grant.grant_uuid = *scratchbird::core::uuid::IssueRuntimeIdentityV7();
        grant.grantee_uuid = served.bootstrap;
        grant.grantee_kind = "principal";
        grant.privilege = "CONNECT";
        grant.grant_effect = "allow";
        grant.security_generation = amended->security_generation;
        amended->grants.push_back(grant);
        auth.registry->auth_contexts_by_uuid.begin()->second.native_login_security_state = amended;
      }
      const auto attach_frame2 = MakeFrame(sbps::MessageType::kAttachDatabase, AttachPayload(auth.connection, *context), auth.connection);
      const auto attached2 = srv::HandleAttachDatabase(auth.registry.get(), served.started.state, attach_frame2);
      const auto codes = attach_codes(attached2);
      Check(!attached2.accepted && auth.registry->sessions_by_uuid.empty(), "the amended native attach is not accepted and no session exists: " + codes);
      Check(codes.find("native_attach_requires_s4e") != std::string::npos,
            "with CONNECT the attach reaches the specific refusal native_attach_requires_s4e (the legacy transaction begin is not run): " + codes);
      }
      srv::ServerSessionRegistry embedded_registry;
      const auto embedded = srv::HandleEmbeddedSysarchAttach(&embedded_registry, served.started.state, "default", "native_login_test");
      bool embedded_refused = false;
      for (const auto& d : embedded.diagnostics)
        if (d.code == "PARSER_SERVER_IPC.EMBEDDED_ATTACH_NATIVE_UNAVAILABLE") embedded_refused = true;
      Check(!embedded.accepted && embedded_refused && embedded_registry.sessions_by_uuid.empty(), "the embedded attach on a native database refuses specifically (Q285: not lifted)");
    }
    if (!restricted) {
      // 8. Timing: the unknown-name path burns one PBKDF2 like a wrong password does.
      for (int i = 0; i < 3; ++i) {
        const auto w = Handoff({std::string(kBootstrapName), std::string(kWrongPassword) + std::to_string(i)});
        wrong_ms.push_back(w.milliseconds);
        const auto u = Handoff({std::string(kUnknownName) + std::to_string(i), std::string(kWrongPassword)});
        unknown_ms.push_back(u.milliseconds);
      }
      const auto mean = [](const std::vector<double>& v) { double s = 0; for (const double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
      std::cout << "TIMING right_ms=" << mean(right_ms) << " wrong_ms=" << mean(wrong_ms) << " unknown_ms=" << mean(unknown_ms) << std::endl;
      Check(mean(unknown_ms) >= 0.6 * mean(wrong_ms), "an unknown name costs about what a wrong password costs (the equaliser burn): unknown " +
                                                          std::to_string(mean(unknown_ms)) + " ms vs wrong " + std::to_string(mean(wrong_ms)) + " ms");
    }
  }

  // ---- nothing was written, whatever happened -------------------------------------------------------------------------
  Check(Snapshot(served.directory) == before, "the directory and every file's bytes are unchanged after every attempt");
  for (const auto& entry : fs::directory_iterator(served.directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  if (state != "failed") Check(NextTransaction() == next_before, "the TIP's Next is unchanged: no login, successful or failed, took a transaction number");
  NoNameOrPasswordInOutput(all_wires);
  NoNameOrPasswordInOutput(all_internal);

  // After the loop closes, a login is an infrastructure refusal with its own code (not the generic failure).
  Check(served.loop->Close().ok, "the loop closes");
  if (state != "failed" && state != "throttle") {
    const auto after = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(!after.result.accepted && !after.result.diagnostics.empty() &&
              after.result.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_NOT_SERVING",
          "a login after the loop closed: the infrastructure code, not the generic one: " + after.wire);
  }
  served.facts.reset();
  served.started = {};
  std::cout << "LOGIN_OK checks=" << checks << " state=" << state << std::endl;
  return 0;
}

// ---- driver --------------------------------------------------------------------------------------------------------
std::string Capture(const std::string& command, int* status) {
  std::string output;
  FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
  Check(pipe != nullptr, "popen");
  char chunk[512];
  std::size_t got;
  while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) output.append(chunk, got);
  const int raw = ::pclose(pipe);
  *status = raw == -1 ? -1 : (WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw));
  return output;
}
std::string Field(const std::string& text, std::string_view key) {
  const auto at = text.find(std::string(key) + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  const auto to = text.find_first_of(" \n", from);
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

void RunState(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::string& state) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / (state + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "login.sdb";
  int status = 0;
  const std::string material = SB_NATIVE_LOGIN_MATERIAL_TEST_BIN;
  const auto created = Capture("'" + material + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
  Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
  const auto database = Field(created, "database"), bootstrap = Field(created, "bootstrap"), service = Field(created, "service"),
             binding = Field(created, "binding");
  // (restricted: the file carries a committing slot from native_serving's own writer; the names forge needs a clean TIP, so a
  // restricted database is logged in by its create-written names only: no alias and no hidden name.)
  if (state == "serving" || state == "throttle") {
    const auto forged = Capture(std::string("'") + SB_NATIVE_NAME_RESOLVE_TEST_BIN + "' forge '" + path.string() + "' login " + bootstrap + " " + service, &status);
    Check(status == 0 && forged.find("FORGED kind=login") != std::string::npos, tag + state + ": the login names were written by the engine's writers: " + forged.substr(0, 1500));
  }
  if (state == "restricted") {
    Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
          tag + "the native_serving writer left a committing slot");
  } else if (state == "disabled" || state == "nocredential") {
    const auto forged = Capture("'" + material + "' forge '" + path.string() + "' " + state + " " + bootstrap, &status);
    Check(status == 0 && forged.find("FORGED kind=" + state) != std::string::npos, tag + state + ": the engine's writers changed the row: " + forged.substr(0, 1500));
  } else if (state == "failed") {
    Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' active", &status).find("FIXTURE ok") != std::string::npos && status == 0,
          tag + "the native_serving writer left an active slot (the Restart will write; the seam fails it)");
  }
  // L5's (and the agents') derived passwords, read offline BEFORE the server owns the file; handed over in the environment.
  std::string derived;
  if (state == "serving" || state == "restricted" || state == "throttle") {
    const auto d = Capture("'" + material + "' derive '" + path.string() + "' " + binding, &status);
    Check(status == 0 && d.starts_with("DERIVED"), tag + state + ": the offline derivation: " + d.substr(0, 200));
    derived = d.substr(std::string("DERIVED").size());
    while (!derived.empty() && (derived.back() == '\n' || derived.back() == ' ')) derived.pop_back();
    if (!derived.empty() && derived.front() == ' ') derived.erase(0, 1);
  }
  const auto start = std::chrono::steady_clock::now();
  std::string command = "'" + self + "' serve '" + path.string() + "' " + state + " " + database + " " + bootstrap + " " + service;
  if (!derived.empty()) {
    ::setenv("SB_LOGIN_TEST_DERIVED", derived.c_str(), 1);
  } else {
    ::unsetenv("SB_LOGIN_TEST_DERIVED");
  }
  const auto served_out = Capture(command, &status);
  ::unsetenv("SB_LOGIN_TEST_DERIVED");
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Check(status == 0 && served_out.find("LOGIN_OK") != std::string::npos, tag + state + ": serve child: " + served_out.substr(0, 3000));
  for (const auto& output : {created, served_out})
    Check(output.find(kBootstrapPassword) == std::string::npos && output.find(kWrongPassword) == std::string::npos, tag + state + ": no test password in any child output");
  if (const auto info = served_out.find("INFO "); info != std::string::npos)
    std::printf("%s%s: %s\n", tag.c_str(), state.c_str(), served_out.substr(info, served_out.find('\n', info) - info).c_str());
  if (const auto timing = served_out.find("TIMING"); timing != std::string::npos)
    std::printf("%s%s: %.1f s; %s\n", tag.c_str(), state.c_str(), seconds, served_out.substr(timing, served_out.find('\n', timing) - timing).c_str());
  else
    std::printf("%s%s: %.1f s\n", tag.c_str(), state.c_str(), seconds);
  fflush(stdout);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {   // serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size") || (argc == 2 && std::string(argv[1]) == "common")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 2) RunState(self, artifacts.path(), 8192, "throttle");
    else if (argc == 3) {
      const auto size = static_cast<std::uint32_t>(std::stoul(argv[2]));
      for (const char* state : {"serving", "restricted", "failed", "disabled", "nocredential"}) RunState(self, artifacts.path(), size, state);
    } else {
      RunState(self, artifacts.path(), 8192, "throttle");
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u})
        for (const char* state : {"serving", "restricted", "failed", "disabled", "nocredential"}) RunState(self, artifacts.path(), size, state);
    }
    artifacts.Cleanup();
    std::printf("native_login: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
