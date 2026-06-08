// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_ltls.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sb_listener_tls_required_fail_closed_smoke <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");

  const auto control_dir = work / "c";
  const auto runtime_dir = work / "r";
  const auto stdout_path = work / "listener.out";
  const auto stderr_path = work / "listener.err";

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
    const std::string parser_arg = "--parser-executable=" + parser.string();
    const std::string control_arg = "--control-dir=" + control_dir.string();
    const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            "--database-selector=dev_bootstrap_path:/tmp/sb_ltls.sbdb",
            "--server-endpoint=unix:/tmp/sb_ltls.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            "--port=0",
            "--tls-required=true",
            nullptr);
    _exit(127);
  }
  Require(pid > 0, "could not fork listener");

  int status = 0;
  bool exited = false;
  for (int i = 0; i < 100; ++i) {
    const auto rc = ::waitpid(pid, &status, WNOHANG);
    if (rc == pid) {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!exited) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    std::cerr << "listener did not fail closed for tls_required=true\n";
    return EXIT_FAILURE;
  }

  Require(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
          "listener exited successfully even though tls_required=true has no provider");
  const auto stderr_text = ReadFile(stderr_path);
  Require(stderr_text.find("LISTENER.TLS_POLICY_FAILED") != std::string::npos,
          "listener did not emit LISTENER.TLS_POLICY_FAILED");
  std::cout << "tls_required_fail_closed_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
