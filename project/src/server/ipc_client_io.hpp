// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird::server::ipc_detail {
#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket invalid_socket = -1;
#endif

// Single native owner. Cancellation never closes another thread's descriptor.
// The cohort joins the owning thread before releasing its shared server state.
class OwnedSocket {
 public:
  explicit OwnedSocket(Socket fd) noexcept : fd_(fd) {}
  OwnedSocket(const OwnedSocket&) = delete;
  OwnedSocket& operator=(const OwnedSocket&) = delete;
  OwnedSocket(OwnedSocket&& other) noexcept : fd_(std::exchange(other.fd_, invalid_socket)) {}
  ~OwnedSocket() { Reset(); }
  Socket get() const noexcept { return fd_; }
  void Reset() noexcept {
    const auto fd = std::exchange(fd_, invalid_socket);
    if (fd == invalid_socket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd); // Never retry close: that descriptor number may be reused.
#endif
  }
  bool MakeNonblocking() const noexcept {
#ifdef _WIN32
    u_long enabled = 1;
    return ::ioctlsocket(fd_, FIONBIO, &enabled) == 0;
#else
    const int flags = ::fcntl(fd_, F_GETFL);
    return flags >= 0 && ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
  }
 private:
  Socket fd_;
};

inline bool Interrupted() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}
inline bool WouldBlock() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// A bounded native wait makes the existing signal-safe atomic stop observable.
// This is not a frame deadline, busy polling, or cancellation of an engine call.
// Caller must retain the nonblocking socket throughout this call.
inline bool Wait(Socket fd, bool writing, const std::atomic_bool& stop) noexcept {
  while (!stop.load(std::memory_order_acquire)) {
#ifdef _WIN32
    fd_set selected;
    FD_ZERO(&selected); FD_SET(fd, &selected);
    timeval timeout{0, 100000};
    const int rc = ::select(0, writing ? nullptr : &selected,
                            writing ? &selected : nullptr, nullptr, &timeout);
    if (rc > 0) return !stop.load(std::memory_order_acquire);
#else
    pollfd selected{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
    const int rc = ::poll(&selected, 1, 100);
    if (rc > 0) {
      return (selected.revents & selected.events) != 0 &&
             !stop.load(std::memory_order_acquire);
    }
#endif
    if (rc < 0 && !Interrupted()) return false;
  }
  return false;
}

inline bool Transfer(Socket fd, std::span<std::uint8_t> read,
                     std::span<const std::uint8_t> write,
                     bool writing, const std::atomic_bool& stop) noexcept {
  std::size_t position = 0;
  const auto size = writing ? write.size() : read.size();
  while (position < size) {
    if (stop.load(std::memory_order_acquire)) return false;
    const auto count = static_cast<int>(std::min<std::size_t>(
        size - position, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
    const auto rc = writing
        ? ::send(fd, reinterpret_cast<const char*>(write.data() + position), count, 0)
        : ::recv(fd, reinterpret_cast<char*>(read.data() + position), count, 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    if (writing) flags |= MSG_NOSIGNAL;
#endif
    const auto rc = writing ? ::send(fd, write.data() + position, count, flags)
                            : ::recv(fd, read.data() + position, count, 0);
#endif
    if (rc > 0) { position += static_cast<std::size_t>(rc); continue; }
    if (rc == 0) return false;
    if (Interrupted()) continue;
    if (!WouldBlock() || !Wait(fd, writing, stop)) return false;
  }
  return true; // Bytes transferred, never transaction or shutdown finality.
}
inline bool ReadExact(Socket fd, std::span<std::uint8_t> bytes,
                      const std::atomic_bool& stop) noexcept {
  return Transfer(fd, bytes, {}, false, stop);
}
inline bool WriteAll(Socket fd, std::span<const std::uint8_t> bytes,
                     const std::atomic_bool& stop) noexcept {
  return Transfer(fd, {}, bytes, true, stop);
}
} // namespace scratchbird::server::ipc_detail
