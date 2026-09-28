// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "parser_server_client.hpp"
#include "../../src/parsers/sbsql_worker/wire/copy_row_identity.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Id = std::array<std::uint8_t, 16>;
namespace ipc = scratchbird::parser::ipc;
std::atomic<unsigned> entropy_fault{0}, entropy_calls{0};
unsigned checks = 0, failures = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::cerr << message << '\n'; }
}
bool Transfer(int fd, void* bytes, std::size_t count, bool write) {
  auto* data = static_cast<std::uint8_t*>(bytes);
  while (count) {
    const auto n = write ? ::write(fd, data, count) : ::read(fd, data, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    count -= static_cast<std::size_t>(n); data += n;
  }
  return true;
}
std::vector<Id> Generate(unsigned count) {
  std::vector<Id> result; result.reserve(count * 6);
  for (unsigned i = 0; i < count; ++i) {
    ipc::SbpsClient client("/not-opened/parser-uuid-emission.sock");
    const auto payload = client.V2HelloPayloadForTest();
    if (payload.size() < 136 || client.V2HelloPayloadForTest() != payload)
      throw std::runtime_error("client did not retain its complete binary HELLO");
    for (const auto offset : {std::size_t(0), std::size_t(16), std::size_t(32),
                             std::size_t(48), payload.size() - 72, payload.size() - 56}) {
      Id id{};
      std::copy_n(payload.begin() + offset, 16, id.begin());
      result.push_back(id);
    }
  }
  return result;
}
bool Valid(const Id& id) { return (id[6] & 0xf0) == 0x70 && (id[8] & 0xc0) == 0x80; }
}

extern "C" int __real_RAND_bytes(unsigned char*, int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes, int count) {
  const auto fault = entropy_fault.load();
  if (!fault) return __real_RAND_bytes(bytes, count);
  ++entropy_calls;
  if (fault == 2 && count > 0) std::fill_n(bytes, count / 2 + 1, 0x5a);
  return 0;
}

int main() {
  try {
    constexpr unsigned count = 256, threads = 8;
    {
      const auto milliseconds = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
      };
      const auto before = milliseconds();
      std::vector<Id> groups;
      for (unsigned i = 0; i != 4096; ++i)
        groups.push_back(scratchbird::parser::sbsql::IssueCopyRowGroupIdentity().bytes);
      const auto after = milliseconds();
      for (std::size_t i = 0; i != groups.size(); ++i) {
        std::uint64_t timestamp = 0;
        for (unsigned byte = 0; byte != 6; ++byte) timestamp = timestamp * 256 + groups[i][byte];
        Check(Valid(groups[i]) && timestamp >= static_cast<std::uint64_t>(before) &&
                  timestamp <= static_cast<std::uint64_t>(after),
              "COPY group issuance fabricated time or emitted an invalid UUID");
        if (i) Check(groups[i - 1] < groups[i], "COPY grouping IDs did not advance uniquely");
      }
      for (unsigned mode : {1u, 2u}) {
        entropy_fault = mode; entropy_calls = 0;
        bool refused = false, recovered = false;
        std::thread probe([&] {
          refused = scratchbird::parser::sbsql::IssueCopyRowGroupIdentity().is_nil();
          entropy_fault = 0;
          recovered = !scratchbird::parser::sbsql::IssueCopyRowGroupIdentity().is_nil();
        });
        probe.join(); entropy_fault = 0;
        Check(entropy_calls > 0 && refused && recovered,
              "COPY group issuance lost entropy-failure refusal or recovery");
      }
    }
    {
      ipc::SbpsClient client("/not-opened/parser-hello-retention.sock");
      const auto original = client.V2HelloPayloadForTest();
      for (const auto& variant : {client.PreparedMetadataTransferV1HelloPayloadForTest(),
                                  client.RelationDescriptorV3HelloPayloadForTest()}) {
        Check(variant.size() == original.size() && original.size() >= 32 &&
                  std::equal(original.begin(), original.end() - 32, variant.begin()),
              "capability selection replaced the retained HELLO identity tuple");
        Check(original.size() >= 32 && variant.size() >= 32 &&
                  !std::equal(original.end() - 32, original.end(), variant.end() - 32),
              "capability variants lost their distinct negotiation bits");
      }
      ipc::SbpsClient moved(std::move(client));
      Check(moved.V2HelloPayloadForTest() == original && client.V2HelloPayloadForTest().empty(),
            "moving a client changed or duplicated its HELLO ownership");
      ipc::SbpsClient destination("/not-opened/replaced-client.sock");
      destination = std::move(moved);
      Check(destination.V2HelloPayloadForTest() == original && moved.V2HelloPayloadForTest().empty(),
            "move assignment changed or duplicated HELLO ownership");
    }
    // Initialize the real client generator before forking, so copied PRNG
    // state cannot be hidden by the parent's and child's initial seeding.
    const auto initial = Generate(1);
    for (const auto& id : initial) Check(Valid(id), "initial HELLO identity is not UUIDv7");
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
    std::vector<Id> child_ids(parent.size());
    const bool read = Transfer(fds[0], child_ids.data(), child_ids.size() * sizeof(Id), false);
    ::close(fds[0]);
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0 && read, "forked parser identity generation failed");
    std::set<Id> all(parent.begin(), parent.end());
    Check(all.size() == parent.size(), "parent reused an identity");
    unsigned cloned_suffixes = 0;
    for (std::size_t i = 0; i < child_ids.size(); ++i) {
      Check(Valid(parent[i]) && Valid(child_ids[i]), "fork emitted malformed UUIDv7");
      Check(all.insert(child_ids[i]).second, "fork reused a binary identity");
      cloned_suffixes += std::equal(parent[i].begin() + 6, parent[i].end(), child_ids[i].begin() + 6);
    }
    Check(cloned_suffixes == 0, "fork cloned the parser UUID generator suffix stream");
    std::barrier ready(threads);
    std::array<std::vector<Id>, threads> batches;
    std::array<bool, threads> failed{};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < threads; ++i) workers.emplace_back([&, i] {
      ready.arrive_and_wait();
      try { batches[i] = Generate(count); } catch (...) { failed[i] = true; }
    });
    for (auto& worker : workers) worker.join();
    for (unsigned i = 0; i < threads; ++i) {
      Check(!failed[i] && batches[i].size() == count * 6, "concurrent client generation failed");
      for (const auto& id : batches[i]) {
        Check(Valid(id), "concurrent client emitted malformed UUIDv7");
        Check(all.insert(id).second, "concurrent clients reused an identity");
      }
    }
    for (unsigned mode : {1u, 2u}) {
      entropy_fault = mode; entropy_calls = 0;
      bool refused = false;
      bool recovery_failed = false;
      std::vector<Id> recovered;
      // Each fresh thread owns a fresh Core issuer, requiring a real seed.
      std::thread probe([&] {
        try { (void)Generate(1); } catch (const std::runtime_error&) { refused = true; }
        entropy_fault = 0;
        try { recovered = Generate(1); } catch (...) { recovery_failed = true; }
      });
      probe.join(); entropy_fault = 0;
      Check(entropy_calls > 0, "parser bypassed the approved cryptographic backend");
      Check(refused, "entropy failure emitted a client identity or used a fallback");
      Check(!recovery_failed && recovered.size() == 6,
            "the failed issuer could not recover after entropy was restored");
      for (const auto& id : recovered)
        Check(Valid(id) && all.insert(id).second, "entropy recovery reused or corrupted identity");
    }
    std::cout << "parser_uuid_emission checks=" << checks << " samples=" << all.size()
              << " cloned_suffixes=" << cloned_suffixes << " failures=" << failures << '\n';
  } catch (const std::exception& error) {
    std::cerr << "parser UUID emission exception: " << error.what() << '\n'; return 2;
  }
  return failures ? 1 : 0;
}
