// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "listener_orchestrator.hpp"
#include "ipc_server.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
pid_t child_pid = -1;
bool deny_kill = false;
bool deny_wait = false;
bool interrupt_wait = false;
bool deny_poll = false;
unsigned kill_calls = 0;
unsigned wait_faults = 0;
unsigned foreign_native_calls = 0;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}
extern "C" int __real_kill(pid_t, int);
extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" int __wrap_kill(pid_t pid, int signal) {
  if (pid != child_pid) {
    ++foreign_native_calls;
    errno = EPERM;
    return -1;  // Never let a regression signal an unrelated process/group.
  }
  if (pid == child_pid && signal == SIGKILL) {
    ++kill_calls;
    if (deny_kill) { errno = EPERM; return -1; }
  }
  return __real_kill(pid, signal);
}
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
  if (pid != child_pid) {
    ++foreign_native_calls;
    errno = ECHILD;
    return -1;
  }
  if (pid == child_pid && options == WNOHANG && deny_poll) {
    ++wait_faults;
    errno = ECHILD;
    return -1;
  }
  if (pid == child_pid && options == 0) {
    if (deny_wait || interrupt_wait) {
      ++wait_faults;
      errno = deny_wait ? EIO : EINTR;
      interrupt_wait = false;
      return -1;
    }
  }
  return __real_waitpid(pid, status, options);
}
namespace {
class Child {
 public:
  Child() {
    int pipe_fds[2];
    Check(::pipe(pipe_fds) == 0, "pipe");
    child_pid = ::fork();
    if (child_pid == 0) {
      ::close(pipe_fds[1]);
      char byte;
      while (::read(pipe_fds[0], &byte, 1) < 0 && errno == EINTR) {}
      ::close(pipe_fds[0]);
      _exit(0);
    }
    ::close(pipe_fds[0]);
    if (child_pid < 0) { ::close(pipe_fds[1]); throw std::runtime_error("fork"); }
    release_ = pipe_fds[1];
  }
  ~Child() {
    deny_kill = deny_wait = interrupt_wait = deny_poll = false;
    Release();
    int status;
    while (__real_waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
  }
  void Release() { if (release_ >= 0) { ::close(release_); release_ = -1; } }
 private:
  int release_ = -1;
};
void Run(const std::string& mode) {
  Child child;
  scratchbird::server::ServerIpcEndpointOwner owner({});
  auto& orchestrator = owner.listeners;
  orchestrator.profiles.emplace_back();
  auto& profile = orchestrator.profiles.back();
  profile.listener_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000001");
  profile.listener_profile_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000000002");
  profile.pid = child_pid;
  if (mode == "invalid-native-pid") profile.pid = std::numeric_limits<std::int64_t>::max();
  const auto retained_pid = profile.pid;
  profile.enabled = true;
  profile.state = "running";
  if (mode == "embedded-graceful-retry") {
    const auto pending = scratchbird::server::DrainServerIpcEndpoint(owner);
    Check(!pending.complete && !pending.listeners_complete && profile.pid == child_pid && kill_calls == 0,
          "embedded pending listener retained without termination");
    child.Release();
    siginfo_t exit{};
    Check(::waitid(P_PID, child_pid, &exit, WEXITED | WNOWAIT) == 0, "child actually exited, not yet reaped");
    Check(scratchbird::server::DrainServerIpcEndpoint(owner).complete && profile.pid == -1 && kill_calls == 0,
          "graceful retry reconciles actual owned child exit");
    return;
  }
  deny_kill = mode == "kill-denied" || mode == "retry" || mode == "restart-denied";
  deny_wait = mode == "wait-failed" || mode == "wait-retry";
  deny_poll = mode == "unconfirmed-owner";
  interrupt_wait = mode == "wait-interrupted";
  const auto result = mode == "restart-denied"
      ? scratchbird::server::ApplyListenerOperation(&orchestrator, {}, {},
            "restart_listener", profile.listener_uuid, "force")
      : scratchbird::server::StopManagedServerListeners(&orchestrator,
            mode == "graceful-refused" ? "graceful" : "force");
  Check(foreign_native_calls == 0, "invalid PID reached native process operation");
  if (deny_kill || deny_wait || deny_poll || mode == "graceful-refused" || mode == "invalid-native-pid") {
    Check(!result.ok && result.outcome != "completed", "false stop completion");
    Check(profile.pid == retained_pid && profile.enabled && profile.state == "draining",
          "lost unconfirmed child ownership");
    Check(!result.diagnostics.empty(), "missing failure diagnostic");
    if (!deny_wait) Check(__real_kill(child_pid, 0) == 0, "owned child not alive");
    if (mode == "graceful-refused" || deny_poll || mode == "invalid-native-pid") Check(kill_calls == 0, "unconfirmed/unrequested force escalation");
    else Check(kill_calls == 1, "termination fault not reached");
    if (deny_wait) Check(wait_faults == 1, "wait fault not reached");
    if (mode != "retry" && mode != "wait-retry") return;
    deny_kill = deny_wait = false;
    const auto retry = scratchbird::server::StopManagedServerListeners(&orchestrator, "force");
    Check(retry.ok, "stop retry failed");
  } else {
    Check(result.ok, "stop did not complete");
    if (mode == "wait-interrupted") Check(wait_faults == 1, "EINTR not exercised");
  }
  Check(profile.pid == -1 && !profile.enabled && profile.state == "stopped",
        "confirmed exit not published");
  int status;
  Check(__real_waitpid(child_pid, &status, WNOHANG) == -1 && errno == ECHILD,
        "completed before native child reap");
}
}
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "requires test mode");
    Run(argv[1]);
    std::cout << argv[1] << " PASS; owned child released/reaped\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
