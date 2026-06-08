// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
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

constexpr std::string_view kAliceVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kAlicePrincipalUuid =
    "019f0a11-ce00-7000-8000-000000000001";

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
  timeout.tv_sec = 15;
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbp_sbsql_full_route.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool WaitForPath(const std::filesystem::path& path) {
  for (int i = 0; i < 120; ++i) {
    if (std::filesystem::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool FileContains(const std::filesystem::path& path, const std::string& needle) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool WaitForFileContains(const std::filesystem::path& path, const std::string& needle) {
  for (int i = 0; i < 120; ++i) {
    if (FileContains(path, needle)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

bool WriteLocalPasswordAuthStore(const std::filesystem::path& database_path) {
  std::ofstream out(database_path.string() + ".sb.local_password_auth", std::ios::trunc);
  if (!out) return false;
  out << "alice\tlocal_password\t" << kAliceVerifier << '\n';
  return static_cast<bool>(out);
}

std::string LocalPasswordEvidence(std::string_view verifier) {
  return "scheme=local_password_v1;principal=alice;principal_uuid=" +
         std::string(kAlicePrincipalUuid) +
         ";storage_authority=mga_security_principal_lifecycle;"
         "authorization_tags=right:CONNECT;verifier=" +
         std::string(verifier);
}

bool RunExampleDatabaseSeeder(const std::filesystem::path& seeder,
                              const std::filesystem::path& database_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const std::string user = "alice";
    const std::string verifier(kAliceVerifier);
    ::execl(seeder.c_str(),
            seeder.c_str(),
            database_path.c_str(),
            user.c_str(),
            verifier.c_str(),
            nullptr);
    _exit(127);
  }
  if (pid <= 0) return false;
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ExecuteAndExpectResult(int fd,
                            std::string_view sql,
                            std::string_view operation_id,
                            std::string* line) {
  std::string command = "EXECUTE ";
  command += sql;
  command += '\n';
  if (!WriteAll(fd, command)) return false;
  const std::string result_prefix = "RESULT " + std::string(operation_id) + " ";
  for (int i = 0; i < 10 && ReadLine(fd, line); ++i) {
    if (line->starts_with(result_prefix)) {
      if (!operation_id.starts_with("transaction.")) return true;
      for (int drain = 0; drain < 20 && ReadLine(fd, line); ++drain) {
        if (line->starts_with("transaction_uuid=")) return true;
        if (line->starts_with("MESSAGE ")) return false;
      }
      return false;
    }
    if (line->starts_with("MESSAGE ")) return false;
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
    const std::string control_arg = "--control-dir";
    const std::string runtime_arg = "--runtime-dir";
    const std::string database_arg = "--database";
    const std::string endpoint_arg = "--sbps-endpoint";
    ::execl(server.c_str(),
            server.c_str(),
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            control_arg.c_str(),
            control_dir.c_str(),
            runtime_arg.c_str(),
            runtime_dir.c_str(),
            database_arg.c_str(),
            database_path.c_str(),
            endpoint_arg.c_str(),
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
                     const std::filesystem::path& database_path,
                     const std::filesystem::path& endpoint,
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
    const std::string database_selector_arg = "--database-selector=dev_bootstrap_path:" + database_path.string();
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            database_selector_arg.c_str(),
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

} // namespace

int main(int argc, char** argv) {
  ::alarm(75);
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: sbp_sbsql_full_route_execution_smoke <sb_server> <sb_listener> <sbp_sbsql> [sbsql_example_database_seed]\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path server = argv[1];
  const std::filesystem::path listener = argv[2];
  const std::filesystem::path parser = argv[3];
  const std::filesystem::path example_database_seed = argc == 5 ? std::filesystem::path(argv[4])
                                                                 : std::filesystem::path{};
  const auto work = MakeTempDir();
  if (work.empty()) return EXIT_FAILURE;
  const auto server_control = work / "server-control";
  const auto server_runtime = work / "server-runtime";
  const auto listener_control = work / "listener-control";
  const auto listener_runtime = work / "listener-runtime";
  const auto database_path = work / "full_route.sbdb";
  const auto endpoint = server_control / "sb_server.sbps.sock";
  const int port = FindFreePort();
  if (port <= 0) return EXIT_FAILURE;
  const bool copy_fixture_seeded = !example_database_seed.empty();
  if (copy_fixture_seeded && !RunExampleDatabaseSeeder(example_database_seed, database_path)) {
    std::cerr << "failed to seed example database fixture using " << example_database_seed
              << " under " << work << '\n';
    return EXIT_FAILURE;
  }
  if (!copy_fixture_seeded && !WriteLocalPasswordAuthStore(database_path)) {
    std::cerr << "failed to create database-local password verifier store under " << work << '\n';
    return EXIT_FAILURE;
  }

  const pid_t server_pid = LaunchServer(server,
                                        server_control,
                                        server_runtime,
                                        database_path,
                                        endpoint,
                                        work / "server.out",
                                        work / "server.err");
  if (server_pid <= 0) return EXIT_FAILURE;
  if (!WaitForPath(endpoint)) {
    StopProcess(server_pid);
    std::cerr << "sb_server did not create SBPS endpoint under " << work << '\n';
    return EXIT_FAILURE;
  }

  const pid_t listener_pid = LaunchListener(listener,
                                            parser,
                                            listener_control,
                                            listener_runtime,
                                            database_path,
                                            endpoint,
                                            port,
                                            work / "listener.out",
                                            work / "listener.err");
  if (listener_pid <= 0) {
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }

  int fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "listener did not accept client connection under " << work << '\n';
    return EXIT_FAILURE;
  }

  std::string line;
  if (!ReadLine(fd, &line) || line != "ScratchBird SBSQL parser ready") {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "unexpected parser greeting: " << line << '\n';
    return EXIT_FAILURE;
  }
  const std::string invalid_auth =
      "AUTH alice " + LocalPasswordEvidence(kWrongVerifier) + "\n";
  if (!WriteAll(fd, invalid_auth)) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_invalid_auth_rejection = false;
  for (int i = 0; i < 6 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MESSAGE ERROR SECURITY.AUTHENTICATION")) {
      saw_invalid_auth_rejection = true;
      break;
    }
    if (line == "OK AUTHENTICATED") break;
  }
  if (!saw_invalid_auth_rejection) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "invalid credential evidence was not rejected by engine auth, last line: "
              << line << '\n';
    return EXIT_FAILURE;
  }
  ::close(fd);
  fd = -1;
  for (int i = 0; i < 120; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0 || !ReadLine(fd, &line) || line != "ScratchBird SBSQL parser ready") {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "listener did not accept a second client connection under " << work << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "EXECUTE SELECT 1\n")) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_preauth_execute_refusal = false;
  for (int i = 0; i < 6 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MESSAGE ERROR SBSQL.AUTH.REQUIRED")) {
      saw_preauth_execute_refusal = true;
      break;
    }
    if (line.starts_with("RESULT ")) break;
  }
  if (!saw_preauth_execute_refusal) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "pre-auth execute did not fail closed, last line: " << line << '\n';
    return EXIT_FAILURE;
  }
  const std::string valid_auth =
      "AUTH alice " + LocalPasswordEvidence(kAliceVerifier) + "\n";
  if (!WriteAll(fd, valid_auth)) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool authenticated = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK AUTHENTICATED") {
      authenticated = true;
      break;
    }
  }
  if (!authenticated) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "authentication did not complete, last line: " << line << '\n';
    return EXIT_FAILURE;
  }
  if (!ExecuteAndExpectResult(fd, "BEGIN", "transaction.begin", &line)) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "full route did not begin an MGA transaction after auth under "
              << work << " last_line=" << line << '\n';
    return EXIT_FAILURE;
  }
  const std::string routed_select_sql = copy_fixture_seeded
      ? "SELECT * FROM users.public.sbsfc021_stream_table"
      : "SELECT * FROM sys.version";
  if (!WriteAll(fd, "EXECUTE " + routed_select_sql + "\n")) {
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_prepared = false;
  bool saw_result = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("PREPARED sblr.query.relational.v3")) saw_prepared = true;
    if (line.starts_with("RESULT dml.select_rows 1 ")) {
      saw_result = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_prepared || !saw_result) {
    std::cerr << "full route did not return prepared/result lines under " << work
              << " prepared=" << saw_prepared << " result=" << saw_result
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "EXECUTE " + routed_select_sql + "\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_name_prepared = false;
  bool saw_name_result = false;
  for (int i = 0; i < 10 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("PREPARED sblr.query.relational.v3")) saw_name_prepared = true;
    if (line.starts_with("RESULT dml.select_rows 1 ")) {
      saw_name_result = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_name_prepared || !saw_name_result) {
    std::cerr << "full route did not resolve visible seeded object under " << work
              << " prepared=" << saw_name_prepared << " result=" << saw_name_result
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "STREAM 5 " + routed_select_sql + "\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_stream_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("CURSOR ")) {
      saw_stream_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_stream_cursor) {
    std::cerr << "full route did not open streaming cursor under " << work
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1024\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_fetch_limit_error = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MESSAGE ERROR SERVER.STREAM.CHUNK_TOO_LARGE")) {
      saw_fetch_limit_error = true;
      break;
    }
    if (line.starts_with("FETCH ")) break;
  }
  if (!saw_fetch_limit_error) {
    std::cerr << "full route did not return streaming chunk-limit diagnostic under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 2\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_fetch_first = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") && line.find(" 2 end=false ") != std::string::npos &&
        line.find("\"row_index\":0") != std::string::npos &&
        line.find("\"row_index\":1") != std::string::npos) {
      saw_fetch_first = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_fetch_first) {
    std::cerr << "full route did not fetch first streaming chunk under " << work
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 3\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_fetch_final = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") && line.find(" 3 end=true ") != std::string::npos &&
        line.find("\"row_index\":2") != std::string::npos &&
        line.find("\"row_index\":4") != std::string::npos) {
      saw_fetch_final = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_fetch_final) {
    std::cerr << "full route did not fetch final streaming chunk under " << work
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CLOSE CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_cursor_closed = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK CURSOR_CLOSED") {
      saw_cursor_closed = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_cursor_closed) {
    std::cerr << "full route did not close streaming cursor under " << work
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!ExecuteAndExpectResult(fd, "COMMIT", "transaction.commit", &line)) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "full route did not commit the initial MGA transaction under "
              << work << " last_line=" << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "ENGINE STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_engine_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("CURSOR ") && line.find(" source=engine") != std::string::npos) {
      saw_engine_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_engine_cursor) {
    std::cerr << "full route did not open engine-backed streaming cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_engine_fetch_header = false;
  bool saw_engine_fetch_product = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find(" 1 end=true ") != std::string::npos &&
        line.find("detail={\"cursor_metadata\"") != std::string::npos &&
        line.find("operation_id=observability.show_version") != std::string::npos) {
      saw_engine_fetch_header = true;
      continue;
    }
    if (line.find("product=ScratchBird") != std::string::npos) {
      saw_engine_fetch_product = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_engine_fetch_header || !saw_engine_fetch_product) {
    std::cerr << "full route did not render engine-backed stream metadata/payload under "
              << work << " header=" << saw_engine_fetch_header
              << " product=" << saw_engine_fetch_product
              << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CLOSE CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_engine_cursor_closed = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK CURSOR_CLOSED") {
      saw_engine_cursor_closed = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_engine_cursor_closed) {
    std::cerr << "full route did not close engine-backed streaming cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "SBPS CHUNKED EXECUTE\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_chunked_execute = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("CHUNKED_EXECUTE accepted ") &&
        line.find("row_count=24000") != std::string::npos &&
        line.find("last_row=true") != std::string::npos) {
      saw_chunked_execute = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_chunked_execute) {
    std::cerr << "full route did not round-trip chunked SBPS request/response under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (copy_fixture_seeded) {
  if (!WriteAll(fd, "COPY STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_copy_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("COPY_CURSOR ") &&
        line.find(" events=4") != std::string::npos &&
        line.find(" source=engine") != std::string::npos &&
        line.find("operation_id=dml.execute_import_rows") != std::string::npos &&
        line.find("committed=true") != std::string::npos) {
      saw_copy_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_copy_cursor) {
    std::cerr << "full route did not open COPY streaming cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 2\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_copy_progress = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"progress\"") != std::string::npos &&
        line.find("\"event\":\"reject_record\"") != std::string::npos &&
        line.find("\"rows_processed\":2") != std::string::npos &&
        line.find("\"rows_rejected\":1") != std::string::npos &&
        (line.find("\"diagnostic_detail\":\"crud.unique_index:unique_index_duplicate\"") != std::string::npos ||
         line.find("\"diagnostic_detail\":\"constraint.primary_key.violation:duplicate_key") != std::string::npos ||
         line.find("\"diagnostic_detail\":\"constraint.unique.violation:duplicate_key") != std::string::npos ||
         line.find("\"diagnostic_detail\":\"duplicate_key:") != std::string::npos) &&
        line.find("\"value_redacted\":true") != std::string::npos) {
      saw_copy_progress = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_copy_progress) {
    std::cerr << "full route did not render COPY progress/reject records under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 3\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_copy_summary = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"bulk_summary\"") != std::string::npos &&
        line.find("\"event\":\"final_status\"") != std::string::npos &&
        line.find("\"accepted_rows\":1") != std::string::npos &&
        line.find("\"rejected_rows\":1") != std::string::npos &&
        line.find("\"status\":\"completed_with_rejects\"") != std::string::npos &&
        line.find("end=true") != std::string::npos) {
      saw_copy_summary = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_copy_summary) {
    std::cerr << "full route did not render COPY summary/final status under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CLOSE CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_copy_cursor_closed = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK CURSOR_CLOSED") {
      saw_copy_cursor_closed = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_copy_cursor_closed) {
    std::cerr << "full route did not close COPY streaming cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  }
  if (!WriteAll(fd, "MULTI RESULT\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_multi_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MULTI_CURSOR ") && line.find(" events=7") != std::string::npos) {
      saw_multi_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_multi_cursor) {
    std::cerr << "full route did not open multi-result cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 2\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_multi_first = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"result_set_metadata\"") != std::string::npos &&
        line.find("\"event\":\"command_tag\"") != std::string::npos &&
        line.find("\"tag\":\"SELECT 1\"") != std::string::npos) {
      saw_multi_first = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_multi_first) {
    std::cerr << "full route did not render first multi-result metadata/tag under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 4\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_multi_middle = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"tag\":\"SELECT 3\"") != std::string::npos &&
        line.find("\"event\":\"command_tag\"") != std::string::npos &&
        line.find("end=false") != std::string::npos) {
      saw_multi_middle = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_multi_middle) {
    std::cerr << "full route did not render middle multi-result sequence under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_multi_final = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"multi_result_finality\"") != std::string::npos &&
        line.find("end=true") != std::string::npos) {
      saw_multi_final = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_multi_final) {
    std::cerr << "full route did not render multi-result finality under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CLOSE CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_multi_cursor_closed = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK CURSOR_CLOSED") {
      saw_multi_cursor_closed = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_multi_cursor_closed) {
    std::cerr << "full route did not close multi-result cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "WARNING STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_warning_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("WARNING_CURSOR ") && line.find(" events=6") != std::string::npos) {
      saw_warning_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_warning_cursor) {
    std::cerr << "full route did not open warning/partial-result cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 3\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_partial_rows = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"partial_result_row\"") != std::string::npos &&
        line.find("\"partial_result\":true") != std::string::npos &&
        line.find("end=false") != std::string::npos) {
      saw_partial_rows = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_partial_rows) {
    std::cerr << "full route did not render warning stream partial rows under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 2\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_warning_chain = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"warning\"") != std::string::npos &&
        line.find("\"diagnostic_code\":\"STREAM.WARNING.0\"") != std::string::npos &&
        line.find("\"does_not_abort\":true") != std::string::npos) {
      saw_warning_chain = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_warning_chain) {
    std::cerr << "full route did not render non-aborting warning chain under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_warning_finality = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"event\":\"partial_result_finality\"") != std::string::npos &&
        line.find("\"status\":\"completed_with_warnings\"") != std::string::npos &&
        line.find("end=true") != std::string::npos) {
      saw_warning_finality = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_warning_finality) {
    std::cerr << "full route did not render warning/partial-result finality under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CLOSE CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_warning_cursor_closed = false;
  for (int i = 0; i < 4 && ReadLine(fd, &line); ++i) {
    if (line == "OK CURSOR_CLOSED") {
      saw_warning_cursor_closed = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_warning_cursor_closed) {
    std::cerr << "full route did not close warning/partial-result cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "TIMEOUT STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_timeout_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("TIMEOUT_CURSOR ") && line.find(" events=2") != std::string::npos) {
      saw_timeout_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_timeout_cursor) {
    std::cerr << "full route did not open timeout stream cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_timeout_initial_row = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"row_index\":0") != std::string::npos &&
        line.find("end=false") != std::string::npos) {
      saw_timeout_initial_row = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_timeout_initial_row) {
    std::cerr << "full route did not render timeout stream initial row under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_timeout_finality = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MESSAGE ERROR SERVER.STREAM.TIMEOUT")) {
      saw_timeout_finality = true;
      break;
    }
    if (line.starts_with("FETCH ")) break;
  }
  if (!saw_timeout_finality) {
    std::cerr << "full route did not return deterministic stream timeout diagnostic under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "DRAIN STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_drain_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("DRAIN_CURSOR ") && line.find(" events=2") != std::string::npos) {
      saw_drain_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_drain_cursor) {
    std::cerr << "full route did not open drain stream cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "FETCH 1\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_drain_finality = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("FETCH ") &&
        line.find("\"stream_finality\"") != std::string::npos &&
        line.find("\"state\":\"drained\"") != std::string::npos &&
        line.find("end=true") != std::string::npos) {
      saw_drain_finality = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_drain_finality) {
    std::cerr << "full route did not render deterministic stream drain finality under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CANCEL STREAM\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_cancel_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("CANCEL_CURSOR ") && line.find(" events=2") != std::string::npos) {
      saw_cancel_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_cancel_cursor) {
    std::cerr << "full route did not open cancel stream cursor under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "CANCEL CURSOR\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_cancel_finality = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("OK CURSOR_CANCELLED") &&
        line.find("\"state\":\"cancelled\"") != std::string::npos) {
      saw_cancel_finality = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_cancel_finality) {
    std::cerr << "full route did not return deterministic stream cancel finality under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "EXECUTE SELECT * FROM hidden_table\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_hidden_safe_error = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("MESSAGE ERROR SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE")) {
      saw_hidden_safe_error = true;
      break;
    }
    if (line.starts_with("RESULT ")) break;
  }
  if (!saw_hidden_safe_error) {
    std::cerr << "full route did not return safe hidden/missing name error under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  if (!ExecuteAndExpectResult(fd, "BEGIN", "transaction.begin", &line)) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    std::cerr << "full route did not begin a disconnect-cleanup MGA transaction under "
              << work << " last_line=" << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "STREAM 3 " + routed_select_sql + "\n")) {
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  bool saw_disconnect_cursor = false;
  for (int i = 0; i < 8 && ReadLine(fd, &line); ++i) {
    if (line.starts_with("CURSOR ")) {
      saw_disconnect_cursor = true;
      break;
    }
    if (line.starts_with("MESSAGE ")) break;
  }
  if (!saw_disconnect_cursor) {
    std::cerr << "full route did not open cursor for disconnect cleanup under "
              << work << " last_line=" << line << '\n';
    ::close(fd);
    StopProcess(listener_pid);
    StopProcess(server_pid);
    return EXIT_FAILURE;
  }
  ::close(fd);
  const bool saw_disconnect_notice =
      WaitForFileContains(server_control / "sb_server.audit.jsonl", "server.disconnect_notice");
  StopProcess(listener_pid);
  StopProcess(server_pid);
  if (!saw_disconnect_notice) {
    std::cerr << "full route did not emit parser disconnect notice under " << work << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "sbp_sbsql_full_route_execution_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
