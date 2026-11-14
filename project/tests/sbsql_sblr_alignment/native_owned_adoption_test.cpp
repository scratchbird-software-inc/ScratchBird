// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// Owned-source adoption and native writer custody over a database written by
// the production create (drift removal C3d). Adoption validates the file
// through page zero's fixed roots. Both entry points are exercised: direct
// adoption of caller-opened files, and adoption through the real route
// transfer (the one production custody uses). Faults are real: they are
// injected at the OS and provider boundary (pread, pwrite, fsync, close, heap,
// digest provider), never by replacing an engine response. Each fault run
// asserts the fault was reached, and every sweep ends in a clean run.
// The production create writes one file, so every check is a one-member cohort.
// Purpose inventory: Claude-temp-directory/2026-10-30-full-run/
//   firebird-mga-research/16-c3d-test-purpose-inventory.md (private notes).
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "native_owned_checkpoint_writer.hpp"
#include "../../src/server/database_ownership.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
// Heap: count, fail the Nth, or run a callback inside the next allocation.
long allocation_budget = -1;
bool counting = false;
unsigned long allocations = 0;
void (*allocation_callback)() = nullptr;
// Memory fence probe: an allocation made while another thread cannot take the
// probed device mutex is an allocation under that device's guard.
std::recursive_mutex* fence_mutex = nullptr;
bool fence_backing_only = false;
unsigned fence_probes = 0, fence_locked = 0;
void ProbeFence(bool backing = false) {
  if (fence_backing_only && !backing) return;
  auto* mutex = fence_mutex;
  if (!mutex) return;
  fence_mutex = nullptr;
  bool available = false;
  std::thread probe([&] {
    available = mutex->try_lock();
    if (available) mutex->unlock();
  });
  probe.join();
  ++fence_probes;
  if (!available) ++fence_locked;
  fence_mutex = mutex;
}
// I/O: count, fail, corrupt, tear, pause.
bool io_counting = false;
unsigned reads = 0, writes = 0, syncs = 0, read_fault = 0, corrupt_read = 0, write_fault = 0, sync_fault = 0;
std::size_t torn_bytes = 0;
std::uint64_t observed_read_bytes = 0;
std::atomic<unsigned> read_pause{0};  // 1 armed, 2 paused in a pread at offset 0, 3 released
void (*read_callback)() = nullptr;    // runs once, on the reading thread, in the next pread at offset 0
// Digest provider: fail one phase at the Nth digest site.
unsigned hash_fault = 0, hash_target = 1, hash_seen = 0;
bool hash_active = false, hash_counting = false;
// close(): a real release followed by EINTR, with the descriptor number reused.
bool late_close_fault = false;
int reused_fd = -1, retired_fd = -1;
}  // namespace

void* operator new(std::size_t n) {
  ProbeFence();
  if (counting) ++allocations;
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t alignment) {
  ProbeFence(true);
  if (counting) ++allocations;
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  const auto a = static_cast<std::size_t>(alignment);
  n = n ? n : 1;
  if (n > std::numeric_limits<std::size_t>::max() - (a - 1)) throw std::bad_alloc();
  if (auto* p = std::aligned_alloc(a, (n + a - 1) / a * a)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { ProbeFence(); std::free(p); }
void operator delete[](void* p) noexcept { ProbeFence(); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ProbeFence(); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { ProbeFence(); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { ProbeFence(true); std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ProbeFence(true); std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ProbeFence(true); std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ProbeFence(true); std::free(p); }

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t bytes) {
  if (auto callback = allocation_callback) {
    allocation_callback = nullptr;
    callback();
  }
  return __real_malloc(bytes);
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t n, off_t at) {
  if (at == 0)
    if (auto callback = read_callback) {
      read_callback = nullptr;
      callback();
    }
  if (at == 0) {
    unsigned expected = 1;
    if (read_pause.compare_exchange_strong(expected, 2))
      while (read_pause == 2) std::this_thread::yield();
  }
  if (io_counting && ++reads == read_fault) { errno = EIO; return -1; }
  const auto result = __real_pread(fd, data, n, at);
  if (io_counting && result > 0) observed_read_bytes += static_cast<std::uint64_t>(result);
  if (io_counting && reads == corrupt_read && result > 0) static_cast<unsigned char*>(data)[result - 1] ^= 1;
  return result;
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t n, off_t at) {
  if (io_counting && ++writes == write_fault) {
    if (torn_bytes) {
      const auto result = __real_pwrite(fd, data, std::min(n, torn_bytes), at);
      if (result < 0) return result;
    }
    errno = EIO;
    return -1;
  }
  return __real_pwrite(fd, data, n, at);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (io_counting && ++syncs == sync_fault) { errno = EIO; return -1; }
  return __real_fsync(fd);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
  if (!late_close_fault) return __real_close(fd);
  late_close_fault = false;
  const int status = __real_close(fd);
  if (status) return status;
  retired_fd = fd;
  reused_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  errno = EINTR;
  return -1;
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  hash_active = (hash_fault || hash_counting) && ++hash_seen == hash_target && hash_fault;
  if (hash_active && hash_fault == 1) { hash_fault = 0; return nullptr; }
  return __real_EVP_MD_CTX_new();
}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c, const EVP_MD* m, ENGINE* e) {
  if (hash_active && hash_fault == 2) { hash_fault = 0; return 0; }
  return __real_EVP_DigestInit_ex(c, m, e);
}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*, const void*, size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c, const void* b, size_t n) {
  if (hash_active && hash_fault == 3) { hash_fault = 0; return 0; }
  return __real_EVP_DigestUpdate(c, b, n);
}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c, unsigned char* b, unsigned int* n) {
  if (hash_active && hash_fault == 4) { hash_fault = 0; return 0; }
  const int r = __real_EVP_DigestFinal_ex(c, b, n);
  if (hash_active && hash_fault == 5) { hash_fault = 0; *n = 31; }
  return r;
}
extern "C" int __real_EVP_Digest(const void*, size_t, unsigned char*, unsigned int*, const EVP_MD*, ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data, size_t bytes, unsigned char* out, unsigned int* size, const EVP_MD* md,
                                 ENGINE* engine) {
  const bool selected = (hash_fault || hash_counting) && ++hash_seen == hash_target && hash_fault;
  const auto mode = selected ? hash_fault : 0;
  if (selected) hash_fault = 0;
  if (mode && mode != 5) return 0;
  const auto result = __real_EVP_Digest(data, bytes, out, size, md, engine);
  if (mode == 5) *size = 31;
  return result;
}

namespace owned_adoption_test {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
namespace server = scratchbird::server;
using namespace scratchbird::core::platform;
using Source = db::NativeOwnedCheckpointSource;
using Writer = db::NativeOwnedCheckpointWriter;
using E = db::NativeOwnedSourceError;
using WE = db::NativeWriterError;
template <class T> concept ExposesDevices = requires(T value) { value.devices(); };
static_assert(!ExposesDevices<Writer> && !ExposesDevices<Writer::Operation> &&
              !ExposesDevices<Writer::FixedRootReadLease>);
static_assert(!std::is_default_constructible_v<Writer> && !std::is_copy_constructible_v<Writer::Operation> &&
              !std::is_copy_constructible_v<Writer::FixedRootReadLease>);

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " line=" + std::to_string(at.line()));
}
Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
std::string Why(const db::NativeOwnedSourceDiagnostics& r) {
  return " error=" + std::to_string(int(r.error)) + " bootstrap=" + std::to_string(int(r.bootstrap_error)) +
         " fixed_root=" + std::to_string(int(r.fixed_root_error)) + " page_zero=" + std::to_string(int(r.page_zero_error)) +
         " inventory=" + std::to_string(int(r.inventory_error)) + " allocation=" + std::to_string(int(r.allocation_error)) +
         " directory=" + std::to_string(int(r.directory_error)) + " route=" + std::to_string(int(r.route_error)) +
         " memory=" + std::to_string(int(r.memory_error)) + " bytes=" + std::to_string(r.physical_bytes_read);
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

// The parent points TMPDIR at its private directory; a child process creates
// its own directory inside it, and the parent's cleanup removes both.
struct Artifacts {
  scratchbird::tests::OwnedTempDirectory directory;
  std::optional<std::string> previous;
  Artifacts() {
    if (const auto* value = std::getenv("TMPDIR")) previous = value;
    Check(!::setenv("TMPDIR", directory.path().c_str(), 1), "private adoption fixture namespace");
  }
  ~Artifacts() { Restore(); }
  void Restore() {
    if (previous) (void)::setenv("TMPDIR", previous->c_str(), 1);
    else (void)::unsetenv("TMPDIR");
  }
  void Cleanup() { Restore(); directory.Cleanup(); }
};
Artifacts* artifacts = nullptr;

struct Grant {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 1ull << 30;
    p.per_context_limit_bytes = 1ull << 30;
    return p;
  }
  Grant(const Uuid& database, u64 bytes) : manager(Policy()), binding{database, Fixed(61), Fixed(62), Fixed(63)} {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.owned.adoption";
    r.purpose = "owned adoption conformance";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "owned adoption conformance";
    for (const auto& scope : r.scope_chain) {
      m::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "parent budget");
    }
    auto grant = m::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "node-issued grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "grant adoption");
    memory = std::move(adopted.memory);
  }
  u64 Charged() { return manager.Snapshot().current_bytes; }
  u64 Truncations() { return manager.Snapshot().telemetry_truncation_count; }
};

// One production-created database per page-size profile, written once per
// process; every fixture works on its own byte-exact copy.
struct Created {
  std::string path;
  db::NativeDatabaseCreateReceipt receipt;
  u64 size = 0;
  Uuid profile;
  byte first = 0;  // the file's first byte, read independently
};
const Created& Production(unsigned profile_index) {
  static std::map<unsigned, Created> made;
  if (const auto at = made.find(profile_index); at != made.end()) return at->second;
  const auto& p = d::kCanonicalFilespacePageProfiles[profile_index];
  Created out;
  out.size = p.page_size_bytes;
  out.profile = p.uuid;
  db::NativeDatabaseCreateRequest r;
  r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  r.database_name = "orders";
  r.database_default_language = "en";
  r.page_size_profile_uuid = p.uuid;
  r.page_size_bytes = p.page_size_bytes;
  r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
  r.creation_utc_millis = 1789357072000ULL;
  r.engine_uuid = Fixed(4);
  r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
  r.bootstrap_principal_name = "BootstrapAdmin";
  r.bootstrap_credential_fingerprint = kFingerprint;
  r.service_principal_name = "NodeService";
  r.service_credential_fingerprint = kFingerprint;
  r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
  out.path = (artifacts->directory.path() / ("created-" + std::to_string(profile_index))).string();
  d::FileDevice device;
  Check(device.Open(out.path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "production create device");
  u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
  auto result = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
  Check(result.ok(), "the production create writes a complete native database");
  out.receipt = *result.receipt;
  Check(device.Close().ok(), "release the creating handle");
  std::ifstream raw(out.path, std::ios::binary);
  char first = 0;
  Check(bool(raw.read(&first, 1)), "independent read of the created file's first byte");
  out.first = static_cast<byte>(first);
  return made.emplace(profile_index, std::move(out)).first->second;
}
std::string CopyOf(const Created& made) {
  static unsigned next = 0;
  const auto path = (artifacts->directory.path() / ("copy-" + std::to_string(next++))).string();
  std::filesystem::copy_file(made.path, path);
  return path;
}
void FlipByte(const std::string& path, u64 offset) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  char value = 0;
  Check(bool(file.seekg(static_cast<std::streamoff>(offset)).read(&value, 1)), "read a byte to corrupt");
  value = static_cast<char>(value ^ 1);
  Check(bool(file.seekp(static_cast<std::streamoff>(offset)).write(&value, 1)) && bool(file.flush()), "corrupt the byte");
}

// ---------------------------------------------------------------------------
// Owned-source adoption, either entry point.
struct Adoption {
  const Created& made;
  std::string path;
  bool via_route;
  server::DatabaseOwnershipResult route;
  d::RouteSourceTransitionResult transition;
  std::vector<std::unique_ptr<d::FileDevice>> files;  // direct: the primary; route: no secondaries
  bool owns_copy;
  Adoption(unsigned profile, bool route_entry, std::string existing = {})
      : made(Production(profile)), path(existing.empty() ? CopyOf(made) : existing), via_route(route_entry),
        owns_copy(existing.empty()) { Open(); }
  ~Adoption() {
    Release();
    if (owns_copy) std::filesystem::remove(path);
  }
  std::size_t CallerCount() const { return via_route ? 0 : 1; }
  void Open() {
    Release();
    if (via_route) {
      server::DatabaseOwnershipRequest request;
      request.database_path = path;
      request.storage_access = d::RouteStorageAccess::read_write;
      route = server::AcquireDatabaseOwnership(request);
      Check(route.acquired && route.lock, "actual route owner for governed native source");
      transition = route.lock->BeginNativeSourceTransition();
      Check(transition.ok(), "actual private route transition for memory-backed source");
    } else {
      auto file = std::make_unique<d::FileDevice>();
      Check(file->Open(path, d::FileOpenMode::open_existing).ok(), "open the production file for direct adoption");
      files.push_back(std::move(file));
    }
  }
  void Release() {
    files.clear();
    transition.transition.reset();
    route.lock.reset();
  }
  db::NativeOwnedCheckpointMemoryLimits Limits(std::size_t owner_bytes = 64 * 1024) const {
    return {owner_bytes, {256 * made.size, 64u << 20}};
  }
  db::NativeOwnedSourceResult Adopt(db::NativeOwnedCheckpointMemoryLimits limits, Grant& persistent, Grant& transient,
                                    const db::NativeStorageMemoryBinding* persistent_binding = nullptr,
                                    const db::NativeStorageMemoryBinding* transient_binding = nullptr,
                                    std::optional<Uuid> database = std::nullopt, std::optional<Uuid> primary = std::nullopt) {
    const auto& pb = persistent_binding ? *persistent_binding : persistent.binding;
    const auto& tb = transient_binding ? *transient_binding : transient.binding;
    const auto& dbid = database ? *database : made.receipt.database_uuid;
    const auto& fs = primary ? *primary : made.receipt.filespace_uuid;
    if (via_route)
      return Source::AdoptRouteWithMemory(transition.transition, dbid, fs, files, limits, persistent.memory, pb,
                                          transient.memory, tb);
    return Source::AdoptWithMemory(dbid, fs, files, limits, persistent.memory, pb, transient.memory, tb);
  }
  std::recursive_mutex* PrimaryMutex() {
    Check(!via_route && !files.empty(), "fence probe needs the caller-held primary");
    auto guard = files.front()->AcquireOperationGuard();
    return guard.mutex();
  }
};

// A refused adoption leaves every caller handle open, the grant unconsumed and
// no owned payload charged.
void Preserved(Adoption& a, Grant& persistent, const db::NativeOwnedSourceResult& r, std::string_view what) {
  Check(!r.ok() && !r.owner && a.files.size() == a.CallerCount() && persistent.memory.Matches(persistent.binding) &&
            !persistent.Charged(),
        std::string(what) + ": refused adoption preserves caller files and grant with no owned payload" + Why(r));
  for (const auto& f : a.files) Check(f && f->is_open(), "every refused native member remains open and caller-owned");
}

void CompleteAdoption(unsigned profile, bool via_route) {
  Adoption a(profile, via_route);
  Grant transient(a.made.receipt.database_uuid, 128u << 20);
  std::size_t exact_owner = 0;
  {
    Grant persistent(a.made.receipt.database_uuid, 1u << 20);
    std::shared_ptr<d::RouteOwnershipLease> legacy;
    if (via_route) {
      // A live legacy borrower blocks the transfer before any byte is read.
      a.Release();
      server::DatabaseOwnershipRequest request;
      request.database_path = a.path;
      request.storage_access = d::RouteStorageAccess::read_write;
      a.route = server::AcquireDatabaseOwnership(request);
      Check(a.route.acquired && a.route.lock, "route owner before legacy pin");
      legacy = d::RouteOwnershipLease::Borrow(a.path + ".sb.route.owner.lock");
      Check(bool(legacy), "actual legacy borrower before the native fence");
      a.transition = a.route.lock->BeginNativeSourceTransition();
      Check(a.transition.ok(), "transition fences legacy admission");
      const auto busy = a.Adopt(a.Limits(), persistent, transient);
      Check(busy.error == E::route_admission && busy.route_error == d::RouteSourceTransitionError::not_drained &&
                !busy.owner && !busy.physical_bytes_read && persistent.memory.Matches(persistent.binding),
            "a legacy borrower prevents route-owned admission without consuming grants");
      legacy.reset();
    }
    reads = writes = syncs = 0;
    observed_read_bytes = 0;
    io_counting = true;
    auto admitted = a.Adopt(a.Limits(), persistent, transient);
    io_counting = false;
    Check(admitted.ok() && a.files.empty() && admitted.owner->memory_backed(),
          "complete native source takes the device and persistent memory once" + Why(admitted));
    Check(!writes && !syncs && admitted.physical_bytes_read == observed_read_bytes && observed_read_bytes,
          "adoption only reads, and reports exactly the bytes it read");
    Check(persistent.memory.CheckBinding(persistent.binding) == db::NativeStorageMemoryError::invalid_grant &&
              persistent.Charged() && persistent.Charged() <= 64 * 1024,
          "persistent owner payload actually charged and original wrapper consumed");
    exact_owner = admitted.owner->owner_backing_bytes_used();
    Check(exact_owner && exact_owner <= 64 * 1024, "actual ownership and descriptor layout bounded");
    Check(!transient.Charged(), "the transient validation read returns every byte after adoption");
    if (via_route) {
      std::vector<std::unique_ptr<d::FileDevice>> none;
      Grant second(a.made.receipt.database_uuid, 1u << 20);
      const auto duplicate = Source::AdoptRouteWithMemory(a.transition.transition, a.made.receipt.database_uuid,
                                                          a.made.receipt.filespace_uuid, none, a.Limits(),
                                                          second.memory, second.binding, transient.memory,
                                                          transient.binding);
      Check(duplicate.error == E::route_admission &&
                duplicate.route_error == d::RouteSourceTransitionError::already_transferred && !duplicate.owner,
            "the route transition is consumed once");
    }
    admitted.owner.reset();
    Check(!persistent.Charged(), "final release of the owner returns its persistent charge");
  }
  // Refusals before effects, then exact backing, then a fresh owner.
  a.Open();
  Grant persistent(a.made.receipt.database_uuid, exact_owner);
  for (unsigned field = 0; field < 3; ++field) {
    auto zero = a.Limits(exact_owner);
    if (field == 0) zero.maximum_owner_backing_bytes = 0;
    if (field == 1) zero.source.maximum_verification_image_bytes = 0;
    if (field == 2) zero.source.maximum_backing_bytes = 0;
    const auto r = a.Adopt(zero, persistent, transient);
    Preserved(a, persistent, r, "zero allowance");
    Check(r.error == E::invalid_request && !r.physical_bytes_read, "zero owner or source allowance refuses before effects");
  }
  for (unsigned field = 0; field < 2; ++field) {
    const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient, nullptr, nullptr,
                           field ? std::optional<Uuid>{} : Uuid{}, field ? Uuid{} : std::optional<Uuid>{});
    Preserved(a, persistent, r, "nil identity");
    Check(r.error == E::invalid_request && !r.physical_bytes_read, "nil native ownership identity is not admitted");
  }
  {
    const auto r = a.Adopt(a.Limits(exact_owner - 1), persistent, transient);
    Preserved(a, persistent, r, "short owner layout");
    Check(r.error == E::resource_exhausted && !r.physical_bytes_read, "one-byte-short real ownership layout refuses before I/O");
  }
  for (unsigned side = 0; side < 2; ++side)
    for (unsigned field = 0; field < 4; ++field) {
      auto binding = persistent.binding;
      const std::array<Uuid*, 4> ids{&binding.database_uuid, &binding.operation_uuid, &binding.owner_uuid,
                                     &binding.context_uuid};
      *ids[field] = Fixed(249);
      const auto r = side ? a.Adopt(a.Limits(exact_owner), persistent, transient, nullptr, &binding)
                          : a.Adopt(a.Limits(exact_owner), persistent, transient, &binding, nullptr);
      Preserved(a, persistent, r, "binding dimension");
      Check(r.error == E::memory_failure && !r.physical_bytes_read,
            "all persistent and transient binary ownership dimensions checked before I/O");
    }
  {
    const auto r = a.via_route
                       ? Source::AdoptRouteWithMemory(a.transition.transition, a.made.receipt.database_uuid,
                                                      a.made.receipt.filespace_uuid, a.files, a.Limits(exact_owner),
                                                      persistent.memory, persistent.binding, persistent.memory,
                                                      persistent.binding)
                       : Source::AdoptWithMemory(a.made.receipt.database_uuid, a.made.receipt.filespace_uuid, a.files,
                                                 a.Limits(exact_owner), persistent.memory, persistent.binding,
                                                 persistent.memory, persistent.binding);
    Preserved(a, persistent, r, "one grant for both");
    Check(r.error == E::invalid_request && !r.physical_bytes_read, "persistent and transient work require distinct actual grants");
  }
  {
    Grant short_owner(a.made.receipt.database_uuid, exact_owner - 1);
    const auto r = a.Adopt(a.Limits(exact_owner), short_owner, transient);
    Check(!r.ok() && r.error == E::memory_failure && !r.physical_bytes_read && !short_owner.Charged() &&
              a.files.size() == a.CallerCount(),
          "one byte short actual persistent grant refuses before source I/O");
  }
  {
    Grant short_source(a.made.receipt.database_uuid, 4096);
    const auto r = a.Adopt(a.Limits(exact_owner), persistent, short_source);
    Preserved(a, persistent, r, "short transient grant");
    Check(r.error == E::source_failure && r.fixed_root_error != db::NativeFixedRootMemoryError::none &&
              r.physical_bytes_read >= d::SerializedFilespaceBootstrap{}.size() && !short_source.Charged(),
          "a transient grant too small for the fixed-root read refuses after the bootstrap read, keeping no source" + Why(r));
  }
  if (!via_route) {
    // A null member, a closed member, a foreign thread and an extra member
    // that is a byte copy of the primary are all refused without adoption.
    {
      auto saved = std::move(a.files.front());
      const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient);
      Check(!r.ok() && r.error == E::device_ownership && !r.physical_bytes_read &&
                persistent.memory.Matches(persistent.binding) && !persistent.Charged(),
            "null owned member refuses before locks or grants");
      a.files.front() = std::move(saved);
    }
    Check(a.files.front()->Close().ok(), "closed owned member fixture");
    {
      const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient);
      Check(!r.ok() && r.error == E::device_ownership && !r.physical_bytes_read && a.files.size() == 1 &&
                persistent.memory.Matches(persistent.binding),
            "closed member refuses without partial adoption");
    }
    a.Open();
    {
      E foreign = E::none;
      std::thread wrong([&] { foreign = a.Adopt(a.Limits(exact_owner), persistent, transient).error; });
      wrong.join();
      Check(foreign == E::device_ownership, "wrong opening thread cannot move independently locked devices");
    }
    {
      const auto copy = CopyOf(a.made);
      auto extra = std::make_unique<d::FileDevice>();
      Check(extra->Open(copy, d::FileOpenMode::open_existing).ok(), "a byte copy of the primary as an extra member");
      a.files.push_back(std::move(extra));
      const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient);
      Check(!r.ok() && !r.owner && a.files.size() == 2 && persistent.memory.Matches(persistent.binding),
            "a duplicate of the primary cannot join a one-file cohort" + Why(r));
      a.files.pop_back();
    }
    // Admission and refused cleanup allocate nothing under the device guard.
    {
      fence_probes = fence_locked = 0;
      fence_backing_only = true;
      reads = 0;
      read_fault = 1;
      io_counting = true;
      fence_mutex = a.PrimaryMutex();
      const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient);
      fence_mutex = nullptr;
      io_counting = false;
      read_fault = 0;
      fence_backing_only = false;
      Preserved(a, persistent, r, "fenced read fault");
      Check(fence_probes && !fence_locked, "refused admission cleanup allocates no backing under the device guard");
    }
  }
  {
    // A malformed bootstrap is refused, typed, and never repaired.
    a.Release();
    FlipByte(a.path, 0);
    a.Open();
    const auto r = a.Adopt(a.Limits(exact_owner), persistent, transient);
    Preserved(a, persistent, r, "malformed bootstrap");
    Check(r.error == E::bootstrap_failure && r.bootstrap_error != d::FilespaceBootstrapError::none && r.physical_bytes_read,
          "malformed actual bootstrap preserves typed failure without ownership transfer");
    a.Release();
    std::ifstream raw(a.path, std::ios::binary);
    char first = 0;
    Check(bool(raw.read(&first, 1)) && static_cast<byte>(first) == static_cast<byte>(a.made.first ^ 1),
          "refused admission does not repair source");
    raw.close();
    FlipByte(a.path, 0);
    a.Open();
  }
  if (via_route) {
    // Withdrawal while adoption is paused in a real pread: a competing
    // admission is refused and no stale owner is published.
    read_pause = 1;
    bool entered = false;
    d::RouteSourceTransitionError competing = d::RouteSourceTransitionError::none;
    std::thread withdrawer([&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (read_pause != 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      entered = read_pause == 2;
      if (entered) {
        std::vector<std::unique_ptr<d::FileDevice>> none;
        Grant other(a.made.receipt.database_uuid, exact_owner);
        competing = Source::AdoptRouteWithMemory(a.transition.transition, a.made.receipt.database_uuid,
                                                 a.made.receipt.filespace_uuid, none, a.Limits(exact_owner),
                                                 other.memory, other.binding, transient.memory, transient.binding)
                        .route_error;
        a.route.lock->release();
      }
      read_pause = 3;
    });
    const auto withdrawn = a.Adopt(a.Limits(exact_owner), persistent, transient);
    withdrawer.join();
    read_pause = 0;
    Preserved(a, persistent, withdrawn, "withdrawal");
    Check(entered && competing == d::RouteSourceTransitionError::wrong_thread && withdrawn.error == E::route_admission &&
              withdrawn.route_error == d::RouteSourceTransitionError::withdrawn,
          "issuer withdrawal during I/O, and an admission from another thread, cannot publish a stale native owner" +
              Why(withdrawn));
    a.Open();
    // A second admission on the admitting thread itself, while the first is
    // reading, finds the admission busy.
    static Adoption* reentrant = nullptr;
    static d::RouteSourceTransitionError reentrant_error = d::RouteSourceTransitionError::none;
    static std::size_t reentrant_owner = 0;
    reentrant = &a;
    reentrant_owner = exact_owner;
    reentrant_error = d::RouteSourceTransitionError::none;
    read_callback = [] {
      std::vector<std::unique_ptr<d::FileDevice>> none;
      Grant other(reentrant->made.receipt.database_uuid, reentrant_owner);
      Grant scratch(reentrant->made.receipt.database_uuid, 128u << 20);
      reentrant_error = Source::AdoptRouteWithMemory(reentrant->transition.transition, reentrant->made.receipt.database_uuid,
                                                     reentrant->made.receipt.filespace_uuid, none,
                                                     reentrant->Limits(reentrant_owner), other.memory, other.binding,
                                                     scratch.memory, scratch.binding)
                            .route_error;
    };
    {
      Grant first_grant(a.made.receipt.database_uuid, exact_owner);
      auto first = a.Adopt(a.Limits(exact_owner), first_grant, transient);
      read_callback = nullptr;
      Check(reentrant_error == d::RouteSourceTransitionError::admission_busy && first.ok(),
            "a reentrant admission on the admitting thread finds the admission busy; the first completes" + Why(first));
      first.owner.reset();
    }
    reentrant = nullptr;
    a.Open();
  }
  {
    auto admitted = a.Adopt(a.Limits(exact_owner), persistent, transient);
    Check(admitted.ok() && a.files.empty() && persistent.Charged() == exact_owner,
          "exact persistent backing succeeds without hidden device-array storage" + Why(admitted));
    admitted.owner.reset();
  }
  {
    Grant fresh(a.made.receipt.database_uuid, exact_owner);
    a.Open();
    auto again = a.Adopt(a.Limits(exact_owner), fresh, transient);
    Check(again.ok(), "fresh owner after prior final release" + Why(again));
    again.owner.reset();
    Check(!fresh.Charged(), "the fresh owner returns its charge on release");
  }
  if (!via_route) {
    // A successful admission allocates nothing under the device guard.
    Grant fenced(a.made.receipt.database_uuid, exact_owner);
    a.Open();
    fence_probes = fence_locked = 0;
    fence_mutex = a.PrimaryMutex();
    auto admitted = a.Adopt(a.Limits(exact_owner), fenced, transient);
    fence_mutex = nullptr;
    Check(admitted.ok() && fence_probes && !fence_locked, "owner admission allocates nothing under the device guard");
    admitted.owner.reset();
  }
}

// Every allocation, read, corrupted read and digest phase of an adoption, in
// turn. kind 0 heap, 1 read failure, 2 read corruption, 3..7 digest phases.
void AdoptionFaults(bool via_route, unsigned kind, unsigned shard, unsigned shards) {
  const unsigned profile = 0;
  struct Counts { unsigned long allocations = 0; unsigned reads = 0, hashes = 0; };
  // Adoption never writes (asserted on every run), so one copy serves the sweep.
  const auto shared = CopyOf(Production(profile));
  const auto run = [&](int fault, unsigned long site, Counts* measured) {
    Adoption a(profile, via_route, shared);
    Grant persistent(a.made.receipt.database_uuid, 1u << 20), transient(a.made.receipt.database_uuid, 128u << 20);
    const auto truncations = persistent.Truncations() + transient.Truncations();
    allocations = reads = writes = syncs = 0;
    observed_read_bytes = 0;
    hash_seen = 0;
    hash_fault = 0;
    hash_active = false;
    io_counting = hash_counting = counting = true;
    if (fault == 0) allocation_budget = static_cast<long>(site);
    if (fault == 1) read_fault = static_cast<unsigned>(site);
    if (fault == 2) corrupt_read = static_cast<unsigned>(site);
    if (fault >= 3) { hash_fault = static_cast<unsigned>(fault) - 2; hash_target = static_cast<unsigned>(site); }
    auto r = a.Adopt(a.Limits(), persistent, transient);
    const auto remaining = allocation_budget;
    const bool hash_consumed = !hash_fault;
    const Counts seen{allocations, reads, hash_seen};
    counting = io_counting = hash_counting = false;
    allocation_budget = -1;
    read_fault = corrupt_read = hash_fault = 0;
    hash_active = false;
    if (measured) *measured = seen;
    Check(!writes && !syncs && r.physical_bytes_read == observed_read_bytes,
          "adoption has no write/sync effects and reports exactly the bytes it read" + Why(r) +
              " observed=" + std::to_string(observed_read_bytes) + " fault=" + std::to_string(fault) +
              " site=" + std::to_string(site));
    if (fault < 0) Check(r.ok(), "complete real-file adoption baseline succeeds" + Why(r));
    if (fault == 0) {
      Check(remaining < 0, "every indexed adoption allocation fault is consumed");
      if (!r.ok())
        Check(r.error == E::resource_exhausted ||
                  (r.error == E::memory_failure && r.memory_error == db::NativeStorageMemoryError::resource_exhausted) ||
                  (r.error == E::route_admission && r.route_error == d::RouteSourceTransitionError::resource_exhausted) ||
                  (r.error == E::source_failure &&
                   (r.fixed_root_error == db::NativeFixedRootMemoryError::resource_exhausted ||
                    r.fixed_root_error == db::NativeFixedRootMemoryError::memory_allocation_failure)),
              "a required allocation failure keeps its typed resource classification" + Why(r));
      else
        Check(persistent.Truncations() + transient.Truncations() > truncations,
              "only a recorded optional telemetry failure can coexist with a complete adoption");
    }
    if (fault == 1) {
      Check(!r.ok() && !r.owner && seen.reads >= site && !r.io_status.ok(),
            "an injected read fault is reached and refuses the adoption with a failed io status" + Why(r));
      Check((r.error == E::bootstrap_failure && r.bootstrap_error == d::FilespaceBootstrapError::io_failure) ||
                (r.error == E::source_failure && r.fixed_root_error == db::NativeFixedRootMemoryError::io_failure),
            "a read fault classifies as bootstrap io_failure or fixed-root io_failure" + Why(r));
    }
    if (fault == 2)
      Check(!r.ok() && !r.owner && seen.reads >= site &&
                (r.error == E::bootstrap_failure || r.error == E::source_failure),
            "every corrupted adoption read is refused, typed, with no owner" + Why(r));
    if (fault >= 3) {
      Check(hash_consumed && !r.ok() && !r.owner, "every digest-provider phase is consumed without partial ownership" + Why(r));
      Check((r.error == E::bootstrap_failure && r.bootstrap_error == d::FilespaceBootstrapError::hash_provider_failure) ||
                (r.error == E::source_failure && r.fixed_root_error != db::NativeFixedRootMemoryError::none),
            "a digest failure is typed as a bootstrap or fixed-root refusal" + Why(r));
    }
    if (r.ok()) {
      Check(a.files.empty() && !persistent.memory.Matches(persistent.binding),
            "a successful run consumes the device and the persistent grant");
      r.owner.reset();
    } else {
      Preserved(a, persistent, r, "fault");
    }
    Check(!transient.Charged(), "every refused or released validation read returns its transient bytes");
  };
  Counts measured;
  run(-1, 0, nullptr);  // warm shared observation registrations
  run(-1, 0, &measured);
  Check(measured.allocations && measured.reads && measured.hashes, "measure every adoption fault dimension");
  const auto included = [&](unsigned long n) { return n % shards == shard; };
  if (kind == 0)
    for (unsigned long site = 0; site < measured.allocations; ++site)
      if (included(site)) run(0, site, nullptr);
  if (kind == 1 || kind == 2)
    for (unsigned site = 1; site <= measured.reads; ++site)
      if (included(site - 1)) run(static_cast<int>(kind), site, nullptr);
  if (kind >= 3 && kind <= 7)
    for (unsigned site = 1; site <= measured.hashes; ++site)
      if (included(site - 1)) run(static_cast<int>(kind), site, nullptr);
  run(-1, 0, nullptr);  // the sweep leaves nothing behind: a clean adoption follows
  std::cout << "adoption fault sites route=" << via_route << " allocations=" << measured.allocations
            << " reads=" << measured.reads << " hashes=" << measured.hashes << '\n';
}

// ---------------------------------------------------------------------------
// The native writer, adopted through the real route transfer.
struct WriterFixture {
  const Created& made;
  std::string path;
  Grant persistent, transient, reading;
  std::vector<std::unique_ptr<d::FileDevice>> secondary;  // a production create has none
  server::DatabaseOwnershipResult route;
  d::RouteSourceTransitionResult transition;
  std::shared_ptr<Writer> writer;
  explicit WriterFixture(unsigned profile = 0)
      : made(Production(profile)),
        path(CopyOf(made)),
        persistent(made.receipt.database_uuid, 1u << 20),
        transient(made.receipt.database_uuid, 128u << 20),
        reading(made.receipt.database_uuid, 512u << 20) {
    Start();
  }
  ~WriterFixture() {
    writer.reset();
    secondary.clear();
    transition.transition.reset();
    route.lock.reset();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  const Uuid& database() const { return made.receipt.database_uuid; }
  const Uuid& primary() const { return made.receipt.filespace_uuid; }
  void Start(d::RouteStorageAccess access = d::RouteStorageAccess::read_write,
             std::shared_ptr<d::RouteOwnershipLease>* legacy = nullptr) {
    writer.reset();
    secondary.clear();
    transition.transition.reset();
    route.lock.reset();
    server::DatabaseOwnershipRequest request;
    request.database_path = path;
    request.storage_access = access;
    route = server::AcquireDatabaseOwnership(request);
    Check(route.acquired && route.lock, "actual initial route/storage/data ownership");
    if (legacy) {
      *legacy = d::RouteOwnershipLease::Borrow(path + ".sb.route.owner.lock");
      Check(bool(*legacy), "actual legacy admission before native fence");
    }
    transition = route.lock->BeginNativeSourceTransition();
    Check(transition.ok(), "actual single-consumption transition fences legacy admission");
  }
  db::NativeOwnedCheckpointMemoryLimits Limits() const { return {64 * 1024, {256 * made.size, 64u << 20}}; }
  db::NativeFixedRootMemoryLimits ReadLimits() const { return {256 * made.size, 64u << 20}; }
  db::NativeOwnedWriterResult Adopt(std::optional<Uuid> database = std::nullopt,
                                    const db::NativeStorageMemoryBinding* supplied = nullptr) {
    return Writer::AdoptRouteWithMemory(transition.transition, database ? *database : this->database(), primary(),
                                        secondary, Limits(), persistent.memory, supplied ? *supplied : persistent.binding,
                                        transient.memory, transient.binding);
  }
  void Accept() {
    auto adopted = Adopt();
    Check(adopted.ok() && !persistent.memory.Matches(persistent.binding),
          "the production database and persistent grant are adopted exactly once" + Why(adopted));
    writer = std::move(adopted.writer);
  }
  Writer::FixedRootReadResult Read(Writer::Operation& operation) {
    return operation.ReadFixedRootWithMemory(ReadLimits(), reading.memory, reading.binding);
  }
  // The last free page of the created allocation map: a page no root reaches.
  u64 ScratchOffset() {
    auto operation = Writer::TryBeginOperation(writer, Fixed(1099));
    Check(operation.ok(), "scratch lookup operation");
    auto observed = operation.operation.ObserveFixedRootWithMemory(ReadLimits(), reading.memory, reading.binding);
    Check(observed.ok(), "observe the created allocation map");
    const auto& chain = observed.observation.allocation();
    for (auto image = chain.pages.rbegin(); image != chain.pages.rend(); ++image) {
      const auto& map = image->map;
      for (std::size_t n = map.states.size(); n--;)
        if (map.states[n] == scratchbird::storage::page::NativeAllocationState::free) return (map.first_page + n) * made.size;
    }
    throw std::runtime_error("the production database has no free physical scratch page");
  }
};
void FailedAdoption(WriterFixture& f, const db::NativeOwnedWriterResult& result) {
  Check(!result.ok() && !result.writer && f.secondary.empty() && f.persistent.memory.Matches(f.persistent.binding),
        "failed writer adoption leaves the grant owned and publishes no writer" + Why(result));
}

void InitialRefusals() {
  WriterFixture f;
  std::shared_ptr<d::RouteOwnershipLease> legacy;
  f.Start(d::RouteStorageAccess::read_write, &legacy);
  reads = 0;
  io_counting = true;
  auto undrained = f.Adopt();
  io_counting = false;
  FailedAdoption(f, undrained);
  Check(undrained.route_error == d::RouteSourceTransitionError::not_drained && !reads,
        "actual counted borrower must drain before writable transfer or native reads");
  legacy.reset();
  for (unsigned field = 0; field < 4; ++field) {
    auto bad = f.persistent.binding;
    const std::array<Uuid*, 4> fields{&bad.database_uuid, &bad.operation_uuid, &bad.owner_uuid, &bad.context_uuid};
    *fields[field] = Fixed(1000);
    reads = 0;
    io_counting = true;
    auto result = f.Adopt(std::nullopt, &bad);
    io_counting = false;
    FailedAdoption(f, result);
    Check(result.error == E::memory_failure && !reads, "every actual binary grant dimension checked before native I/O");
  }
  FailedAdoption(f, f.Adopt(Fixed(1000)));
  // Owner ruling 2026-11-07: the writer refuses a route handover from any
  // thread but the one that began the transition, before any read or grant
  // use, even when the cohort is one route-held file.
  {
    std::optional<db::NativeOwnedWriterResult> foreign;
    reads = 0;
    io_counting = true;
    std::thread other([&] { foreign = f.Adopt(); });
    other.join();
    io_counting = false;
    Check(foreign.has_value(), "the foreign-thread attempt returned");
    FailedAdoption(f, *foreign);
    Check(foreign->error == E::route_admission && foreign->route_error == d::RouteSourceTransitionError::wrong_thread &&
              !reads,
          "a foreign thread cannot adopt the writer through the route transition");
    f.Start();
  }
  read_pause = 1;
  bool paused = false;
  std::thread withdrawer([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (read_pause.load() != 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    paused = read_pause.load() == 2;
    f.route.lock->release();
    read_pause = 3;
  });
  auto withdrawn = f.Adopt();
  withdrawer.join();
  read_pause = 0;
  FailedAdoption(f, withdrawn);
  Check(paused && withdrawn.route_error == d::RouteSourceTransitionError::withdrawn,
        "withdrawal during fixed-root validation prevents the final physical transfer");
  f.Start(d::RouteStorageAccess::read_only);
  reads = 0;
  io_counting = true;
  auto readonly = f.Adopt();
  io_counting = false;
  FailedAdoption(f, readonly);
  Check(readonly.route_error == d::RouteSourceTransitionError::writable_not_requested && !reads,
        "initial readonly physical capability cannot become writable");
  f.Start();
  {
    struct rlimit original{};
    Check(!::getrlimit(RLIMIT_NOFILE, &original), "observe actual descriptor limit");
    auto exhausted = original;
    exhausted.rlim_cur = 0;
    Check(!::setrlimit(RLIMIT_NOFILE, &exhausted), "bounded real duplication failure");
    auto failure = f.Adopt();
    const auto restored = ::setrlimit(RLIMIT_NOFILE, &original);
    Check(!restored, "restore descriptor limit before diagnostic/test work");
    FailedAdoption(f, failure);
    Check(failure.route_error == d::RouteSourceTransitionError::io_failure,
          "actual retained writable descriptor duplication failure preserves source inputs");
  }
  f.Start();
  auto read = Source::AdoptRouteWithMemory(f.transition.transition, f.database(), f.primary(), f.secondary, f.Limits(),
                                           f.persistent.memory, f.persistent.binding, f.transient.memory,
                                           f.transient.binding);
  Check(read.ok(), "actual read source single-consumption under initially RW route" + Why(read));
  auto upgrade = f.Adopt();
  Check(!upgrade.ok() && !upgrade.writer && upgrade.route_error == d::RouteSourceTransitionError::already_transferred,
        "committed read source cannot be upgraded even when original route retained writable capability");
  read.owner.reset();
}

void PhysicalOperations(unsigned profile) {
  WriterFixture f(profile);
  f.Accept();
  const auto scratch = f.ScratchOffset();
  const auto second = f.Adopt();
  Check(!second.ok() && second.route_error == d::RouteSourceTransitionError::already_transferred,
        "writer transfer consumes exact transition once");
  auto begun = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(begun.ok(), "actual atomic writer operation reservation");
  Check(Writer::TryBeginOperation(f.writer, Fixed(1002)).error == WE::busy,
        "concurrent/reentrant operation cannot reconstruct physical custody");
  std::array<byte, 32> payload{}, actual{};
  for (unsigned n = 0; n < payload.size(); ++n) payload[n] = static_cast<byte>(n + 17);
  auto source = f.Read(begun.operation);
  Check(source.ok() && source.lease.source().identity().database_uuid == f.database() &&
            source.lease.source().identity().filespace_uuid == f.primary() &&
            source.lease.source().identity().page_size_profile_uuid == f.made.profile,
        "restricted source reads the adopted database through its fixed roots");
  const auto no_heap = [&](auto&& call) {
    allocation_budget = 0;
    auto result = call();
    const auto remaining = allocation_budget;
    allocation_budget = -1;
    Check(remaining == 0, "live-read refusal uses no memory/observation callback under device guards");
    return result;
  };
  Check(no_heap([&] { return begun.operation.WriteAtBounded(f.primary(), f.made.profile, scratch, payload); }).error ==
                WE::read_handle_live &&
            no_heap([&] { return begun.operation.ReadAtBounded(f.primary(), f.made.profile, scratch, actual); }).error ==
                WE::read_handle_live &&
            no_heap([&] { return begun.operation.Sync(f.primary(), f.made.profile); }).error == WE::read_handle_live,
        "every bounded physical primitive refuses while the restricted source remains live");
  allocation_budget = 0;
  auto competing = Writer::TryBeginOperation(f.writer, Fixed(1002));
  const auto left = allocation_budget;
  allocation_budget = -1;
  Check(left == 0 && competing.error == WE::busy, "busy operation refuses before control allocation under source guard");
  Check(f.Read(begun.operation).writer_error == WE::read_handle_live,
        "a second source handle cannot recursively reenter native guards");
  source.lease.Reset();
  Check(Writer::TryBeginOperation(f.writer, Fixed(1002)).error == WE::busy, "outer operation survives source handle Reset");
  std::array<byte, 1> first{};
  Check(begun.operation.ReadAtBounded(f.primary(), f.made.profile, 0, first).ok() && first[0] == f.made.first,
        "bounded read returns the created file's own first byte");
  auto bad = begun.operation.WriteAtBounded(Fixed(1010), f.made.profile, scratch, payload);
  Check(bad.error == WE::filespace_absent && !bad.io, "foreign filespace has no native write attempt");
  bad = begun.operation.WriteAtBounded(f.primary(), Fixed(1010), scratch, payload);
  Check(bad.error == WE::profile_mismatch && !bad.io, "actual bound page profile cannot be caller-replaced");
  bad = begun.operation.WriteAtBounded(f.primary(), f.made.profile, UINT64_MAX, payload);
  Check(bad.error == WE::extent_invalid && !bad.io, "overflow refused before physical access");
  bad = begun.operation.WriteAtBounded(f.primary(), f.made.profile, UINT64_C(1) << 40, payload);
  Check(bad.error == WE::extent_outside_file && bad.size_observation && bad.size_observation->ok() && !bad.io,
        "bounded writes cannot extend or allocate filespace storage");
  const auto retained = f.path + ".retained";
  std::filesystem::rename(f.path, retained);
  const int replacement = ::open(f.path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  const byte unrelated = 0xda;
  Check(replacement >= 0 && ::write(replacement, &unrelated, 1) == 1 && ::close(replacement) == 0,
        "replace diagnostic pathname with unrelated inode");
  auto write = begun.operation.WriteAtBounded(f.primary(), f.made.profile, scratch, payload);
  Check(write.ok() && write.io->native_attempted && write.io->bytes_transferred == payload.size() &&
            write.effects.write_attempts == 1 && write.effects.write_bytes == payload.size(),
        "the retained writable inode accepts exact physical bytes, not acceptance-only success");
  const auto sync = begun.operation.Sync(f.primary(), f.made.profile);
  Check(sync.ok() && sync.io->native_attempted && sync.io->synchronized,
        "actual native synchronization receipt, not MGA durable completion");
  Check(begun.operation.ReadAtBounded(f.primary(), f.made.profile, scratch, actual).ok() && actual == payload,
        "same-inode physical readback under actual retained owner");
  auto changed = payload;
  changed.fill(0xa5);
  io_counting = true;
  reads = writes = syncs = 0;
  write_fault = 1;
  torn_bytes = 7;
  auto partial = begun.operation.WriteAtBounded(f.primary(), f.made.profile, scratch, changed);
  io_counting = false;
  write_fault = 0;
  torn_bytes = 0;
  Check(!partial.ok() && partial.error == WE::io_failure && partial.io && partial.io->native_attempted &&
            partial.effects.uncertain_write && partial.operation_uuid == Fixed(1001) && partial.effects.write_attempts == 2,
        "partial failed write retains actual native cause/cumulative uncertain effects and operation identity");
  Check(begun.operation.ReadAtBounded(f.primary(), f.made.profile, scratch, actual).ok() &&
            std::equal(actual.begin(), actual.begin() + 7, changed.begin()) &&
            std::equal(actual.begin() + 7, actual.end(), payload.begin() + 7),
        "independent physical read proves retained partial bytes despite failed write receipt");
  io_counting = true;
  syncs = 0;
  sync_fault = 1;
  const auto failed_sync = begun.operation.Sync(f.primary(), f.made.profile);
  io_counting = false;
  sync_fault = 0;
  Check(!failed_sync.ok() && failed_sync.io && failed_sync.io->native_attempted && !failed_sync.io->synchronized &&
            failed_sync.io->native_error == EIO && failed_sync.effects.uncertain_write,
        "failed sync cannot erase preceding real effects");
  const auto accepted = begun.operation.effects();
  f.writer->Withdraw();
  f.route.lock->release();
  Check(Writer::TryBeginOperation(f.writer, Fixed(1002)).error == WE::withdrawn,
        "actual owner withdrawal closes new physical admission");
  auto weak = std::weak_ptr(f.writer);
  f.writer.reset();
  Check(!weak.expired() && begun.operation.WriteAtBounded(f.primary(), f.made.profile, scratch, payload).ok() &&
            begun.operation.Sync(f.primary(), f.made.profile).ok() &&
            begun.operation.effects().write_attempts == accepted.write_attempts + 1,
        "accepted operation retains real owner and effects after external owner withdrawal");
  begun.operation.Reset();
  Check(weak.expired(), "last actual operation releases source/route custody after guards");
  f.transition.transition.reset();
  f.route.lock.reset();
  d::FileDevice reopened;
  Check(reopened.Open(retained, d::FileOpenMode::open_existing_read_only).ok(), "independent reopen of original retained inode");
  Check(reopened.ReadAt(scratch, actual.data(), actual.size()).ok() && actual == payload && reopened.Close().ok(),
        "independent reopen proves physical write persistence");
  const int untouched = ::open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
  byte value = 0;
  struct stat state{};
  Check(untouched >= 0 && ::fstat(untouched, &state) == 0 && state.st_size == 1 && ::pread(untouched, &value, 1, 0) == 1 &&
            value == unrelated && ::close(untouched) == 0,
        "mutable diagnostic path never redirected retained native I/O");
  std::filesystem::remove(f.path);
  std::filesystem::rename(retained, f.path);
}

// The adopted source is not a cached authority: bytes changed underneath it
// after adoption are refused by the next read, never repaired, and a restored
// file reads again.
void MutationAfterAdoption() {
  WriterFixture f;
  f.Accept();
  auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(operation.ok(), "operation for the mutation check");
  Check(f.Read(operation.operation).ok(), "the adopted source reads before mutation");
  // Page zero is hash-sealed and read by every fixed-root read.
  const u64 offset = 0;
  std::array<byte, 1> original{}, changed{};
  Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, offset, original).ok(), "read the inventory byte");
  changed[0] = static_cast<byte>(original[0] ^ 1);
  Check(operation.operation.WriteAtBounded(f.primary(), f.made.profile, offset, changed).ok() &&
            operation.operation.Sync(f.primary(), f.made.profile).ok(),
        "explicit source mutation after adoption");
  const auto refused = f.Read(operation.operation);
  Check(!refused.ok() && !refused.lease && refused.writer_error == WE::source_failure,
        "memory-owned adoption is not cached source-currentness authority" + Why(refused));
  std::array<byte, 1> after{};
  Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, offset, after).ok() && after == changed,
        "a refused read has no repair effects");
  Check(operation.operation.WriteAtBounded(f.primary(), f.made.profile, offset, original).ok() &&
            operation.operation.Sync(f.primary(), f.made.profile).ok(),
        "restore the original byte");
  Check(f.Read(operation.operation).ok(), "the restored source reads again");
}

void ReaderRetainsReservation() {
  WriterFixture f;
  f.Accept();
  auto op = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(op.ok(), "actual reader-lifetime operation");
  auto reader = f.Read(op.operation);
  Check(reader.ok(), "actual restricted reader");
  op.operation.Reset();
  allocation_budget = 0;
  auto busy = Writer::TryBeginOperation(f.writer, Fixed(1002));
  const auto remaining = allocation_budget;
  allocation_budget = -1;
  Check(remaining == 0 && busy.error == WE::busy, "dropping external operation cannot release reservation behind a retained read guard");
  auto weak = std::weak_ptr(f.writer);
  f.writer->Withdraw();
  f.writer.reset();
  Check(!weak.expired(), "reader retains actual writer and source owner");
  reader.lease.Reset();
  Check(weak.expired(), "last reader resets native guards before retiring actual operation/custody");
}

void OperationAllocationFailure() {
  WriterFixture f;
  f.Accept();
  allocation_budget = 0;
  auto failed = Writer::TryBeginOperation(f.writer, Fixed(1001));
  const auto remaining = allocation_budget;
  allocation_budget = -1;
  Check(remaining < 0 && failed.error == WE::resource_exhausted && !failed.operation,
        "actual operation control allocation failure publishes no reservation");
  auto retry = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(retry.ok(), "failed control allocation leaves genuine next operation admissible");
  retry.operation.Reset();
  f.route.lock->release();
  auto revoked = Writer::TryBeginOperation(f.writer, Fixed(1002));
  Check(revoked.error == WE::withdrawn && revoked.route_error == d::RouteSourceTransitionError::withdrawn,
        "actual route withdrawal independently closes new operations without a writer-local flag");
}

void ReaderFailureRetry() {
  WriterFixture f;
  f.Accept();
  auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(operation.ok(), "actual operation retained across reader preparation failures");
  const auto baseline = f.reading.memory.Snapshot();
  const auto preserved = [&] {
    const auto current = f.reading.memory.Snapshot();
    Check(f.reading.memory.CheckBinding(f.reading.binding) == db::NativeStorageMemoryError::none && current.active &&
              current.reserved_bytes == baseline.reserved_bytes && current.allocated_bytes == baseline.allocated_bytes,
          "failed read preserves exact admitted grant and releases every attempted backing allocation");
    Check(operation.operation && operation.operation.identity() == Fixed(1001) &&
              Writer::TryBeginOperation(f.writer, Fixed(1002)).error == WE::busy,
          "failed read never releases or replaces the outer actual operation reservation");
    std::array<byte, 1> value{};
    Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, value).ok() && value[0] == f.made.first,
          "reader preparation failure releases its flags and actual device guards before bounded reuse");
  };
  allocation_budget = 0;
  auto allocation = f.Read(operation.operation);
  const auto remaining = allocation_budget;
  allocation_budget = -1;
  Check(remaining < 0 && !allocation.ok() && !allocation.lease && allocation.writer_error == WE::resource_exhausted &&
            allocation.error == E::resource_exhausted,
        "first reader-state allocation failure publishes no source and preserves its typed cause" + Why(allocation));
  preserved();
  reads = 0;
  read_fault = 1;
  io_counting = true;
  auto failed = f.Read(operation.operation);
  io_counting = false;
  read_fault = 0;
  Check(reads == 1 && !failed.ok() && !failed.lease && failed.writer_error == WE::source_failure &&
            failed.error == E::source_failure && !failed.io_status.ok(),
        "a pread failure after reservation retains the exact failed I/O status, no usable reader" + Why(failed));
  preserved();
  auto retried = f.Read(operation.operation);
  Check(retried.ok() && retried.lease.source().identity().database_uuid == f.database() &&
            operation.operation.identity() == Fixed(1001),
        "same actual operation successfully retries the full fixed-root acquisition after both failures");
  retried.lease.Reset();
  preserved();
}

WriterFixture* callback_fixture = nullptr;
Writer::Operation* callback_operation = nullptr;
bool callback_ran = false;
WE callback_read_error = WE::none, callback_io_error = WE::none;
void ResetOwnerCallback() {
  callback_ran = true;
  callback_fixture->writer.reset();
}
void ResetOperationCallback() {
  callback_ran = true;
  auto& f = *callback_fixture;
  const auto read = f.Read(*callback_operation);
  callback_read_error = read.writer_error;
  std::array<byte, 1> bytes{};
  callback_io_error = callback_operation->ReadAtBounded(f.primary(), f.made.profile, 0, bytes).error;
  callback_operation->Reset();
  f.writer.reset();
}
void CallbackCustody() {
  {
    WriterFixture f;
    f.Accept();
    callback_fixture = &f;
    callback_ran = false;
    auto weak = std::weak_ptr(f.writer);
    allocation_callback = ResetOwnerCallback;
    auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
    allocation_callback = nullptr;
    Check(callback_ran && !f.writer && operation.ok() && !weak.expired(),
          "operation factory pins actual owner across allocation callback resetting caller's shared pointer");
    operation.operation.Reset();
    Check(weak.expired(), "operation factory callback owner releases on actual operation retirement");
  }
  {
    WriterFixture f;
    f.Accept();
    auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
    Check(operation.ok(), "actual read callback operation");
    callback_fixture = &f;
    callback_operation = &operation.operation;
    callback_ran = false;
    auto weak = std::weak_ptr(f.writer);
    allocation_callback = ResetOperationCallback;
    auto read = f.Read(operation.operation);
    allocation_callback = nullptr;
    Check(callback_ran && callback_read_error == WE::read_handle_live && callback_io_error == WE::read_handle_live &&
              read.ok() && !operation.operation && !f.writer && !weak.expired(),
          "read reserves before allocation callback, refuses recursive I/O, and survives external operation/owner Reset");
    read.lease.Reset();
    Check(weak.expired(), "last actual read releases callback-retained reservation and owner");
  }
  {
    WriterFixture f;
    f.Accept();
    auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
    Check(operation.ok(), "actual bounded I/O callback operation");
    callback_fixture = &f;
    callback_operation = &operation.operation;
    callback_ran = false;
    auto weak = std::weak_ptr(f.writer);
    std::array<byte, 1> actual{};
    allocation_callback = ResetOperationCallback;
    const auto read = operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, actual);
    allocation_callback = nullptr;
    Check(callback_ran && callback_read_error == WE::busy && callback_io_error == WE::busy && read.ok() &&
              actual[0] == f.made.first && !operation.operation && !f.writer && weak.expired(),
          "real I/O reporting callback cannot reenter or invalidate active device/batch cleanup after external Reset");
  }
  callback_fixture = nullptr;
  callback_operation = nullptr;
}

void CloseControls() {
  {
    WriterFixture f;
    f.Accept();
    std::array<db::NativeWriterMemberCloseReceipt, 1> members{};
    reads = writes = syncs = 0;
    io_counting = true;
    const auto invalid = Writer::Close(f.writer, std::span(members).first(0));
    io_counting = false;
    Check(invalid.error == WE::invalid_request && !invalid.member_count && !reads && !writes && !syncs,
          "short fixed close receipt backing refuses before withdrawal or effects");
    auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
    Check(operation.ok(), "invalid close backing leaves genuine physical admission open");
    auto reader = f.Read(operation.operation);
    Check(reader.ok(), "real fixed-root read before bounded drain");
    allocation_budget = 0;
    auto busy = Writer::Close(f.writer, members);
    const auto remaining = allocation_budget;
    allocation_budget = -1;
    Check(busy.error == WE::busy && remaining == 0 && !busy.member_count &&
              Writer::TryBeginOperation(f.writer, Fixed(1002)).error == WE::withdrawn,
          "busy close withdraws new operations without waiting, allocating or disturbing live reader/accepted operation");
    operation.operation.Reset();
    Check(Writer::Close(f.writer, members).error == WE::busy, "reader alone retains true close/drain obligation");
    reader.lease.Reset();
    auto closed = Writer::Close(f.writer, members);
    Check(closed.ok() && closed.member_count == 1 && !closed.historical_close_uncertainty,
          "the synchronized one-member cohort closes after last read release");
    Check(members[0].synchronization && members[0].synchronization->native_attempted &&
              members[0].synchronization->synchronized && members[0].data.attempted &&
              members[0].data.disposition == db::NativeWriterHandleCloseDisposition::released,
          "the writable member has its own actual synchronization and physical release receipt");
    io_counting = true;
    syncs = 0;
    const auto duplicate = Writer::Close(f.writer, members);
    io_counting = false;
    Check(duplicate.ok() && !syncs && !members[0].synchronization && !members[0].data.attempted &&
              !members[0].ownership.attempted,
          "complete close retry cannot resynchronize or reclose retired descriptors");
  }
  {
    WriterFixture f;
    f.Accept();
    std::array<db::NativeWriterMemberCloseReceipt, 1> members{};
    io_counting = true;
    syncs = 0;
    sync_fault = 1;
    auto failed = Writer::Close(f.writer, members);
    io_counting = false;
    sync_fault = 0;
    Check(!failed.ok() && failed.error == WE::io_failure && !failed.all_members_retired && members[0].synchronization &&
              members[0].synchronization->native_error == EIO && !members[0].synchronization->synchronized &&
              !members[0].data.attempted,
          "actual failed close-time synchronization preserves failed member and exact cause for retry");
    io_counting = true;
    syncs = 0;
    auto retried = Writer::Close(f.writer, members);
    io_counting = false;
    Check(retried.ok() && syncs == 1 && members[0].synchronization && members[0].synchronization->synchronized,
          "actual close retry synchronizes and retires only the genuinely retained failed member");
  }
  {
    WriterFixture f;
    f.Accept();
    std::array<db::NativeWriterMemberCloseReceipt, 1> members{};
    reused_fd = retired_fd = -1;
    late_close_fault = true;
    const auto failed = Writer::Close(f.writer, members);
    Check(!late_close_fault && reused_fd == retired_fd && reused_fd >= 0 && failed.error == WE::close_failure &&
              failed.historical_close_uncertainty && failed.first_uncertain_filespace == f.primary() &&
              failed.first_uncertain_native_error == EINTR && members[0].data.native_error == EINTR &&
              members[0].data.disposition == db::NativeWriterHandleCloseDisposition::released_after_error,
          "real Linux descriptor release followed by late EINTR is retained as uncertain error, not retried");
    const auto retry = Writer::Close(f.writer, members);
    Check(!retry.ok() && retry.historical_close_uncertainty && !members[0].data.attempted &&
              ::fcntl(reused_fd, F_GETFD) >= 0,
          "close retry never targets reused descriptor or erases the original uncertain release");
    f.writer.reset();
    Check(::fcntl(reused_fd, F_GETFD) >= 0, "writer destruction never recloses a retired/reused native descriptor");
    Check(!::close(reused_fd), "release test-owned replacement descriptor");
    reused_fd = -1;
  }
}

void ThreadAffinityDeath(unsigned mode) {
  WriterFixture f;
  f.Accept();
  auto result = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(result.ok(), "actual operation before thread-affinity misuse");
  auto source = f.Read(result.operation);
  Check(source.ok(), "actual guarded source before thread-affinity misuse");
  std::set_terminate([] { ::_exit(73); });
  std::thread wrong([&] {
    if (mode == 0) (void)result.operation.identity();
    else if (mode == 1) { auto moved = std::move(result.operation); }
    else if (mode == 2) (void)source.lease.source();
    else { auto moved = std::move(source.lease); }
  });
  wrong.join();
  throw std::runtime_error("thread-affine native writer misuse unexpectedly returned");
}
void ThreadControls(const char* executable) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    const auto argument = std::to_string(mode);
    const auto pid = ::fork();
    Check(pid >= 0, "fork owned fresh-process thread control");
    if (!pid) {
      ::execl(executable, executable, "--thread-death", argument.c_str(), nullptr);
      ::_exit(79);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "real foreign-thread operation/read access and move terminate without returning forged custody");
  }
}

void ForkControls() {
  WriterFixture f;
  auto child = ::fork();
  Check(child >= 0, "fork before native writer transfer");
  if (!child) {
    allocation_budget = 0;
    auto result = f.Adopt();
    ::_exit(result.route_error == d::RouteSourceTransitionError::wrong_process && allocation_budget == 0 ? 0 : 78);
  }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "inherited transition refuses before native mutex/governor/allocation");
  f.Accept();
  auto operation = Writer::TryBeginOperation(f.writer, Fixed(1001));
  Check(operation.ok(), "actual operation before fork");
  auto reader = f.Read(operation.operation);
  Check(reader.ok(), "actual retained source before fork");
  const auto charged = f.persistent.Charged();
  child = ::fork();
  Check(child >= 0, "fork inherited native writer/source owners");
  if (!child) {
    auto refused = Writer::TryBeginOperation(f.writer, Fixed(1002));
    std::array<byte, 1> one{};
    auto io = operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, one);
    reader.lease.Reset();
    operation.operation.Reset();
    f.writer.reset();
    f.transition.transition.reset();
    f.route.lock.reset();
    ::_exit(refused.error == WE::wrong_process && io.error == WE::wrong_process && !io.io ? 0 : 79);
  }
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fork rejects source use and abandons inherited guards with close-only child cleanup");
  reader.lease.Reset();
  std::array<byte, 1> one{};
  Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, one).ok() && one[0] == f.made.first,
        "child cleanup never unlocks/closes parent custody");
  Check(f.persistent.Charged() == charged, "child cleanup cannot release the parent's persistent charge");
}
}  // namespace owned_adoption_test

int main(int argc, char** argv) {
  using namespace owned_adoption_test;
  try {
    Artifacts owned;
    artifacts = &owned;
    if (argc == 3 && std::string_view(argv[1]) == "--thread-death") {
      const auto mode = std::stoul(argv[2]);
      Check(mode < 4, "thread control mode");
      ThreadAffinityDeath(mode);
    } else if (argc == 6 && std::string_view(argv[1]) == "--faults") {
      const auto route = std::stoul(argv[2]), kind = std::stoul(argv[3]), shard = std::stoul(argv[4]),
                 shards = std::stoul(argv[5]);
      Check(route < 2 && kind < 8 && shards && shard < shards, "bounded adoption fault selection");
      AdoptionFaults(route == 1, static_cast<unsigned>(kind), static_cast<unsigned>(shard), static_cast<unsigned>(shards));
    } else {
      Check(argc == 1, "unknown native owned adoption test mode");
      ThreadControls(argv[0]);
      for (unsigned p = 0; p < 5; ++p)
        for (bool route : {false, true}) CompleteAdoption(p, route);
      InitialRefusals();
      for (unsigned p = 0; p < 5; ++p) PhysicalOperations(p);
      MutationAfterAdoption();
      ReaderRetainsReservation();
      OperationAllocationFailure();
      ReaderFailureRetry();
      CallbackCustody();
      CloseControls();
      ForkControls();
    }
    owned.Cleanup();
    artifacts = nullptr;
    std::cout << "native owned adoption PASS checks=" << checks
              << " production_create=true one_member_cohort=true physical_only_not_startup_admission=true\n";
    return 0;
  } catch (const std::exception& e) {
    allocation_budget = -1;
    counting = io_counting = hash_counting = false;
    read_fault = corrupt_read = write_fault = sync_fault = hash_fault = 0;
    allocation_callback = nullptr;
    fence_mutex = nullptr;
    late_close_fault = false;
    read_pause = 0;
    if (reused_fd >= 0) (void)::close(reused_fd);
    std::cerr << "native owned adoption FAIL " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
