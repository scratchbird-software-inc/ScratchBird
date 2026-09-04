// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "uuid.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
constexpr char kVerifier[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAliceUuid = "019f0a22-ce00-7000-8000-000000000101";
constexpr std::string_view kSysdbaUuid = "019f0a22-ce00-7000-8000-000000000102";

bool CreateFixtureDatabase(const std::filesystem::path& path) {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("server_restart_killed_listener_smoke");
  db::DatabaseCreateConfig create;
  create.path = path.string();
  const auto du = uuid::GenerateEngineIdentityV7(UuidKind::database, 1780700000000);
  const auto fu = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780700000001);
  if (!du.ok() || !fu.ok()) return false;
  create.database_uuid = du.value; create.filespace_uuid = fu.value;
  create.page_size = 16384; create.creation_unix_epoch_millis = 1780700000000;
  create.allow_minimal_resource_bootstrap = true; create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "fixture_sysarch";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0358b60b6875c81e17d3e0ab67f8b785f49d4146547c79da401f21dc641c2c16";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = true;
  if (!db::CreateDatabaseFile(create).ok()) return false;
  const auto database_uuid = uuid::UuidToString(create.database_uuid.value);
  const auto bootstrap =
      scratchbird::tests::database_lifecycle::BeginDurableBootstrapTransaction(
          path, "server_managed_listener_smoke");
  const auto tx_uuid = bootstrap.transaction_uuid.canonical;
  const auto tx_id = bootstrap.local_transaction_id;
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      path, database_uuid, kAliceUuid, "alice", kVerifier, tx_id,
      "server_managed_listener_smoke:alice", tx_uuid);
  scratchbird::tests::database_lifecycle::GrantDurablePrincipalPrivilege(
      path, database_uuid, kAliceUuid, database_uuid, "database", "CONNECT", tx_id,
      "server_managed_listener_smoke:alice-connect", tx_uuid);
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      path, database_uuid, kSysdbaUuid, "sysdba", kVerifier, tx_id,
      "server_managed_listener_smoke:sysdba", tx_uuid);
  scratchbird::tests::database_lifecycle::GrantDurablePrincipalPrivilege(
      path, database_uuid, kSysdbaUuid, database_uuid, "database", "CONNECT", tx_id,
      "server_managed_listener_smoke:sysdba-connect", tx_uuid);
  scratchbird::tests::database_lifecycle::GrantDurablePrincipalPrivilege(
      path, database_uuid, kSysdbaUuid, "", "server_management", "OBS_MANAGEMENT_CONTROL", tx_id,
      "server_managed_listener_smoke:sysdba-management", tx_uuid);
  scratchbird::tests::database_lifecycle::CommitDurableBootstrapTransaction(
      bootstrap);
  return true;
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
      line->push_back(ch);
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sbsl.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool WriteConfig(const std::filesystem::path& config_path,
                 const std::filesystem::path& work,
                 const std::filesystem::path& listener,
                 const std::filesystem::path& parser,
                 int port) {
  const auto data_dir = work / "d";
  const auto control_dir = work / "c";
  const auto listener_control_dir = work / "lc";
  const auto listener_runtime_dir = work / "lr";
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) return false;
  std::filesystem::create_directories(control_dir, ec);
  if (ec) return false;
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) return false;
  out << "[config]\n";
  out << "format=SBCD1\n\n";
  out << "[server]\n";
  out << "mode=foreground\n\n";
  out << "[server.runtime]\n";
  out << "data_dir=" << data_dir.string() << "\n";
  out << "control_dir=" << control_dir.string() << "\n\n";
  out << "[server.database]\n";
  out << "default_path=" << (work / "t.sbdb").string() << "\n";
  out << "auto_create=false\n";
  out << "open_mode=normal\n\n";
  out << "[server.parser]\n";
  out << "sbps_enabled=true\n";
  out << "sbps_endpoint=" << (control_dir / "sb_server.sbps.sock").string() << "\n\n";
  out << "[server.listener]\n";
  out << "executable_path=" << listener.string() << "\n";
  out << "control_dir=" << listener_control_dir.string() << "\n";
  out << "runtime_dir=" << listener_runtime_dir.string() << "\n\n";
  out << "[server.listener.profile.echo_fixture]\n";
  out << "enabled=true\n";
  out << "protocol_family=fixture.echo.wire\n";
  out << "profile_id=fixture.echo.profile\n";
  out << "parser_package=fixture.echo.package\n";
  out << "parser_package_uuid=fixture-echo-package-v1\n";
  out << "dialect_profile_uuid=fixture-echo-dialect-v1\n";
  out << "bundle_contract_id=bundle.default@1\n";
  out << "parser_api_major=1\n";
  out << "parser_executable_path=" << parser.string() << "\n";
  out << "bind_address=127.0.0.1\n";
  out << "port=" << port << "\n";
  out << "database_selector=dev_bootstrap_path:" << (work / "t.sbdb").string() << "\n";
  out << "sbps_endpoint=" << (control_dir / "sb_server.sbps.sock").string() << "\n";
  out << "tls_required=false\n";
  out << "ready_timeout_ms=8000\n";
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sb_server_managed_listener_smoke <sb_server> <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path server = argv[1];
  const std::filesystem::path listener = argv[2];
  const std::filesystem::path parser = argv[3];
  ::setenv("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX",
           "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
           1);
  const auto work = MakeTempDir();
  if (work.empty()) {
    std::cerr << "could not create temp dir\n";
    return EXIT_FAILURE;
  }
  const int port = FindFreePort();
  if (port <= 0) {
    std::cerr << "could not allocate test port\n";
    return EXIT_FAILURE;
  }
  const auto config_path = work / "sb_server.conf";
  if (!WriteConfig(config_path, work, listener, parser, port)) {
    std::cerr << "could not write server config\n";
    return EXIT_FAILURE;
  }
  if (!CreateFixtureDatabase(work / "t.sbdb")) {
    std::cerr << "could not create fixture database\n";
    return EXIT_FAILURE;
  }

  const auto stdout_path = work / "server.out";
  const auto stderr_path = work / "server.err";
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
    ::execl(server.c_str(), server.c_str(), "--config", config_path.c_str(), "--foreground", nullptr);
    _exit(127);
  }
  if (pid <= 0) {
    std::cerr << "could not fork sb_server\n";
    return EXIT_FAILURE;
  }

  auto cleanup = [&] {
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 150; ++i) {
      const auto rc = ::waitpid(pid, &status, WNOHANG);
      if (rc == pid) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  };

  int fd = -1;
  for (int i = 0; i < 200; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      std::cerr << "sb_server exited before listener accepted connections\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) {
    cleanup();
    std::cerr << "server-managed listener did not accept connections\n";
    return EXIT_FAILURE;
  }

  std::string line;
  if (!ReadLine(fd, &line) || line != "ScratchBird dummy parser ready") {
    cleanup();
    std::cerr << "unexpected greeting: " << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "server-managed-listener-smoke\n") ||
      !ReadLine(fd, &line) ||
      line != "server-managed-listener-smoke") {
    cleanup();
    std::cerr << "echo failed: " << line << '\n';
    return EXIT_FAILURE;
  }
  ::close(fd);
  cleanup();
  std::cout << "server_managed_listener_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
