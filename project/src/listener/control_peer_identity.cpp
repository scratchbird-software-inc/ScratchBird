// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "control_peer_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace scratchbird::listener {
namespace {
struct Descriptor {
  int value = -1;
  ~Descriptor() { if (value >= 0) ::close(value); }
};

bool Live(int handle) {
  if (handle < 0) return false;
  pollfd event{handle, POLLIN, 0};
  int rc;
  do { rc = ::poll(&event, 1, 0); } while (rc < 0 && errno == EINTR);
  return rc == 0 && event.revents == 0;
}
}  // namespace

ControlPeerIdentity::~ControlPeerIdentity() {
  if (process_handle_ >= 0) ::close(process_handle_);
}

bool ControlPeerIdentity::Prepare(std::intptr_t socket) {
  if (prepared_ || refused_ || socket < 0 || socket > INT_MAX) return false;
  socklen_t cookie_size = sizeof(socket_cookie_);
  if (::getsockopt(static_cast<int>(socket), SOL_SOCKET, SO_COOKIE,
                   &socket_cookie_, &cookie_size) != 0 ||
      cookie_size != sizeof(socket_cookie_) || socket_cookie_ == 0) {
    refused_ = true;
    return false;
  }
  const int enabled = 1;
  if (::setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_PASSCRED,
                   &enabled, sizeof(enabled)) != 0 ||
      ::setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_PASSPIDFD,
                   &enabled, sizeof(enabled)) != 0) {
    refused_ = true;
    return false;
  }
  // Automatic SCM_CREDENTIALS carries real UID/GID, not the HELLO's claims.
  expected_uid_ = ::getuid();
  expected_gid_ = ::getgid();
  prepared_ = true;
  return true;
}

bool ControlPeerIdentity::ExpectSender(std::int64_t process_pid) {
  if (!prepared_ || refused_ || expected_pid_ != 0 || process_pid <= 0) return false;
  expected_pid_ = process_pid;
  return true;
}

bool ControlPeerIdentity::Alive() const {
  return !refused_ && Live(process_handle_);
}

bool ControlPeerIdentity::ReadExact(
    std::intptr_t socket, std::span<std::uint8_t> bytes,
    std::chrono::steady_clock::time_point deadline) {
  while (!bytes.empty()) {
    if (process_handle_ >= 0 && !Alive()) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return false;
    pollfd ready{static_cast<int>(socket), POLLIN, 0};
    const int rc = ::poll(&ready, 1, static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0 || (ready.revents & POLLIN) == 0) return false;

    // Room for every permitted SCM_RIGHTS descriptor plus credentials/pidfd.
    // Unexpected rights are always closed, including on truncated frames.
    alignas(cmsghdr) std::array<char, CMSG_SPACE(253 * sizeof(int)) +
        CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(sizeof(int))> ancillary{};
    iovec iov{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    const auto received = ::recvmsg(static_cast<int>(socket), &message,
                                    MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (received <= 0) return false;
    bool valid = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) == 0;
    bool credentials_seen = false;
    ucred credentials{};
    Descriptor sender;
    for (auto* control = CMSG_FIRSTHDR(&message); control != nullptr;
         control = CMSG_NXTHDR(&message, control)) {
      if (control->cmsg_len < CMSG_LEN(0)) { valid = false; break; }
      const auto length = control->cmsg_len - CMSG_LEN(0);
      if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_RIGHTS) {
        valid = false;
        for (std::size_t offset = 0; offset + sizeof(int) <= length; offset += sizeof(int)) {
          int descriptor;
          std::memcpy(&descriptor, CMSG_DATA(control) + offset, sizeof(descriptor));
          if (descriptor >= 0) ::close(descriptor);
        }
      } else if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_PIDFD) {
        if (length != sizeof(int)) { valid = false; continue; }
        int descriptor;
        std::memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
        if (sender.value >= 0) {
          if (descriptor >= 0) ::close(descriptor);
          valid = false;
        } else {
          sender.value = descriptor;
        }
      } else if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_CREDENTIALS) {
        if (credentials_seen || length != sizeof(credentials)) { valid = false; continue; }
        std::memcpy(&credentials, CMSG_DATA(control), sizeof(credentials));
        credentials_seen = true;
      } else {
        valid = false;
      }
    }
    if (!valid || !credentials_seen || credentials.pid != expected_pid_ ||
        credentials.uid != expected_uid_ || credentials.gid != expected_gid_ ||
        !Live(sender.value)) return false;
    const int flags = ::fcntl(sender.value, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0) return false;
    // The retained handle must still be live, so a reused numeric PID cannot
    // rebind this channel to a replacement process. Never pidfd_open a claimed PID.
    if (process_handle_ >= 0 && !Alive()) return false;
    if (process_handle_ < 0) {
      process_handle_ = sender.value;
      sender.value = -1;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(received));
  }
  return true;
}

bool ControlPeerIdentity::ReadFrame(std::intptr_t socket,
                                   ListenerControlDecodeResult* decoded,
                                   std::uint32_t timeout_ms) {
  if (decoded == nullptr) return false;
  *decoded = {};
  const auto refuse = [&] {
    refused_ = true;
    if (process_handle_ >= 0) { ::close(process_handle_); process_handle_ = -1; }
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic(
        "LISTENER.CONTROL_CHANNEL_FAILED", "ERROR",
        "Parser control sender identity or frame could not be verified.",
        "sb_listener.control")}), false};
    return false;
  };
  if (!prepared_ || refused_ || expected_pid_ <= 0 || socket < 0 || socket > INT_MAX)
    return refuse();
  std::uint64_t cookie = 0;
  socklen_t cookie_size = sizeof(cookie);
  if (::getsockopt(static_cast<int>(socket), SOL_SOCKET, SO_COOKIE,
                   &cookie, &cookie_size) != 0 || cookie_size != sizeof(cookie) ||
      cookie != socket_cookie_) return refuse();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::vector<std::uint8_t> bytes(28);
  if (!ReadExact(socket, bytes, deadline)) return refuse();
  std::uint64_t payload = 0;
  for (unsigned i = 0; i != 8; ++i) payload |= std::uint64_t(bytes[20 + i]) << (8 * i);
  if (payload > 64 * 1024) return refuse();
  bytes.resize(28 + static_cast<std::size_t>(payload));
  if (!ReadExact(socket, std::span(bytes).subspan(28), deadline) || !Alive()) return refuse();
  auto result = DecodeControlFrame(bytes);
  if (!result.ok) return refuse();
  *decoded = std::move(result);
  return true;
}
}  // namespace scratchbird::listener
