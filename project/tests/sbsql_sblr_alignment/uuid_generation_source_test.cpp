// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"
#include "time.hpp"
#include "diagnostic_identity.hpp"
#include "canonical_diagnostic_catalog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace u = scratchbird::core::uuid;
namespace p = scratchbird::core::platform;
namespace d = scratchbird::core::diagnostics;
namespace {
// Linker interception exists only in this Linux test executable. Production
// sb_core_uuid has no injectable provider or deterministic entropy path.
std::atomic<int> mode{0};
std::atomic<unsigned> calls{0};
std::atomic<int> clock_mode{0};
std::atomic<std::uint64_t> clock_reads{0};
std::atomic<std::uint64_t> clock_millis{123456};
std::atomic<bool> clock_failed{false};
unsigned checks = 0, failures = 0;
void Check(bool value, const char* detail) {
  ++checks;
  if (!value && ++failures <= 20) std::cerr << "FAIL " << detail << '\n';
}
template<class Result>
void Refused(const Result& result, const char* code) {
  Check(!result.ok() && result.status.code == p::StatusCode::uuid_invalid,
        "generation failure did not preserve error status");
  if constexpr (requires { result.value.bytes; })
    Check(result.value.is_nil(), "failed raw generation leaked bytes");
  else
    Check(result.value.value.is_nil() && result.value.kind == p::UuidKind::unknown,
          "failed typed generation leaked an identity");
  Check(result.diagnostic.diagnostic_code == code, "wrong source refusal");
  const auto* registered = d::FindCanonicalDiagnosticCode(code);
  Check(registered && registered->is_failure && registered->severity == d::CanonicalSeverity::error,
        "refusal is absent from actual diagnostic registration");
}
void Layout(const p::Uuid& value, std::uint64_t millis, unsigned version) {
  std::array<p::byte,16> expected;
  expected.fill(0xa5);
  if (version == 7)
    for (unsigned i = 0; i < 6; ++i)
      expected[i] = static_cast<p::byte>(millis >> (40 - 8*i));
  expected[6] = static_cast<p::byte>((version << 4) | 5);
  expected[8] = 0xa5;
  Check(value.bytes == expected, "binary timestamp/version/variant/random bits changed");
}
void SourceFaultsAndLayout() {
  constexpr std::array<std::uint64_t,7> valid{
      0, 1, 255, 256, 0x010203040506ULL, 0xfffffffffffeULL, 0xffffffffffffULL};
  mode = 1;
  for (const auto millis : valid) {
    const auto raw = u::GenerateCompatibilityUnixTimeV7(millis);
    Check(raw.ok(), "representable compatibility timestamp refused");
    Layout(raw.value, millis, 7);
    for (const auto& typed : {u::GenerateEngineIdentityV7(p::UuidKind::object, millis),
                             u::GenerateDurableEngineIdentityV7(p::UuidKind::row, millis)}) {
      Check(typed.ok(), "representable typed timestamp refused");
      Layout(typed.value.value, millis, 7);
    }
  }
  const auto v4 = u::GenerateCompatibilityRandomV4();
  Check(v4.ok(), "compatibility random UUIDv4 refused");
  Layout(v4.value, 0, 4);
  Check(!u::MakeTypedUuid(p::UuidKind::object, v4.value).ok(),
        "older-version compatibility UUID became typed engine authority");
  for (const auto millis : std::array<std::uint64_t,4>{
           0x1000000000000ULL, 0x1000000000001ULL,
           0xffff010203040506ULL, std::numeric_limits<std::uint64_t>::max()}) {
    const auto before = calls.load();
    Refused(u::GenerateCompatibilityUnixTimeV7(millis), "TIME.UUID_TIMESTAMP_OUT_OF_RANGE");
    Refused(u::GenerateEngineIdentityV7(p::UuidKind::object, millis), "TIME.UUID_TIMESTAMP_OUT_OF_RANGE");
    Refused(u::GenerateDurableEngineIdentityV7(p::UuidKind::row, millis), "TIME.UUID_TIMESTAMP_OUT_OF_RANGE");
    Check(calls == before, "invalid timestamp consumed entropy before refusal");
  }
  // Failure after partial writes and unsupported-provider (-1) both refuse.
  for (const int failure_mode : {2, 3}) {
    mode = failure_mode;
    auto before = calls.load();
    Refused(u::GenerateCompatibilityRandomV4(), "TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Refused(u::GenerateCompatibilityUnixTimeV7(1), "TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Refused(u::GenerateEngineIdentityV7(p::UuidKind::object, 1), "TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Refused(u::GenerateDurableEngineIdentityV7(p::UuidKind::row, 1), "TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Check(calls == before + 4, "failed provider was retried or recursively invoked");
    before = calls;
    Check(!u::IssueRuntimeIdentityV7(), "runtime issuer published failed entropy");
    Check(calls == before + 1, "runtime issuer recursively allocated error identity");
    bool threw = false;
    before = calls;
    try { (void)u::NewDiagnosticOccurrenceUuid(); }
    catch (const std::runtime_error&) { threw = true; }
    Check(threw && calls == before + 1, "diagnostic occurrence did not fail without recursion");
  }
  mode = 0;
}
void WallClockConversion() {
  namespace t = scratchbird::core::time;
  struct Example { p::i64 seconds; p::u32 nanos; bool admitted; p::u64 millis; };
  constexpr std::array examples{
      Example{0,0,true,0}, Example{0,999999,true,0},
      Example{0,1000000,true,1}, Example{1,999999999,true,1999},
      Example{281474976710LL,655000000,true,0xffffffffffffULL},
      Example{281474976710LL,655999999,true,0xffffffffffffULL},
      Example{281474976710LL,656000000,false,0},
      Example{281474976711LL,0,false,0},
      Example{-1,0,false,0}, Example{0,1000000000,false,0},
      Example{0,std::numeric_limits<p::u32>::max(),false,0},
      Example{std::numeric_limits<p::i64>::min(),0,false,0},
      Example{std::numeric_limits<p::i64>::max(),0,false,0},
      // The old multiplication wrapped this positive second count to zero.
      Example{p::i64{1} << 61,0,false,0}
  };
  for (const auto& example : examples) {
    const auto converted = t::WallClockToUuidV7Millis({example.seconds,example.nanos});
    Check(converted.ok() == example.admitted, "wall clock bound admitted an invalid timestamp");
    Check(converted.unix_epoch_millis == example.millis, "wall clock overflow/precision changed");
    if (!example.admitted)
      Check(converted.diagnostic.diagnostic_code == "TIME.UUID_TIMESTAMP_OUT_OF_RANGE",
            "wall clock refusal lost canonical diagnostic");
  }
}
template<class Scenario>
void FreshRuntime(Scenario scenario) {
  // A fresh real thread gives the production thread-owned instance its normal
  // lifetime. There is no test reset hook or modified allocation width.
  clock_mode = 1;
  clock_millis = 123456;
  clock_reads = 0;
  clock_failed = false;
  mode = 1;
  std::thread worker(scenario);
  worker.join();
  clock_mode = 0;
  mode = 0;
}
void RuntimeAllocation() {
  FreshRuntime([] {
    const auto before = calls.load();
    auto previous = u::IssueRuntimeIdentityV7();
    Check(previous.has_value(), "runtime initial seed refused");
    for (unsigned i = 0; i < 4096; ++i) {
      const auto current = u::IssueRuntimeIdentityV7();
      Check(current && previous && *previous < *current, "same-ms runtime allocation did not increase");
      if (current && previous) {
        // Independent carry oracle: all 74 allocation bits are concatenated,
        // incremented as a ten-byte integer, then split back into UUID fields.
        std::array<unsigned char,10> allocation{};
        allocation[0] = static_cast<unsigned char>((previous->bytes[6] & 15) >> 2);
        allocation[1] = static_cast<unsigned char>(((previous->bytes[6] & 3) << 6) |
                                                 (previous->bytes[7] >> 2));
        allocation[2] = static_cast<unsigned char>((previous->bytes[7] << 6) |
                                                 (previous->bytes[8] & 63));
        std::copy(previous->bytes.begin()+9,previous->bytes.end(),allocation.begin()+3);
        for (int digit=9; digit>=0; --digit) if (++allocation[digit]) break;
        auto expected=*previous;
        expected.bytes[6]=static_cast<unsigned char>(0x70 | (allocation[0] << 2) | (allocation[1] >> 6));
        expected.bytes[7]=static_cast<unsigned char>((allocation[1] << 2) | (allocation[2] >> 6));
        expected.bytes[8]=static_cast<unsigned char>(0x80 | (allocation[2] & 63));
        std::copy(allocation.begin()+3,allocation.end(),expected.bytes.begin()+9);
        Check(current->bytes == expected.bytes, "runtime74bit increment differs from byte oracle");
      }
      previous=current;
    }
    Check(calls==before+1, "same-ms allocations consumed fresh entropy instead of sequence");
    const auto diagnostic=u::NewDiagnosticOccurrenceUuid();
    Check(previous && previous->bytes < diagnostic && calls==before+1,
          "diagnostic issuer bypassed the runtime allocation instance");
    clock_millis=123457;
    const auto later=u::IssueRuntimeIdentityV7();
    Check(later && previous && *previous < *later && calls==before+2,
          "later accepted millisecond did not start a fresh entropy allocation");
    clock_millis=123456;
    Check(!u::IssueRuntimeIdentityV7(), "runtime accepted regressing wall time");
    clock_millis=123457;
    const auto recovered=u::IssueRuntimeIdentityV7();
    Check(recovered && later && *later < *recovered && calls==before+2,
          "clock refusal changed retained allocation or prevented valid recovery");
    clock_millis=123458;
    mode=2;
    Check(!u::IssueRuntimeIdentityV7(), "later-millisecond entropy failure published a UUID");
    clock_millis=123457;
    mode=1;
    const auto retained=u::IssueRuntimeIdentityV7();
    Check(retained && recovered && *recovered < *retained && calls==before+3,
          "entropy failure advanced clock authority or consumed retained sequence");
    clock_failed=true;
    Check(!u::IssueRuntimeIdentityV7(), "failed actual clock source admitted runtime identity");
    clock_failed=false;
    clock_mode=2; // A regressed monotonic observation despite equal wall time.
    Check(!u::IssueRuntimeIdentityV7(), "runtime accepted regressing monotonic time");
  });
  // Carry across the62-bit rand_b /12-bit rand_a split without changing time,
  // version or variant. The deterministic bytes are injected only at RAND_bytes.
  FreshRuntime([] {
    mode=4;
    const auto first=u::IssueRuntimeIdentityV7();
    const auto second=u::IssueRuntimeIdentityV7();
    Check(first && second, "74bit cross-field carry refused");
    if (first && second) {
      Check(first->bytes[6]==0x75 && first->bytes[7]==0xa5 && first->bytes[8]==0xbf,
            "carry seed oracle mismatch");
      auto expected=*first;
      expected.bytes[7]=0xa6;
      expected.bytes[8]=0x80;
      std::fill(expected.bytes.begin()+9,expected.bytes.end(),0);
      Check(second->bytes==expected.bytes, "rand_b overflow corrupted rand_a or UUID bits");
    }
  });
  FreshRuntime([] {
    mode=6;
    const auto first=u::IssueRuntimeIdentityV7();
    const auto second=u::IssueRuntimeIdentityV7();
    Check(first && second, "rand_a byte carry refused");
    if (first && second) {
      auto expected=*first;
      expected.bytes[6]=0x76;
      expected.bytes[7]=0;
      expected.bytes[8]=0x80;
      std::fill(expected.bytes.begin()+9,expected.bytes.end(),0);
      Check(second->bytes==expected.bytes, "rand_a carry changed version or timestamp bits");
    }
  });
  FreshRuntime([] {
    mode=5; // All74 bits set: the first allocation consumes the maximum.
    const auto before=calls.load();
    Check(u::IssueRuntimeIdentityV7().has_value(), "maximum allocation seed refused");
    const auto start=std::chrono::steady_clock::now();
    Check(!u::IssueRuntimeIdentityV7(), "exhausted runtime reused a suffix or invented time");
    const auto elapsed=std::chrono::steady_clock::now()-start;
    Check(elapsed>=std::chrono::milliseconds(1000) && elapsed<std::chrono::seconds(5),
          "exhaustion wait was absent or unbounded");
    Check(calls==before+1, "exhaustion bypassed sequence with fresh same-ms entropy");
    clock_millis=123457;
    Check(u::IssueRuntimeIdentityV7().has_value() && calls==before+2,
          "exhausted instance failed to recover on a later accepted millisecond");
  });
  FreshRuntime([] {
    mode=5;
    const auto first=u::IssueRuntimeIdentityV7();
    clock_mode=3; // Return a later accepted millisecond after three observations.
    const auto second=u::IssueRuntimeIdentityV7();
    Check(first && second && *first < *second, "bounded wait failed to use later actual clock observation");
  });
}
bool Transfer(int fd, void* bytes, std::size_t count, bool writing) {
  auto* cursor = static_cast<unsigned char*>(bytes);
  while (count) {
    const auto n = writing ? ::write(fd, cursor, count) : ::read(fd, cursor, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    cursor += n;
    count -= static_cast<std::size_t>(n);
  }
  return true;
}
std::optional<p::Uuid> RealIssue(bool runtime) {
  if (runtime) return u::IssueRuntimeIdentityV7();
  const auto issued=u::GenerateEngineIdentityV7(p::UuidKind::object,123456);
  if (!issued.ok()) return std::nullopt;
  return issued.value.value;
}
void RealBackend(bool runtime) {
  constexpr unsigned threads = 4, per_thread = 1024;
  std::array<std::vector<p::Uuid>,threads> values;
  std::array<std::thread,threads> workers;
  std::atomic<bool> valid{true};
  for (unsigned i = 0; i < threads; ++i) {
    values[i].resize(per_thread);
    workers[i] = std::thread([&,i] {
      for (auto& value : values[i]) {
        const auto issued = RealIssue(runtime);
        if (!issued) { valid = false; return; }
        value = *issued;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  Check(valid, "real backend generation failed on a worker");
  std::vector<p::Uuid> all;
  for (const auto& list : values) all.insert(all.end(),list.begin(),list.end());
  if (runtime) {
    // This calling thread has a fresh instance. Warm it at a fixed accepted
    // timestamp so continuing its copied suffix in the child is detectable.
    // Entropy remains the actual crypto backend, in both parent and child.
    clock_reads=0;
    clock_millis=123456;
    clock_mode=1;
    const auto warm=RealIssue(true);
    Check(warm.has_value(), "pre-fork runtime warmup failed");
    if (warm) all.push_back(*warm);
  }
  // Warmed backend state is inherited by fork. Children must not replay the
  // parent's random stream. This is an actual backend/process test, not a mock.
  int channel[2];
  if (::pipe(channel) != 0) { Check(false,"pipe failed"); return; }
  const auto child = ::fork();
  if (child == 0) {
    ::close(channel[0]);
    std::array<p::Uuid,64> produced;
    for (auto& value : produced) {
      const auto issued = RealIssue(runtime);
      if (!issued) ::_exit(2);
      value = *issued;
    }
    const bool written = Transfer(channel[1], produced.data(), sizeof(produced), true);
    ::close(channel[1]);
    ::_exit(written ? 0 : 3);
  }
  ::close(channel[1]);
  if (child < 0) { ::close(channel[0]); Check(false,"fork failed"); return; }
  std::array<p::Uuid,64> received;
  for (unsigned i = 0; i < received.size(); ++i) {
    const auto issued = RealIssue(runtime);
    Check(issued.has_value(), "post-fork parent backend failed");
    all.push_back(issued.value_or(p::Uuid{}));
  }
  Check(Transfer(channel[0],received.data(),sizeof(received),false), "child UUID transfer failed");
  ::close(channel[0]);
  int status = 0;
  pid_t reaped;
  do { reaped = ::waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
  Check(reaped == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child issuer failed");
  all.insert(all.end(),received.begin(),received.end());
  for (const auto& value : all)
    Check(value.bytes[6] >> 4 == 7 && value.bytes[8] >> 6 == 2 && !value.is_nil(),
          "real backend produced malformed UUIDv7");
  std::sort(all.begin(),all.end());
  Check(std::adjacent_find(all.begin(),all.end()) == all.end(), "thread/fork stream reused a UUID");
  clock_mode=0;
  Check(u::IssueRuntimeIdentityV7().has_value(), "runtime issuer failed to recover after provider restoration");
}
} // namespace

extern "C" int __real_RAND_bytes(unsigned char*, int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes, int count) {
  ++calls;
  const auto selected = mode.load();
  if (selected == 0) return __real_RAND_bytes(bytes,count);
  if (selected==4 || selected==5 || selected==6) {
    std::memset(bytes,0xff,static_cast<std::size_t>(count));
    if (selected==4 && count>8) { bytes[6]=0xa5; bytes[7]=0xa5; }
    if (selected==6 && count>8) bytes[6]=0xa5;
    return 1;
  }
  if (count > 0)
    std::memset(bytes, 0xa5, static_cast<std::size_t>(selected == 1 ? count : count/2));
  return selected == 1 ? 1 : selected == 2 ? 0 : -1;
}
extern "C" scratchbird::core::time::ClockSnapshotResult
__real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult
__wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  const auto selected=clock_mode.load();
  if (!selected) return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  const auto sequence=++clock_reads;
  scratchbird::core::time::ClockSnapshotResult result;
  if (clock_failed) {
    result.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};
    return result;
  }
  const auto millis=clock_millis.load() + (selected==3 && sequence>3 ? 1 : 0);
  result.value.wall_clock={static_cast<p::i64>(millis/1000),
                          static_cast<p::u32>((millis%1000)*1000000)};
  result.value.monotonic.ticks=selected==2 ? 0 : sequence;
  return result;
}
int main() {
  try {
    SourceFaultsAndLayout();
    WallClockConversion();
    RuntimeAllocation();
    RealBackend(false);
    std::thread runtime_thread([] { RealBackend(true); });
    runtime_thread.join();
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return 2;
  }
  std::cout << "checks=" << checks << " failures=" << failures
            << " real_thread_uuids=8192 real_fork_uuids=256\n";
  return failures ? 1 : 0;
}
