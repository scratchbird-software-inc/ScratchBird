// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"
#include "time.hpp"

#include <chrono>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace scratchbird::core::uuid {
namespace {
namespace time = scratchbird::core::time;

std::uint64_t ProcessIdentity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

struct RuntimeGenerator {
  std::mutex mutex;
  std::uint64_t process = ProcessIdentity();
  time::LocalTimeAuthorityState clock;
  std::optional<std::uint64_t> last_millis;
  Uuid last_value;
};

// Increment exactly the 74 allocation bits. Work on an unpublished copy:
// exhaustion must neither wrap the retained value nor advance its timestamp.
bool IncrementAllocation(Uuid& value) noexcept {
  for (unsigned i = 15; i > 8; --i) {
    if (value.bytes[i] != 255) { ++value.bytes[i]; return true; }
    value.bytes[i] = 0;
  }
  if ((value.bytes[8] & 63) != 63) { ++value.bytes[8]; return true; }
  value.bytes[8] = 0x80;
  if (value.bytes[7] != 255) { ++value.bytes[7]; return true; }
  value.bytes[7] = 0;
  if ((value.bytes[6] & 15) != 15) { ++value.bytes[6]; return true; }
  return false;
}
// OS entropy is private to bootstrap identities and startup diagnostics.
// It is never consulted by ordinary generation, even after RAND failure.
std::optional<Uuid> BootstrapSeed(u64 millis) noexcept {
  Uuid candidate{};
  struct Erase {
    Uuid& value;
    ~Erase() {volatile auto* p=value.bytes.data(); for (unsigned i=0;i<16;++i) p[i]=0;}
  } erase{candidate};
  if (millis>0xffffffffffffULL) return std::nullopt;
#if defined(__linux__)
  std::size_t offset=0;
  for (unsigned attempts=0;offset<16 && attempts<32;++attempts) {
    const auto count=::getrandom(candidate.bytes.data()+offset,16-offset,GRND_NONBLOCK);
    if (count<0 && errno==EINTR) continue;
    if (count<=0 || static_cast<std::size_t>(count)>16-offset) return std::nullopt;
    offset+=static_cast<std::size_t>(count);
  }
  if (offset!=16) return std::nullopt;
#elif defined(_WIN32)
  if (::BCryptGenRandom(nullptr,candidate.bytes.data(),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
    return std::nullopt;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  bool complete=false;
  for (unsigned attempts=0;attempts<32;++attempts) {
    if (::getentropy(candidate.bytes.data(),16)==0) {complete=true; break;}
    if (errno!=EINTR) return std::nullopt;
  }
  if (!complete) return std::nullopt;
#else
  return std::nullopt;
#endif
  for (unsigned i=0;i<6;++i) candidate.bytes[i]=static_cast<byte>(millis>>(40-8*i));
  candidate.bytes[6]=static_cast<byte>((candidate.bytes[6]&15)|0x70);
  candidate.bytes[8]=static_cast<byte>((candidate.bytes[8]&63)|0x80);
  return candidate;
}

std::optional<Uuid> IssueIdentity(RuntimeGenerator& generator, bool bootstrap) noexcept {
  try {
    // Ordinary issuance retains one instance per thread; bootstrap retains one
    // for its complete batch. Neither inherits another thread's held mutex.
    // This bounded runtime state is not database/cluster time authority.
    const auto process = ProcessIdentity();
    if (generator.process != process) {
      // A child must not continue its parent's allocation suffix or clock
      // history, even when both read the same millisecond after fork.
      generator.process = process;
      generator.clock = {};
      generator.last_millis.reset();
      generator.last_value = {};
    }
    constexpr auto wait_limit = std::chrono::milliseconds(1000);
    std::optional<std::chrono::steady_clock::time_point> wait_started;
    for (;;) {
      if (wait_started && std::chrono::steady_clock::now() - *wait_started >= wait_limit)
        return std::nullopt;
      {
        std::lock_guard lock(generator.mutex);
        const auto snapshot = time::ReadLocalNodeClockSnapshot();
        if (!snapshot.ok() ||
            generator.clock.accepted_observations == (std::numeric_limits<u64>::max)())
          return std::nullopt;
        // High-security standalone runtime default: reject backward wall or
        // monotonic movement. No cluster state is fabricated from this clock.
        const auto observed = time::ObserveLocalNodeClock(
            generator.clock, snapshot.value, time::LocalTimeAuthorityPolicy{});
        if (!observed.ok()) return std::nullopt;
        const auto converted = time::WallClockToUuidV7Millis(snapshot.value.wall_clock);
        if (!converted.ok()) return std::nullopt;
        const auto millis = converted.unix_epoch_millis;
        if (generator.last_millis && millis < *generator.last_millis) return std::nullopt;

        Uuid candidate;
        bool available = true;
        if (!generator.last_millis || millis > *generator.last_millis) {
          if (bootstrap) {
            const auto generated=BootstrapSeed(millis);
            if (!generated) return std::nullopt;
            candidate=*generated;
          } else {
            const auto generated = GenerateCompatibilityUnixTimeV7(millis);
            if (!generated.ok()) return std::nullopt;
            candidate = generated.value;
          }
        } else {
          candidate = generator.last_value;
          available = IncrementAllocation(candidate);
        }
        if (available) {
          if (wait_started && std::chrono::steady_clock::now() - *wait_started >= wait_limit)
            return std::nullopt;
          // Publish the complete binary allocation and its clock observation
          // together, only after every source and range check succeeded.
          generator.last_value = candidate;
          generator.last_millis = millis;
          generator.clock = observed.state;
          return candidate;
        }
      }
      // An exhausted suffix waits for an actual, strictly later accepted
      // physical millisecond. Never invent a future tick or reuse the maximum.
      const auto now = std::chrono::steady_clock::now();
      if (!wait_started) wait_started = now;
      if (now - *wait_started >= wait_limit) return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (...) {
    // The caller's optional failure channel also survives allocation, clock
    // and mutex failures. There is no diagnostic-UUID recursion or nil success.
    return std::nullopt;
  }
}
} // namespace

namespace {
// Establish process ownership before touching a function-static generator.
// A fork during initialization/issuance refuses before any inherited mutex or
// initialization guard, even if the parent later fails the source operation.
std::atomic<std::uint64_t> diagnostic_process{0};
std::atomic_flag diagnostic_claim=ATOMIC_FLAG_INIT;
std::atomic<bool> diagnostic_sealed{false};
bool DiagnosticProcessMatches() noexcept {
  const auto process=ProcessIdentity();
  std::uint64_t unclaimed=0;
  diagnostic_process.compare_exchange_strong(unclaimed,process);
  return diagnostic_process.load()==process;
}
struct DiagnosticClaimRelease {
  ~DiagnosticClaimRelease() {diagnostic_claim.clear(std::memory_order_release);}
};
}

CryptoBootstrapDiagnosticIdentityResult IssueCryptoBootstrapDiagnosticIdentityV7() noexcept {
  using E=CryptoBootstrapDiagnosticError;
  if constexpr (!std::atomic<std::uint64_t>::is_always_lock_free)
    return {E::unsupported_platform,{}};
  if(!DiagnosticProcessMatches()) return {E::foreign_process,{}};
  if(diagnostic_claim.test_and_set(std::memory_order_acquire)) return {E::busy,{}};
  DiagnosticClaimRelease release;
  if(diagnostic_sealed.load()) return {E::sealed,{}};
  try {
    static unsigned attempts=0; // Serialized by the claim; failures consume quota too.
    if(attempts==kCryptoBootstrapDiagnosticAttemptLimit) return {E::exhausted,{}};
    ++attempts;
    static RuntimeGenerator generator;
    const auto generated=IssueIdentity(generator,true);
    return generated ? CryptoBootstrapDiagnosticIdentityResult{E::none,*generated} :
                       CryptoBootstrapDiagnosticIdentityResult{E::source_failure,{}};
  } catch(...) {return {E::source_failure,{}};}
}

CryptoBootstrapDiagnosticError SealCryptoBootstrapDiagnosticIdentities() noexcept {
  using E=CryptoBootstrapDiagnosticError;
  if constexpr (!std::atomic<std::uint64_t>::is_always_lock_free)
    return E::unsupported_platform;
  if(!DiagnosticProcessMatches()) return E::foreign_process;
  if(diagnostic_claim.test_and_set(std::memory_order_acquire)) return E::busy;
  DiagnosticClaimRelease release;
  diagnostic_sealed.store(true);
  return E::none;
}

std::optional<Uuid> IssueRuntimeIdentityV7() noexcept {
  try {
    thread_local RuntimeGenerator generator;
    return IssueIdentity(generator,false);
  } catch (...) {return std::nullopt;}
}

std::optional<CryptoBootstrapIdentities> IssueCryptoBootstrapIdentitiesV7() noexcept {
  static std::atomic_flag claimed=ATOMIC_FLAG_INIT;
  if (claimed.test_and_set(std::memory_order_acquire)) return std::nullopt;
  bool published=false;
  struct ReleaseClaim {
    std::atomic_flag& flag; bool& published;
    ~ReleaseClaim() {if (!published) flag.clear(std::memory_order_release);}
  } release{claimed,published};
  try {
    RuntimeGenerator generator;
    CryptoBootstrapIdentities batch{};
    for (auto* id : {&batch.process,&batch.operation,&batch.owner,&batch.context}) {
      const auto next=IssueIdentity(generator,true);
      if (!next) return std::nullopt;
      *id=*next;
    }
    published=true;
    return batch;
  } catch (...) {return std::nullopt;}
}
} // namespace scratchbird::core::uuid
