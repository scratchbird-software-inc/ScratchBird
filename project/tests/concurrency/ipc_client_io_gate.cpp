// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/ipc_client_io.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <thread>
#include <vector>
namespace io = scratchbird::server::ipc_detail;
using namespace std::chrono_literals;
std::binary_semaphore waiting{0};
std::atomic<int> watched{-1};
std::atomic<bool> watch{false};
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t n, int timeout) {
  if (n == 1 && fds[0].fd == watched.load() && watch.exchange(false)) waiting.release();
  return __real_poll(fds, n, timeout);
}
extern "C" int __real___poll_chk(pollfd*, nfds_t, int, size_t);
extern "C" int __wrap___poll_chk(pollfd* fds, nfds_t n, int timeout, size_t size) {
  if (n == 1 && fds[0].fd == watched.load() && watch.exchange(false)) waiting.release();
  return __real___poll_chk(fds, n, timeout, size);
}
int main() {
  for (bool writing : {false, true}) {
    int fds[2]; Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "actual socket pair");
    io::OwnedSocket owner(fds[0]), peer(fds[1]);
    Check(owner.MakeNonblocking(), "native nonblocking mode");
    int capacity = 4096;
    Check(::setsockopt(owner.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0,
          "bounded native send buffer");
    std::atomic_bool stop{false};
    std::vector<std::uint8_t> bytes(writing ? 1024*1024 : 32, 0x5a);
    if (!writing) Check(::send(peer.get(), bytes.data(), 3, MSG_NOSIGNAL) == 3, "real partial input");
    watched = owner.get(); watch = true;
    bool result = true;
    std::thread worker([&] {
      result = writing ? io::WriteAll(owner.get(), bytes, stop) : io::ReadExact(owner.get(), bytes, stop);
    });
    Check(waiting.try_acquire_for(5s), "actual blocked native transfer reaches wait");
    stop.store(true, std::memory_order_release);
    worker.join();
    Check(!result, "stop refuses incomplete transfer");
    Check(::fcntl(owner.get(), F_GETFD) >= 0, "cancellation retains exact descriptor owner");
    Check(::send(peer.get(), bytes.data(), 1, MSG_NOSIGNAL) == 1, "cancellation did not close socket");
    const auto fd = owner.get(); owner.Reset();
    int replacement[2]; Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0, "replacement pair");
    io::OwnedSocket replacement_owner(replacement[0]), replacement_peer(replacement[1]);
    Check(replacement_owner.get() == fd, "native descriptor number reused only after owner close");
    stop.store(true); owner.Reset();
    Check(::send(replacement_owner.get(), bytes.data(), 1, MSG_NOSIGNAL) == 1,
          "late stop and old owner reset preserve replacement descriptor");
  }
  int fds[2]; Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "positive pair");
  io::OwnedSocket left(fds[0]), right(fds[1]);
  Check(left.MakeNonblocking() && right.MakeNonblocking(), "positive nonblocking pair");
  std::atomic_bool stop{false};
  std::vector<std::uint8_t> sent(1024*1024), received(sent.size());
  for (std::size_t i=0; i<sent.size(); ++i) sent[i] = static_cast<std::uint8_t>(i);
  bool read = false;
  std::thread reader([&] { read = io::ReadExact(right.get(), received, stop); });
  const auto written = io::WriteAll(left.get(), sent, stop);
  reader.join();
  Check(written && read && received == sent, "actual complete bidirectional progress and exact bytes");
  right.Reset();
  std::array<std::uint8_t,1> byte{};
  Check(!io::ReadExact(left.get(), byte, stop), "peer EOF is incomplete read");
  Check(!io::WriteAll(left.get(), byte, stop), "peer loss is typed failure without SIGPIPE");
  std::cout << checks << " native socket checks PASS\n";
}
