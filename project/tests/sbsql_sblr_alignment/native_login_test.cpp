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
//     reports; an alias name logs in too; [S4e: an approved login ALSO holds a session record and its first transaction
//     (was: none exists afterwards); the approval, the ledger records and the disconnect are asserted by the s4e states];
//   * EVERY credential cause - wrong password, unknown name, hidden-class name, a name two different objects match,
//     an empty name, a role's name, an inadmissible password, L5 with its correct derived password, a named agent with
//     its correct derived password - returns the SAME client-visible failure: the full serialized result (diagnostics,
//     payload, flags) is compared byte for byte; the SPECIFIC internal code (result.native_login_audit) differs;
//   * a disabled principal and a credential-less principal (forged by the engine's own writers) fail the same way;
//   * infrastructure causes keep their own codes (failed database, closed loop, an unsupported provider);
//   * a restricted database logs in (Q309) [S4e: but its session is refused with the Q299 code, nothing written];
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
#include "../support/native_offline_file_reader.hpp"
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
#include <functional>
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
std::uint64_t g_approvals = 0;            // S4e: logins approved through Handoff (each took one transaction number)
std::vector<std::string> all_wires;       // every client-visible serialization the test saw
std::vector<std::string> all_internal;    // every internal code the test saw

// The handoff itself, with no test-global state touched: safe to call from several threads (each attempt has its own registry).
Attempt HandoffRaw(const LoginAsk& ask, srv::ServerSessionRegistry* shared_registry = nullptr, const std::string& source = {},
                   const srv::ServerAdmittedParserChannelIdentity* hello = nullptr) {
  Attempt attempt;
  auto* registry = shared_registry ? shared_registry : attempt.registry.get();
  registry->login_source_key = source;
  attempt.connection = sbps::MakeUuidV7Bytes();
  // 2026-11-23 (512-byte ledger record): a channel whose HELLO was admitted (the identities the server retained).
  if (hello) registry->admitted_parser_identity_by_connection_uuid[p::Uuid{attempt.connection}] = *hello;
  const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(attempt.connection, ask), attempt.connection);
  const auto started = std::chrono::steady_clock::now();
  attempt.result = srv::HandleAuthHandoff(registry, served.started.state, frame);
  attempt.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  attempt.wire = Wire(attempt.result);
  return attempt;
}
// What the test records of an attempt (single thread).
void Record(const Attempt& attempt) {
  all_wires.push_back(attempt.wire);
  all_internal.push_back(attempt.result.native_login_audit.internal_code);
  if (attempt.result.native_approval.present) ++g_approvals;
}
Attempt Handoff(const LoginAsk& ask, srv::ServerSessionRegistry* shared_registry = nullptr, const std::string& source = {},
                const srv::ServerAdmittedParserChannelIdentity* hello = nullptr) {
  auto attempt = HandoffRaw(ask, shared_registry, source, hello);
  Record(attempt);
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

std::uint64_t NextTransaction();   // defined below (the TIP's Next at a snapshot point)

// One credential-cause case: both entry points refuse with the SAME client-visible vector, the internal code is the
// expected specific one, and the throttle (cleared first: each case is its own name) never sees a non-credential cause.
struct Collected {
  std::string handoff_wire, engine_wire, internal;
};
std::vector<std::pair<std::string, Collected>> credential_cases;
Collected CredentialCase(const std::string& what, const LoginAsk& ask, const std::vector<std::string>& expected_internal) {
  srv::NativeLoginThrottle().Clear();
  const auto next_around = NextTransaction();   // S4e: a failed login takes no transaction number
  const auto handoff = Handoff(ask);
  Check(!handoff.result.accepted, what + ": the handoff refuses");
  Check(NextTransaction() == next_around, what + ": a failed login took no transaction number (and wrote nothing)");
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

// ============================================================================================================================
// SEARCH_KEY: SB_NATIVE_SESSION_APPROVAL_TEST (S4e; owner 2026-11-21: the user's session and first transaction begin only when
// authentication is APPROVED; 2026-11-22: session end = a NEW ledger record; S4d answer 3: close rolls back). The states below
// are served children like the S4c ones: a PRODUCTION-created database served by StartHostedEngine, logged in through the real
// HandleAuthHandoff. The file is read BYTES ONLY (tests/support/native_offline_file_reader.hpp), by hand: txn id -> header ->
// +96 session record -> principal equals +64; +80 attachment -> connection.
//   approve         success returns a session id and a transaction id; the ledger holds connection, session, attachment in
//                   that order and the TIP slot is active with header +48/+64/+80/+96 correct; a second session; six logins
//                   from six threads; an abrupt disconnect rolls the transaction back and appends the session-ended record;
//                   the graceful close does the same for every session that is left; a disconnect AFTER the close is settled.
//   reopen          the next open: the verification accepts the ended sessions, nothing is orphaned, Restart marks nothing.
//   crash           two approvals, then a forced _Exit; the driver reads the file, then runs `reopen_crash`.
//   reopen_crash    after the kill: Restart marks the slots dead, the facts report the orphaned (un-ended) sessions, login works.
//   failed_later    the writer FAILS after an approval: the next approval is refused, the live serving state is `failed`
//                   (the facts' cell), login and attach refuse with the failed codes.
//   role_*, group_connect, transitive   the role / group privilege cases (forged by the engine's own writers).
// ============================================================================================================================
namespace off = scratchbird::tests::offline;
namespace pgs = scratchbird::storage::page;

constexpr std::uint8_t kSnapshotStableProfile = 4;   // IsolationProfileKind::snapshot_stable (the Firebird-like snapshot default)

std::array<p::byte, 32> SetDigest(std::vector<p::Uuid> uuids) {
  std::array<p::byte, 32> out{};
  Check(api::NativeUuidSetDigest(std::move(uuids), out), "the set digest computes");
  return out;
}

// Everything the S4a read says of the bootstrap principal (the effective sets the digests are computed from).
api::NativeLoginMaterial BootstrapMaterial() {
  PasswordFactory passwords(served.database);
  srv::NativeLoopLoginRequest request;
  request.principal_uuid = served.bootstrap;
  request.password = passwords.Make(kBootstrapPassword);
  request.password_length = kBootstrapPassword.size();
  const auto read = served.loop->SubmitReadLoginMaterial(std::move(request)).get();
  Check(read.ok() && read.material.has_value(), "the S4a read of the bootstrap principal");
  return *read.material;
}

srv::SessionOperationResult Disconnect(Attempt& attempt, std::string_view reason) {
  const auto& approval = attempt.result.native_approval;
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = attempt.connection;
  frame.header.session_uuid = approval.session_uuid;
  frame.payload.assign(approval.session_uuid.begin(), approval.session_uuid.end());
  PutString(&frame.payload, reason);
  return srv::HandleDisconnectNotice(attempt.registry.get(), frame);
}

std::size_t IndexOf(const off::FileView& file, const p::Uuid& uuid, pgs::SessionLedgerKind kind, bool latest = false) {
  std::size_t found = file.ledger.size();
  for (std::size_t i = 0; i < file.ledger.size(); ++i)
    if (file.ledger[i].record_uuid == uuid && file.ledger[i].kind() == kind && (latest || found == file.ledger.size())) found = i;
  return found;
}

// The audit trace of one approved session BY HAND, from its transaction id.
void CheckAuditTrace(const off::FileView& file, const srv::ServerNativeApproval& approval, const p::Uuid& principal,
                     const p::Uuid& instance, pgs::TipStateCode expected_state, const std::string& what) {
  const p::Uuid session{approval.session_uuid}, attachment{approval.attachment_uuid}, connection{approval.connection_record_uuid};
  const auto word = file.words.find(approval.transaction_order);
  Check(word != file.words.end() && word->second.state == expected_state,
        what + ": the TIP slot of order " + std::to_string(approval.transaction_order) + " is " +
            (word == file.words.end() ? std::string("absent") : std::string(pgs::TipStateCodeName(word->second.state))) + ", expected " +
            pgs::TipStateCodeName(expected_state));
  // 1. txn id -> header
  const auto header = file.headers.find(approval.transaction_order);
  Check(header != file.headers.end(), what + ": the transaction has a durable header");
  const auto& h = header->second;
  Check(h.transaction_order == approval.transaction_order && h.transaction_uuid == approval.transaction_uuid, what + ": header order and UUID are the approval's");
  Check(h.engine_instance_uuid == instance, what + ": header +48 is the engine instance");
  Check(h.principal_or_agent_uuid == principal, what + ": header +64 is the AUTHENTICATED principal (the create's bootstrap UUID)");
  Check(h.attachment_uuid == attachment, what + ": header +80 is the attachment");
  Check(h.session_uuid == session, what + ": header +96 is the session");
  Check(h.isolation_profile == kSnapshotStableProfile && !h.read_only, what + ": isolation = the snapshot default, read-write");
  // 2. +96 -> the session record; its principal equals +64
  const auto s_open = IndexOf(file, session, pgs::SessionLedgerKind::session);
  Check(s_open < file.ledger.size(), what + ": the session record is in the ledger");
  const auto& s = std::get<pgs::LedgerSessionRecord>(file.ledger[s_open].body);
  Check(s.principal_uuid == h.principal_or_agent_uuid && s.principal_uuid == principal, what + ": session principal == header +64 == the authenticated principal");
  Check(s.engine_instance_uuid == instance && !s.ended() && s.opened_at_utc_ns != 0, what + ": the opening record: engine instance, not ended, opened_at set");
  // 3. +80 -> the attachment record -> the session and the connection
  const auto a_at = IndexOf(file, attachment, pgs::SessionLedgerKind::attachment);
  Check(a_at < file.ledger.size(), what + ": the attachment record is in the ledger");
  const auto& a = std::get<pgs::LedgerAttachmentRecord>(file.ledger[a_at].body);
  Check(a.session_uuid == session && a.connection_uuid == connection && a.engine_instance_uuid == instance && a.predecessor_uuid.is_nil(),
        what + ": the attachment names the session and the connection record");
  const auto c_at = IndexOf(file, connection, pgs::SessionLedgerKind::connection);
  Check(c_at < file.ledger.size(), what + ": the connection record is in the ledger");
  // 4. durable before cited, in the order the approval wrote them
  Check(c_at < s_open && s_open < a_at, what + ": ledger order is connection, session, attachment");
  Check(file.ledger[c_at].ordinal == c_at && file.ledger[s_open].ordinal == s_open && file.ledger[a_at].ordinal == a_at, what + ": ordinals are the chain positions");
  Check(file.ledger[s_open].ordinal == approval.session_ordinal && file.ledger[a_at].ordinal == approval.attachment_ordinal, what + ": the approval reports the same ordinals");
}

struct Approved {
  Attempt attempt;
  srv::ServerNativeApproval approval;
};
Approved Approve(const LoginAsk& ask = {std::string(kBootstrapName), std::string(kBootstrapPassword)},
                 const srv::ServerAdmittedParserChannelIdentity* hello = nullptr) {
  Approved out;
  out.attempt = Handoff(ask, nullptr, {}, hello);
  Check(out.attempt.result.accepted && out.attempt.result.native_approval.present, "the login is approved: " + out.attempt.wire);
  out.approval = out.attempt.result.native_approval;
  const auto& ap = out.approval;
  const p::Uuid session{ap.session_uuid};
  Check(out.attempt.result.session_uuid == ap.session_uuid, "the success result carries the session id");
  Check(ap.transaction_order != 0 && !ap.transaction_uuid.is_nil() && !session.is_nil(), "and the transaction id (order and UUID)");
  const auto found = out.attempt.registry->sessions_by_uuid.find(session);
  Check(found != out.attempt.registry->sessions_by_uuid.end(), "the session is recorded in the registry (sessions_by_uuid) under its own session UUID");
  const auto& rec = found->second;
  Check(rec.session_uuid == ap.session_uuid && rec.local_transaction_id == ap.transaction_order && rec.transaction_uuid == ap.transaction_uuid &&
            rec.default_local_transaction_id == ap.transaction_order && rec.transactions_by_local_id.size() == 1 &&
            rec.attachment_id == ap.attachment_uuid && rec.native_session_approved && !rec.native_loop.expired(),
        "the registry record carries the transaction order and UUID, the attachment, and the loop that holds them");
  Check(rec.session_uuid != rec.auth_context_uuid, "the session UUID is the server's own issue, not the auth context's");
  return out;
}

void CheckSetDigestDefinition();

p::Uuid Fresh() {
  const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(id.has_value(), "an identity");
  return *id;
}

int S4eState(const std::string& state, const fs::path& path) {
  using TS = pgs::TipStateCode;
  const auto page_size = served.facts->page_size_bytes;
  const p::Uuid instance = served.facts->engine_instance_uuid;
  const auto read = [&] { return off::ReadFile(path, page_size); };
  const auto dir_before = Snapshot(served.directory);

  if (state == "approve") {
    const auto file0 = read();
    Check(file0.ledger.empty(), "the created file's ledger is empty");
    const auto next0 = file0.counters.next_transaction;
    const auto material = BootstrapMaterial();
    Check(!material.effective_role_uuids.empty() && !material.sysarch_role_uuid.is_nil(), "the bootstrap principal is effectively a SYSARCH member (Part 1)");
    CheckSetDigestDefinition();
    // A FAILED login writes nothing: not a byte of the file, no ledger record, Next unchanged.
    {
      const auto bytes = Snapshot(served.directory);
      const auto failed = Handoff({std::string(kBootstrapName), std::string(kWrongPassword)});
      Check(!failed.result.accepted && !failed.result.native_approval.present && failed.registry->sessions_by_uuid.empty(), "a wrong password is refused, no session");
      Check(Snapshot(served.directory) == bytes && read().ledger.empty() && read().counters.next_transaction == next0, "and wrote NOTHING: bytes, ledger and Next are unchanged");
      srv::NativeLoginThrottle().Clear();
    }
    // Session 1.
    auto s1 = Approve();
    Check(s1.approval.transaction_order == next0, "the first user transaction takes the TIP's Next (" + std::to_string(next0) + ")");
    Check(s1.approval.writes >= 4 && s1.approval.syncs >= 4, "the approval wrote three ledger records and began a transaction (writes=" + std::to_string(s1.approval.writes) + " syncs=" + std::to_string(s1.approval.syncs) + ")");
    auto f1 = read();
    Check(f1.ledger.size() == 3, "three ledger records after one approval");
    CheckAuditTrace(f1, s1.approval, served.bootstrap, instance, TS::active, "session 1");
    {
      const auto& conn = std::get<pgs::LedgerConnectionRecord>(f1.ledger[0].body);
      const auto& sess = std::get<pgs::LedgerSessionRecord>(f1.ledger[1].body);
      Check(conn.listener_uuid.is_nil() && conn.parser_instance_uuid.is_nil(), "the connection record: listener and parser instance are zero (no HELLO was admitted)");
      Check(conn.transport == pgs::LedgerTransport::unix_socket && conn.peer_address_type == pgs::LedgerPeerAddressType::none && conn.peer_port == 0 &&
                conn.tls_evidence_kind == 0 && conn.route_uuid.is_nil() && conn.opened_at_utc_ns != 0,
            "the connection record: unix socket, no peer address (the server sees only its untrusted parser peer), no TLS evidence");
      Check(sess.role_set_digest == SetDigest(material.effective_role_uuids), "the session's role-set digest is the digest of the EFFECTIVE roles (sorted binary UUIDs)");
      Check(sess.group_set_digest == SetDigest(material.effective_group_uuids), "and its group-set digest that of the effective groups");
      Check(sess.settings.isolation_profile == kSnapshotStableProfile && !sess.settings.read_only && sess.settings.lock_wait_timeout_ms == 0 && sess.settings.language_uuid.is_nil(),
            "the settings block: snapshot default, read-write, no lock wait limit, language unset");
      Check(sess.policy_snapshot_uuid.is_nil(), "no policy snapshot before plan 32");
      // 2026-11-23 (the 512-byte record): the spec's remaining session fields, from what the server really has. The generations
      // are those of the login material the approval was decided on; a local login has no external-authorization hash; no
      // HELLO was admitted on this channel, so both profile identities are zero.
      Check(sess.policy_generation == material.policy_generation && sess.security_generation == material.security_generation,
            "the session record's policy and security generations are the login material's (" + std::to_string(material.policy_generation) + "/" +
                std::to_string(material.security_generation) + ")");
      Check(sess.security_generation != 0, "the bootstrap material has a real (non-zero) security generation");
      Check(std::all_of(sess.external_authorization_hash.begin(), sess.external_authorization_hash.end(), [](std::uint8_t b) { return b == 0; }),
            "a local login has a zero external-authorization hash");
      Check(sess.parser_profile_uuid.is_nil() && sess.donor_profile_uuid.is_nil(), "no HELLO admitted: parser and donor profile are zero");
      Check(sess.auth_context_uuid.is_nil() || sess.auth_context_uuid == p::Uuid{s1.attempt.registry->auth_contexts_by_uuid.begin()->second.auth_context_uuid},
            "the auth context field is the registry's auth context (or zero when it is not a v7 identity)");
    }
    // Session 2: a different session, a different transaction. Its channel has an ADMITTED HELLO (2026-11-23): the connection
    // record names the listener and parser instance, and the session record carries the HELLO's dialect profile as the parser
    // profile (the HELLO states no donor profile, so that stays zero).
    srv::ServerAdmittedParserChannelIdentity hello;
    hello.parser_instance_uuid = Fresh().bytes;
    hello.dialect_profile_uuid = Fresh().bytes;
    hello.listener_uuid = Fresh().bytes;
    auto s2 = Approve({std::string(kBootstrapName), std::string(kBootstrapPassword)}, &hello);
    {
      const auto f = read();
      const auto& sess2 = std::get<pgs::LedgerSessionRecord>(f.ledger[IndexOf(f, p::Uuid{s2.approval.session_uuid}, pgs::SessionLedgerKind::session)].body);
      const auto& conn2 = std::get<pgs::LedgerConnectionRecord>(f.ledger[IndexOf(f, p::Uuid{s2.approval.connection_record_uuid}, pgs::SessionLedgerKind::connection)].body);
      Check(sess2.parser_profile_uuid == p::Uuid{hello.dialect_profile_uuid} && sess2.donor_profile_uuid.is_nil(),
            "an admitted HELLO: the session record's parser profile is its dialect profile, the donor profile stays zero");
      Check(conn2.parser_instance_uuid == p::Uuid{hello.parser_instance_uuid} && conn2.listener_uuid == p::Uuid{hello.listener_uuid},
            "and the connection record names the HELLO's parser instance and listener");
      Check(sess2.policy_generation == material.policy_generation && sess2.security_generation == material.security_generation,
            "its generations are the login material's too");
    }
    Check(s2.approval.session_uuid != s1.approval.session_uuid && s2.approval.transaction_uuid != s1.approval.transaction_uuid &&
              s2.approval.transaction_order > s1.approval.transaction_order && s2.approval.attachment_uuid != s1.approval.attachment_uuid &&
              s2.approval.connection_record_uuid != s1.approval.connection_record_uuid,
          "a second session gets a different session, transaction, attachment and connection record");
    // Six logins from six threads.
    constexpr int kThreads = 6;
    std::vector<std::future<Attempt>> futures;
    for (int i = 0; i < kThreads; ++i)
      futures.push_back(std::async(std::launch::async, [] { return HandoffRaw({std::string(kBootstrapName), std::string(kBootstrapPassword)}); }));
    std::vector<Attempt> concurrent;
    for (auto& f : futures) {
      concurrent.push_back(f.get());
      Record(concurrent.back());
    }
    std::set<std::array<std::uint8_t, 16>> sessions;
    std::set<std::uint64_t> orders;
    for (const auto& c : concurrent) {
      Check(c.result.accepted && c.result.native_approval.present, "a concurrent login is approved: " + c.wire);
      sessions.insert(c.result.native_approval.session_uuid);
      orders.insert(c.result.native_approval.transaction_order);
    }
    Check(sessions.size() == kThreads && orders.size() == kThreads && *orders.begin() > s2.approval.transaction_order,
          "six concurrent logins: six distinct sessions, six distinct transaction numbers, none reused");
    auto f2 = read();
    Check(f2.ledger.size() == 3 * (2 + kThreads), "the ledger holds three records per approval (" + std::to_string(f2.ledger.size()) + ")");
    for (std::size_t i = 0; i < f2.ledger.size(); i += 3)
      Check(f2.ledger[i].kind() == pgs::SessionLedgerKind::connection && f2.ledger[i + 1].kind() == pgs::SessionLedgerKind::session &&
                f2.ledger[i + 2].kind() == pgs::SessionLedgerKind::attachment,
            "every approval's three records are contiguous, in order (one request on the loop)");
    CheckAuditTrace(f2, s1.approval, served.bootstrap, instance, TS::active, "session 1 again");
    CheckAuditTrace(f2, s2.approval, served.bootstrap, instance, TS::active, "session 2");
    for (const auto& c : concurrent) CheckAuditTrace(f2, c.result.native_approval, served.bootstrap, instance, TS::active, "a concurrent session");
    Check(f2.counters.next_transaction == next0 + 2 + kThreads, "Next advanced by exactly one per approval");
    // The loop's own view agrees.
    {
      std::vector<std::uint64_t> orders_on_loop;
      std::vector<p::Uuid> sessions_on_loop;
      Check(served.loop->RunOnLoopThreadForTest([&] {
              orders_on_loop = served.loop->OpenTransactionOrdersOnLoopThread();
              sessions_on_loop = served.loop->OpenSessionsOnLoopThread();
            }),
            "the loop answers");
      Check(orders_on_loop.size() == 2 + kThreads && sessions_on_loop.size() == 2 + kThreads, "the loop owns one open transaction and one open session per approval");
    }
    // ---- a setup that fails AFTER the session record is durable: the session is ENDED (setup_failed), never left half-visible ----
    // The request reuses session 2's CONNECTION record (a second approval on the same channel: ledger UUIDs are write-once, so the
    // durable connection record is reused, not appended again) and a DUPLICATE attachment UUID (session 2's), which the ledger refuses
    // as a rule (writer_refused, nothing written for that step): the connection step is skipped, the session record is appended, the
    // attachment step is refused, the ended record (setup_failed) is appended, no transaction is begun, no number is taken.
    {
      const auto before_setup = read();
      const auto session2 = p::Uuid{s2.approval.session_uuid};
      const auto session2_open = IndexOf(before_setup, session2, pgs::SessionLedgerKind::session);
      const auto connection2 = IndexOf(before_setup, p::Uuid{s2.approval.connection_record_uuid}, pgs::SessionLedgerKind::connection);
      Check(session2_open < before_setup.ledger.size() && connection2 < before_setup.ledger.size(), "session 2's records are in the file");
      srv::NativeLoopSessionOpenRequest request;
      request.connection_uuid = p::Uuid{s2.approval.connection_record_uuid};
      request.connection = std::get<pgs::LedgerConnectionRecord>(before_setup.ledger[connection2].body);
      request.session_uuid = Fresh();
      request.session = std::get<pgs::LedgerSessionRecord>(before_setup.ledger[session2_open].body);
      request.attachment_uuid = p::Uuid{s2.approval.attachment_uuid};   // already in the ledger: refused
      const auto refused = served.loop->SubmitOpenSession(request).get();
      Check(!refused.ok() && refused.error == srv::NativeLoopTransactionError::writer_refused && refused.failed_step == srv::NativeLoopSessionStep::attachment &&
                refused.detail.find("ledger_duplicate_record") != std::string::npos,
            "the approval is refused at the attachment step by the ledger's rule: " + refused.detail);
      Check(refused.connection_ordinal == before_setup.ledger[connection2].ordinal && refused.session_ordinal == before_setup.ledger.size() && refused.attachment_ordinal == 0 &&
                refused.begun_order == 0 && refused.setup_failed_recorded,
            "the durable connection record was reused, the session record was appended, no attachment, no transaction, and the ended record was appended");
      const auto after_setup = read();
      Check(after_setup.ledger.size() == before_setup.ledger.size() + 2 && std::equal(before_setup.ledger.begin(), before_setup.ledger.end(), after_setup.ledger.begin()),
            "exactly two records were appended (the session, then its end), nothing rewritten");
      const auto& ended = std::get<pgs::LedgerSessionRecord>(after_setup.ledger.back().body);
      Check(after_setup.ledger.back().record_uuid == request.session_uuid && ended.ended() && ended.end_reason == pgs::LedgerEndReason::setup_failed,
            "the last record is that session's end, reason setup_failed");
      Check(after_setup.counters.next_transaction == before_setup.counters.next_transaction, "no transaction number was taken");
      std::vector<p::Uuid> sessions_on_loop;
      Check(served.loop->RunOnLoopThreadForTest([&] { sessions_on_loop = served.loop->OpenSessionsOnLoopThread(); }), "the loop answers");
      Check(std::find(sessions_on_loop.begin(), sessions_on_loop.end(), request.session_uuid) == sessions_on_loop.end(), "the half-made session is not open on the loop");
      f2 = after_setup;   // the later comparisons are against the file as it stands now
    }
    // ---- abrupt disconnect of session 1: its transaction is rolled back and the session-ended record is appended ----
    const auto before_disconnect = read();
    {
      const auto d = Disconnect(s1.attempt, "physical_parser_channel_lost");
      Check(d.accepted, "the disconnect of session 1 is accepted");
      Check(s1.attempt.registry->sessions_by_uuid.empty(), "the session left the registry");
      bool rolled_back_reported = false;
      for (const auto& diag : d.diagnostics)
        if (diag.code == "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK") rolled_back_reported = true;
      Check(rolled_back_reported, "the detach reports the transaction as rolled back");
    }
    auto f3 = read();
    Check(f3.ledger.size() == f2.ledger.size() + 1, "exactly ONE record was appended: the session-ended record");
    Check(std::equal(f2.ledger.begin(), f2.ledger.end(), f3.ledger.begin()), "every earlier record is byte-for-byte the record it was (no overwrite)");
    {
      const auto& end = f3.ledger.back();
      Check(end.kind() == pgs::SessionLedgerKind::session && end.record_uuid == p::Uuid{s1.approval.session_uuid}, "the new record is session 1's");
      const auto& body = std::get<pgs::LedgerSessionRecord>(end.body);
      const auto& open_body = std::get<pgs::LedgerSessionRecord>(f2.ledger[IndexOf(f2, p::Uuid{s1.approval.session_uuid}, pgs::SessionLedgerKind::session)].body);
      Check(body.ended() && body.end_reason == pgs::LedgerEndReason::connection_lost && body.ended_at_utc_ns >= body.opened_at_utc_ns, "ended, reason connection_lost");
      Check(body.principal_uuid == open_body.principal_uuid && body.opened_at_utc_ns == open_body.opened_at_utc_ns && body.role_set_digest == open_body.role_set_digest &&
                body.group_set_digest == open_body.group_set_digest && body.settings == open_body.settings && body.engine_instance_uuid == open_body.engine_instance_uuid &&
                body.external_authorization_hash == open_body.external_authorization_hash && body.policy_generation == open_body.policy_generation &&
                body.security_generation == open_body.security_generation && body.parser_profile_uuid == open_body.parser_profile_uuid &&
                body.donor_profile_uuid == open_body.donor_profile_uuid,
            "the ended record repeats the session's own facts");
    }
    {
      const auto word = f3.words.find(s1.approval.transaction_order);
      Check(word != f3.words.end() && word->second.state == TS::rolled_back, "session 1's transaction is ROLLED BACK in the file (no replacement was begun)");
      const auto header = f3.headers.at(s1.approval.transaction_order);
      Check(header.final_state == TS::rolled_back && header.commit_time_utc_ns == 0 && header.completed_at_utc_ns != 0, "its header says rolled back");
      Check(f3.counters.next_transaction == f2.counters.next_transaction, "no replacement transaction took a number");
      CheckAuditTrace(f3, s2.approval, served.bootstrap, instance, TS::active, "session 2 is untouched");
      std::vector<std::uint64_t> orders_on_loop;
      std::vector<p::Uuid> sessions_on_loop;
      Check(served.loop->RunOnLoopThreadForTest([&] {
              orders_on_loop = served.loop->OpenTransactionOrdersOnLoopThread();
              sessions_on_loop = served.loop->OpenSessionsOnLoopThread();
            }),
            "the loop answers");
      Check(std::find(orders_on_loop.begin(), orders_on_loop.end(), s1.approval.transaction_order) == orders_on_loop.end() && orders_on_loop.size() == 1 + kThreads &&
                std::find(sessions_on_loop.begin(), sessions_on_loop.end(), p::Uuid{s1.approval.session_uuid}) == sessions_on_loop.end(),
            "the loop no longer owns session 1's transaction or session");
    }
    // ---- an orderly client disconnect of session 2: reason closed_by_client ----
    {
      const auto d = Disconnect(s2.attempt, "parser_disconnect_notice");
      Check(d.accepted && s2.attempt.registry->sessions_by_uuid.empty(), "session 2's orderly disconnect is accepted");
      const auto f = read();
      const auto& end = std::get<pgs::LedgerSessionRecord>(f.ledger.back().body);
      Check(f.ledger.back().record_uuid == p::Uuid{s2.approval.session_uuid} && end.ended() && end.end_reason == pgs::LedgerEndReason::closed_by_client, "its ended record: closed_by_client");
    }
    // ---- the graceful close: every session left is ended (server_shutdown) and its transaction rolled back ----
    const auto before_close = read();
    Check(served.loop->Close().ok, "the loop closes");
    const auto after_close = read();
    Check(after_close.ledger.size() == before_close.ledger.size() + kThreads, "the close appended exactly one ended record per remaining session");
    Check(std::equal(before_close.ledger.begin(), before_close.ledger.end(), after_close.ledger.begin()), "and rewrote nothing before it");
    for (std::size_t i = before_close.ledger.size(); i < after_close.ledger.size(); ++i) {
      const auto& body = std::get<pgs::LedgerSessionRecord>(after_close.ledger[i].body);
      Check(after_close.ledger[i].kind() == pgs::SessionLedgerKind::session && body.ended() && body.end_reason == pgs::LedgerEndReason::server_shutdown, "a close-time record: ended, server_shutdown");
    }
    for (const auto& c : concurrent) {
      const auto order = c.result.native_approval.transaction_order;
      Check(after_close.words.at(order).state == TS::rolled_back, "after the close the transaction of order " + std::to_string(order) + " is rolled back (OLD: left in progress, dead at the next open)");
    }
    for (const auto& [order, word] : after_close.words)
      Check(word.state != TS::active && word.state != TS::dead, "no slot of the file is left in progress or dead after a graceful close");
    // ---- shutdown ordering: the loop is closed BEFORE the registry's disconnect arrives ----
    {
      auto& late = concurrent.front();
      const auto d = Disconnect(late, "server_shutdown");
      Check(d.accepted && late.registry->sessions_by_uuid.empty(), "a disconnect that arrives after the loop closed is settled (the close already rolled back and ended): the session is released");
    }
    for (const auto& entry : fs::directory_iterator(served.directory))
      Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
    Check(Snapshot(served.directory).size() == dir_before.size(), "the directory holds the database file only");
    std::printf("INFO approve ledger_records=%zu writes_per_approval=%u syncs_per_approval=%u\n", after_close.ledger.size(), s1.approval.writes, s1.approval.syncs);
    NoNameOrPasswordInOutput(all_wires);
    NoNameOrPasswordInOutput(all_internal);
  } else if (state == "trace") {
    // The strace half's child (native_session_approval_strace.py): regions marked on stderr (the script splits the data-file
    // writes by region), the loop's own counts printed per region. A failed login (writes nothing), two approvals, both sessions
    // ended through the loop (so every write of the run is one the loop reports), then the close, which finds nothing to write.
    const auto mark = [](const char* name) { ::dprintf(2, "MARK %s\n", name); };
    const auto measure = [](const char* op, std::uint32_t writes, std::uint32_t syncs) {
      std::printf("MEASURE op=%s writes=%u syncs=%u\n", op, writes, syncs);
      std::fflush(stdout);
    };
    mark("failed");
    const auto failed = Handoff({std::string(kBootstrapName), std::string(kWrongPassword)});
    Check(!failed.result.accepted, "a wrong password is refused");
    measure("failed", 0, 0);
    srv::NativeLoginThrottle().Clear();
    mark("approve");
    auto a = Approve();
    auto b = Approve();
    measure("approve", a.approval.writes + b.approval.writes, a.approval.syncs + b.approval.syncs);
    mark("end_sessions");
    std::uint32_t writes = 0, syncs = 0;
    for (const auto* approval : {&a.approval, &b.approval}) {
      srv::NativeLoopSessionEndRequest end;
      end.session_uuid = p::Uuid{approval->session_uuid};
      end.reason = pgs::LedgerEndReason::closed_by_client;
      const auto ended = served.loop->SubmitEndSession(end).get();
      Check(ended.ok() && ended.transactions_rolled_back == 1, "the loop ends the session and rolls its transaction back");
      writes += ended.writes;
      syncs += ended.syncs;
    }
    measure("end_sessions", writes, syncs);
    mark("end");
    NoNameOrPasswordInOutput(all_wires);
  } else if (state == "reopen") {
    // The next open after a graceful close: the verification accepted ENDED sessions (their rolled-back headers still cite them).
    const auto file = read();
    const auto facts = [&] {
      std::optional<srv::NativeLoopServingFacts> out;
      Check(served.loop->RunOnLoopThreadForTest([&] { out = served.loop->FactsOnLoopThread(); }), "the loop answers");
      Check(out.has_value(), "the loop has its facts");
      return *out;
    }();
    Check(facts.ledger_records == file.ledger.size() && facts.ledger_orphaned_sessions == 0 && !facts.ledger_torn_tail,
          "reopen: the ledger loaded (" + std::to_string(facts.ledger_records) + " records), no orphaned session");
    Check(facts.restart.marked_dead == 0 && facts.serving_state == srv::NativeServingState::serving, "reopen: Restart marked nothing dead (nothing was left in progress)");
    auto again = Approve();
    Check(again.approval.transaction_order == file.counters.next_transaction, "reopen: the next approval takes the next number; none is reused");
    std::printf("REOPEN orphans=%llu marked_dead=%llu ledger=%llu\n", static_cast<unsigned long long>(facts.ledger_orphaned_sessions),
                static_cast<unsigned long long>(facts.restart.marked_dead), static_cast<unsigned long long>(facts.ledger_records));
    Check(served.loop->Close().ok, "the loop closes");
  } else if (state == "crash") {
    const auto a = Approve();
    const auto b = Approve();
    std::printf("CRASH orders=%llu,%llu sessions=%s,%s\n", static_cast<unsigned long long>(a.approval.transaction_order),
                static_cast<unsigned long long>(b.approval.transaction_order), Hex(p::Uuid{a.approval.session_uuid}).c_str(), Hex(p::Uuid{b.approval.session_uuid}).c_str());
    std::printf("LOGIN_OK checks=%d state=%s\n", checks, state.c_str());
    std::fflush(stdout);
    std::_Exit(0);   // forced: no close, no disconnect, no ended record, nothing flushed that the engine holds
  } else if (state == "reopen_crash") {
    const auto file = read();
    Check(file.ledger.size() >= 6, "the crashed file's ledger is still there");
    std::size_t open_sessions = 0;
    for (const auto& r : file.ledger)
      if (r.kind() == pgs::SessionLedgerKind::session && !std::get<pgs::LedgerSessionRecord>(r.body).ended()) ++open_sessions;
    std::optional<srv::NativeLoopServingFacts> facts;
    Check(served.loop->RunOnLoopThreadForTest([&] { facts = served.loop->FactsOnLoopThread(); }) && facts.has_value(), "the loop has its facts");
    Check(facts->ledger_orphaned_sessions == open_sessions && open_sessions >= 2,
          "after the kill the facts report the un-ended sessions as orphaned: " + std::to_string(facts->ledger_orphaned_sessions) + " (the ledger shows " + std::to_string(open_sessions) + ")");
    Check(facts->restart.marked_dead >= 2 && facts->serving_state == srv::NativeServingState::serving, "Restart marked the killed transactions dead and the database serves");
    // The killed sessions' transactions are dead in the file (Restart wrote), their sessions un-ended.
    std::size_t dead = 0;
    for (const auto& [order, word] : file.words)
      if (word.state == TS::dead) ++dead;
    Check(dead >= 2, "the killed slots are dead in the file");
    auto fresh = Approve();
    Check(fresh.approval.transaction_order >= file.counters.next_transaction, "a new login after the crash takes a new number");
    std::printf("REOPEN_CRASH orphans=%llu marked_dead=%llu\n", static_cast<unsigned long long>(facts->ledger_orphaned_sessions), static_cast<unsigned long long>(facts->restart.marked_dead));
    Check(served.loop->Close().ok, "the loop closes (the new session is ended; the orphans stay un-ended: reported, never repaired)");
  } else if (state == "failed_later") {
    auto ok = Approve();
    Check(served.facts->CurrentServingState() == api::NativeServedServingState::serving, "serving before the failure");
    // The writer fails on the NEXT write the loop makes: the second login is refused, the database becomes `failed`.
    srv::SetNativeTransactionSinkWrapperForTest([](scratchbird::storage::disk::CarefulWriteSink& inner) -> std::unique_ptr<scratchbird::storage::disk::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
    srv::NativeLoginThrottle().Clear();
    const auto broken = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(!broken.result.accepted && !broken.result.native_approval.present && !broken.result.diagnostics.empty() &&
              broken.result.diagnostics.front().code == "ENGINE.NATIVE.TRANSACTION_WRITER_FAILED",
          "the approval whose write failed is refused with the writer-failed code: " + broken.wire);
    Check(broken.registry->sessions_by_uuid.empty() && broken.registry->auth_contexts_by_uuid.empty(), "and leaves no session and no auth context");
    // Part 4: the facts are immutable and were built `serving`, but the live state is the cell's.
    Check(served.facts->serving_state == api::NativeServedServingState::serving && served.facts->CurrentServingState() == api::NativeServedServingState::failed,
          "the facts' state at open is serving, the LIVE state is failed (the atomic cell beside the immutable facts)");
    Check(served.facts->CurrentServingDiagnosticCode() == "DATABASE.TRANSACTION_WRITE_FAILED", "with the writer-failure token as its reason");
    const auto login = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(!login.result.accepted && !login.result.diagnostics.empty() && login.result.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_FAILED",
          "a login on the failed database is refused with the failed code, not the generic failure: " + login.wire);
    // A session approved BEFORE the failure is not bound by an attach: the native branch asks the live state.
    {
      const auto context = srv::DecodeAuthContextUuidForTest(ok.attempt.result.payload);
      Check(context.has_value(), "the first session's auth context decodes");
      const auto frame = MakeFrame(sbps::MessageType::kAttachDatabase, AttachPayload(ok.attempt.connection, *context), ok.attempt.connection);
      const auto attached = srv::HandleAttachDatabase(ok.attempt.registry.get(), served.started.state, frame);
      bool failed_code = false;
      for (const auto& d : attached.diagnostics)
        if (d.code == "ENGINE.NATIVE.TRANSACTION_DATABASE_FAILED") failed_code = true;
      Check(!attached.accepted && failed_code, "the attach of a session approved before the failure is refused with the failed code");
    }
    const auto direct = Engine({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    Check(!direct.ok && direct.diagnostics.front().code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_FAILED", "EngineAuthenticate refuses with the same failed code");
    // The loop cannot end the session on a failed database: the server keeps it for recovery (outcome unknown), it does not lie.
    const auto d = Disconnect(ok.attempt, "parser_disconnect_notice");
    Check(!ok.attempt.registry->sessions_by_uuid.empty(), "a disconnect on a failed database cannot end the session: it is retained for recovery (outcome unknown)");
    (void)d;
    srv::SetNativeTransactionSinkWrapperForTest(nullptr);
    std::printf("FAILED_LATER order=%llu\n", static_cast<unsigned long long>(ok.approval.transaction_order));
  } else {
    // ---- role / group privilege cases ----
    const bool refused_case = state == "role_noconnect" || state == "role_revoked" || state == "role_deny";
    const auto material = BootstrapMaterial();
    const auto role_0100 = FromHex("018f7a10128070008000000000000100"), group_0200 = FromHex("018f7a10128070008000000000000200"),
               group_0201 = FromHex("018f7a10128070008000000000000201");
    Check(material.sysarch_role_uuid.is_nil(), state + ": the principal is no longer a member of SYSARCH (the forge moved its membership)");
    if (state == "role_connect" || state == "role_revoked" || state == "role_deny")
      Check(material.effective_role_uuids == std::vector<p::Uuid>{role_0100} && material.effective_group_uuids.empty(), state + ": effective roles = {...0100}");
    if (state == "group_connect")
      Check(material.effective_role_uuids.empty() && material.effective_group_uuids == std::vector<p::Uuid>{group_0200}, state + ": effective groups = {...0200}, no role");
    if (state == "transitive")
      Check(material.effective_role_uuids == std::vector<p::Uuid>{role_0100} && material.effective_group_uuids == std::vector<p::Uuid>{group_0201},
            state + ": the group's own membership is followed: effective roles {...0100}, groups {...0201}");
    // The privilege rows of the roles / groups are in the material (the adapter's input), not only the principal's own.
    std::size_t privilege_edges = 0;
    for (const auto& edge : material.edges)
      if (edge.grant_class == cat::CatalogSecurityGrantClass::privilege && edge.subject_uuid != served.bootstrap) ++privilege_edges;
    if (state == "role_revoked") Check(privilege_edges == 0, state + ": the REVOKED grant (a dropped row) is not in the material");
    else if (state != "role_noconnect") Check(privilege_edges >= 1, state + ": the material carries privilege rows owned by the role / group (" + std::to_string(privilege_edges) + ")");
    const auto next_before_login = NextTransaction();
    const auto bytes_before = Snapshot(served.directory);
    srv::NativeLoginThrottle().Clear();
    const auto a = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
    if (refused_case) {
      Check(!a.result.accepted && !a.result.native_approval.present && !a.result.diagnostics.empty() && a.result.diagnostics.front().code == "SECURITY.AUTHORIZATION.DENIED",
            state + ": authenticated but NOT authorized to CONNECT: refused with SECURITY.AUTHORIZATION.DENIED: " + a.wire);
      Check(a.registry->auth_contexts_by_uuid.empty() && a.registry->sessions_by_uuid.empty(), state + ": no auth context, no session");
      Check(Snapshot(served.directory) == bytes_before && NextTransaction() == next_before_login, state + ": nothing was written (bytes and Next unchanged)");
    } else {
      Check(a.result.accepted && a.result.native_approval.present, state + ": CONNECT granted through the role / group is honoured: " + a.wire);
      const auto file = read();
      CheckAuditTrace(file, a.result.native_approval, served.bootstrap, instance, TS::active, state);
      const auto& sess = std::get<pgs::LedgerSessionRecord>(file.ledger[1].body);
      Check(sess.role_set_digest == SetDigest(material.effective_role_uuids) && sess.group_set_digest == SetDigest(material.effective_group_uuids),
            state + ": the ledger's role-set and group-set digests are those of the effective sets");
      Check(sess.policy_generation == material.policy_generation && sess.security_generation == material.security_generation,
            state + ": the ledger's policy and security generations are the login material's (2026-11-23)");
      const auto& rec = a.registry->sessions_by_uuid.begin()->second;
      Check(rec.effective_role_uuids.size() == material.effective_role_uuids.size() && rec.effective_group_uuids.size() == material.effective_group_uuids.size(),
            state + ": the session's effective sets are the closure");
    }
  }
  return 0;
}

// The role-set digest, defined (see NativeUuidSetDigest): known answers, order independence, duplicates and nil removed.
void CheckSetDigestDefinition() {
  const auto id = [](std::uint8_t n) {
    p::Uuid u;
    for (std::size_t i = 0; i < 16; ++i) u.bytes[i] = static_cast<p::byte>(n + i);
    return u;
  };
  const auto sha = [](const std::vector<p::byte>& bytes) {
    const auto d = scratchbird::core::hash::ComputeSha256Digest(bytes);
    Check(d.ok(), "sha256");
    std::array<p::byte, 32> out{};
    std::copy(d.digest.begin(), d.digest.end(), out.begin());
    return out;
  };
  const auto a = id(0x10), b = id(0x40), c = id(0x70);
  std::vector<p::byte> sorted_concat;
  for (const auto* u : {&a, &b, &c}) sorted_concat.insert(sorted_concat.end(), u->bytes.begin(), u->bytes.end());
  const auto expected = sha(sorted_concat);
  Check(SetDigest({a, b, c}) == expected, "digest = SHA-256 over the sorted 16-byte UUIDs, concatenated, no prefix or count");
  Check(SetDigest({c, a, b}) == expected && SetDigest({b, c, a}) == expected, "the order the set is given in does not matter");
  Check(SetDigest({c, a, a, b, b, p::Uuid{}}) == expected, "duplicates and nil UUIDs are removed");
  Check(SetDigest({a, b}) != expected && SetDigest({a, b, c, id(0x90)}) != expected, "a different set digests differently");
  Check(SetDigest({}) == sha({}) && SetDigest({p::Uuid{}}) == sha({}), "the empty set is SHA-256 of the empty input (non-zero: told apart from the ledger's 'none stated')");
  const auto empty = SetDigest({});
  Check(HexBytes(std::vector<std::uint8_t>(empty.begin(), empty.end())) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "and that is the well-known SHA-256 of nothing");
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
  if (state == "approve" || state == "reopen" || state == "crash" || state == "reopen_crash" || state == "failed_later" ||
      state == "role_connect" || state == "role_noconnect" || state == "group_connect" || state == "transitive" ||
      state == "role_revoked" || state == "role_deny" || state == "trace") {
    if (const int rc = S4eState(state, path); rc != 0) return rc;
    if (state != "approve" && state != "reopen" && state != "reopen_crash") {
      (void)served.loop->Close();   // idempotent; the S4e states that close themselves already did
    }
    served.facts.reset();
    served.started = {};
    std::cout << "LOGIN_OK checks=" << checks << " state=" << state << std::endl;
    return 0;
  }
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
    // S4e (owner 2026-11-21: the user's session and first transaction begin only when authentication is APPROVED). OLD (S4c):
    // an accepted handoff left one auth context and NO session record. NOW an approved login on a SERVING database also holds
    // the session and its first transaction (see native_session_approval below / the approve state for the file-level
    // assertions); on a RESTRICTED database the login is allowed (Q309) but the session is refused with the Q299 code
    // before anything is written (Q299 / Q309; the loop refuses the approval).
    if (restricted) {
      Check(!good.result.accepted, "restricted: the approval is refused although the login succeeded: " + good.wire);
      Check(!good.result.diagnostics.empty() && good.result.diagnostics.front().code == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY",
            "restricted: the refusal is the Q299 code, got " + (good.result.diagnostics.empty() ? std::string("none") : good.result.diagnostics.front().code));
      Check(good.result.native_login_audit.accepted && good.result.native_login_audit.internal_code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_ACCEPTED",
            "restricted: the login itself was accepted (the audit says so)");
      Check(good.registry->auth_contexts_by_uuid.empty() && good.registry->sessions_by_uuid.empty() && !good.result.native_approval.present,
            "restricted: no auth context, no session, no approval");
      Check(Snapshot(served.directory) == before && NextTransaction() == next_before,
            "restricted: nothing was written (every file's bytes and the TIP's Next are unchanged)");
    } else {
    Check(good.result.accepted, "bootstrap name + right password: accepted by the handoff: " + good.wire);
    Check(good.result.native_login_audit.accepted && good.result.native_login_audit.internal_code == "SECURITY.AUTHENTICATION.NATIVE_LOGIN_ACCEPTED",
          "the audit records the acceptance");
    Check(good.registry->auth_contexts_by_uuid.size() == 1 && good.registry->sessions_by_uuid.size() == 1 && good.result.native_approval.present &&
              good.result.native_approval.transaction_order != 0,
          "one auth context AND one session record with its first transaction (S4e; was: no session record)");
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
    // 7. Attach and the embedded attach on a native database. S4e (owner 2026-11-21): the session and its first transaction
    //    begin at APPROVAL, so the attach only BINDS the approved session. OLD (S4c): the attach was refused at CONNECT ("no
    //    role-owned privilege rows in the material"), and, with CONNECT amended into the adapter state in memory, with
    //    native_attach_requires_s4e. NOW (Part 1 made the material carry role-owned privileges and the SYSARCH bundle): the
    //    approved bootstrap principal connects, the attach is accepted with NO second transaction, and a replay is refused.
    //    A restricted database has no approved session to attach (Q299 above); only the embedded attach is checked there.
    if (!restricted) {
      const auto auth = Handoff({std::string(kBootstrapName), std::string(kBootstrapPassword)});
      Check(auth.result.accepted && auth.result.native_approval.present, "the login for the attach case is approved");
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
      const auto approved_order = auth.result.native_approval.transaction_order;
      const auto next_at_attach = NextTransaction();
      const auto attach_frame = MakeFrame(sbps::MessageType::kAttachDatabase, AttachPayload(auth.connection, *context), auth.connection);
      const auto attached = srv::HandleAttachDatabase(auth.registry.get(), served.started.state, attach_frame);
      Check(attached.accepted, "the native attach of the approved session is accepted: " + attach_codes(attached));
      Check(auth.registry->sessions_by_uuid.size() == 1 && auth.registry->sessions_by_uuid.begin()->second.session_binding_present &&
                auth.registry->sessions_by_uuid.begin()->second.local_transaction_id == approved_order,
            "the registry still holds the ONE approved session, now bound, with the transaction begun at approval");
      Check(NextTransaction() == next_at_attach, "the attach began no transaction and wrote nothing (the TIP's Next is unchanged)");
      const auto replay = srv::HandleAttachDatabase(auth.registry.get(), served.started.state, attach_frame);
      Check(!replay.accepted && attach_codes(replay).find("auth_context_replay_refused") != std::string::npos,
            "a second attach with the same context is refused as a replay: " + attach_codes(replay));
    }
    {
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

  // ---- nothing was written by a failed login, whatever happened ---------------------------------------------------------
  // S4e: an APPROVED login writes (three ledger records and the first transaction) and takes exactly one transaction number.
  // OLD (S4c): the file's bytes and the TIP's Next were unchanged after every attempt. NOW that holds for every state in which
  // no login was approved (restricted, failed, disabled, nocredential); where logins were approved (serving, throttle) the
  // directory still holds only the database file, and Next advanced by EXACTLY the number of approvals: no failed login, no
  // throttled attempt and no refusal took a number. (Each failure case also checks Next around itself, in CredentialCase.)
  if (g_approvals == 0) Check(Snapshot(served.directory) == before, "the directory and every file's bytes are unchanged after every attempt");
  else Check(Snapshot(served.directory).size() == before.size(), "the directory holds the same files (the database file only)");
  for (const auto& entry : fs::directory_iterator(served.directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  if (state != "failed")
    Check(NextTransaction() == next_before + g_approvals,
          "the TIP's Next advanced by exactly the number of approved logins (" + std::to_string(g_approvals) + "): no failed login took a transaction number");
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

bool IsRoleState(const std::string& state) {
  return state == "role_connect" || state == "role_noconnect" || state == "group_connect" || state == "transitive" ||
         state == "role_revoked" || state == "role_deny";
}

// `then`: S4e follow-up children run on the SAME file after the first one ended (a reopen after a close or after a crash);
// `between(state_just_run, child_output)` lets the driver read the file between them.
void RunState(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::string& state,
              const std::vector<std::string>& then = {},
              const std::function<void(const std::string&, const std::string&, const fs::path&)>& between = {}) {
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
  } else if (state == "disabled" || state == "nocredential" || IsRoleState(state)) {
    const auto forged = Capture("'" + material + "' forge '" + path.string() + "' " + state + " " + bootstrap, &status);
    Check(status == 0 && forged.find("FORGED kind=" + state) != std::string::npos, tag + state + ": the engine's writers changed the rows: " + forged.substr(0, 1500));
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
  std::vector<std::string> sequence{state};
  sequence.insert(sequence.end(), then.begin(), then.end());
  for (const auto& child : sequence) {
    const auto start = std::chrono::steady_clock::now();
    std::string command = "'" + self + "' serve '" + path.string() + "' " + child + " " + database + " " + bootstrap + " " + service;
    if (!derived.empty()) {
      ::setenv("SB_LOGIN_TEST_DERIVED", derived.c_str(), 1);
    } else {
      ::unsetenv("SB_LOGIN_TEST_DERIVED");
    }
    const auto served_out = Capture(command, &status);
    ::unsetenv("SB_LOGIN_TEST_DERIVED");
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && served_out.find("LOGIN_OK") != std::string::npos, tag + child + ": serve child: " + served_out.substr(0, 3000));
    for (const auto& output : {created, served_out})
      Check(output.find(kBootstrapPassword) == std::string::npos && output.find(kWrongPassword) == std::string::npos, tag + child + ": no test password in any child output");
    if (const auto info = served_out.find("INFO "); info != std::string::npos)
      std::printf("%s%s: %s\n", tag.c_str(), child.c_str(), served_out.substr(info, served_out.find('\n', info) - info).c_str());
    if (const auto timing = served_out.find("TIMING"); timing != std::string::npos)
      std::printf("%s%s: %.1f s; %s\n", tag.c_str(), child.c_str(), seconds, served_out.substr(timing, served_out.find('\n', timing) - timing).c_str());
    else
      std::printf("%s%s: %.1f s\n", tag.c_str(), child.c_str(), seconds);
    fflush(stdout);
    if (between) between(child, served_out, path);
  }
}

// The S4e driver for one page size: the chains that need the file between children.
void RunS4e(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  namespace pgx = scratchbird::storage::page;
  // approve -> reopen: after the graceful close the file opens (ended sessions verify), nothing orphaned.
  RunState(self, root, page_size, "approve", {"reopen"}, [&](const std::string& child, const std::string& out, const fs::path& path) {
    if (child != "reopen") return;
    Check(out.find("REOPEN orphans=0 marked_dead=0") != std::string::npos, tag + "reopen after a graceful close: no orphan, nothing marked dead: " + out.substr(0, 500));
    const auto file = off::ReadFile(path, page_size);
    std::size_t open = 0;
    for (const auto& [uuid, word] : file.words) {
      (void)uuid;
      if (word.state == pgx::TipStateCode::active || word.state == pgx::TipStateCode::dead) ++open;
    }
    Check(open == 0, tag + "the file after approve + reopen + close has no slot in progress or dead");
  });
  // crash -> reopen_crash: the killed sessions stay un-ended (orphaned), their slots are dead at the next open.
  RunState(self, root, page_size, "crash", {"reopen_crash"}, [&](const std::string& child, const std::string& out, const fs::path& path) {
    const auto file = off::ReadFile(path, page_size);
    if (child == "crash") {
      const auto at = out.find("CRASH orders=");
      Check(at != std::string::npos, tag + "the crash child reported its orders");
      const auto line = out.substr(at, out.find('\n', at) - at);
      const auto first = std::stoull(line.substr(std::string("CRASH orders=").size()));
      const auto second = std::stoull(line.substr(line.find(',') + 1));
      Check(file.ledger.size() == 6, tag + "after the kill the ledger holds the two approvals' six records, written before the kill");
      for (const auto order : {first, second}) {
        Check(file.words.at(order).state == pgx::TipStateCode::active, tag + "after the kill the slot of order " + std::to_string(order) + " is still ACTIVE in the file");
        Check(file.headers.at(order).session_uuid != p::Uuid{}, tag + "its header cites the session");
      }
      for (const auto& r : file.ledger)
        if (r.kind() == pgx::SessionLedgerKind::session) Check(!std::get<pgx::LedgerSessionRecord>(r.body).ended(), tag + "the killed sessions' records are un-ended");
    } else {
      std::size_t orphans = 0, dead = 0;
      for (const auto& r : file.ledger)
        if (r.kind() == pgx::SessionLedgerKind::session) {
          const auto& body = std::get<pgx::LedgerSessionRecord>(r.body);
          if (!body.ended()) ++orphans;
        }
      for (const auto& [order, word] : file.words)
        if (word.state == pgx::TipStateCode::dead) ++dead;
      Check(dead >= 2, tag + "after the reopen the killed slots are dead in the file");
      // the two killed sessions never got an end record (3 new records for the post-crash session: 6 + 3 + its end)
      Check(file.ledger.size() == 6 + 3 + 1 && orphans >= 2, tag + "the orphans stay un-ended: reported, never repaired (ledger " + std::to_string(file.ledger.size()) + ")");
    }
  });
  RunState(self, root, page_size, "failed_later");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {   // serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 4 && std::string(argv[1]) == "pages") {   // pages <path> <page_size>: the chains' page numbers (for the strace half)
    const auto file = off::ReadFile(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
    const auto list = [](const std::vector<p::u64>& pages) {
      std::string out;
      for (std::size_t i = 0; i < pages.size(); ++i) out += (i ? "," : "") + std::to_string(pages[i]);
      return out;
    };
    std::printf("PAGES state=%s header=%s ledger=%s next=%llu\n", list(file.state_pages).c_str(), list(file.header_pages).c_str(),
                list(file.ledger_pages).c_str(), static_cast<unsigned long long>(file.counters.next_transaction));
    return 0;
  }
  if (argc == 3 && (std::string(argv[1]) == "s4e" || std::string(argv[1]) == "roles")) {
    // S4e: `s4e <size>` the approval, reopen, crash and failed-later chains; `roles <size>` the role / group privilege cases.
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    const auto size = static_cast<std::uint32_t>(std::stoul(argv[2]));
    if (std::string(argv[1]) == "s4e") RunS4e(self, artifacts.path(), size);
    else
      for (const char* state : {"role_connect", "group_connect", "transitive", "role_noconnect", "role_revoked", "role_deny"})
        RunState(self, artifacts.path(), size, state);
    artifacts.Cleanup();
    std::printf("native_login %s: %d driver checks passed\n", argv[1], checks);
    return 0;
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
