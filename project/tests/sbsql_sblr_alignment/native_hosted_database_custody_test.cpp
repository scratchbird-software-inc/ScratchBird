// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// Hosted database custody over a database written by the production create
// (drift removal C3d, owner ruling Q18=1). Custody pins the original route
// owner, fences legacy borrowing, adopts the writer through the real route
// transfer (the owned source validates the file through page zero's fixed
// roots), reserves operations, and retires the writer separately from the
// original OS owner. The reads here are fixed-root reads: each stands for any
// source read that custody must pin. The production create writes one file,
// so every check covers a one-member cohort.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "../../src/server/native_hosted_database_custody.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
long allocation_budget = -1;                 // allocations left before the next one fails
void (*host_allocation_callback)() = nullptr;  // runs once, inside the next allocation
bool io_counting = false;
unsigned syncs = 0, sync_fault = 0;
}  // namespace

void* operator new(std::size_t n) {
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t alignment) {
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  const auto a = static_cast<std::size_t>(alignment);
  n = n ? n : 1;
  if (n > std::numeric_limits<std::size_t>::max() - (a - 1)) throw std::bad_alloc();
  if (auto* p = std::aligned_alloc(a, (n + a - 1) / a * a)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
  if (auto callback = host_allocation_callback) {
    host_allocation_callback = nullptr;
    callback();
  }
  return __real_malloc(size);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (io_counting && ++syncs == sync_fault) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

namespace hosted_custody_test {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
namespace server = scratchbird::server;
using namespace scratchbird::core::platform;
using Host = server::NativeHostedDatabaseCustody;
using HE = server::NativeHostedCustodyError;
using WE = db::NativeWriterError;
template <class T> concept ExposesWriter = requires(T value) { value.writer(); };
template <class T> concept ExposesDevices = requires(T value) { value.devices(); };
static_assert(!std::is_default_constructible_v<Host> && !std::is_copy_constructible_v<Host>);
// (W4d: the selected-checkpoint Host::ReadLease was deleted with that layer.)
static_assert(!ExposesWriter<Host> && !ExposesDevices<Host> && !ExposesDevices<Host::FixedRootReadLease>);
static_assert(!std::is_copy_constructible_v<Host::Operation> && !std::is_copy_constructible_v<Host::FixedRootReadLease>);

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
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

// Every fixture lives in this process's directory. The parent points TMPDIR
// at its own private directory, so a child process (fork or exec) creates its
// directory inside the parent's and the parent's cleanup removes it.
struct Artifacts {
  scratchbird::tests::OwnedTempDirectory directory;
  std::optional<std::string> previous;
  Artifacts() {
    if (const auto* value = std::getenv("TMPDIR")) previous = value;
    Check(!::setenv("TMPDIR", directory.path().c_str(), 1), "private host fixture namespace");
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
    r.route_label = "storage.hosted.custody";
    r.purpose = "hosted custody conformance";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "hosted custody conformance";
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
};

// One production-created database per page-size profile, written once per
// process. Each fixture adopts its own byte-exact copy of it.
struct Created {
  std::string path;
  db::NativeDatabaseCreateReceipt receipt;
  u64 size = 0;
  Uuid profile;
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
  Check(device.Open(out.path, d::FileOpenMode::create_new).ok(), "production create device");
  u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
  auto result = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
  Check(result.ok(), "the production create writes a complete native database");
  out.receipt = *result.receipt;
  Check(device.Close().ok(), "release the creating handle before route ownership");
  return made.emplace(profile_index, std::move(out)).first->second;
}

struct Fixture {
  const Created& made;
  std::string path;
  Grant persistent, transient, reading;
  // The production create writes one file: the route supplies the primary and
  // there is never a secondary member to hand over.
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  server::DatabaseOwnershipResult route;
  std::shared_ptr<Host> host;
  static std::string NextPath() {
    static unsigned next = 0;
    return (artifacts->directory.path() / ("hosted-" + std::to_string(next++))).string();
  }
  explicit Fixture(unsigned profile = 0, d::RouteStorageAccess capability = d::RouteStorageAccess::read_write)
      : made(Production(profile)),
        path(NextPath()),
        persistent(made.receipt.database_uuid, 1u << 20),
        transient(made.receipt.database_uuid, 128u << 20),
        reading(made.receipt.database_uuid, 512u << 20) {
    std::filesystem::copy_file(made.path, path);
    server::DatabaseOwnershipRequest request;
    request.database_path = path;
    request.storage_access = capability;
    route = server::AcquireDatabaseOwnership(request);
    Check(route.acquired && route.lock, "actual original host route/OS owner");
  }
  ~Fixture() {
    host.reset();
    route.lock.reset();
  }
  const Uuid& database() const { return made.receipt.database_uuid; }
  const Uuid& primary() const { return made.receipt.filespace_uuid; }
  std::string RouteLock() const { return path + ".sb.route.owner.lock"; }
  db::NativeFixedRootMemoryLimits Limits() const { return {256 * made.size, 64u << 20}; }
  void Begin() {
    auto begun = Host::Begin(route.lock);
    Check(begun.ok(), "real host initial custody");
    host = std::move(begun.custody);
  }
  Host::AdoptionResult Adopt(const db::NativeStorageMemoryBinding* binding = nullptr) {
    return Host::AdoptWriter(host, database(), primary(), secondary, {64 * 1024, {256 * made.size, 64u << 20}},
                             persistent.memory, binding ? *binding : persistent.binding, transient.memory,
                             transient.binding);
  }
  void Mount() {
    Begin();
    const auto adopted = Adopt();
    if (!adopted.ok())
      std::cerr << "host adoption=" << int(adopted.error) << " source=" << int(adopted.source.error)
                << " fixed_root=" << int(adopted.source.fixed_root_error)
                << " page_zero=" << int(adopted.source.page_zero_error)
                << " inventory=" << int(adopted.source.inventory_error)
                << " memory=" << int(adopted.source.memory_error) << '\n';
    Check(adopted.ok(), "the production database is adopted by host custody");
  }
  Host::FixedRootReadResult Read(Host::Operation& operation) {
    return operation.ReadFixedRootWithMemory(Limits(), reading.memory, reading.binding);
  }
};

void InitialCustody() {
  {
    Fixture f(0, d::RouteStorageAccess::read_only);
    allocation_budget = 0;
    auto result = Host::Begin(f.route.lock);
    const auto remaining = allocation_budget;
    allocation_budget = -1;
    Check(!result.ok() && result.route_error == d::RouteSourceTransitionError::writable_not_requested && remaining == 0,
          "read-only initial route cannot become a hosted writer; refuses before allocation");
  }
  for (long budget : {0, 1, 2, 3}) {
    Fixture f;
    allocation_budget = budget;
    auto result = Host::Begin(f.route.lock);
    allocation_budget = -1;
    Check(!result.ok() && f.route.lock->valid(), "host-control allocation failure preserves original owner");
    auto borrower = d::RouteOwnershipLease::Borrow(f.RouteLock());
    Check(bool(borrower), "host wrapper allocation failure precedes irreversible legacy-admission fence");
  }
  Fixture f;
  auto borrower = d::RouteOwnershipLease::Borrow(f.RouteLock());
  Check(bool(borrower), "actual pre-fence legacy pin");
  f.Begin();
  Check(!d::RouteOwnershipLease::Borrow(f.RouteLock()), "host fence closes new legacy admission");
  Check(Host::TryBeginOperation(f.host, Fixed(1001)).error == HE::not_mounted,
        "fenced route alone has no native writer operation");
  const auto pending = f.Adopt();
  Check(!pending.ok() && pending.source.route_error == d::RouteSourceTransitionError::not_drained &&
            f.persistent.memory.Matches(f.persistent.binding),
        "undrained host adoption preserves admitted memory");
  const auto deadline = Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
  Check(!deadline.ok() && deadline.observation.state == d::RouteSourceDrainWaitState::deadline_expired,
        "finite host drain wait reports the native deadline; cannot mount by copied observation");
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled =
      Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now() + std::chrono::seconds(1), stop.get_token());
  Check(!cancelled.ok() && cancelled.observation.state == d::RouteSourceDrainWaitState::cancelled,
        "host drain cancellation retains original fence");
  borrower.reset();
  Check(Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now() + std::chrono::seconds(1)).ok(),
        "same host owner drains after actual borrower release");
  const auto probe = Host::ProbeBootstrap(f.host);
  Check(probe.ok() && probe.source.bootstrap->database_uuid == f.database() &&
            probe.source.bootstrap->filespace_uuid == f.primary(),
        "private retained primary supplies the created database and filespace identities");
  {
    // The writer layer does not refuse a foreign thread when the cohort is one
    // route-held file; custody must, before any read or grant use.
    std::optional<Host::AdoptionResult> foreign;
    std::thread other([&] { foreign = f.Adopt(); });
    other.join();
    Check(foreign && foreign->error == HE::wrong_thread && !foreign->source.physical_bytes_read &&
              f.persistent.memory.Matches(f.persistent.binding),
          "a foreign thread cannot adopt the writer into the opening thread's custody");
  }
  auto wrong = f.persistent.binding;
  wrong.context_uuid = Fixed(1999);
  Check(!f.Adopt(&wrong).ok() && f.persistent.memory.Matches(f.persistent.binding),
        "failed host adoption does not consume the admitted grant");
  Check(f.Adopt().ok() && !f.persistent.memory.Matches(f.persistent.binding),
        "same retained transition retries full native adoption once");
  Check(f.Adopt().error == HE::already_mounted && Host::ProbeBootstrap(f.host).error == HE::already_mounted,
        "mounted custody cannot be reimported or probed through old route admission");
}

void CompletePins(unsigned profile) {
  Fixture f(profile);
  f.Mount();
  const auto route = std::weak_ptr(f.route.lock);
  const auto host = std::weak_ptr(f.host);
  f.route.lock.reset();
  auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
  Check(operation.ok(), "host operation retains full native custody");
  auto reader = f.Read(operation.operation);
  Check(reader.ok() && reader.lease.operation_uuid() == Fixed(1001),
        "host reader retains exact operation and fixed-root source");
  std::array<byte, 1> one{};
  Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, one).error == WE::read_handle_live,
        "actual source guards exclude physical writer I/O");
  allocation_budget = 0;
  const auto nested = f.Read(operation.operation);
  const auto remaining = allocation_budget;
  allocation_budget = -1;
  Check(nested.writer_error == WE::read_handle_live && remaining == 0 && !nested.ok(),
        "retained source guards exclude a nested source read before any allocation");
  operation.operation.Reset();
  f.host.reset();
  Check(!host.expired() && !route.expired() && reader.lease.source().identity().database_uuid == f.database(),
        "restricted reader alone retains original server route owner as well as native writer after external handle reset");
  Host::FixedRootReadLease moved(std::move(reader.lease));
  Check(!reader.lease && moved.operation_uuid() == Fixed(1001), "same-thread read move retains exactly one full host pin");
  if (!profile)
    for (unsigned mode = 0; mode < 2; ++mode) {
      const auto child = ::fork();
      Check(child >= 0, "inherited live or moved-from host reader");
      if (!child) {
        std::set_terminate([] { ::_exit(73); });
        if (mode) {
          auto inherited = std::move(reader.lease);
          (void)inherited;
        } else
          (void)moved.source();
        ::_exit(79);
      }
      int status = 0;
      Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
            "inherited read access and moved-from move reject before inherited custody");
    }
  moved.Reset();
  Check(host.expired() && route.expired(), "retiring last read releases full host custody after device guard cleanup");
}

void CloseObligations() {
  Fixture f;
  f.Mount();
  const auto route = std::weak_ptr(f.route.lock);
  f.route.lock.reset();
  auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
  Check(operation.ok(), "host close fixture operation");
  auto reader = f.Read(operation.operation);
  Check(reader.ok(), "host close fixture reader");
  std::array<db::NativeWriterMemberCloseReceipt, 1> receipts{};
  Check(!Host::RetireWriter(f.host, std::span(receipts).first(0)).ok(),
        "short receipt capacity refuses without withdrawing actual writer");
  const auto busy = Host::RetireWriter(f.host, receipts);
  Check(!busy.ok() && busy.writer.error == WE::busy && !route.expired(),
        "busy writer close retains complete original host custody");
  Check(Host::RetireWriterBacking(f.host) == HE::writer_failure,
        "live writer backing cannot retire before actual successful physical close");
  operation.operation.Reset();
  Check(Host::RetireWriter(f.host, receipts).writer.error == WE::busy, "reader alone blocks writer close");
  reader.lease.Reset();
  syncs = 0;
  io_counting = true;
  sync_fault = 1;
  const auto failed = Host::RetireWriter(f.host, receipts);
  sync_fault = 0;
  io_counting = false;
  Check(!failed.ok() && !failed.writer.all_members_retired && !route.expired(),
        "failed native sync preserves member and original host owner for retry");
  Check(Host::RetireWriterBacking(f.host) == HE::writer_failure,
        "failed native close retains writer backing for the original retry");
  const auto retired = Host::RetireWriter(f.host, receipts);
  const auto state = Host::Observe(f.host);
  Check(retired.ok() && state.stage == server::NativeHostedCustodyStage::writer_retired &&
            state.initial_route_owner_retained && !route.expired(),
        "adopted member retirement is explicitly distinct from original route/OS owner release");
  Check(Host::RetireWriterBacking(f.host) == HE::none && Host::RetireWriterBacking(f.host) == HE::none,
        "successfully closed writer releases its backing idempotently before original route retirement");
  const auto released = Host::Observe(f.host);
  Check(released.database_uuid == state.database_uuid && released.primary_filespace_uuid == state.primary_filespace_uuid &&
            released.database_uuid == f.database() && released.primary_filespace_uuid == f.primary() &&
            released.initial_route_owner_retained,
        "backing retirement preserves actual adopted identity and original OS custody");
  server::DatabaseOwnershipRequest contender;
  contender.database_path = f.path;
  Check(!server::AcquireDatabaseOwnership(contender).acquired, "adopted-member close cannot expose node to a second host");
  Check(!Host::TryBeginOperation(f.host, Fixed(1002)).ok(), "retired writer cannot admit another operation");
  f.host.reset();
  Check(route.expired(), "coordinator's final custody release retires initial route owner");
  auto reopened = server::AcquireDatabaseOwnership(contender);
  Check(reopened.acquired, "new owner can acquire OS custody only after actual host lifetime ends");
}

Fixture* callback_fixture = nullptr;
Host::Operation* callback_operation = nullptr;
HE callback_reentrant = HE::none;
void DropRouteDuringBegin() { callback_fixture->route.lock.reset(); }
void DropHostDuringOperation() {
  std::array<db::NativeWriterMemberCloseReceipt, 1> receipts{};
  callback_reentrant = Host::RetireWriter(callback_fixture->host, receipts).error;
  callback_fixture->host.reset();
}
void DropOperationDuringRead() {
  callback_operation->Reset();
  callback_fixture->host.reset();
}
void CallbackPins() {
  {
    Fixture f;
    callback_fixture = &f;
    const auto route = std::weak_ptr(f.route.lock);
    host_allocation_callback = DropRouteDuringBegin;
    auto begun = Host::Begin(f.route.lock);
    host_allocation_callback = nullptr;
    Check(begun.ok() && !f.route.lock && !route.expired(),
          "factory pins actual initial owner before allocation callback resets caller's route handle");
  }
  {
    Fixture f;
    f.Mount();
    callback_fixture = &f;
    const auto route = std::weak_ptr(f.route.lock);
    f.route.lock.reset();
    host_allocation_callback = DropHostDuringOperation;
    auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
    host_allocation_callback = nullptr;
    Check(operation.ok() && !f.host && !route.expired() && callback_reentrant == HE::busy,
          "operation factory pins whole host and refuses reentrant close during allocation");
    operation.operation.Reset();
    Check(route.expired(), "operation retirement releases callback-retained initial owner");
  }
  {
    Fixture f;
    f.Mount();
    callback_fixture = &f;
    auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
    Check(operation.ok(), "callback read operation");
    callback_operation = &operation.operation;
    const auto route = std::weak_ptr(f.route.lock);
    f.route.lock.reset();
    host_allocation_callback = DropOperationDuringRead;
    auto reader = f.Read(operation.operation);
    host_allocation_callback = nullptr;
    Check(reader.ok() && !operation.operation && !f.host && !route.expired(),
          "read call pins original host across callback resetting both external owner and operation");
    reader.lease.Reset();
    Check(route.expired(), "callback-retained original route retires only after actual fixed-root read cleanup");
  }
  callback_fixture = nullptr;
  callback_operation = nullptr;
}

void ProcessAndThreadChecks() {
  Fixture f;
  f.Mount();
  HE foreign = HE::none;
  std::thread wrong([&] { foreign = Host::ProbeBootstrap(f.host).error; });
  wrong.join();
  Check(foreign == HE::wrong_thread, "opening-thread source operations reject foreign threads before native access");
  const auto child = ::fork();
  Check(child >= 0, "actual inherited host child");
  if (!child) {
    allocation_budget = 0;
    std::array<db::NativeWriterMemberCloseReceipt, 1> receipts{};
    const bool refused = Host::TryBeginOperation(f.host, Fixed(1002)).error == HE::wrong_process &&
                         Host::RetireWriter(f.host, receipts).error == HE::wrong_process &&
                         Host::Observe(f.host).error == HE::wrong_process;
    allocation_budget = -1;
    f.host.reset();
    f.route.lock.reset();
    ::_exit(refused ? 0 : 79);
  }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "inherited host rejects before allocation/locks and permits underlying descriptor-only abandonment");
  auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
  Check(operation.ok(), "parent custody remains usable after child abandonment");
}

void ThreadMisuse(unsigned mode) {
  Fixture f;
  f.Mount();
  auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
  Check(operation.ok(), "actual host operation before foreign-thread misuse");
  auto reader = f.Read(operation.operation);
  Check(reader.ok(), "actual host reader before foreign-thread misuse");
  if (mode >= 4) {
    reader.lease.Reset();
    operation.operation.Reset();
  }
  std::set_terminate([] { ::_exit(73); });
  std::thread wrong([&] {
    switch (mode % 4) {
      case 0: (void)bool(reader.lease); break;
      case 1: { auto moved = std::move(reader.lease); (void)moved; break; }
      case 2: (void)bool(operation.operation); break;
      case 3: { auto moved = std::move(operation.operation); (void)moved; break; }
    }
  });
  wrong.join();
  throw std::runtime_error("foreign host custody unexpectedly returned");
}
void ThreadControls(const char* executable) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    const auto option = std::to_string(mode);
    const auto child = ::fork();
    Check(child >= 0, "fresh host thread-affinity child");
    if (!child) {
      ::execl(executable, executable, "--thread-death", option.c_str(), nullptr);
      ::_exit(79);
    }
    int status = 0;
    Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "foreign-thread live and reset host operation/reader access and move reject before custody");
  }
}

void ObservationPins() {
  for (unsigned profile = 0; profile < 5; ++profile) {
    Fixture f(profile);
    f.Mount();
    auto operation = Host::TryBeginOperation(f.host, Fixed(1001));
    Check(operation.ok(), "actual source observation operation");
    auto observed = operation.operation.ObserveFixedRootWithMemory(f.Limits(), f.reading.memory, f.reading.binding);
    Check(observed.ok() && observed.observation.operation_uuid() == Fixed(1001) &&
              observed.observation.identity().database_uuid == f.database() && observed.observation.directory().ok(),
          "immutable observation retains the complete fixed-root source");
    std::array<byte, 8> bytes{};
    Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, bytes).ok(),
          "same original writer performs bounded I/O after observation guards retire");
    auto reader = f.Read(operation.operation);
    Check(reader.ok(), "new fenced reader can coexist with immutable old observation");
    observed.observation.Reset();
    Check(operation.operation.ReadAtBounded(f.primary(), f.made.profile, 0, bytes).error == WE::read_handle_live,
          "retiring an old observation cannot release a later read reservation");
    reader.lease.Reset();
    const auto route = std::weak_ptr(f.route.lock);
    f.route.lock.reset();
    callback_fixture = &f;
    callback_operation = &operation.operation;
    host_allocation_callback = DropOperationDuringRead;
    auto detached = operation.operation.ObserveFixedRootWithMemory(f.Limits(), f.reading.memory, f.reading.binding);
    const auto called = !host_allocation_callback;
    host_allocation_callback = nullptr;
    Check(called && detached.ok() && !operation.operation && !f.host && !route.expired() &&
              detached.observation.identity().database_uuid == f.database(),
          "immutable observation pins full original host through callback reset and guard retirement");
    detached.observation.Reset();
    Check(route.expired(), "last immutable source observation retires original host custody");
    callback_fixture = nullptr;
    callback_operation = nullptr;
  }
}
}  // namespace hosted_custody_test

int main(int argc, char** argv) {
  using namespace hosted_custody_test;
  try {
    Artifacts owned;
    artifacts = &owned;
    if (argc == 3 && std::string_view(argv[1]) == "--thread-death") {
      const auto mode = std::stoul(argv[2]);
      Check(mode < 8, "host thread-affinity mode");
      ThreadMisuse(mode);
    } else {
      Check(argc == 1, "unknown host custody test mode");
      ThreadControls(argv[0]);
      InitialCustody();
      for (unsigned p = 0; p < 5; ++p) CompletePins(p);
      CloseObligations();
      CallbackPins();
      ProcessAndThreadChecks();
      ObservationPins();
    }
    owned.Cleanup();
    artifacts = nullptr;
    std::cout << "native hosted database custody PASS checks=" << checks
              << " production_create=true one_member_cohort=true physical_custody_only=true\n";
    return 0;
  } catch (const std::exception& error) {
    allocation_budget = -1;
    sync_fault = 0;
    io_counting = false;
    host_allocation_callback = nullptr;
    std::cerr << "native hosted database custody FAIL " << error.what() << " checks=" << checks << '\n';
    return 1;
  }
}
