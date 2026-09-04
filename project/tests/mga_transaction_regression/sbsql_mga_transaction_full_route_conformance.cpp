// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include "../database_lifecycle/database_lifecycle_test_memory.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::string_view kAliceVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAlicePrincipalUuid =
    "019f0a11-ce00-7000-8000-0000000000a1";

namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace sbsql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

pid_t g_server_pid = 0;
pid_t g_listener_pid = 0;
std::filesystem::path g_work_dir;

void StopProcess(pid_t pid);

void DumpFile(const std::filesystem::path& path, std::string_view label) {
  std::ifstream input(path);
  if (!input) return;
  std::cerr << "----- " << label << ": " << path << " -----\n";
  std::string line;
  while (std::getline(input, line)) {
    std::cerr << line << '\n';
  }
}

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  if (!g_work_dir.empty()) {
    DumpFile(g_work_dir / "listener.err", "listener.err");
    DumpFile(g_work_dir / "listener.out", "listener.out");
    DumpFile(g_work_dir / "server.err", "server.err");
    DumpFile(g_work_dir / "server.out", "server.out");
  }
  StopProcess(g_listener_pid);
  StopProcess(g_server_pid);
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& lines, std::string_view needle) {
  for (const auto& line : lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

void RequireContains(const std::vector<std::string>& lines,
                     std::string_view needle,
                     std::string_view message) {
  if (Contains(lines, needle)) return;
  std::cerr << "missing response fragment: " << needle << '\n';
  for (const auto& line : lines) std::cerr << line << '\n';
  Fail(message);
}

int HexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

std::uint16_t ReadLe16(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

std::uint64_t ReadLe64(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

bool AnyNonZero(const std::vector<std::uint8_t>& bytes,
                std::size_t offset,
                std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (bytes[offset + i] != 0) return true;
  }
  return false;
}

struct TransactionIdentity {
  std::array<std::uint8_t, 16> uuid{};
  std::uint64_t local_id = 0;
};

struct SavepointIdentity {
  std::array<std::uint8_t, 16> uuid{};
  std::uint64_t generation = 0;
  std::uint64_t ordinal = 0;
  TransactionIdentity transaction;
};

std::vector<std::uint8_t> RequireCanonicalBinaryResult(
    const std::vector<std::string>& lines,
    std::string_view operation_id,
    std::size_t expected_size,
    std::string_view context) {
  constexpr std::string_view kPayloadPrefix =
      "canonical_binary_payload_hex=";
  const std::string result_prefix =
      "RESULT " + std::string(operation_id) + " 0 ";
  const std::string* result_line = nullptr;
  for (const auto& line : lines) {
    if (line.starts_with(result_prefix)) {
      result_line = &line;
      break;
    }
  }
  if (result_line == nullptr) {
    std::cerr << context << ": missing canonical binary result\n";
    for (const auto& line : lines) std::cerr << line << '\n';
    Fail("transaction control did not execute through its canonical operation");
  }
  const auto payload_pos = result_line->find(kPayloadPrefix);
  Require(payload_pos != std::string::npos,
          "transaction control did not publish its canonical binary result");
  const std::string_view hex(*result_line);
  const auto encoded = hex.substr(payload_pos + kPayloadPrefix.size());
  Require(encoded.size() == expected_size * 2,
          "transaction control result did not have its exact carrier extent");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(expected_size);
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    const int high = HexNibble(encoded[i]);
    const int low = HexNibble(encoded[i + 1]);
    Require(high >= 0 && low >= 0,
            "transaction control result was not canonical hexadecimal");
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

TransactionIdentity RequireCanonicalBeginResult(
    const std::vector<std::string>& lines,
    std::string_view context) {
  const auto bytes = RequireCanonicalBinaryResult(
      lines, "engine.op.txn_begin", 152, context);
  Require(bytes[0] == 'T' && bytes[1] == 'X' && bytes[2] == 'B' &&
              bytes[3] == 'H' && ReadLe16(bytes, 4) == 1 &&
              ReadLe16(bytes, 6) == 152 && ReadLe32(bytes, 8) == 152 &&
              ReadLe32(bytes, 12) == 0,
          "BEGIN result was not an exact TXBH v1 carrier");
  Require(AnyNonZero(bytes, 16, 16) && ReadLe64(bytes, 32) != 0 &&
              AnyNonZero(bytes, 40, 16) && AnyNonZero(bytes, 56, 16) &&
              ReadLe64(bytes, 72) != 0 && AnyNonZero(bytes, 80, 16) &&
              ReadLe64(bytes, 96) != 0,
          "BEGIN TXBH omitted transaction, snapshot, or policy authority");
  Require((bytes[104] == 1 || bytes[104] == 2) && bytes[105] == 1 &&
              (bytes[106] == 1 || bytes[106] == 2),
          "BEGIN TXBH lifecycle or authority scope was invalid");
  Require(!AnyNonZero(bytes, 107, 5) && AnyNonZero(bytes, 112, 32) &&
              ReadLe64(bytes, 144) != 0,
          "BEGIN TXBH reserved bytes, evidence, or executor generation was invalid");
  TransactionIdentity identity;
  for (std::size_t i = 0; i < identity.uuid.size(); ++i) {
    identity.uuid[i] = bytes[16 + i];
  }
  identity.local_id = ReadLe64(bytes, 32);
  return identity;
}

TransactionIdentity RequireCanonicalTransactionFinalityResult(
    const std::vector<std::string>& lines,
    bool committed,
    std::string_view context,
    const TransactionIdentity* expected = nullptr) {
  const std::string_view operation_id =
      committed ? "engine.op.txn_commit" : "engine.op.txn_rollback";
  const auto bytes =
      RequireCanonicalBinaryResult(lines, operation_id, 120, context);
  Require(bytes[0] == 'T' && bytes[1] == 'X' &&
              bytes[2] == (committed ? 'C' : 'R') && bytes[3] == 'R' &&
              ReadLe16(bytes, 4) == 1 && ReadLe16(bytes, 6) == 120 &&
              ReadLe32(bytes, 8) == 120 && ReadLe32(bytes, 12) == 0,
          "transaction finality result was not an exact TXCR/TXRR v1 carrier");
  Require(AnyNonZero(bytes, 16, 16) && ReadLe64(bytes, 32) != 0 &&
              ReadLe64(bytes, 40) != 0 && AnyNonZero(bytes, 48, 16) &&
              ReadLe64(bytes, 64) != 0,
          "transaction finality result omitted identity, sequence, or policy authority");
  Require(bytes[72] == (committed ? 2 : 3) &&
              (bytes[73] == 1 || bytes[73] == 2) &&
              !AnyNonZero(bytes, 74, 6) && AnyNonZero(bytes, 80, 32) &&
              ReadLe64(bytes, 112) != 0,
          "transaction finality lifecycle, evidence, or availability was invalid");
  TransactionIdentity identity;
  for (std::size_t i = 0; i < identity.uuid.size(); ++i) {
    identity.uuid[i] = bytes[16 + i];
  }
  identity.local_id = ReadLe64(bytes, 32);
  if (expected != nullptr) {
    Require(identity.uuid == expected->uuid &&
                identity.local_id == expected->local_id,
            "transaction finality result finalized a different transaction identity");
  }
  return identity;
}

SavepointIdentity RequireCanonicalSavepointResult(
    const std::vector<std::string>& lines,
    std::string_view context,
    const TransactionIdentity& expected_transaction) {
  const auto bytes = RequireCanonicalBinaryResult(
      lines, "engine.op.txn_savepoint", 144, context);
  Require(bytes[0] == 'S' && bytes[1] == 'P' && bytes[2] == 'H' &&
              bytes[3] == 'D' && ReadLe16(bytes, 4) == 1 &&
              ReadLe16(bytes, 6) == 144 && ReadLe32(bytes, 8) == 144 &&
              ReadLe32(bytes, 12) == 0,
          "SAVEPOINT result was not an exact SPHD v1 carrier");
  Require(AnyNonZero(bytes, 16, 16) && ReadLe64(bytes, 32) != 0 &&
              AnyNonZero(bytes, 40, 16) && ReadLe64(bytes, 56) != 0 &&
              ReadLe64(bytes, 64) != 0 && ReadLe64(bytes, 72) != 0 &&
              bytes[80] == 1,
          "SAVEPOINT result omitted its bound transaction or stack identity");
  Require(!AnyNonZero(bytes, 81, 7) && AnyNonZero(bytes, 88, 32) &&
              ReadLe64(bytes, 120) != 0 && !AnyNonZero(bytes, 128, 16),
          "SAVEPOINT result reserved bytes, evidence, or availability was invalid");
  SavepointIdentity identity;
  for (std::size_t i = 0; i < identity.uuid.size(); ++i) {
    identity.uuid[i] = bytes[16 + i];
    identity.transaction.uuid[i] = bytes[40 + i];
  }
  identity.generation = ReadLe64(bytes, 32);
  identity.transaction.local_id = ReadLe64(bytes, 56);
  identity.ordinal = ReadLe64(bytes, 64);
  Require(identity.transaction.uuid == expected_transaction.uuid &&
              identity.transaction.local_id == expected_transaction.local_id,
          "SAVEPOINT result belonged to a different transaction identity");
  return identity;
}

void RequireCanonicalSavepointRollbackResult(
    const std::vector<std::string>& lines,
    const SavepointIdentity& expected) {
  const auto bytes = RequireCanonicalBinaryResult(
      lines, "engine.op.txn_rollback_to_savepoint", 168,
      "ROLLBACK TO SAVEPOINT");
  Require(bytes[0] == 'S' && bytes[1] == 'P' && bytes[2] == 'R' &&
              bytes[3] == 'B' && ReadLe16(bytes, 4) == 1 &&
              ReadLe16(bytes, 6) == 168 && ReadLe32(bytes, 8) == 168 &&
              ReadLe32(bytes, 12) == 0,
          "ROLLBACK TO SAVEPOINT result was not an exact SPRB v1 carrier");
  for (std::size_t i = 0; i < expected.uuid.size(); ++i) {
    Require(bytes[16 + i] == expected.transaction.uuid[i] &&
                bytes[40 + i] == expected.uuid[i],
            "ROLLBACK TO SAVEPOINT result changed transaction or savepoint identity");
  }
  Require(ReadLe64(bytes, 32) == expected.transaction.local_id &&
              ReadLe64(bytes, 56) == expected.generation &&
              ReadLe64(bytes, 64) == expected.ordinal &&
              ReadLe64(bytes, 72) != 0 && ReadLe64(bytes, 80) != 0 &&
              bytes[88] == 1 && !AnyNonZero(bytes, 89, 7) &&
              AnyNonZero(bytes, 96, 32) && AnyNonZero(bytes, 128, 32) &&
              ReadLe64(bytes, 160) != 0,
          "ROLLBACK TO SAVEPOINT result omitted stack, rollback, or evidence authority");
}

void RequireCanonicalSavepointReleaseResult(
    const std::vector<std::string>& lines,
    const SavepointIdentity& expected) {
  const auto bytes = RequireCanonicalBinaryResult(
      lines, "engine.op.txn_release_savepoint", 120,
      "RELEASE SAVEPOINT");
  Require(bytes[0] == 'S' && bytes[1] == 'P' && bytes[2] == 'R' &&
              bytes[3] == 'R' && ReadLe16(bytes, 4) == 1 &&
              ReadLe16(bytes, 6) == 120 && ReadLe32(bytes, 8) == 120 &&
              ReadLe32(bytes, 12) == 0,
          "RELEASE SAVEPOINT result was not an exact SPRR v1 carrier");
  for (std::size_t i = 0; i < expected.uuid.size(); ++i) {
    Require(bytes[16 + i] == expected.transaction.uuid[i] &&
                bytes[40 + i] == expected.uuid[i],
            "RELEASE SAVEPOINT result changed transaction or savepoint identity");
  }
  Require(ReadLe64(bytes, 32) == expected.transaction.local_id &&
              ReadLe64(bytes, 56) == expected.generation &&
              ReadLe64(bytes, 64) == expected.ordinal &&
              ReadLe64(bytes, 72) != 0 && AnyNonZero(bytes, 80, 32) &&
              ReadLe64(bytes, 112) != 0,
          "RELEASE SAVEPOINT result omitted stack, evidence, or availability authority");
}

void RequireTransactionControlRegistryRow(std::string_view surface_id,
                                          std::string_view canonical_name,
                                          std::string_view surface_kind,
                                          std::string_view validation_fixture_id) {
  const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(surface_id);
  Require(row != nullptr, "missing generated transaction-control registry row");
  Require(row->canonical_name == canonical_name,
          "generated transaction-control canonical_name mismatch");
  Require(row->surface_kind == surface_kind,
          "generated transaction-control surface_kind mismatch");
  Require(row->family == "transaction",
          "generated transaction-control family mismatch");
  Require(row->source_status == "native_now",
          "generated transaction-control source_status mismatch");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "generated transaction-control cluster_scope mismatch");
  Require(row->sblr_operation_family == "sblr.transaction.control.v3",
          "generated transaction-control SBLR family mismatch");
  Require(row->parser_handler_key == "parser.statement_family.transaction",
          "generated transaction-control parser handler mismatch");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_transaction_control_v3",
          "generated transaction-control lowering handler mismatch");
  Require(row->server_admission_key == "server.admission.sblr_transaction_control_v3",
          "generated transaction-control server admission key mismatch");
  Require(row->engine_rule_key == "engine.rule.sblr_transaction_control_v3",
          "generated transaction-control engine rule key mismatch");
  Require(row->validation_fixture_id == validation_fixture_id,
          "generated transaction-control validation fixture mismatch");
}

void RequireSavepointNameRegistryRow() {
  RequireTransactionControlRegistryRow("SBSQL-AD76CD74FC10",
                                       "savepoint_name",
                                       "grammar_production",
                                       "SBSQL-SURFACE-099EDE32877C");
}

void RequireSavepointStmtRegistryRow() {
  RequireTransactionControlRegistryRow("SBSQL-35C5F6EA0613",
                                       "savepoint_stmt",
                                       "grammar_production",
                                       "SBSQL-SURFACE-38024C075136");
}

void RequireSetTransactionRegistryRows() {
  RequireTransactionControlRegistryRow("SBSQL-2072BB4C308D",
                                       "set_transaction_stmt",
                                       "grammar_production",
                                       "SBSQL-SURFACE-C2B9A3EE333E");
  RequireTransactionControlRegistryRow("SBSQL-1F20B86504C3",
                                       "transaction_mode",
                                       "grammar_production",
                                       "SBSQL-SURFACE-35023F023FE5");
  RequireTransactionControlRegistryRow("SBSQL-564BD6C7C93C",
                                       "transaction_mode_list",
                                       "grammar_production",
                                       "SBSQL-SURFACE-231AAFA76600");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsql_mga_full_route.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

int FindFreePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

int ConnectLoopback(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeval timeout{};
  timeout.tv_sec = 5;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

bool WriteAll(int fd, const std::string& text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const auto rc = ::write(fd, text.data() + written, text.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WaitForPath(const std::filesystem::path& path) {
  for (int i = 0; i < 120; ++i) {
    if (std::filesystem::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void StopProcess(pid_t pid) {
  if (pid <= 0) return;
  ::kill(pid, SIGTERM);
  int status = 0;
  for (int i = 0; i < 80; ++i) {
    const auto rc = ::waitpid(pid, &status, WNOHANG);
    if (rc == pid) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
}

void CreateDatabase(const std::filesystem::path& path) {
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779000001000);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779000001001);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779000001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "fixture_sysarch";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0358b60b6875c81e17d3e0ab67f8b785f49d4146547c79da401f21dc641c2c16";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database creation for SBsql MGA route test failed");
  const auto bootstrap =
      scratchbird::tests::database_lifecycle::BeginDurableBootstrapTransaction(
          path, "sbsql_mga_transaction_full_route_conformance");
  const auto bootstrap_transaction_uuid = bootstrap.transaction_uuid.canonical;
  const auto bootstrap_local_transaction_id = bootstrap.local_transaction_id;
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      path,
      uuid::UuidToString(database_uuid.value.value),
      kAlicePrincipalUuid,
      "alice",
      kAliceVerifier,
      bootstrap_local_transaction_id,
      "sbsql_mga_transaction_full_route_conformance",
      bootstrap_transaction_uuid);
  scratchbird::tests::database_lifecycle::GrantDurablePrincipalPrivilege(
      path,
      uuid::UuidToString(database_uuid.value.value),
      kAlicePrincipalUuid,
      uuid::UuidToString(database_uuid.value.value),
      "database",
      "CONNECT",
      bootstrap_local_transaction_id,
      "sbsql_mga_transaction_full_route_conformance:connect",
      bootstrap_transaction_uuid);
  scratchbird::tests::database_lifecycle::CommitDurableBootstrapTransaction(
      bootstrap);
}

pid_t LaunchServer(const std::filesystem::path& server,
                   const std::filesystem::path& control_dir,
                   const std::filesystem::path& runtime_dir,
                   const std::filesystem::path& database_path,
                   const std::filesystem::path& endpoint,
                   const std::filesystem::path& stdout_path,
                   const std::filesystem::path& stderr_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    int out = ::creat(stdout_path.c_str(), 0600);
    int err = ::creat(stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    ::execl(server.c_str(),
            server.c_str(),
            "--foreground",
            "--no-listeners",
            "--control-dir",
            control_dir.c_str(),
            "--runtime-dir",
            runtime_dir.c_str(),
            "--database",
            database_path.c_str(),
            "--sbps-endpoint",
            endpoint.c_str(),
            nullptr);
    _exit(127);
  }
  return pid;
}

pid_t LaunchListener(const std::filesystem::path& listener,
                     const std::filesystem::path& parser,
                     const std::filesystem::path& control_dir,
                     const std::filesystem::path& runtime_dir,
                     const std::filesystem::path& endpoint,
                     const std::filesystem::path& database_path,
                     int port,
                     const std::filesystem::path& stdout_path,
                     const std::filesystem::path& stderr_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    int out = ::creat(stdout_path.c_str(), 0600);
    int err = ::creat(stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    const std::string port_arg = "--port=" + std::to_string(port);
    const std::string parser_arg = "--parser-executable=" + parser.string();
    const std::string control_arg = "--control-dir=" + control_dir.string();
    const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
    const std::string endpoint_arg = "--server-endpoint=unix:" + endpoint.string();
    const std::string database_arg = "--database-selector=dev_bootstrap_path:" + database_path.string();
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            database_arg.c_str(),
            endpoint_arg.c_str(),
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=1",
            "--warm-pool-max=2",
            nullptr);
    _exit(127);
  }
  return pid;
}

std::vector<std::string> ReadCommandResponse(int fd,
                                             const std::string& command,
                                             std::string_view stop_prefix,
                                             int max_lines) {
  Require(WriteAll(fd, command + "\n"), "failed to write SBsql command");
  std::vector<std::string> lines;
  std::string line;
  for (int i = 0; i < max_lines; ++i) {
    if (!ReadLine(fd, &line)) {
      std::cerr << "failed to read response for command: " << command << '\n';
      for (const auto& item : lines) std::cerr << item << '\n';
      Fail("failed to read SBsql command response");
    }
    lines.push_back(line);
    if (line.starts_with(stop_prefix)) return lines;
    if (line.starts_with("MESSAGE ERROR")) return lines;
  }
  std::cerr << "response for command did not reach stop line: " << command << '\n';
  for (const auto& item : lines) std::cerr << item << '\n';
  Fail("SBsql command response did not reach the expected stop line");
}

void RequireInventoryFinality(const std::filesystem::path& database_path) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  if (!loaded.ok()) {
    std::cerr << loaded.diagnostic.diagnostic_code << ":" << loaded.diagnostic.message_key << '\n';
  }
  Require(loaded.ok(), "could not reload transaction inventory after SBsql route test");
  bool saw_committed = false;
  bool saw_rolled_back = false;
  for (const auto& entry : loaded.inventory.entries) {
    saw_committed = saw_committed || entry.state == mga::TransactionState::committed;
    saw_rolled_back = saw_rolled_back || entry.state == mga::TransactionState::rolled_back;
    Require(entry.state != mga::TransactionState::active,
            "SBsql route left an active MGA transaction in durable inventory");
  }
  Require(saw_committed, "SBsql route did not persist committed transaction evidence");
  Require(saw_rolled_back, "SBsql route did not persist rolled-back transaction evidence");
}

}  // namespace

int main(int argc, char** argv) {
  ::alarm(60);
  if (argc != 4) {
    std::cerr << "usage: sbsql_mga_transaction_full_route_conformance <sb_server> <sb_listener> <sbp_sbsql>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path server = argv[1];
  const std::filesystem::path listener = argv[2];
  const std::filesystem::path parser = argv[3];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp directory");
  g_work_dir = work;
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "sbsql_mga_transaction_full_route_conformance");
  const auto database_path = work / "sbsql_mga_route.sbdb";
  CreateDatabase(database_path);

  const auto server_control = work / "server-control";
  const auto server_runtime = work / "server-runtime";
  const auto listener_control = work / "listener-control";
  const auto listener_runtime = work / "listener-runtime";
  const auto endpoint = server_control / "sb_server.sbps.sock";
  const int port = FindFreePort();
  Require(port > 0, "could not allocate loopback port");

  const pid_t server_pid = LaunchServer(server,
                                        server_control,
                                        server_runtime,
                                        database_path,
                                        endpoint,
                                        work / "server.out",
                                        work / "server.err");
  Require(server_pid > 0, "failed to launch sb_server");
  g_server_pid = server_pid;
  if (!WaitForPath(endpoint)) {
    std::cerr << "server endpoint was not created under " << work << '\n';
    Fail("server endpoint was not created");
  }

  const pid_t listener_pid = LaunchListener(listener,
                                            parser,
                                            listener_control,
                                            listener_runtime,
                                            endpoint,
                                            database_path,
                                            port,
                                            work / "listener.out",
                                            work / "listener.err");
  Require(listener_pid > 0, "failed to launch sb_listener");
  g_listener_pid = listener_pid;

  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) {
    std::cerr << "listener did not accept client connection under " << work << '\n';
    Fail("listener did not accept client connection");
  }

  std::string line;
  Require(ReadLine(fd, &line) && line == "ScratchBird SBSQL parser ready",
          "unexpected SBsql parser greeting");
  const std::string auth_command =
      "AUTH alice " + std::string(kAliceVerifier);
  const auto auth = ReadCommandResponse(fd, auth_command, "OK AUTHENTICATED", 6);
  RequireContains(auth, "OK AUTHENTICATED", "SBsql authentication failed");

  const auto set_transaction = ReadCommandResponse(fd,
                                                   "EXECUTE SET TRANSACTION READ WRITE",
                                                   "evidence=parser_finality:false",
                                                   24);
  RequireSetTransactionRegistryRows();
  RequireContains(set_transaction,
                  "PREPARED sblr.transaction.control.v3",
                  "SET TRANSACTION did not prepare as SBsql transaction SBLR");
  RequireContains(set_transaction,
                  "\"surface_key\":\"SBSQL-2072BB4C308D\"",
                  "SET TRANSACTION did not bind the set_transaction_stmt surface row");
  RequireContains(set_transaction,
                  "\"sblr_operation\":\"SBLR_TRANSACTION_SET_CHARACTERISTICS\"",
                  "SET TRANSACTION did not lower to exact SBLR_TRANSACTION_SET_CHARACTERISTICS");
  RequireContains(set_transaction,
                  "\"transaction_read_mode\":\"read_write\"",
                  "SET TRANSACTION did not carry read_write mode in the SBLR envelope");
  RequireContains(set_transaction,
                  "\"transaction_read_only\":\"false\"",
                  "SET TRANSACTION did not carry read-only=false in the SBLR envelope");
  RequireContains(set_transaction,
                  "\"transaction_isolation_level\":\"read_committed\"",
                  "SET TRANSACTION did not carry the default isolation level in the SBLR envelope");
  RequireContains(set_transaction,
                  "SBSQL-1F20B86504C3",
                  "SET TRANSACTION did not publish transaction_mode row evidence");
  RequireContains(set_transaction,
                  "SBSQL-564BD6C7C93C",
                  "SET TRANSACTION did not publish transaction_mode_list row evidence");
  Require(Contains(set_transaction, "RESULT transaction.set_characteristics"),
          "SET TRANSACTION did not execute through engine transaction characteristics operation");
  Require(Contains(set_transaction, "evidence=transaction_characteristics:session_defaults_applied"),
          "SET TRANSACTION did not return session-default transaction evidence");
  Require(Contains(set_transaction, "evidence=transaction_read_mode:read_write"),
          "SET TRANSACTION did not return read_write transaction evidence");
  Require(Contains(set_transaction, "evidence=transaction_read_only:false"),
          "SET TRANSACTION did not return read-only=false transaction evidence");
  Require(Contains(set_transaction, "evidence=transaction_isolation_level:read_committed"),
          "SET TRANSACTION did not return read_committed transaction evidence");
  Require(!Contains(set_transaction, "WAL") && !Contains(set_transaction, "wal_required=true"),
          "SET TRANSACTION route unexpectedly exposed WAL authority evidence");

  const auto begin_commit = ReadCommandResponse(fd, "EXECUTE BEGIN TRANSACTION", "RESULT engine.op.txn_begin", 40);
  Require(Contains(begin_commit, "PREPARED sblr.transaction.control.v3"),
          "BEGIN did not prepare as SBsql transaction SBLR");
  RequireContains(begin_commit,
                  "\"surface_key\":\"SBSQL-41AABA342C25\"",
                  "BEGIN TRANSACTION did not bind the begin_transaction surface row");
  RequireContains(begin_commit,
                  "\"sblr_operation\":\"SBLR_TXN_BEGIN\"",
                  "BEGIN TRANSACTION did not lower to exact SBLR_TXN_BEGIN");
  const auto begin_commit_identity =
      RequireCanonicalBeginResult(begin_commit, "BEGIN TRANSACTION");

  const auto commit = ReadCommandResponse(fd, "EXECUTE COMMIT", "RESULT engine.op.txn_commit", 64);
  RequireContains(commit, "\"surface_key\":\"SBSQL-37B92A5842F6\"",
                  "COMMIT did not bind the commit surface row");
  RequireContains(commit, "\"sblr_operation\":\"SBLR_TXN_COMMIT\"",
                  "COMMIT did not lower to exact SBLR_TXN_COMMIT");
  RequireCanonicalTransactionFinalityResult(
      commit, true, "COMMIT", &begin_commit_identity);

  const auto replacement_commit = ReadCommandResponse(fd, "EXECUTE COMMIT", "RESULT engine.op.txn_commit", 64);
  RequireCanonicalTransactionFinalityResult(
      replacement_commit, true, "COMMIT after replacement");

  const auto begin_stmt = ReadCommandResponse(fd, "EXECUTE BEGIN", "RESULT engine.op.txn_begin", 40);
  RequireContains(begin_stmt, "\"surface_key\":\"SBSQL-1B59D6E97591\"",
                  "BEGIN did not bind the begin_stmt grammar row");
  RequireContains(begin_stmt, "\"sblr_operation\":\"SBLR_TXN_BEGIN\"",
                  "BEGIN statement did not lower to exact SBLR_TXN_BEGIN");
  const auto begin_stmt_identity =
      RequireCanonicalBeginResult(begin_stmt, "BEGIN statement");

  const auto commit_stmt = ReadCommandResponse(fd, "EXECUTE COMMIT WORK", "RESULT engine.op.txn_commit", 64);
  RequireContains(commit_stmt, "\"surface_key\":\"SBSQL-7A09CE443D7A\"",
                  "COMMIT WORK did not bind the commit_stmt grammar row");
  RequireContains(commit_stmt, "\"sblr_operation\":\"SBLR_TXN_COMMIT\"",
                  "COMMIT WORK did not lower to exact SBLR_TXN_COMMIT");
  RequireCanonicalTransactionFinalityResult(
      commit_stmt, true, "COMMIT WORK", &begin_stmt_identity);

  const auto begin_rollback = ReadCommandResponse(fd, "EXECUTE BEGIN", "RESULT engine.op.txn_begin", 40);
  const auto begin_rollback_identity =
      RequireCanonicalBeginResult(begin_rollback, "second BEGIN");
  const auto rollback = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "RESULT engine.op.txn_rollback", 64);
  RequireContains(rollback, "\"surface_key\":\"SBSQL-EACF8DB1CB02\"",
                  "ROLLBACK did not bind the rollback surface row");
  RequireContains(rollback, "\"sblr_operation\":\"SBLR_TXN_ROLLBACK\"",
                  "ROLLBACK did not lower to exact SBLR_TXN_ROLLBACK");
  RequireCanonicalTransactionFinalityResult(
      rollback, false, "ROLLBACK", &begin_rollback_identity);

  const auto begin_rollback_stmt = ReadCommandResponse(fd, "EXECUTE BEGIN", "RESULT engine.op.txn_begin", 40);
  const auto begin_rollback_stmt_identity =
      RequireCanonicalBeginResult(begin_rollback_stmt, "third BEGIN");
  const auto rollback_stmt = ReadCommandResponse(fd, "EXECUTE ROLLBACK WORK", "RESULT engine.op.txn_rollback", 64);
  RequireContains(rollback_stmt, "\"surface_key\":\"SBSQL-129ADA0B6225\"",
                  "ROLLBACK WORK did not bind the rollback_stmt grammar row");
  RequireContains(rollback_stmt, "\"sblr_operation\":\"SBLR_TXN_ROLLBACK\"",
                  "ROLLBACK WORK did not lower to exact SBLR_TXN_ROLLBACK");
  RequireCanonicalTransactionFinalityResult(
      rollback_stmt, false, "ROLLBACK WORK", &begin_rollback_stmt_identity);

  const auto begin_savepoint = ReadCommandResponse(fd, "EXECUTE BEGIN", "RESULT engine.op.txn_begin", 40);
  const auto begin_savepoint_identity =
      RequireCanonicalBeginResult(begin_savepoint, "savepoint BEGIN");
  const auto savepoint = ReadCommandResponse(fd, "EXECUTE SAVEPOINT route_sp", "RESULT engine.op.txn_savepoint", 24);
  RequireContains(savepoint, "\"surface_key\":\"SBSQL-9EC31122A564\"",
                  "SAVEPOINT did not bind the savepoint surface row");
  RequireContains(savepoint, "\"statement_surface_name\":\"savepoint\"",
                  "SAVEPOINT did not expose the positive canonical savepoint statement route");
  RequireSavepointStmtRegistryRow();
  RequireSavepointNameRegistryRow();
  Require(Contains(savepoint, "PREPARED sblr.transaction.control.v3"),
          "SAVEPOINT did not prepare through server transaction-control admission");
  RequireContains(savepoint, "\"sblr_operation\":\"SBLR_TXN_SAVEPOINT\"",
                  "SAVEPOINT did not lower to exact SBLR_TXN_SAVEPOINT");
  const auto savepoint_identity = RequireCanonicalSavepointResult(
      savepoint, "SAVEPOINT", begin_savepoint_identity);
  Require(!Contains(savepoint, "WAL") && !Contains(savepoint, "wal_required=true"),
          "SAVEPOINT route unexpectedly exposed WAL authority evidence");

  const auto rollback_to = ReadCommandResponse(fd,
                                               "EXECUTE ROLLBACK TO SAVEPOINT route_sp",
                                               "RESULT engine.op.txn_rollback_to_savepoint",
                                               24);
  RequireContains(rollback_to, "\"surface_key\":\"SBSQL-3BF8303CFB36\"",
                  "ROLLBACK TO SAVEPOINT did not bind the rollback_to_savepoint_stmt grammar row");
  RequireContains(rollback_to, "\"sblr_operation\":\"SBLR_TXN_ROLLBACK_TO_SAVEPOINT\"",
                  "ROLLBACK TO SAVEPOINT did not lower to exact SBLR_TXN_ROLLBACK_TO_SAVEPOINT");
  RequireCanonicalSavepointRollbackResult(rollback_to, savepoint_identity);

  const auto release = ReadCommandResponse(fd, "EXECUTE RELEASE SAVEPOINT route_sp", "RESULT engine.op.txn_release_savepoint", 24);
  RequireContains(release, "\"surface_key\":\"SBSQL-9E33ED8C3B3D\"",
                  "RELEASE SAVEPOINT did not bind the release_savepoint_stmt grammar row");
  RequireContains(release, "\"sblr_operation\":\"SBLR_TXN_RELEASE_SAVEPOINT\"",
                  "RELEASE SAVEPOINT did not lower to exact SBLR_TXN_RELEASE_SAVEPOINT");
  RequireCanonicalSavepointReleaseResult(release, savepoint_identity);

  const auto savepoint_stmt_invalid = ReadCommandResponse(fd, "EXECUTE SAVEPOINT", "MESSAGE ERROR", 12);
  Require(Contains(savepoint_stmt_invalid, "SBLR.OPERAND_INVALID"),
          "SAVEPOINT without a name did not refuse the incomplete canonical operand");
  Require(Contains(savepoint_stmt_invalid,
                   "savepoint structural symbol or transaction handle was unavailable"),
          "SAVEPOINT without a name did not identify the missing structural symbol");

  const auto missing_rollback_to = ReadCommandResponse(fd,
                                                       "EXECUTE ROLLBACK TO SAVEPOINT missing_sp",
                                                       "MESSAGE ERROR",
                                                       12);
  RequireContains(missing_rollback_to, "SBLR.OPERAND_INVALID",
                  "ROLLBACK TO missing savepoint did not refuse its absent canonical handle");
  Require(Contains(missing_rollback_to, "savepoint handle"),
          "ROLLBACK TO missing savepoint did not identify the unavailable handle");

  const auto missing_release = ReadCommandResponse(fd,
                                                   "EXECUTE RELEASE SAVEPOINT missing_sp",
                                                   "MESSAGE ERROR",
                                                   12);
  RequireContains(missing_release, "SBLR.OPERAND_INVALID",
                  "RELEASE missing savepoint did not refuse its absent canonical handle");
  Require(Contains(missing_release, "savepoint handle"),
          "RELEASE missing savepoint did not identify the unavailable handle");

  const auto rollback_savepoint_tx = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "RESULT engine.op.txn_rollback", 64);
  RequireCanonicalTransactionFinalityResult(
      rollback_savepoint_tx, false, "savepoint transaction cleanup rollback",
      &begin_savepoint_identity);

  const auto replacement_rollback = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "RESULT engine.op.txn_rollback", 64);
  RequireCanonicalTransactionFinalityResult(
      replacement_rollback, false, "ROLLBACK after replacement");

  ::close(fd);
  StopProcess(listener_pid);
  g_listener_pid = 0;
  StopProcess(server_pid);
  g_server_pid = 0;
  RequireInventoryFinality(database_path);
  std::cout << "sbsql_mga_transaction_full_route_conformance=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
