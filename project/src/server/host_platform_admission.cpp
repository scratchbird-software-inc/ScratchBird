// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "host_platform_admission.hpp"

#include <charconv>
#include <cstdint>
#include <system_error>

#if defined(__linux__) && !defined(__ANDROID__)
#include "control_peer_identity.hpp"
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace scratchbird::server {

bool MeetsLinuxServerKernelMinimum(std::string_view release) {
  std::uint32_t parts[3]{};
  for (unsigned i = 0; i != 3; ++i) {
    if (release.empty()) return false;
    const auto parsed = std::from_chars(release.data(), release.data() + release.size(), parts[i]);
    if (parsed.ec != std::errc{} || parsed.ptr == release.data()) return false;
    release.remove_prefix(static_cast<std::size_t>(parsed.ptr - release.data()));
    if (i != 2) {
      if (release.empty() || release.front() != '.') return false;
      release.remove_prefix(1);
    }
  }
  if (!release.empty() && release.front() != '-' && release.front() != '+') return false;
  // Release candidates are not the supported stable runtime floor.
  if (release.find("-rc") != std::string_view::npos) return false;
  return parts[0] > 6 || (parts[0] == 6 && parts[1] >= 6);
}

HostPlatformAdmission ProbeServerHostPlatform() {
  HostPlatformAdmission result;
#if defined(__linux__) && !defined(__ANDROID__)
  result.platform = "linux";
  utsname system{};
  if (::uname(&system) != 0) {
    result.native_error = errno;
    result.failed_capability = "kernel_release_query";
    return result;
  }
  result.kernel_release = system.release;
  if (!MeetsLinuxServerKernelMinimum(result.kernel_release)) {
    result.failed_capability = "linux_kernel_6_6_minimum";
    return result;
  }
#ifdef SO_PEERPIDFD
  struct Descriptor {
    int value = -1;
    ~Descriptor() { if (value >= 0) ::close(value); }
  } first, second, peer;
  int pair[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
    result.native_error = errno;
    result.failed_capability = "unix_stream_socketpair";
    return result;
  }
  first.value = pair[0];
  second.value = pair[1];
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(first.value, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
    result.native_error = errno;
    result.failed_capability = "unix_peer_credentials";
    return result;
  }
  if (size != sizeof(credentials) || credentials.pid != ::getpid() ||
      credentials.uid != ::geteuid() || credentials.gid != ::getegid()) {
    result.failed_capability = "unix_peer_credentials_inconsistent";
    return result;
  }
  size = sizeof(peer.value);
  if (::getsockopt(first.value, SOL_SOCKET, SO_PEERPIDFD, &peer.value, &size) != 0) {
    result.native_error = errno;
    result.failed_capability = "socket_bound_peer_process_handle";
    return result;
  }
  if (size != sizeof(peer.value) || peer.value < 0) {
    result.failed_capability = "socket_bound_peer_process_handle_invalid";
    return result;
  }
  const int flags = ::fcntl(peer.value, F_GETFD);
  if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
    result.native_error = flags < 0 ? errno : 0;
    result.failed_capability = "peer_process_handle_close_on_exec";
    return result;
  }
  // Both socket ends were created here. This probes the current live process,
  // not an inherited child's identity or installed parser provenance.
  pollfd live{peer.value, POLLIN, 0};
  int observed;
  do { observed = ::poll(&live, 1, 0); } while (observed < 0 && errno == EINTR);
  if (observed != 0 || live.revents != 0) {
    result.native_error = observed < 0 ? errno : 0;
    result.failed_capability = "peer_process_handle_liveness_poll";
    return result;
  }
  // Exercise the same per-message credential/pidfd adapter used for inherited
  // parser channels. This self-probe proves capabilities only, not child ownership.
  scratchbird::listener::ControlPeerIdentity sender;
  errno = 0;
  if (!sender.Prepare(first.value) || !sender.ExpectSender(::getpid())) {
    result.native_error = errno;
    result.failed_capability = "message_sender_identity_setup";
    return result;
  }
  scratchbird::listener::ListenerControlFrame probe;
  probe.opcode = scratchbird::listener::ListenerControlOpcode::kHealthCheck;
  probe.sequence = 1;
  probe.payload = {0x53, 0x42};
  errno = 0;
  if (!scratchbird::listener::SendControlFrame(second.value, probe)) {
    result.native_error = errno;
    result.failed_capability = "message_sender_probe_send";
    return result;
  }
  scratchbird::listener::ListenerControlDecodeResult received;
  errno = 0;
  if (!sender.ReadFrame(first.value, &received, 1000) ||
      received.frame.opcode != probe.opcode || received.frame.sequence != probe.sequence ||
      received.frame.payload != probe.payload) {
    result.native_error = errno;
    result.failed_capability = "message_sender_identity_receive";
    return result;
  }
  result.supported = true;
#else
  result.failed_capability = "build_headers_missing_peer_process_handle_api";
#endif
#else
  // Other OS implementations require their own qualified native profile.
  // Do not report Linux-equivalent ownership based on portable compilation.
  result.platform = "non_linux";
  result.failed_capability = "native_server_platform_profile_not_qualified";
#endif
  return result;
}

}  // namespace scratchbird::server
