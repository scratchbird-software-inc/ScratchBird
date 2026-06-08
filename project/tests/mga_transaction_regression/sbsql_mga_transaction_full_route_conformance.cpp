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
#include <cerrno>
#include <chrono>
#include <csignal>
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
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database creation for SBsql MGA route test failed");
  std::ofstream auth_store(path.string() + ".sb.local_password_auth", std::ios::trunc);
  auth_store << "alice\tlocal_password\t" << kAliceVerifier << '\n';
  Require(static_cast<bool>(auth_store), "database-local password verifier store creation failed");
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      path,
      uuid::UuidToString(database_uuid.value.value),
      kAlicePrincipalUuid,
      "alice",
      kAliceVerifier,
      11,
      "sbsql_mga_transaction_full_route_conformance");
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
    Require(ReadLine(fd, &line), "failed to read SBsql command response");
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
      "AUTH alice scheme=local_password_v1;principal=alice;principal_uuid=" +
      std::string(kAlicePrincipalUuid) +
      ";storage_authority=mga_security_principal_lifecycle;"
      "authorization_tags=right:CONNECT;verifier=" +
      std::string(kAliceVerifier);
  const auto auth = ReadCommandResponse(fd, auth_command, "OK AUTHENTICATED", 6);
  Require(Contains(auth, "OK AUTHENTICATED"), "SBsql authentication failed");

  const auto set_transaction = ReadCommandResponse(fd,
                                                   "EXECUTE SET TRANSACTION READ WRITE",
                                                   "evidence=parser_finality:false",
                                                   24);
  RequireSetTransactionRegistryRows();
  Require(Contains(set_transaction, "PREPARED sblr.transaction.control.v3"),
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

  const auto begin_commit = ReadCommandResponse(fd, "EXECUTE BEGIN TRANSACTION", "transaction_timestamp=", 40);
  Require(Contains(begin_commit, "PREPARED sblr.transaction.control.v3"),
          "BEGIN did not prepare as SBsql transaction SBLR");
  RequireContains(begin_commit,
                  "\"surface_key\":\"SBSQL-41AABA342C25\"",
                  "BEGIN TRANSACTION did not bind the begin_transaction surface row");
  RequireContains(begin_commit,
                  "\"sblr_operation\":\"SBLR_TRANSACTION_BEGIN\"",
                  "BEGIN TRANSACTION did not lower to exact SBLR_TRANSACTION_BEGIN");
  Require(Contains(begin_commit, "RESULT transaction.begin"),
          "BEGIN did not execute through server transaction operation");
  Require(Contains(begin_commit, "local_transaction_id="),
          "BEGIN did not return MGA local transaction evidence");
  Require(Contains(begin_commit, "evidence=transaction_state:active"),
          "BEGIN did not return active MGA state evidence");

  const auto commit = ReadCommandResponse(fd, "EXECUTE COMMIT", "evidence=always_active_transaction_replacement:", 24);
  RequireContains(commit, "\"surface_key\":\"SBSQL-37B92A5842F6\"",
                  "COMMIT did not bind the commit surface row");
  RequireContains(commit, "\"sblr_operation\":\"SBLR_TRANSACTION_COMMIT\"",
                  "COMMIT did not lower to exact SBLR_TRANSACTION_COMMIT");
  Require(Contains(commit, "RESULT transaction.commit"),
          "COMMIT did not execute through engine transaction operation");
  Require(Contains(commit, "evidence=transaction_state:committed"),
          "COMMIT did not return committed MGA state evidence");

  const auto replacement_commit = ReadCommandResponse(fd, "EXECUTE COMMIT", "evidence=always_active_transaction_replacement:", 24);
  Require(Contains(replacement_commit, "RESULT transaction.commit"),
          "COMMIT after replacement did not execute through engine transaction operation");
  Require(Contains(replacement_commit, "evidence=transaction_state:committed"),
          "COMMIT after replacement did not return committed MGA state evidence");

  const auto begin_stmt = ReadCommandResponse(fd, "EXECUTE BEGIN", "transaction_timestamp=", 40);
  RequireContains(begin_stmt, "\"surface_key\":\"SBSQL-1B59D6E97591\"",
                  "BEGIN did not bind the begin_stmt grammar row");
  RequireContains(begin_stmt, "\"sblr_operation\":\"SBLR_TRANSACTION_BEGIN\"",
                  "BEGIN statement did not lower to exact SBLR_TRANSACTION_BEGIN");
  Require(Contains(begin_stmt, "RESULT transaction.begin"),
          "BEGIN statement did not execute through engine transaction operation");
  Require(Contains(begin_stmt, "local_transaction_id="),
          "BEGIN statement did not return MGA local transaction evidence");
  Require(Contains(begin_stmt, "evidence=transaction_state:active"),
          "BEGIN statement did not return active MGA state evidence");

  const auto commit_stmt = ReadCommandResponse(fd, "EXECUTE COMMIT WORK", "evidence=always_active_transaction_replacement:", 24);
  RequireContains(commit_stmt, "\"surface_key\":\"SBSQL-7A09CE443D7A\"",
                  "COMMIT WORK did not bind the commit_stmt grammar row");
  RequireContains(commit_stmt, "\"sblr_operation\":\"SBLR_TRANSACTION_COMMIT\"",
                  "COMMIT WORK did not lower to exact SBLR_TRANSACTION_COMMIT");
  Require(Contains(commit_stmt, "RESULT transaction.commit"),
          "COMMIT WORK did not execute through engine transaction operation");
  Require(Contains(commit_stmt, "evidence=transaction_state:committed"),
          "COMMIT WORK did not return committed MGA state evidence");

  const auto begin_rollback = ReadCommandResponse(fd, "EXECUTE BEGIN", "transaction_timestamp=", 40);
  Require(Contains(begin_rollback, "RESULT transaction.begin"),
          "second BEGIN did not execute through engine transaction operation");
  const auto rollback = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "evidence=always_active_transaction_replacement:", 24);
  RequireContains(rollback, "\"surface_key\":\"SBSQL-EACF8DB1CB02\"",
                  "ROLLBACK did not bind the rollback surface row");
  RequireContains(rollback, "\"sblr_operation\":\"SBLR_TRANSACTION_ROLLBACK\"",
                  "ROLLBACK did not lower to exact SBLR_TRANSACTION_ROLLBACK");
  Require(Contains(rollback, "RESULT transaction.rollback"),
          "ROLLBACK did not execute through engine transaction operation");
  Require(Contains(rollback, "evidence=transaction_state:rolled_back"),
          "ROLLBACK did not return rolled-back MGA state evidence");

  const auto begin_rollback_stmt = ReadCommandResponse(fd, "EXECUTE BEGIN", "transaction_timestamp=", 40);
  Require(Contains(begin_rollback_stmt, "RESULT transaction.begin"),
          "third BEGIN did not execute through engine transaction operation");
  const auto rollback_stmt = ReadCommandResponse(fd, "EXECUTE ROLLBACK WORK", "evidence=always_active_transaction_replacement:", 24);
  RequireContains(rollback_stmt, "\"surface_key\":\"SBSQL-129ADA0B6225\"",
                  "ROLLBACK WORK did not bind the rollback_stmt grammar row");
  RequireContains(rollback_stmt, "\"sblr_operation\":\"SBLR_TRANSACTION_ROLLBACK\"",
                  "ROLLBACK WORK did not lower to exact SBLR_TRANSACTION_ROLLBACK");
  Require(Contains(rollback_stmt, "RESULT transaction.rollback"),
          "ROLLBACK WORK did not execute through engine transaction operation");
  Require(Contains(rollback_stmt, "evidence=transaction_state:rolled_back"),
          "ROLLBACK WORK did not return rolled-back MGA state evidence");

  const auto begin_savepoint = ReadCommandResponse(fd, "EXECUTE BEGIN", "transaction_timestamp=", 40);
  Require(Contains(begin_savepoint, "RESULT transaction.begin"),
          "savepoint BEGIN did not execute through engine transaction operation");
  const auto savepoint = ReadCommandResponse(fd, "EXECUTE SAVEPOINT route_sp", "evidence=savepoint_name_bound", 24);
  RequireContains(savepoint, "\"surface_key\":\"SBSQL-9EC31122A564\"",
                  "SAVEPOINT did not bind the savepoint surface row");
  RequireContains(savepoint, "\"statement_surface_name\":\"savepoint\"",
                  "SAVEPOINT did not expose the positive canonical savepoint statement route");
  RequireSavepointStmtRegistryRow();
  RequireSavepointNameRegistryRow();
  Require(Contains(savepoint, "PREPARED sblr.transaction.control.v3"),
          "SAVEPOINT did not prepare through server transaction-control admission");
  RequireContains(savepoint, "\"savepoint_name\":\"route_sp\"",
                  "SAVEPOINT did not lower the bound savepoint name into the SBLR envelope");
  RequireContains(savepoint, "\"sblr_operation\":\"SBLR_TRANSACTION_CREATE_SAVEPOINT\"",
                  "SAVEPOINT did not lower to exact SBLR_TRANSACTION_CREATE_SAVEPOINT");
  Require(Contains(savepoint, "RESULT transaction.create_savepoint"),
          "SAVEPOINT did not execute through engine savepoint API");
  Require(Contains(savepoint, "evidence=mga_savepoint:savepoint_create"),
          "SAVEPOINT did not return MGA savepoint create evidence");
  Require(Contains(savepoint, "evidence=savepoint_name_bound"),
          "SAVEPOINT did not return bound savepoint-name evidence");
  Require(!Contains(savepoint, "WAL") && !Contains(savepoint, "wal_required=true"),
          "SAVEPOINT route unexpectedly exposed WAL authority evidence");

  const auto rollback_to = ReadCommandResponse(fd,
                                               "EXECUTE ROLLBACK TO SAVEPOINT route_sp",
                                               "evidence=savepoint_name_bound",
                                               24);
  RequireContains(rollback_to, "\"surface_key\":\"SBSQL-3BF8303CFB36\"",
                  "ROLLBACK TO SAVEPOINT did not bind the rollback_to_savepoint_stmt grammar row");
  RequireContains(rollback_to, "\"sblr_operation\":\"SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT\"",
                  "ROLLBACK TO SAVEPOINT did not lower to exact SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT");
  Require(Contains(rollback_to, "RESULT transaction.rollback_to_savepoint"),
          "ROLLBACK TO SAVEPOINT did not execute through engine savepoint API");
  Require(Contains(rollback_to, "evidence=mga_savepoint:savepoint_rollback"),
          "ROLLBACK TO SAVEPOINT did not return MGA savepoint rollback evidence");

  const auto release = ReadCommandResponse(fd, "EXECUTE RELEASE SAVEPOINT route_sp", "evidence=savepoint_name_bound", 24);
  RequireContains(release, "\"surface_key\":\"SBSQL-9E33ED8C3B3D\"",
                  "RELEASE SAVEPOINT did not bind the release_savepoint_stmt grammar row");
  RequireContains(release, "\"sblr_operation\":\"SBLR_TRANSACTION_RELEASE_SAVEPOINT\"",
                  "RELEASE SAVEPOINT did not lower to exact SBLR_TRANSACTION_RELEASE_SAVEPOINT");
  Require(Contains(release, "RESULT transaction.release_savepoint"),
          "RELEASE SAVEPOINT did not execute through engine savepoint API");
  Require(Contains(release, "evidence=mga_savepoint:savepoint_release"),
          "RELEASE SAVEPOINT did not return MGA savepoint release evidence");

  const auto savepoint_stmt_invalid = ReadCommandResponse(fd, "EXECUTE SAVEPOINT", "MESSAGE ERROR", 12);
  RequireContains(savepoint_stmt_invalid, "\"surface_key\":\"SBSQL-35C5F6EA0613\"",
                  "invalid SAVEPOINT did not bind the savepoint_stmt grammar row");
  RequireContains(savepoint_stmt_invalid, "\"sblr_operation\":\"SBLR_TRANSACTION_CREATE_SAVEPOINT\"",
                  "invalid SAVEPOINT did not lower to exact SBLR_TRANSACTION_CREATE_SAVEPOINT before refusal");
  Require(Contains(savepoint_stmt_invalid, "SB_ENGINE_API_INVALID_REQUEST"),
          "SAVEPOINT without a name did not return the exact engine invalid-request refusal");
  Require(Contains(savepoint_stmt_invalid, "savepoint_name_required"),
          "SAVEPOINT without a name did not return savepoint_name_required detail");

  const auto missing_rollback_to = ReadCommandResponse(fd,
                                                       "EXECUTE ROLLBACK TO SAVEPOINT missing_sp",
                                                       "MESSAGE ERROR",
                                                       12);
  Require(Contains(missing_rollback_to, "SB_ENGINE_API_INVALID_REQUEST"),
          "ROLLBACK TO missing savepoint did not return engine invalid-request refusal");
  Require(Contains(missing_rollback_to, "savepoint_not_found"),
          "ROLLBACK TO missing savepoint did not return savepoint_not_found detail");

  const auto missing_release = ReadCommandResponse(fd,
                                                   "EXECUTE RELEASE SAVEPOINT missing_sp",
                                                   "MESSAGE ERROR",
                                                   12);
  Require(Contains(missing_release, "SB_ENGINE_API_INVALID_REQUEST"),
          "RELEASE missing savepoint did not return engine invalid-request refusal");
  Require(Contains(missing_release, "savepoint_not_found"),
          "RELEASE missing savepoint did not return savepoint_not_found detail");

  const auto rollback_savepoint_tx = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "evidence=always_active_transaction_replacement:", 24);
  Require(Contains(rollback_savepoint_tx, "RESULT transaction.rollback"),
          "savepoint transaction cleanup rollback did not execute");

  const auto replacement_rollback = ReadCommandResponse(fd, "EXECUTE ROLLBACK", "evidence=always_active_transaction_replacement:", 24);
  Require(Contains(replacement_rollback, "RESULT transaction.rollback"),
          "ROLLBACK after replacement did not execute through engine transaction operation");
  Require(Contains(replacement_rollback, "evidence=transaction_state:rolled_back"),
          "ROLLBACK after replacement did not return rolled-back MGA state evidence");

  ::close(fd);
  StopProcess(listener_pid);
  g_listener_pid = 0;
  StopProcess(server_pid);
  g_server_pid = 0;
  RequireInventoryFinality(database_path);
  std::cout << "sbsql_mga_transaction_full_route_conformance=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
