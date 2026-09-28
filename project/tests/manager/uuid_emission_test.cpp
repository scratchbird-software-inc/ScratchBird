// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "manager_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace proto = scratchbird::manager::protocol;
using Id = proto::UuidBytes;
static_assert(sizeof(Id) == 16);
std::atomic<unsigned> entropy_fault{0}, entropy_calls{0};
unsigned checks = 0, failures = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) { ++failures; std::cerr << message << '\n'; }
}
bool Valid(const Id& id) {
  return (id[6] & 0xf0) == 0x70 && (id[8] & 0xc0) == 0x80;
}
std::vector<Id> Generate(unsigned count) {
  std::vector<Id> ids;
  ids.reserve(count);
  while (count--) ids.push_back(proto::MakeUuidV7());
  return ids;
}
bool Transfer(int fd, void* data, std::size_t count, bool write) {
  auto* bytes = static_cast<unsigned char*>(data);
  while (count) {
    const auto n = write ? ::write(fd, bytes, count) : ::read(fd, bytes, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    count -= static_cast<std::size_t>(n);
    bytes += n;
  }
  return true;
}
}  // namespace

extern "C" int __real_RAND_bytes(unsigned char*, int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes, int count) {
  const auto fault = entropy_fault.load();
  if (!fault) return __real_RAND_bytes(bytes, count);
  ++entropy_calls;
  if (fault == 3) { std::fill_n(bytes, count, 0xa5); return 1; }
  if (fault == 2 && count > 0) std::fill_n(bytes, count / 2 + 1, 0x5a);
  return 0;
}

int main() {
  try {
    constexpr unsigned count = 4096, thread_count = 8;
    // Seed before fork: neither copied allocation state nor a copied random
    // stream may supply the child's identities.
    const auto initial = Generate(1);
    int fds[2];
    if (::pipe(fds) != 0) return 2;
    const auto child = ::fork();
    if (child < 0) { ::close(fds[0]); ::close(fds[1]); return 2; }
    if (child == 0) {
      ::close(fds[0]);
      try {
        auto ids = Generate(count);
        const bool sent = Transfer(fds[1], ids.data(), ids.size() * sizeof(Id), true);
        ::close(fds[1]); ::_exit(sent ? 0 : 2);
      } catch (...) { ::_exit(2); }
    }
    ::close(fds[1]);
    const auto parent = Generate(count);
    std::vector<Id> child_ids(count);
    const bool read = Transfer(fds[0], child_ids.data(), child_ids.size() * sizeof(Id), false);
    ::close(fds[0]);
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0 && read, "forked manager UUID issuance failed");
    std::set<Id> all;
    auto check_batch = [&](const std::vector<Id>& ids) {
      Check(std::is_sorted(ids.begin(), ids.end()), "manager issuer lost local allocation order");
      for (const auto& id : ids) {
        Check(Valid(id), "manager emitted a non-v7 identity");
        Check(all.insert(id).second, "manager reused a binary identity");
      }
    };
    check_batch(initial);
    check_batch(parent);
    check_batch(child_ids);
    unsigned cloned_suffixes = 0;
    for (unsigned i = 0; i < count; ++i)
      cloned_suffixes += std::equal(parent[i].begin() + 6, parent[i].end(), child_ids[i].begin() + 6);
    Check(cloned_suffixes == 0, "fork cloned manager allocation suffixes");
    std::barrier ready(thread_count);
    std::array<std::vector<Id>, thread_count> batches;
    std::array<bool, thread_count> failed{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < thread_count; ++i) threads.emplace_back([&, i] {
      ready.arrive_and_wait();
      try { batches[i] = Generate(count); } catch (...) { failed[i] = true; }
    });
    for (auto& thread : threads) thread.join();
    for (unsigned i = 0; i < thread_count; ++i) {
      Check(!failed[i] && batches[i].size() == count, "concurrent manager issuance failed");
      check_batch(batches[i]);
    }
    for (unsigned mode : {1u, 2u}) {
      entropy_fault = mode;
      entropy_calls = 0;
      bool refused = false, recovery_failed = false;
      std::vector<Id> recovered;
      std::thread probe([&] {
        try { (void)Generate(1); } catch (const std::runtime_error&) { refused = true; }
        entropy_fault = 0;
        try { recovered = Generate(1); } catch (...) { recovery_failed = true; }
      });
      probe.join(); entropy_fault = 0;
      Check(entropy_calls > 0, "manager bypassed cryptographic entropy");
      Check(refused, "manager published an identity after entropy failure");
      Check(!recovery_failed && recovered.size() == 1, "failed issuer did not recover");
      check_batch(recovered);
      entropy_fault = mode; entropy_calls = 0;
      bool nonce_refused = false;
      try { (void)proto::MakeRandomNonce16(); }
      catch (const std::runtime_error&) { nonce_refused = true; }
      entropy_fault = 0;
      Check(nonce_refused && entropy_calls == 1, "nonce escaped an entropy failure");
    }
    // A nonce must use every supplied entropy byte, without UUID timestamp,
    // version, variant or retained-counter bits. Two calls require two fills.
    entropy_fault = 3; entropy_calls = 0;
    const auto first_nonce = proto::MakeRandomNonce16();
    const auto second_nonce = proto::MakeRandomNonce16();
    entropy_fault = 0;
    Check(first_nonce == proto::Bytes(16, 0xa5) && second_nonce == first_nonce &&
              entropy_calls == 2, "nonce was derived from UUID allocation or lost entropy bits");
    std::set<proto::Bytes> nonces;
    for (unsigned i = 0; i < count; ++i) {
      const auto nonce = proto::MakeRandomNonce16();
      Check(nonce.size() == 16 && nonces.insert(nonce).second, "random nonce shape or uniqueness failed");
    }
    std::cout << "manager_uuid_emission checks=" << checks << " samples=" << all.size()
              << " cloned_suffixes=" << cloned_suffixes << " failures=" << failures << '\n';
  } catch (const std::exception& error) {
    std::cerr << "manager UUID emission exception: " << error.what() << '\n'; return 2;
  }
  return failures ? 1 : 0;
}
