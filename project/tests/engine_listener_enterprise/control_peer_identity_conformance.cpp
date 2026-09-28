// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "control_peer_identity.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace scratchbird::listener;
void Check(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void Write(int fd, const void* data, std::size_t length) {
  const auto* bytes = static_cast<const char*>(data);
  while (length != 0) {
    const auto n = ::write(fd, bytes, length);
    if (n < 0 && errno == EINTR) continue;
    Check(n > 0, "fixture write failed");
    bytes += n; length -= n;
  }
}
void Wait(int fd) {
  char byte;
  ssize_t n;
  do { n = ::read(fd, &byte, 1); } while (n < 0 && errno == EINTR);
  Check(n == 1, "fixture synchronization failed");
}
std::size_t Descriptors() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator{});
}
enum class Case { valid, fragmented, wrong_sender, mixed_sender, rights,
                  partial_timeout, exited_sender, malformed, stale, wrong_socket,
                  descriptor_reuse };

void Exercise(Case mode) {
  int sockets[2], release[2], ready[2];
  Check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
        "socketpair failed");
  Check(::pipe(release) == 0 && ::pipe(ready) == 0, "pipe failed");
  ControlPeerIdentity peer;
  Check(peer.Prepare(sockets[0]), "native sender identity unavailable");
  Check(!peer.Prepare(sockets[0]), "peer preparation repeated");
  int other[2]{-1, -1};
  ControlPeerIdentity other_setup;
  const bool crossed_socket = mode == Case::wrong_socket || mode == Case::descriptor_reuse;
  if (crossed_socket) {
    Check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, other) == 0,
          "other socketpair failed");
    Check(other_setup.Prepare(other[0]), "other sender setup failed");
  }
  ListenerControlFrame frame;
  frame.opcode = ListenerControlOpcode::kHealthReport;
  frame.sequence = 73;
  frame.payload = {0, 19, 0, 255, 61, 0};
  auto encoded = EncodeControlFrame(frame);
  if (mode == Case::malformed) encoded[0] ^= 0xff;
  const auto child = ::fork();
  Check(child >= 0, "fork failed");
  if (child == 0) {
    ::close(sockets[0]); ::close(release[1]); ::close(ready[0]);
    if (mode == Case::mixed_sender) {
      Write(sockets[1], encoded.data(), 28);
    } else if (mode == Case::partial_timeout) {
      Write(sockets[1], encoded.data(), 3);
    } else if (mode == Case::rights) {
      Check(SendControlFrame(sockets[1], frame, release[0]), "rights send failed");
    } else if (mode == Case::fragmented) {
      for (const auto byte : encoded) Write(sockets[1], &byte, 1);
    } else {
      Write(sockets[1], encoded.data(), encoded.size());
    }
    // Valid traffic with the SAME kernel-authenticated child on another socket
    // must still fail the socket-lifetime binding, not merely time out.
    if (crossed_socket) Write(other[1], encoded.data(), encoded.size());
    Write(ready[1], "r", 1);
    if (mode != Case::exited_sender) Wait(release[0]);
    ::_exit(0);
  }
  ::close(release[0]); ::close(ready[1]);
  Wait(ready[0]); ::close(ready[0]);
  Check(peer.ExpectSender(mode == Case::wrong_sender ? ::getpid() : child),
        "child binding failed");
  Check(!peer.ExpectSender(child), "child lifetime rebound");
  if (mode == Case::mixed_sender) Write(sockets[1], encoded.data() + 28, encoded.size() - 28);
  int status = 0;
  if (mode == Case::exited_sender)
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "exited child wait failed");
  if (mode == Case::descriptor_reuse)
    Check(::dup2(other[0], sockets[0]) == sockets[0], "descriptor reuse failed");
  ListenerControlDecodeResult decoded;
  const auto started = std::chrono::steady_clock::now();
  const bool accepted = peer.ReadFrame(mode == Case::wrong_socket ? other[0] : sockets[0],
                                      &decoded, mode == Case::partial_timeout ? 40 : 2000);
  const bool positive = mode == Case::valid || mode == Case::fragmented || mode == Case::stale;
  Check(accepted == positive, "sender admission result mismatch");
  if (positive) {
    Check(decoded.ok && decoded.frame.sequence == frame.sequence &&
          decoded.frame.opcode == frame.opcode && decoded.frame.payload == frame.payload,
          "authenticated frame changed");
    Check(peer.Alive(), "admitted live child missing");
  } else {
    Check(!decoded.ok && decoded.frame.payload.empty() && !peer.Alive(),
          "refused frame published data or retained authority");
    Check(!peer.ReadFrame(sockets[0], &decoded, 1), "refused channel revived");
  }
  if (mode == Case::partial_timeout)
    Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
          "partial control frame ignored deadline");
  if (mode != Case::exited_sender) {
    Write(release[1], "x", 1);
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child wait failed");
  }
  if (mode == Case::stale) {
    Check(!peer.Alive(), "exited child remains live");
    // Keep the same socket alive through another process; even valid wire bytes
    // cannot revive the admitted lifetime after the original child is reaped.
    Write(sockets[1], encoded.data(), encoded.size());
    Check(!peer.ReadFrame(sockets[0], &decoded, 10), "replacement sender revived lifetime");
  }
  ::close(release[1]); ::close(sockets[0]); ::close(sockets[1]);
  if (other[0] >= 0) { ::close(other[0]); ::close(other[1]); }
}
}  // namespace

int main() {
  const auto before = Descriptors();
  for (int iteration = 0; iteration != 16; ++iteration) {
    for (auto mode : {Case::valid, Case::fragmented, Case::wrong_sender,
                      Case::mixed_sender, Case::rights, Case::partial_timeout,
                      Case::exited_sender, Case::malformed, Case::stale, Case::wrong_socket,
                      Case::descriptor_reuse}) {
      Exercise(mode);
      Check(Descriptors() == before, "control peer descriptor leak");
    }
  }
  std::cout << "PASS 176 real child control-peer cases; sender/lifetime/frame/cleanup checks\n";
}
