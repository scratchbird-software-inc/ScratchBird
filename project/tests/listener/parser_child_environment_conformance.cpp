// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr char kReportSelectorPrefix[] = "environment-report:";

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

std::uint64_t ParseU64(const char* value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != nullptr && *end == '\0'
             ? static_cast<std::uint64_t>(parsed)
             : fallback;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

#ifndef _WIN32
int RunWorker() {
  const int control_fd =
      static_cast<int>(ParseU64(std::getenv("SB_LISTENER_CONTROL_FD"), 0));
  if (control_fd <= 0) return EXIT_FAILURE;

  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = Env("SB_PROTOCOL_FAMILY");
  hello.pid = static_cast<std::uint32_t>(::getpid());
  hello.worker_id =
      ParseU64(std::getenv("SB_PARSER_WORKER_NUMERIC_ID"), 1);
  hello.dialect_protocol_version = 1;
  hello.parser_api_major = static_cast<std::uint32_t>(
      ParseU64(std::getenv("SB_PARSER_API_MAJOR"), 0));
  hello.profile_id = Env("SB_PARSER_PROFILE_ID");
  hello.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID");

  scratchbird::listener::ListenerControlFrame hello_frame;
  hello_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHello;
  hello_frame.sequence = hello.worker_id;
  hello_frame.payload = scratchbird::listener::EncodeHelloPayload(hello);
  if (!scratchbird::listener::SendControlFrame(control_fd, hello_frame)) {
    return EXIT_FAILURE;
  }

  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(control_fd, &decoded,
                                               &received_fd, 5000) ||
      decoded.frame.opcode !=
          scratchbird::listener::ListenerControlOpcode::kHelloAck) {
    if (received_fd >= 0) ::close(received_fd);
    return EXIT_FAILURE;
  }
  if (received_fd >= 0) ::close(received_fd);
  const auto ack = scratchbird::listener::DecodeHelloAckPayload(
      decoded.frame.payload, &decoded.messages);
  if (!ack || !ack->accepted) return EXIT_FAILURE;

  const std::string selector = Env("SB_DATABASE_SELECTOR");
  if (!selector.starts_with(kReportSelectorPrefix)) return EXIT_FAILURE;
  const std::filesystem::path report_path =
      selector.substr(std::string(kReportSelectorPrefix).size());

  static constexpr const char* kObservedNames[] = {
      "PATH",
      "TMPDIR",
      "LANG",
      "HOME",
      "SB_LISTENER_PREAUTH",
      "SB_LISTENER_CONTROL_FD",
      "SB_LISTENER_CONTROL_TRANSPORT",
      "SB_SERVER_ENDPOINT",
      "SB_DATABASE_SELECTOR",
      "SB_DATABASE_TOKEN",
      "SB_PROTOCOL_FAMILY",
      "SB_PARSER_PACKAGE",
      "SB_PARSER_PACKAGE_UUID",
      "SB_PARSER_PROFILE_ID",
      "SB_LISTENER_PROFILE_UUID",
      "SB_DIALECT_PROFILE_UUID",
      "SB_PARSER_BUNDLE_CONTRACT_ID",
      "SB_PARSER_API_MAJOR",
      "SB_PARSER_API_MINOR",
      "SB_LISTENER_UUID",
      "SB_LISTENER_LIFECYCLE_GENERATION",
      "SB_LISTENER_CONTROLLER_TYPE",
      "SB_LISTENER_CONTROLLER_UUID",
      "SB_LISTENER_RUNTIME_DIR",
      "SB_COMPATIBILITY_AUTH_PASSWORD",
      "SB_COMPATIBILITY_AUTH_VERIFIER",
      "SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID",
      "SB_REFERENCE_FIREBIRD_PASSWORD",
      "SB_REFERENCE_FIREBIRD_VERIFIER",
      "SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID",
      "SB_FIREBIRD_PARSER_PATH",
      "SB_POSTGRESQL_PARSER_PATH",
      "SB_MYSQL_PARSER_EXECUTABLE",
      "SB_SBSQL_PARSER_PATH",
      "SB_PARSER_DUMMY_BEHAVIOR",
      "ISC_PASSWORD",
      "PGPASSWORD",
      "MYSQL_PWD",
      "AWS_SECRET_ACCESS_KEY",
      "DATABASE_URL",
      "ARBITRARY_SECRET",
  };
  std::ofstream report(report_path, std::ios::trunc);
  if (!report) return EXIT_FAILURE;
  for (const char* name : kObservedNames) {
    const char* value = std::getenv(name);
    report << name << '=' << (value == nullptr ? "<UNSET>" : value) << '\n';
  }
  report.close();
  if (!report) return EXIT_FAILURE;

  // Remain a healthy idle parser until the listener terminates the pool.
  for (;;) {
    scratchbird::listener::ListenerControlDecodeResult inbound;
    int client_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(control_fd, &inbound,
                                                 &client_fd, 30000)) {
      if (client_fd >= 0) ::close(client_fd);
      return EXIT_SUCCESS;
    }
    if (client_fd >= 0) ::close(client_fd);
  }
}

std::filesystem::path MakeTempDir() {
  std::string pattern = "/tmp/sb_listener_child_environment_XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  char* path = ::mkdtemp(buffer.data());
  return path == nullptr ? std::filesystem::path{}
                         : std::filesystem::path(path);
}

int FindFreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t length = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
    ::close(fd);
    return -1;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

std::map<std::string, std::string> ReadReport(
    const std::filesystem::path& path) {
  std::map<std::string, std::string> values;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    const auto separator = line.find('=');
    if (separator != std::string::npos) {
      values.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
  }
  return values;
}

int RunConformance(const std::filesystem::path& listener,
                   const std::filesystem::path& parser_probe) {
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create child-environment test directory");
  const int port = FindFreePort();
  Require(port > 0, "could not reserve child-environment listener port");

  const auto report_path = work / "parser-environment.txt";
  const auto stdout_path = work / "listener.out";
  const auto stderr_path = work / "listener.err";
  const std::string selector =
      std::string(kReportSelectorPrefix) + report_path.string();

  const pid_t pid = ::fork();
  if (pid == 0) {
    ::setenv("PATH", "/fixture/runtime/path", 1);
    ::setenv("TMPDIR", work.c_str(), 1);
    ::setenv("LANG", "C", 1);
    ::setenv("HOME", "/hostile/home", 1);
    ::setenv("SB_COMPATIBILITY_AUTH_PASSWORD", "neutral-password", 1);
    ::setenv("SB_COMPATIBILITY_AUTH_VERIFIER", "neutral-verifier", 1);
    ::setenv("SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID",
             "11111111-2222-4333-8444-555555555555", 1);
    ::setenv("SB_REFERENCE_FIREBIRD_PASSWORD", "hostile-firebird", 1);
    ::setenv("SB_REFERENCE_FIREBIRD_VERIFIER", "hostile-verifier", 1);
    ::setenv("SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID", "hostile-uuid", 1);
    ::setenv("SB_FIREBIRD_PARSER_PATH", "/hostile/sbp_firebird", 1);
    ::setenv("SB_POSTGRESQL_PARSER_PATH", "/hostile/sbp_postgresql", 1);
    ::setenv("SB_MYSQL_PARSER_EXECUTABLE", "/hostile/sbp_mysql", 1);
    ::setenv("SB_SBSQL_PARSER_PATH", "/hostile/sbp_sbsql", 1);
    ::setenv("SB_PARSER_DUMMY_BEHAVIOR", "hostile-test-hook", 1);
    ::setenv("ISC_PASSWORD", "hostile-isc", 1);
    ::setenv("PGPASSWORD", "hostile-pg", 1);
    ::setenv("MYSQL_PWD", "hostile-mysql", 1);
    ::setenv("AWS_SECRET_ACCESS_KEY", "hostile-cloud-secret", 1);
    ::setenv("DATABASE_URL", "hostile-database-url", 1);
    ::setenv("ARBITRARY_SECRET", "hostile-arbitrary-secret", 1);

    const int out = ::creat(stdout_path.c_str(), 0600);
    const int err = ::creat(stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }

    const std::string port_arg = "--port=" + std::to_string(port);
    const std::string parser_arg =
        "--parser-executable=" + parser_probe.string();
    const std::string selector_arg = "--database-selector=" + selector;
    const std::string control_arg =
        "--control-dir=" + (work / "control").string();
    const std::string runtime_arg =
        "--runtime-dir=" + (work / "runtime").string();
    ::execl(listener.c_str(), listener.c_str(), "--foreground",
            "--protocol-family=Fixture.Future-Wire",
            "--parser-package=fixture.environment-probe",
            "--parser-package-uuid=aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "--listener-profile=fixture-profile",
            "--listener-profile-uuid=12345678-1234-4234-8234-123456789abc",
            "--dialect-profile-uuid=87654321-4321-4321-8321-cba987654321",
            "--listener-uuid=22222222-3333-4444-8555-666666666666",
            "--lifecycle-generation=7", "--controller-type=direct",
            "--controller-uuid=33333333-4444-4555-8666-777777777777",
            "--bundle-contract-id=fixture.environment@1",
            "--parser-api-major=1", "--parser-api-minor=9",
            selector_arg.c_str(),
            "--server-endpoint=unix:/tmp/fixture-environment.sbps.sock",
            parser_arg.c_str(), control_arg.c_str(), runtime_arg.c_str(),
            "--bind-address=127.0.0.1", port_arg.c_str(),
            "--warm-pool-min=1", "--warm-pool-max=1", nullptr);
    _exit(127);
  }
  Require(pid > 0, "could not launch listener for child-environment test");

  auto cleanup = [&] {
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; ++i) {
      const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
      if (reaped == pid) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
  };

  bool report_ready = false;
  for (int i = 0; i < 200; ++i) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(report_path, ec) && !ec &&
        std::filesystem::file_size(report_path, ec) > 0 && !ec) {
      report_ready = true;
      break;
    }
    if (::kill(pid, 0) != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (!report_ready) {
    cleanup();
    std::ifstream errors(stderr_path);
    std::cerr << "parser child did not emit environment report\n"
              << errors.rdbuf();
    return EXIT_FAILURE;
  }

  const auto environment = ReadReport(report_path);
  cleanup();

  const auto expect = [&](const std::string& name, const std::string& value) {
    const auto found = environment.find(name);
    Require(found != environment.end(), "environment report omitted " + name);
    Require(found->second == value,
            name + " mismatch: expected '" + value + "', got '" +
                found->second + "'");
  };
  const auto expect_unset = [&](const std::string& name) {
    expect(name, "<UNSET>");
  };

  expect("PATH", "/fixture/runtime/path");
  expect("TMPDIR", work.string());
  expect("LANG", "C");
  expect("SB_LISTENER_PREAUTH", "1");
  expect("SB_LISTENER_CONTROL_TRANSPORT", "posix-socketpair-v1");
  expect("SB_SERVER_ENDPOINT", "unix:/tmp/fixture-environment.sbps.sock");
  expect("SB_DATABASE_SELECTOR", selector);
  expect("SB_DATABASE_TOKEN", selector);
  expect("SB_PROTOCOL_FAMILY", "Fixture.Future-Wire");
  expect("SB_PARSER_PACKAGE", "fixture.environment-probe");
  expect("SB_PARSER_PACKAGE_UUID", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
  expect("SB_PARSER_PROFILE_ID", "fixture-profile");
  expect("SB_LISTENER_PROFILE_UUID",
         "12345678-1234-4234-8234-123456789abc");
  expect("SB_DIALECT_PROFILE_UUID",
         "87654321-4321-4321-8321-cba987654321");
  expect("SB_PARSER_BUNDLE_CONTRACT_ID", "fixture.environment@1");
  expect("SB_PARSER_API_MAJOR", "1");
  expect("SB_PARSER_API_MINOR", "9");
  expect("SB_LISTENER_UUID", "22222222-3333-4444-8555-666666666666");
  expect("SB_LISTENER_LIFECYCLE_GENERATION", "7");
  expect("SB_LISTENER_CONTROLLER_TYPE", "direct");
  expect("SB_LISTENER_CONTROLLER_UUID",
         "33333333-4444-4555-8666-777777777777");
  expect("SB_LISTENER_RUNTIME_DIR", (work / "runtime").string());
  expect("SB_COMPATIBILITY_AUTH_PASSWORD", "neutral-password");
  expect("SB_COMPATIBILITY_AUTH_VERIFIER", "neutral-verifier");
  expect("SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID",
         "11111111-2222-4333-8444-555555555555");

  for (const char* name : {
           "HOME",
           "SB_REFERENCE_FIREBIRD_PASSWORD",
           "SB_REFERENCE_FIREBIRD_VERIFIER",
           "SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID",
           "SB_FIREBIRD_PARSER_PATH",
           "SB_POSTGRESQL_PARSER_PATH",
           "SB_MYSQL_PARSER_EXECUTABLE",
           "SB_SBSQL_PARSER_PATH",
           "SB_PARSER_DUMMY_BEHAVIOR",
           "ISC_PASSWORD",
           "PGPASSWORD",
           "MYSQL_PWD",
           "AWS_SECRET_ACCESS_KEY",
           "DATABASE_URL",
           "ARBITRARY_SECRET",
       }) {
    expect_unset(name);
  }

  std::filesystem::remove_all(work);
  std::cout << "listener_parser_child_environment_conformance=passed\n";
  return EXIT_SUCCESS;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  (void)argc;
  (void)argv;
  std::cout << "listener_parser_child_environment_conformance=skipped_windows\n";
  return EXIT_SUCCESS;
#else
  if (argc == 2 && std::string(argv[1]) == "--listener-worker") {
    return RunWorker();
  }
  if (argc != 2) {
    std::cerr << "usage: sb_listener_parser_child_environment_conformance "
                 "<sb_listener>\n";
    return EXIT_FAILURE;
  }
  return RunConformance(argv[1], argv[0]);
#endif
}
