// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_MANAGER_PROTOCOL_TESTS

#include "manager_protocol.hpp"
#include "manager_lifecycle.hpp"
#include "manager_runtime.hpp"
#include "manager_runtime_snapshot.hpp"
#include "manager_support_bundle.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <string>
#include <vector>

namespace proto = scratchbird::manager::protocol;
namespace node = scratchbird::manager::node;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

bool HasDiagnostic(const std::vector<proto::Diagnostic>& diagnostics, const std::string& code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& token) {
  return text.find(token) != std::string::npos;
}

std::size_t CountLifecycleStateTemps(const std::filesystem::path& dir) {
  std::size_t count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    const auto name = entry.path().filename().string();
    if (name.rfind("sbmn_manager.lifecycle.state.tmp.", 0) == 0) ++count;
  }
  return count;
}

#ifndef _WIN32
bool IsPrivateFile(const std::filesystem::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  return (st.st_mode & 0777) == 0600;
}
#endif

void TestSbdbFrameRoundTrip() {
  proto::SbdbFrame frame;
  frame.type = 0x65;
  frame.flags = 0x01;
  frame.payload = {0x10, 0x20, 0x30};
  const auto encoded = proto::EncodeSbdbFrame(frame);
  Check(encoded.size() == 15, "SBDB frame size must be 12-byte header plus payload");
  Check(encoded[0] == 'S' && encoded[1] == 'B' && encoded[2] == 'D' && encoded[3] == 'B',
        "SBDB frame magic must encode as SBDB");
  Check(encoded[4] == 0x01 && encoded[5] == 0x01, "SBDB frame version must be 0x0101 little-endian");
  Check(encoded[6] == 0x65, "SBDB frame type byte must be preserved");
  Check(encoded[7] == 0x01, "SBDB frame flags byte must be preserved");
  Check(encoded[8] == 0x03 && encoded[9] == 0 && encoded[10] == 0 && encoded[11] == 0,
        "SBDB frame payload length must be little-endian u32");

  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeSbdbFrame(encoded, &diagnostics);
  Check(decoded.has_value(), "SBDB frame must decode");
  Check(diagnostics.empty(), "valid SBDB frame must not produce diagnostics");
  if (decoded) {
    Check(decoded->type == frame.type, "decoded SBDB type must match");
    Check(decoded->flags == frame.flags, "decoded SBDB flags must match");
    Check(decoded->payload == frame.payload, "decoded SBDB payload must match");
  }
}

void TestSbdbFrameRejectsBadInputs() {
  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeSbdbFrame({0x53, 0x42}, &diagnostics);
  Check(!decoded.has_value(), "truncated SBDB frame must fail");
  Check(HasDiagnostic(diagnostics, "SBDB.FRAME_TRUNCATED"), "truncated SBDB frame diagnostic required");

  diagnostics.clear();
  auto encoded = proto::EncodeSbdbFrame(proto::SbdbFrame{0x65, 0, {}});
  encoded[0] = 0;
  decoded = proto::DecodeSbdbFrame(encoded, &diagnostics);
  Check(!decoded.has_value(), "bad SBDB magic must fail");
  Check(HasDiagnostic(diagnostics, "SBDB.MAGIC_INVALID"), "bad SBDB magic diagnostic required");

  diagnostics.clear();
  encoded = proto::EncodeSbdbFrame(proto::SbdbFrame{0x65, 0, {1, 2, 3}});
  encoded.push_back(0xff);
  decoded = proto::DecodeSbdbFrame(encoded, &diagnostics);
  Check(!decoded.has_value(), "SBDB trailing bytes must fail");
  Check(HasDiagnostic(diagnostics, "SBDB.FRAME_LENGTH_INVALID"), "SBDB length mismatch diagnostic required");
}

void TestControlPlaneFrameRoundTrip() {
  proto::ControlPlaneMessage message;
  message.message_type = 0x0060;
  message.request_id = 77;
  const std::string command = "LPREFACE_VALIDATE abcd";
  message.payload.assign(command.begin(), command.end());
  const auto encoded = proto::EncodeControlPlaneMessage(message);
  Check(encoded.size() == 28 + command.size(), "control-plane frame size must be 28-byte header plus payload");
  Check(encoded[0] == 'S' && encoded[1] == 'B' && encoded[2] == 'C' && encoded[3] == 'T',
        "control-plane frame magic must encode as SBCT");

  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeControlPlaneMessage(encoded, &diagnostics);
  Check(decoded.has_value(), "control-plane frame must decode");
  Check(diagnostics.empty(), "valid control-plane frame must not produce diagnostics");
  if (decoded) {
    Check(decoded->message_type == 0x0060, "control-plane message type must round-trip");
    Check(decoded->request_id == 77, "control-plane request id must round-trip");
    Check(decoded->payload == message.payload, "control-plane payload must round-trip");
  }

  auto bad = encoded;
  bad[0] = 0;
  diagnostics.clear();
  decoded = proto::DecodeControlPlaneMessage(bad, &diagnostics);
  Check(!decoded.has_value(), "bad control-plane magic must fail");
  Check(HasDiagnostic(diagnostics, "CONTROL.MAGIC_INVALID"), "bad control-plane magic diagnostic required");
}

void TestMessageVectorSetV1RoundTrip() {
  proto::MessageVectorSet set;
  set.request_uuid = proto::MakePseudoUuidV7();
  set.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.CONFIG_FIELD_INVALID",
                                                  "Manager configuration field is invalid.",
                                                  {{"key", "manager.proxy.port"}},
                                                  "error"));
  set.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.RELOAD_NOTICE",
                                                  "Manager reload completed.",
                                                  {{"state", "ready"}},
                                                  "info"));

  proto::Bytes encoded;
  auto result = proto::EncodeMessageVectorSetV1(set, &encoded, 7, 4096);
  Check(result.ok, "MessageVectorSetV1 must encode");
  Check(encoded.size() >= 64 + 112, "MessageVectorSetV1 must contain header and record bytes");
  Check(encoded[0] == 'S' && encoded[1] == 'B' && encoded[2] == 'M' && encoded[3] == 'V',
        "MessageVectorSetV1 magic must encode as SBMV");

  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeMessageVectorSetV1(encoded, &diagnostics);
  Check(decoded.has_value(), "MessageVectorSetV1 must decode");
  Check(diagnostics.empty(), "valid MessageVectorSetV1 must not produce diagnostics");
  if (decoded) {
    Check(decoded->diagnostics.size() == 2, "MessageVectorSetV1 vector count must round-trip");
    Check(decoded->diagnostics[0].code == "MANAGER.CONFIG_FIELD_INVALID", "MessageVectorSetV1 diagnostic code must round-trip");
    Check(decoded->diagnostics[0].fields.size() == 1, "MessageVectorSetV1 fields must round-trip");
    Check(decoded->diagnostics[0].fields[0].key == "key", "MessageVectorSetV1 field key must round-trip");
    Check(decoded->diagnostics[0].fields[0].value == "manager.proxy.port", "MessageVectorSetV1 field value must round-trip");
    Check(decoded->diagnostics[1].severity == "info", "MessageVectorSetV1 severity must round-trip");
  }

  encoded[20] ^= 0xff;
  diagnostics.clear();
  decoded = proto::DecodeMessageVectorSetV1(encoded, &diagnostics);
  Check(!decoded.has_value(), "MessageVectorSetV1 bad records CRC must fail");
  Check(HasDiagnostic(diagnostics, "MESSAGE_VECTOR.MALFORMED"), "MessageVectorSetV1 malformed diagnostic required");
}

void TestHmacKnownAnswer() {
  proto::Bytes key(20, 0x0b);
  const std::string body_text = "Hi There";
  proto::Bytes body(body_text.begin(), body_text.end());
  const auto digest = proto::HmacSha256(key, body);
  Check(proto::Hex(digest.data(), digest.size()) ==
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        "HMAC-SHA256 RFC 4231 known answer must match");
}

void TestDbbtRoundTrip() {
  proto::DbbtToken token;
  for (std::size_t i = 0; i < token.db_uuid.size(); ++i) token.db_uuid[i] = static_cast<std::uint8_t>(i);
  for (std::size_t i = 0; i < token.manager_session_id.size(); ++i) {
    token.manager_session_id[i] = static_cast<std::uint8_t>(0xa0 + i);
  }
  token.listener_id = 42;
  token.issued_at_ms = 100000;
  token.expires_at_ms = 130000;
  token.client_nonce = {1, 2, 3, 4};
  token.server_nonce = {5, 6, 7, 8};
  const proto::Bytes key = {0x0b, 0x0c, 0x0d, 0x0e};
  const auto encoded = proto::EncodeDbbt(token, key);

  proto::DbbtToken decoded;
  auto result = proto::ValidateDbbt(encoded, key, proto::DbbtValidationOptions{42, 101000, 2000}, &decoded);
  Check(result.ok, "DBBT must validate with matching key/listener/time");
  Check(result.diagnostics.empty(), "valid DBBT must not produce diagnostics");
  Check(decoded.listener_id == 42, "decoded DBBT listener id must match");
  Check(decoded.client_nonce == token.client_nonce, "decoded DBBT client nonce must match");
  Check(decoded.server_nonce == token.server_nonce, "decoded DBBT server nonce must match");

  result = proto::ValidateDbbt(encoded, key, proto::DbbtValidationOptions{7, 101000, 2000});
  Check(!result.ok, "DBBT listener mismatch must fail");
  Check(HasDiagnostic(result.diagnostics, "MCP.DBBT_LISTENER_MISMATCH"), "DBBT listener mismatch diagnostic required");
}

void TestDbbtKeyringAndReplayCache() {
  const auto path = std::filesystem::temp_directory_path() / "sbmn_manager_dbbt_keyring_unit.txt";
  {
    std::ofstream out(path, std::ios::trunc);
    out << "format=SBMN_DBBT_KEYRING_V1\n";
    out << "active_key_id=active_2026\n";
    out << "active_key_hex=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
    out << "previous_key_hex=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n";
    out << "not_before_ms=90000\n";
    out << "not_after_ms=200000\n";
  }

  proto::DbbtKeyring keyring;
  auto load_result = proto::LoadDbbtKeyring(path, &keyring);
  Check(load_result.ok, "DBBT keyring v1 must load");
  Check(keyring.active_key_id == "active_2026", "DBBT keyring active key id must load");
  Check(keyring.active_key.size() == 32, "DBBT keyring active key must decode to 32 bytes");
  Check(keyring.verification_keys.size() == 2, "DBBT keyring must include active and previous verification keys");

  proto::DbbtToken token;
  for (std::size_t i = 0; i < token.db_uuid.size(); ++i) token.db_uuid[i] = static_cast<std::uint8_t>(0x20 + i);
  for (std::size_t i = 0; i < token.manager_session_id.size(); ++i) {
    token.manager_session_id[i] = static_cast<std::uint8_t>(0x40 + i);
  }
  token.listener_id = 42;
  token.issued_at_ms = 100000;
  token.expires_at_ms = 130000;
  token.client_nonce = {1, 2, 3, 4};
  token.server_nonce = {5, 6, 7, 8};
  const auto encoded = proto::EncodeDbbt(token, keyring.active_key);

  proto::DbbtReplayCache replay_cache(2);
  proto::DbbtToken decoded;
  std::string matched_key_id;
  auto validate_result = proto::ValidateDbbtWithKeyring(encoded,
                                                        keyring,
                                                        proto::DbbtValidationOptions{42, 101000, 2000},
                                                        &replay_cache,
                                                        &decoded,
                                                        &matched_key_id);
  Check(validate_result.ok, "DBBT must validate through keyring");
  Check(matched_key_id == "active_2026", "DBBT keyring validation must report matched active key");
  Check(replay_cache.size() == 1, "DBBT replay cache must insert first valid token");

  validate_result = proto::ValidateDbbtWithKeyring(encoded,
                                                   keyring,
                                                   proto::DbbtValidationOptions{42, 101000, 2000},
                                                   &replay_cache);
  Check(!validate_result.ok, "DBBT replay must fail on second validation");
  Check(HasDiagnostic(validate_result.diagnostics, "MCP.DBBT_REPLAY_DETECTED"), "DBBT replay diagnostic required");
  std::filesystem::remove(path);
}

void TestDbbtKeyringRejectsUnknownAndDuplicateKeys() {
  const auto unknown_path = std::filesystem::temp_directory_path() / "sbmn_manager_dbbt_keyring_unknown_unit.txt";
  {
    std::ofstream out(unknown_path, std::ios::trunc);
    out << "format=SBMN_DBBT_KEYRING_V1\n";
    out << "active_key_id=active_2026\n";
    out << "active_key_hex=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
    out << "unexpected_key=must_fail_closed\n";
  }
  proto::DbbtKeyring keyring;
  auto load_result = proto::LoadDbbtKeyring(unknown_path, &keyring);
  Check(!load_result.ok, "DBBT keyring v1 must reject unknown keys");
  Check(HasDiagnostic(load_result.diagnostics, "MCP.DBBT_KEYRING_INVALID"),
        "unknown DBBT keyring key must produce structured diagnostic");
  std::filesystem::remove(unknown_path);

  const auto duplicate_path = std::filesystem::temp_directory_path() / "sbmn_manager_dbbt_keyring_duplicate_unit.txt";
  {
    std::ofstream out(duplicate_path, std::ios::trunc);
    out << "format=SBMN_DBBT_KEYRING_V1\n";
    out << "active_key_id=active_2026\n";
    out << "active_key_id=active_2027\n";
    out << "active_key_hex=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
  }
  load_result = proto::LoadDbbtKeyring(duplicate_path, &keyring);
  Check(!load_result.ok, "DBBT keyring v1 must reject duplicate singleton keys");
  Check(HasDiagnostic(load_result.diagnostics, "MCP.DBBT_KEYRING_INVALID"),
        "duplicate DBBT keyring key must produce structured diagnostic");
  std::filesystem::remove(duplicate_path);
}

void TestLprefaceRoundTrip() {
  proto::Lpreface preface;
  preface.listener_id = 42;
  preface.dbbt = {1, 2, 3, 4, 5};
  preface.db_selector = "main";
  preface.requested_profile = "SBsql";
  preface.auth_provider_family = "security_database_temporary_token";
  preface.auth_principal = "alice";
  preface.auth_token = "unit-token";

  proto::Bytes encoded;
  auto encode_result = proto::EncodeLpreface(preface, &encoded);
  Check(encode_result.ok, "LPREFACE must encode");
  Check(encoded.size() == 4 + 2 + 2 + 4 + 4 + 5 + 2 + 4 + 2 + 5 +
                            2 + preface.auth_provider_family.size() +
                            2 + preface.auth_principal.size() +
                            2 + preface.auth_token.size() + 4,
        "LPREFACE v2 encoded size must match field layout");

  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeLpreface(encoded, &diagnostics);
  Check(decoded.has_value(), "LPREFACE must decode");
  Check(diagnostics.empty(), "valid LPREFACE must not produce diagnostics");
  if (decoded) {
    Check(decoded->listener_id == preface.listener_id, "LPREFACE listener id must round-trip");
    Check(decoded->dbbt == preface.dbbt, "LPREFACE DBBT must round-trip");
    Check(decoded->db_selector == "main", "LPREFACE db selector must round-trip");
    Check(decoded->requested_profile == "SBsql", "LPREFACE profile must round-trip");
    Check(decoded->auth_provider_family == preface.auth_provider_family,
          "LPREFACE auth provider must round-trip");
    Check(decoded->auth_principal == preface.auth_principal,
          "LPREFACE auth principal must round-trip");
    Check(decoded->auth_token == preface.auth_token,
          "LPREFACE auth token must round-trip");
  }

  proto::Bytes ack_encoded;
  auto ack_result = proto::EncodeLprefaceAck(proto::LprefaceAck{true, 0, ""}, &ack_encoded);
  Check(ack_result.ok, "LPREFACE ack must encode");
  auto ack_decoded = proto::DecodeLprefaceAck(ack_encoded, &diagnostics);
  Check(ack_decoded.has_value(), "LPREFACE ack must decode");
  if (ack_decoded) Check(ack_decoded->accepted, "LPREFACE accepted ack must round-trip");

  encoded.push_back(0xff);
  diagnostics.clear();
  decoded = proto::DecodeLpreface(encoded, &diagnostics);
  Check(!decoded.has_value(), "LPREFACE trailing bytes must fail");
  Check(HasDiagnostic(diagnostics, "LPREFACE.TRUNCATED"), "LPREFACE trailing-byte diagnostic required");
}

void TestLprefaceHandoffClaimRoundTrip() {
  proto::Bytes client_nonce(16);
  proto::Bytes server_nonce(16);
  for (std::size_t i = 0; i < client_nonce.size(); ++i) {
    client_nonce[i] = static_cast<std::uint8_t>(0x10 + i);
    server_nonce[i] = static_cast<std::uint8_t>(0x80 + i);
  }

  const auto encoded = proto::EncodeLprefaceHandoffClaim(client_nonce, server_nonce);
  Check(encoded.rfind("SB-LPREFACE-CLAIM/1 ", 0) == 0,
        "LPREFACE handoff claim must use a versioned prefix");
  Check(proto::IsLprefaceHandoffClaimPrefix(encoded.substr(0, 8)),
        "LPREFACE handoff claim prefix detector must accept partial prefix input");

  std::vector<proto::Diagnostic> diagnostics;
  const auto decoded = proto::DecodeLprefaceHandoffClaim(encoded, &diagnostics);
  Check(decoded.has_value(), "LPREFACE handoff claim must decode");
  Check(diagnostics.empty(), "valid LPREFACE handoff claim must not produce diagnostics");
  Check(decoded && decoded->client_nonce == client_nonce,
        "LPREFACE handoff claim client nonce must round-trip");
  Check(decoded && decoded->server_nonce == server_nonce,
        "LPREFACE handoff claim server nonce must round-trip");

  diagnostics.clear();
  const auto malformed = proto::DecodeLprefaceHandoffClaim(
      "SB-LPREFACE-CLAIM/1 client_nonce=01 server_nonce=02",
      &diagnostics);
  Check(!malformed.has_value(), "short LPREFACE handoff claim nonces must be rejected");
  Check(HasDiagnostic(diagnostics, "LPREFACE.CLAIM_NONCE_LENGTH"),
        "short LPREFACE handoff claim nonce diagnostic required");
}

void TestRuntimeProxyFailsClosed() {
  node::ManagerConfig config;
  config.proxy_enabled = true;
  config.control_dir = std::filesystem::temp_directory_path() / "sbmn_manager_proxy_fails_closed_control";
  config.runtime_dir = std::filesystem::temp_directory_path() / "sbmn_manager_proxy_fails_closed_runtime";
  const auto result = node::RunManager(config);
  Check(result.exit_code != 0, "proxy-enabled runtime must fail closed before DBBT/LPREFACE runtime admission exists");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.SECRET_REQUIRED"), "proxy fail-closed diagnostic required");
}

void TestRuntimeProxyLprefaceRequiresDatabaseUuid() {
  node::ManagerConfig config;
  config.proxy_enabled = true;
  config.mcp_secret_ref = "literal:test-secret";
  config.release_profile = "developer";
  config.listener_control_socket_dir = std::filesystem::temp_directory_path();
  config.dbbt_keyring_path = std::filesystem::temp_directory_path() / "unused-keyring.txt";
  config.control_dir = std::filesystem::temp_directory_path() / "sbmn_manager_proxy_missing_db_uuid_control";
  config.runtime_dir = std::filesystem::temp_directory_path() / "sbmn_manager_proxy_missing_db_uuid_runtime";
  const auto result = node::RunManager(config);
  Check(result.exit_code != 0, "LPREFACE proxy runtime must fail closed without owner database UUID");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.SECRET_REQUIRED"), "LPREFACE missing database UUID diagnostic required");
}

void TestRestartDescriptorValidation() {
  const char* argv_bad_exe[] = {"sbmn_manager", "--server-restart-executable", "relative/sb_server"};
  auto result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_exe));
  Check(!result.ok(), "relative restart executable must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CONFIG_FIELD_INVALID"), "relative restart executable diagnostic required");

  const char* argv_bad_args[] = {"sbmn_manager", "--server-restart-executable", "/bin/true", "--server-restart-arguments", "--config /tmp/x;rm"};
  result = node::ParseManagerCli(5, const_cast<char**>(argv_bad_args));
  Check(!result.ok(), "restart shell metacharacters must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CONFIG_FIELD_INVALID"), "bad restart argument diagnostic required");
}

void TestServiceModeCliValidation() {
  const char* argv_conflict[] = {"sbmn_manager", "--service", "--foreground"};
  auto result = node::ParseManagerCli(3, const_cast<char**>(argv_conflict));
  Check(!result.ok(), "service and foreground modes must conflict");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_MODE_CONFLICT"), "service/foreground conflict diagnostic required");

  const char* argv_validate[] = {"sbmn_manager", "--service", "--validate-config"};
  result = node::ParseManagerCli(3, const_cast<char**>(argv_validate));
  Check(result.ok(), "service plus validate-config must parse because validate-config has no runtime side effects");
}

void TestCliNumericValidation() {
  const char* argv_bad_port[] = {"sbmn_manager", "--port", "65536"};
  auto result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_port));
  Check(!result.ok(), "out-of-range proxy port must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_VALUE_INVALID"), "out-of-range proxy port diagnostic required");

  const char* argv_bad_native_port[] = {"sbmn_manager", "--native-port", "3392tcp"};
  result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_native_port));
  Check(!result.ok(), "native port with trailing text must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_VALUE_INVALID"), "native port trailing text diagnostic required");

  const char* argv_bad_listener_id[] = {"sbmn_manager", "--listener-id", "-1"};
  result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_listener_id));
  Check(!result.ok(), "negative listener id must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_VALUE_INVALID"), "negative listener id diagnostic required");

  const char* argv_bad_management_clients[] = {"sbmn_manager", "--management-max-clients", "0"};
  result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_management_clients));
  Check(!result.ok(), "zero management max clients must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_VALUE_INVALID"), "management max clients diagnostic required");

  const char* argv_bad_management_payload[] = {"sbmn_manager", "--management-max-payload-bytes", "16777217"};
  result = node::ParseManagerCli(3, const_cast<char**>(argv_bad_management_payload));
  Check(!result.ok(), "oversized management payload cap must be rejected");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CLI_VALUE_INVALID"), "management payload cap diagnostic required");

  const char* argv_management_limits[] = {"sbmn_manager",
                                          "--management-backlog",
                                          "8",
                                          "--management-max-clients",
                                          "9",
                                          "--management-max-payload-bytes",
                                          "4096",
                                          "--management-idle-timeout-ms",
                                          "1000"};
  result = node::ParseManagerCli(9, const_cast<char**>(argv_management_limits));
  Check(result.ok(), "valid management limit CLI options must parse");
  Check(result.config.management_backlog == 8, "management backlog CLI value must apply");
  Check(result.config.management_max_clients == 9, "management max clients CLI value must apply");
  Check(result.config.management_max_payload_bytes == 4096, "management payload cap CLI value must apply");
  Check(result.config.management_idle_timeout_ms == 1000, "management idle timeout CLI value must apply");
}

void TestStandaloneDefaultDatabasePathCliScopesRuntime() {
  const auto db_path = std::filesystem::temp_directory_path() / "sbmn_default_database_path_unit.sbdb";
  const auto runtime_dir = std::filesystem::temp_directory_path() / "sbmn_default_database_runtime_unit";
  const std::string db_arg = db_path.string();
  const std::string runtime_arg = runtime_dir.string();
  const char* argv[] = {"sbmn_manager",
                        "--runtime-dir",
                        runtime_arg.c_str(),
                        "--owner-db-path",
                        db_arg.c_str()};
  auto result = node::ParseManagerCli(5, const_cast<char**>(argv));
  Check(result.ok(), "standalone manager owner database path CLI must parse");
  Check(result.config.owner_database_path == std::filesystem::absolute(db_path).lexically_normal(),
        "standalone manager owner database path must be normalized");
  Check(result.config.owner_database_name == result.config.owner_database_path.string(),
        "standalone manager owner database name must default to configured path");
  const auto expected_scope = node::ManagerOwnerDatabaseRuntimeScopeId(result.config);
  const auto paths = node::ResolveManagerRuntimePaths(result.config);
  Check(paths.control_dir == runtime_dir / "manager" / "databases" / expected_scope,
        "standalone manager runtime control dir must be scoped to configured default database");
}

void TestLifecycleRejectsInvalidTransition() {
  const auto dir = std::filesystem::temp_directory_path() / "sbmn_manager_lifecycle_unit";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  node::ManagerLifecycle lifecycle(dir);
  std::vector<proto::Diagnostic> diagnostics;
  Check(lifecycle.Transition(node::ManagerLifecycleState::kRuntimePreparing, "unit start", &diagnostics),
        "created to runtime_preparing is legal for current startup path");
  Check(lifecycle.Transition(node::ManagerLifecycleState::kServerEndpointResolving, "server resolving", &diagnostics),
        "runtime_preparing to server_endpoint_resolving is legal");
  Check(!lifecycle.Transition(node::ManagerLifecycleState::kCreated, "illegal rewind", &diagnostics),
        "lifecycle must reject illegal rewind to created");
  Check(HasDiagnostic(diagnostics, "MANAGER.LIFECYCLE_INVALID_TRANSITION"),
        "invalid lifecycle transition diagnostic required");
  std::filesystem::remove_all(dir);
}

void TestLifecycleDurableAtomicEvidence() {
  const auto dir = std::filesystem::temp_directory_path() / "sbmn_manager_lifecycle_durable_unit";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream stale(dir / "sbmn_manager.lifecycle.state.tmp.stale", std::ios::trunc);
    stale << "partial\n";
  }

  node::ManagerLifecycle lifecycle(dir);
  std::vector<proto::Diagnostic> diagnostics;
  Check(lifecycle.Transition(node::ManagerLifecycleState::kRuntimePreparing,
                             "unit durable start",
                             &diagnostics),
        "lifecycle durable transition must succeed");
  Check(diagnostics.empty(), "durable lifecycle transition must not emit diagnostics");
  Check(CountLifecycleStateTemps(dir) == 0, "stale lifecycle temp files must be removed");

  const auto state_path = dir / "sbmn_manager.lifecycle.state";
  const auto journal_path = dir / "sbmn_manager.lifecycle.journal";
  const auto state = ReadText(state_path);
  const auto journal = ReadText(journal_path);
  Check(Contains(state, "format=SBMN_MANAGER_LIFECYCLE_STATE_V1"),
        "lifecycle state format marker required");
  Check(Contains(state, "state=runtime_preparing"),
        "lifecycle state must contain the committed state");
  Check(Contains(state, "checksum="),
        "lifecycle state checksum evidence required");
  Check(Contains(journal, "state=runtime_preparing"),
        "lifecycle journal must contain the transition state");
  Check(Contains(journal, "checksum="),
        "lifecycle journal checksum evidence required");
#ifndef _WIN32
  Check(IsPrivateFile(state_path), "lifecycle state must be private 0600");
  Check(IsPrivateFile(journal_path), "lifecycle journal must be private 0600");
#endif
  std::filesystem::remove_all(dir);
}

void TestLifecycleWriteFailureReportsDiagnostic() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_lifecycle_write_failure_unit";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto control_as_file = root / "control";
  {
    std::ofstream out(control_as_file, std::ios::trunc);
    out << "not a directory\n";
  }

  node::ManagerLifecycle lifecycle(control_as_file);
  std::vector<proto::Diagnostic> diagnostics;
  Check(!lifecycle.Transition(node::ManagerLifecycleState::kRuntimePreparing,
                              "must fail",
                              &diagnostics),
        "lifecycle transition must fail when durable evidence cannot be written");
  Check(HasDiagnostic(diagnostics, "MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED") ||
            HasDiagnostic(diagnostics, "MANAGER.LIFECYCLE_STATE_WRITE_FAILED"),
        "lifecycle write failure diagnostic required");
  Check(lifecycle.CurrentName() == "created",
        "lifecycle current state must not advance after durable write failure");
  std::filesystem::remove_all(root);
}

void TestSupportBundleModule() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_support_bundle_unit";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "control");
  node::ManagerConfig config;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.proxy_enabled = true;
  config.bind_address = "127.0.0.1";
  config.proxy_port = 3090;
  config.native_bind = "127.0.0.1";
  config.native_port = 3392;
  config.owner_database_name = "main";
  config.listener_id = 1;
  config.mcp_secret_ref = "literal:redacted";
  config.dbbt_keyring_path = root / "secret-keyring";
  config.restart_executable = "/bin/true";
  config.restart_enabled = true;

  const auto state_file = root / "control" / "sbmn_manager.lifecycle.state";
  const auto journal_file = root / "control" / "sbmn_manager.lifecycle.journal";
  const auto audit_file = root / "control" / "sbmn_manager.audit.jsonl";
  const auto metrics_file = root / "control" / "sbmn_manager.metrics.json";
  {
    std::ofstream out(state_file);
    out << "state=ready\n";
  }
  {
    std::ofstream out(journal_file);
    out << "journal\n";
  }
  {
    std::ofstream out(audit_file);
    out << "{\"audit\":true}\n";
  }

  std::string error_code;
  const auto bundle_dir = root / "control" / "support-bundles" / "unit";
  const bool ok = node::GenerateManagerSupportBundle(
      config,
      node::SupportBundleInputs{
          bundle_dir,
          "manager",
          "default",
          "{\"status\":true}\n",
          "{\"metrics\":true}\n",
          audit_file,
          metrics_file,
          state_file,
          journal_file,
      },
      &error_code);
  Check(ok, "support bundle module must generate bundle");
  Check(error_code.empty(), "support bundle success must not set error code");
  Check(std::filesystem::exists(bundle_dir / "manifest.txt"), "support bundle manifest required");
  Check(std::filesystem::exists(bundle_dir / "status.json"), "support bundle status required");
  Check(std::filesystem::exists(bundle_dir / "metrics.json"), "support bundle metrics required");
  Check(std::filesystem::exists(bundle_dir / "config-redacted.txt"), "support bundle redacted config required");
  Check(std::filesystem::exists(bundle_dir / "lifecycle.state"), "support bundle lifecycle state copy required");
  Check(std::filesystem::exists(bundle_dir / "lifecycle.journal"), "support bundle lifecycle journal copy required");
  Check(std::filesystem::exists(bundle_dir / "audit.jsonl"), "support bundle audit copy required");
  std::filesystem::remove_all(root);
}

void TestAuditRecordSchemaRedactsAndChecksums() {
  node::ManagerAuditRecord record;
  record.audit_event_uuid_hex = "00112233445566778899aabbccddeeff";
  record.audit_sequence = 42;
  record.wall_time_ms = 123456789;
  record.operation = "MANAGER_AUTH_DECISION";
  record.success = false;
  record.diagnostic_code = "MANAGER.AUDIT_WRITE_FAILED";
  record.lifecycle_state = "ready";
  record.record_checksum = "abcdef0123456789";
  record.fields = {
      {"principal", "operator"},
      {"security_token", "secret-canary-token"},
      {"config_path", "/tmp/secret/path"},
      {"config_ref", "/tmp/secret/reload.conf"},
      {"reason", "denied"},
  };

  const auto line = node::RenderManagerAuditJsonLine(record);
  Check(Contains(line, "\"format\":\"SBMN_MANAGER_AUDIT_V1\""),
        "audit record must include stable format marker");
  Check(Contains(line, "\"audit_sequence\":42"),
        "audit record must include monotonic sequence evidence");
  Check(Contains(line, "\"record_checksum\":\"abcdef0123456789\""),
        "audit record must include checksum evidence");
  Check(Contains(line, "\"security_token\":\"[redacted]\""),
        "audit record must redact token fields");
  Check(Contains(line, "\"config_path\":\"[path-redacted]\""),
        "audit record must redact path fields");
  Check(Contains(line, "\"config_ref\":\"[path-redacted]\""),
        "audit record must redact config_ref fields");
  Check(!Contains(line, "secret-canary-token"),
        "audit record must not leak token canaries");
  Check(!Contains(line, "/tmp/secret/path"),
        "audit record must not leak local path canaries");
  Check(!Contains(line, "/tmp/secret/reload.conf"),
        "audit record must not leak config_ref path canaries");
}

}  // namespace

int main() {
  TestSbdbFrameRoundTrip();
  TestSbdbFrameRejectsBadInputs();
  TestControlPlaneFrameRoundTrip();
  TestMessageVectorSetV1RoundTrip();
  TestHmacKnownAnswer();
  TestDbbtRoundTrip();
  TestDbbtKeyringAndReplayCache();
  TestDbbtKeyringRejectsUnknownAndDuplicateKeys();
  TestLprefaceRoundTrip();
  TestLprefaceHandoffClaimRoundTrip();
  TestRuntimeProxyFailsClosed();
  TestRuntimeProxyLprefaceRequiresDatabaseUuid();
  TestRestartDescriptorValidation();
  TestServiceModeCliValidation();
  TestCliNumericValidation();
  TestStandaloneDefaultDatabasePathCliScopesRuntime();
  TestLifecycleRejectsInvalidTransition();
  TestLifecycleDurableAtomicEvidence();
  TestLifecycleWriteFailureReportsDiagnostic();
  TestSupportBundleModule();
  TestAuditRecordSchemaRedactsAndChecksums();
  if (failures != 0) {
    std::cerr << failures << " manager protocol/runtime unit failure(s)\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
