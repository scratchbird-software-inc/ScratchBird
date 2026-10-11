// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "config.hpp"
#include "ipc_server.hpp"
#include "standalone_shutdown_deadline.hpp"
#include "memory.hpp"
#include "../support/owned_temp_directory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace s = scratchbird::server;
bool failure_cleanup_armed = false;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc, char** argv) try {
  const std::string mode = argc == 2 ? argv[1] : "config";
  if (mode == "config" || mode == "host-failure") {
    Check(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
        scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "shutdown_policy").ok(), "memory manager");
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (mode == "host-failure") {
      s::ServerBootstrapConfig config;
      config.database_default_path = artifacts.path()/"missing.sbdb";
      const auto refused = s::StartHostedEngine(config, []() noexcept { failure_cleanup_armed = true; });
      Check(!refused.ok() && failure_cleanup_armed, "non-elided failed host return activates cleanup deadline");
      artifacts.Cleanup();
      return 0;
    }
    const auto load = [&](const std::string& value) {
      const auto path = artifacts.path()/"shutdown.conf";
      { std::ofstream file(path); file << "[config]\nformat = SBCD1\n[server.database]\npolicy_seed_pack_root = "
          << SB_DEFAULT_POLICY_PACK_ROOT << "\n[server.memory]\nenable_platform_memory_probe = false\n"
          << "[server.shutdown]\ndrain_timeout_ms = " << value << '\n'; }
      s::ServerCliOptions options; options.config_path = path.string();
      return s::ResolveServerBootstrapConfig(options);
    };
    Check(s::ServerBootstrapConfig{}.shutdown_drain_timeout_ms == 30000, "approved default");
    for (const auto& [text, value] : {std::pair{"1s",1000ull}, {"30s",30000ull}, {"5m",300000ull}}) {
      const auto result = load(text);
      for (const auto& diagnostic : result.diagnostics)
        std::cerr << text << ':' << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      Check(result.ok() && result.config.shutdown_drain_timeout_ms == value, "duration and endpoints");
    }
    for (const auto text : {"0", "999", "300001", "-1", "18446744073709553s", "18446744073709551616ms"}) {
      const auto result = load(text);
      bool exact = false;
      for (const auto& diagnostic : result.diagnostics) exact |= diagnostic.code == "CONFIG.VALUE_INVALID_DURATION";
      Check(!result.ok() && exact, "out of range or wrapped duration must refuse");
    }
    artifacts.Cleanup();
    return 0;
  }
  int pipefd[2]; Check(::pipe(pipefd) == 0, "pipe");
  const auto start = std::chrono::steady_clock::now();
  const auto child = ::fork(); Check(child >= 0, "fork");
  if (child == 0) {
    ::close(pipefd[0]); ::alarm(10);
    s::ResetParserServerStopRequest();
    {
      s::StandaloneShutdownDeadline deadline(1000, s::ParserServerStopRequested);
      struct Owner { int fd; ~Owner() { if (::write(fd, "D", 1) != 1) std::_Exit(3); } } owner{pipefd[1]};
      s::RequestParserServerStop();
      if (mode == "deadline") {
        if (::write(pipefd[1], "P", 1) != 1) std::_Exit(3);
        // Models a blocked cooperative cleanup call, not a synthetic clock.
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    }
    std::_Exit(0);
  }
  ::close(pipefd[1]);
  int status = 0; Check(::waitpid(child, &status, 0) == child, "wait");
  char evidence[4]{}; const auto count = ::read(pipefd[0], evidence, sizeof(evidence)); ::close(pipefd[0]);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (mode == "deadline") {
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 2 && count == 1 && evidence[0] == 'P' &&
          elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(4),
          "actual terminal deadline without owner unwinding");
  } else {
    Check(mode == "clean" && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
          count == 1 && evidence[0] == 'D', "completed ownership disarms watchdog");
  }
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
