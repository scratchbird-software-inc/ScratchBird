// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "host_platform_admission.hpp"
#include "startup.hpp"

#include <cerrno>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void Require(bool condition, std::string_view message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

std::size_t DescriptorCount() {
  // Avoid fdopendir's own fcntl calls: fcntl denial is one of the tested faults.
  const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  Require(directory >= 0, "cannot inspect descriptors");
  std::size_t count = 0;
  char entries[4096];
  for (;;) {
    const auto bytes = ::syscall(SYS_getdents64, directory, entries, sizeof(entries));
    Require(bytes >= 0, "cannot enumerate descriptors");
    if (bytes == 0) break;
    for (long offset = 0; offset < bytes;) {
      std::uint16_t length = 0;
      Require(bytes - offset >= 20, "short directory record");
      std::memcpy(&length, entries + offset + 16, sizeof(length));
      Require(length >= 20 && length <= bytes - offset, "invalid directory record");
      const auto* name = entries + offset + 19;
      if (std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0) ++count;
      offset += length;
    }
  }
  Require(::close(directory) == 0, "descriptor enumeration close failed");
  return count;
}

void DenySyscall(int number, int error) {
  // Child-only real kernel fault. No production injection hook or host policy change.
  sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(number), 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned>(error)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{static_cast<unsigned short>(std::size(instructions)), instructions};
  Require(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no_new_privs failed");
  Require(::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "seccomp failed");
}

void DenyPeerHandle(int error) {
  constexpr auto option_offset = offsetof(seccomp_data, args[2]) +
      (std::endian::native == std::endian::big ? sizeof(std::uint32_t) : 0);
  sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getsockopt, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, option_offset),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SO_PEERPIDFD, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned>(error)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{static_cast<unsigned short>(std::size(instructions)), instructions};
  Require(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no_new_privs failed");
  Require(::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "seccomp failed");
}

void FaultCase(int syscall, int error, std::string_view capability, const std::string& root) {
  const pid_t child = ::fork();
  Require(child >= 0, "fork failed");
  if (child == 0) {
    if (syscall < 0) DenyPeerHandle(error); else DenySyscall(syscall, error);
    const auto before = DescriptorCount();
    for (int n = 0; n != 16; ++n) {
      const auto probe = scratchbird::server::ProbeServerHostPlatform();
      Require(!probe.supported, "denied capability reported supported");
      Require(probe.failed_capability == capability, probe.failed_capability);
      Require(probe.native_error == error, "native error lost");
    }
    Require(DescriptorCount() == before, "failed probe leaked handles");
    scratchbird::server::ServerCliOptions cli;
    cli.foreground = true;
    cli.control_dir = root + "/control";
    cli.runtime_dir = root + "/runtime";
    cli.database_ref = root + "/must-not-open.sbdb";
    const auto started = scratchbird::server::RunServerStartup(cli);
    Require(started.exit_code == 2 && !started.serving_requested, "startup did not refuse");
    Require(started.diagnostics.size() == 1 &&
            started.diagnostics[0].code == "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
            "startup did not preserve registered profile diagnostic");
    Require(started.stdout_text.empty(), "failed startup published success");
    Require(DescriptorCount() == before, "refused startup leaked handles");
    ::_exit(0);
  }
  int status = 0;
  Require(::waitpid(child, &status, 0) == child, "waitpid failed");
  Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fault child failed");
  Require(std::filesystem::is_empty(root), "refused startup created artifacts");
}
}  // namespace

int main() {
  using scratchbird::server::MeetsLinuxServerKernelMinimum;
  for (auto release : {"6.6.0", "6.6.109-vendor", "6.12.0", "6.18.1", "7.0.0-28-generic"})
    Require(MeetsLinuxServerKernelMinimum(release), "valid release refused");
  for (auto release : {"", "6", "6.6", "5.19.0", "6.5.99", "6.1.100", "6.6.0-rc1",
                       "7.0.0-rc1", "x6.6.0", "6.6.x", "6.6.0garbage", "6.-6.0",
                       "4294967296.6.0", "6.4294967296.0"})
    Require(!MeetsLinuxServerKernelMinimum(release), "invalid release admitted");
  const auto before = DescriptorCount();
  for (int n = 0; n != 256; ++n) {
    const auto probe = scratchbird::server::ProbeServerHostPlatform();
    Require(probe.supported && probe.platform == "linux" &&
            probe.failed_capability.empty() && probe.native_error == 0,
            "actual kernel peer-process capability unavailable");
  }
  Require(DescriptorCount() == before, "successful probe leaked handles");
  char directory[] = "/tmp/sb-host-platform-XXXXXX";
  const char* created = ::mkdtemp(directory);
  Require(created != nullptr, "mkdtemp failed");
  const std::string root(created);
  FaultCase(SYS_uname, EACCES, "kernel_release_query", root);
  FaultCase(SYS_socketpair, EMFILE, "unix_stream_socketpair", root);
  FaultCase(SYS_getsockopt, EPERM, "unix_peer_credentials", root);
  FaultCase(-1, EPERM, "socket_bound_peer_process_handle", root);
  FaultCase(-1, ENOPROTOOPT, "socket_bound_peer_process_handle", root);
  FaultCase(SYS_fcntl, EACCES, "peer_process_handle_close_on_exec", root);
  FaultCase(SYS_setsockopt, EPERM, "message_sender_identity_setup", root);
  FaultCase(SYS_recvmsg, EACCES, "message_sender_identity_receive", root);
#ifdef SYS_poll
  FaultCase(SYS_poll, EACCES, "peer_process_handle_liveness_poll", root);
#else
  // Linux architectures without poll (including AArch64) implement it via ppoll.
  FaultCase(SYS_ppoll, EACCES, "peer_process_handle_liveness_poll", root);
#endif
  Require(std::filesystem::remove(root), "owned empty fixture cleanup failed");
  std::cout << "PASS Linux host capability admission; real syscall denials and startup no-side-effect checks\n";
}
