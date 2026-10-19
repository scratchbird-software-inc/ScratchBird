// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main UnusedHostedCustodyFixtureMain
#define __wrap_pread HostedCustodyFixturePread
#define __wrap_pwrite HostedCustodyFixturePwrite
#include "native_management_control_bundle_test.cpp"
#undef __wrap_pread
#undef __wrap_pwrite
#undef main
#include "../../src/server/native_hosted_database_custody.hpp"
#include "native_owned_publication_watermark.hpp"
#include "engine/internal_api/transaction/native_local_transaction_candidate.hpp"
#include "engine/internal_api/transaction/native_startup_transaction_history.hpp"
#include "../../src/engine/internal_api/verification/native_verification_history.hpp"
#include "../../src/engine/internal_api/security/native_policy_activation_history.hpp"
#include "native_inventory_graph_builder.hpp"
#include "native_owned_inventory_publication.hpp"
#include "native_owned_inventory_recovery.hpp"
#include "../support/owned_temp_directory.hpp"
#include <fcntl.h>
#include <sys/wait.h>

namespace {
void (*host_allocation_callback)() = nullptr;
struct WatermarkMutex {pthread_mutex_t* mutex=nullptr;unsigned depth=0;bool selected=false;};
thread_local std::array<WatermarkMutex,512> watermark_mutexes{};
thread_local pthread_mutex_t* watermark_last_mutex=nullptr;
bool watermark_heap_probe=false,watermark_select_mutex=false,watermark_mutex_overflow=false;
std::uint64_t watermark_guarded_allocations=0,watermark_guarded_frees=0;
dev_t watermark_device{};ino_t watermark_inode{};
scratchbird::server::NativeHostedDatabaseCustody::Operation* watermark_reset_on_read=nullptr;
off_t recovery_stop_offset=-1;
bool recovery_stop_hit=false;
unsigned WatermarkGuardDepth(){unsigned n=0;for(const auto& m:watermark_mutexes)if(m.selected)n+=m.depth;return n;}
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex){
  const auto result=__real_pthread_mutex_lock(mutex);
  if(!result&&watermark_heap_probe){watermark_last_mutex=mutex;
    auto slot=std::find_if(watermark_mutexes.begin(),watermark_mutexes.end(),[&](const auto& m){return m.mutex==mutex;});
    if(slot==watermark_mutexes.end())slot=std::find_if(watermark_mutexes.begin(),watermark_mutexes.end(),[](const auto& m){return !m.mutex;});
    if(slot==watermark_mutexes.end())watermark_mutex_overflow=true;
    else {slot->mutex=mutex;++slot->depth;}}
  return result;
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex){
  const auto result=__real_pthread_mutex_unlock(mutex);
  if(!result&&watermark_heap_probe)for(auto& m:watermark_mutexes)if(m.mutex==mutex){if(m.depth)--m.depth;break;}
  return result;
}
extern "C" ssize_t __wrap_pread(int fd,void* bytes,size_t n,off_t offset){
  if(watermark_heap_probe&&watermark_select_mutex){struct stat identity{};
    if(!::fstat(fd,&identity)&&identity.st_dev==watermark_device&&identity.st_ino==watermark_inode){
      for(auto& m:watermark_mutexes)if(m.mutex==watermark_last_mutex&&m.depth>1){m.selected=true;watermark_select_mutex=false;}}}
  if(auto* operation=std::exchange(watermark_reset_on_read,nullptr))operation->Reset();
  return HostedCustodyFixturePread(fd,bytes,n,offset);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* bytes,size_t n,off_t offset){
  if(offset==recovery_stop_offset){recovery_stop_hit=true;errno=EIO;return -1;}
  return HostedCustodyFixturePwrite(fd,bytes,n,offset);
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
  if(watermark_heap_probe&&WatermarkGuardDepth())++watermark_guarded_allocations;
  if (auto callback = host_allocation_callback) { host_allocation_callback = nullptr; callback(); }
  return __real_malloc(size);
}
extern "C" void* __real_aligned_alloc(std::size_t,std::size_t);
extern "C" void* __wrap_aligned_alloc(std::size_t alignment,std::size_t size){
  if(watermark_heap_probe&&WatermarkGuardDepth())++watermark_guarded_allocations;
  return __real_aligned_alloc(alignment,size);
}
extern "C" void __real_free(void*);
extern "C" void __wrap_free(void* pointer){
  if(pointer&&watermark_heap_probe&&WatermarkGuardDepth())++watermark_guarded_frees;
  __real_free(pointer);
}
namespace hosted_custody_test {
namespace server = scratchbird::server;
using Host = server::NativeHostedDatabaseCustody;
using HE = server::NativeHostedCustodyError;
using WE = db::NativeWriterError;
using Grant = checkpoint_inventory_memory::Grant;
template<class T> concept ExposesWriter = requires(T value) { value.writer(); };
template<class T> concept ExposesDevices = requires(T value) { value.devices(); };
static_assert(!std::is_default_constructible_v<Host> && !std::is_copy_constructible_v<Host>);
static_assert(!ExposesWriter<Host> && !ExposesDevices<Host> && !ExposesDevices<Host::ReadLease>);
static_assert(!std::is_copy_constructible_v<Host::Operation> && !std::is_copy_constructible_v<Host::ReadLease>);
struct Artifacts {
  scratchbird::tests::OwnedTempDirectory directory;
  std::optional<std::string> previous;
  Artifacts() {
    if (const auto* value = std::getenv("TMPDIR")) previous = value;
    Check(!::setenv("TMPDIR", directory.path().c_str(), 1), "private host fixture namespace");
  }
  ~Artifacts() { Restore(); }
  void Restore() { if (previous) (void)::setenv("TMPDIR", previous->c_str(), 1); else (void)::unsetenv("TMPDIR"); }
  void Cleanup() { Restore(); directory.Cleanup(); }
};
struct Fixture {
  DirectoryHistoryFixture physical;
  std::size_t source_backing_bytes;
  Grant persistent{64*1024}, transient, reading;
  std::vector<std::string> paths;
  std::vector<Uuid> profiles;
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  server::DatabaseOwnershipResult route;
  std::shared_ptr<Host> host;
  explicit Fixture(unsigned profile = 0, bool readonly = false,
      d::RouteStorageAccess capability = d::RouteStorageAccess::read_write,bool installed_history=false)
      : physical(profile, (profile+1)%5, false, 2, false, false, true, installed_history),
        source_backing_bytes((installed_history?256:32)*1024*1024),
        transient(source_backing_bytes),reading(source_backing_bytes) {
    if(installed_history)physical.budget=16*1024*std::max<u64>(physical.t.fixture.size,
        physical.t.zeros.at(physical.t.other).bootstrap.page_size_bytes);
    for (const auto& member : physical.t.fixture.devices) {
      paths.push_back(member.device->path()); profiles.push_back(member.page_size_profile_uuid);
      Check(member.device->Close().ok(), "retire independent setup owners before actual host ownership");
    }
    server::DatabaseOwnershipRequest request; request.database_path = paths.front(); request.storage_access = capability;
    route = server::AcquireDatabaseOwnership(request); Check(route.acquired && route.lock, "actual original host route/OS owner");
    for (std::size_t n = 1; n < paths.size(); ++n) {
      auto file = std::make_unique<d::FileDevice>();
      Check(file->Open(paths[n], readonly && n+1 == paths.size() ? d::FileOpenMode::open_existing_read_only : d::FileOpenMode::open_existing).ok(),
          "actual native secondary member retains its physical access capability");
      secondary.push_back(std::move(file));
    }
  }
  ~Fixture() { host.reset(); route.lock.reset(); secondary.clear(); }
  void Begin() { auto begun = Host::Begin(route.lock); Check(begun.ok(), "real host initial custody"); host = std::move(begun.custody); }
  auto Adopt(const db::NativeStorageMemoryBinding* binding = nullptr) {
    return Host::AdoptWriter(host, Id(1), Id(2), secondary, {64*1024, {physical.budget, source_backing_bytes}},
        persistent.memory, binding ? *binding : persistent.binding, transient.memory, transient.binding);
  }
  void Mount() {
    Begin();const auto adopted=Adopt();
    if(!adopted.ok())std::cerr<<"host adoption="<<int(adopted.error)<<" source="<<int(adopted.source.error)
        <<" selection="<<int(adopted.source.selection_error)<<" checkpoint="<<int(adopted.source.checkpoint_error)
        <<" allocation="<<int(adopted.source.allocation_error)<<" directory="<<int(adopted.source.directory_error)
        <<" inventory="<<int(adopted.source.inventory_error)<<" memory="<<int(adopted.source.memory_source_error)<<'\n';
    Check(adopted.ok(),"actual native complete graph adopted by host custody");
  }
};

void InitialCustody() {
  { Fixture f(0, false, d::RouteStorageAccess::read_only);
    allocation_budget = 0; auto result = Host::Begin(f.route.lock); const auto remaining = allocation_budget; allocation_budget = -1;
    Check(!result.ok() && result.route_error == d::RouteSourceTransitionError::writable_not_requested && remaining == 0,
        "read-only initial route cannot become a hosted writer; refuses before allocation"); }
  for (long budget : {0, 1, 2, 3}) {
    Fixture f; allocation_budget = budget; auto result = Host::Begin(f.route.lock); allocation_budget = -1;
    Check(!result.ok() && f.route.lock->valid(), "host-control allocation failure preserves original owner");
    auto borrower = d::RouteOwnershipLease::Borrow(f.paths.front()+".sb.route.owner.lock");
    Check(bool(borrower), "host wrapper allocation failure precedes irreversible legacy-admission fence");
  }
  Fixture f;
  auto borrower = d::RouteOwnershipLease::Borrow(f.paths.front()+".sb.route.owner.lock"); Check(bool(borrower), "actual pre-fence legacy pin");
  f.Begin(); Check(!d::RouteOwnershipLease::Borrow(f.paths.front()+".sb.route.owner.lock"), "host fence closes new legacy admission");
  Check(Host::TryBeginOperation(f.host, Id(61001)).error == HE::not_mounted, "fenced route alone has no native writer operation");
  const auto pending = f.Adopt();
  Check(!pending.ok() && pending.source.route_error == d::RouteSourceTransitionError::not_drained && f.secondary.size() == 2 &&
      f.persistent.memory.Matches(f.persistent.binding), "undrained host adoption preserves members and admitted memory");
  const auto deadline = Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now()+std::chrono::milliseconds(5));
  Check(!deadline.ok() && deadline.observation.state == d::RouteSourceDrainWaitState::deadline_expired,
      "finite host drain wait reports the native deadline; cannot mount by copied observation");
  std::stop_source stop; stop.request_stop();
  const auto cancelled = Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now()+std::chrono::seconds(1), stop.get_token());
  Check(!cancelled.ok() && cancelled.observation.state == d::RouteSourceDrainWaitState::cancelled, "host drain cancellation retains original fence");
  borrower.reset();
  Check(Host::WaitForLegacyDrain(f.host, std::chrono::steady_clock::now()+std::chrono::seconds(1)).ok(), "same host owner drains after actual borrower release");
  const auto probe = Host::ProbeBootstrap(f.host);
  Check(probe.ok() && probe.source.bootstrap->database_uuid == Id(1) && probe.source.bootstrap->filespace_uuid == Id(2),
      "private retained primary supplies actual binary memory-admission selectors");
  auto wrong = f.persistent.binding; wrong.context_uuid = Id(61999);
  Check(!f.Adopt(&wrong).ok() && f.secondary.size() == 2 && f.persistent.memory.Matches(f.persistent.binding), "failed host adoption does not consume actual members/grant");
  Check(f.Adopt().ok() && f.secondary.empty() && !f.persistent.memory.Matches(f.persistent.binding), "same retained transition retries full native adoption once");
  Check(f.Adopt().error == HE::already_mounted && Host::ProbeBootstrap(f.host).error == HE::already_mounted, "mounted custody cannot be reimported or probed through old route admission");
}

void CompletePins(unsigned profile) {
  Fixture f(profile, profile == 4); f.Mount();
  const auto route = std::weak_ptr(f.route.lock); const auto host = std::weak_ptr(f.host);
  f.route.lock.reset();
  auto operation = Host::TryBeginOperation(f.host, Id(61001)); Check(operation.ok(), "host operation retains full native custody");
  auto reader = operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024}, f.reading.memory, f.reading.binding);
  Check(reader.ok() && reader.lease.operation_uuid() == Id(61001), "host reader retains exact operation and selected source");
  std::array<byte,1> byte{};
  Check(operation.operation.ReadAtBounded(Id(2), f.profiles.front(), 0, byte).error == WE::read_handle_live, "actual source guards exclude physical writer I/O");
  allocation_budget=0;
  const auto history=operation.operation.ReadHistoryWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(history.writer_error==WE::read_handle_live&&remaining==0&&!history.ok(),
      "retained selected guards exclude history allocation and nested source reads");
  operation.operation.Reset(); f.host.reset();
  Check(!host.expired() && !route.expired() && reader.lease.source().selection().selection->header.database_uuid == Id(1),
      "restricted reader alone retains original server route owner as well as native writer after external handle reset");
  Host::ReadLease moved(std::move(reader.lease));
  Check(!reader.lease && moved.operation_uuid() == Id(61001), "same-thread read move retains exactly one full host pin");
  if (!profile) for (unsigned mode = 0; mode < 2; ++mode) {
    const auto child = ::fork(); Check(child >= 0, "inherited live or moved-from host reader");
    if (!child) {
      std::set_terminate([] { ::_exit(73); });
      if (mode) { auto inherited = std::move(reader.lease); (void)inherited; }
      else (void)moved.source();
      ::_exit(79);
    }
    int status = 0; Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
        "inherited read access and moved-from move reject before inherited custody");
  }
  moved.Reset(); Check(host.expired() && route.expired(), "retiring last read releases full host custody after device guard cleanup");
}

void CloseObligations() {
  Fixture f; f.Mount(); const auto route = std::weak_ptr(f.route.lock); f.route.lock.reset();
  auto operation = Host::TryBeginOperation(f.host, Id(61001)); Check(operation.ok(), "host close fixture operation");
  auto reader = operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024}, f.reading.memory, f.reading.binding);
  Check(reader.ok(), "host close fixture reader"); std::array<db::NativeWriterMemberCloseReceipt,3> receipts{};
  Check(!Host::RetireWriter(f.host, std::span(receipts).first(2)).ok(), "short receipt capacity refuses without withdrawing actual writer");
  const auto busy = Host::RetireWriter(f.host, receipts);
  Check(!busy.ok() && busy.writer.error == WE::busy && !route.expired(), "busy writer close retains complete original host custody");
  Check(Host::RetireWriterBacking(f.host)==HE::writer_failure,
      "live writer backing cannot retire before actual successful physical close");
  operation.operation.Reset(); Check(Host::RetireWriter(f.host, receipts).writer.error == WE::busy, "reader alone blocks writer close");
  reader.lease.Reset(); syncs = 0; io_counting = true; sync_fault = 1;
  const auto failed = Host::RetireWriter(f.host, receipts); sync_fault = 0; io_counting = false;
  Check(!failed.ok() && !failed.writer.all_members_retired && !route.expired(), "failed native sync preserves member and original host owner for retry");
  Check(Host::RetireWriterBacking(f.host)==HE::writer_failure,
      "failed native close retains writer backing for the original retry");
  const auto retired = Host::RetireWriter(f.host, receipts); const auto state = Host::Observe(f.host);
  Check(retired.ok() && state.stage == server::NativeHostedCustodyStage::writer_retired && state.initial_route_owner_retained && !route.expired(),
      "adopted member retirement is explicitly distinct from original route/OS owner release");
  Check(Host::RetireWriterBacking(f.host)==HE::none&&Host::RetireWriterBacking(f.host)==HE::none,
      "successfully closed writer releases its backing idempotently before original route retirement");
  const auto released=Host::Observe(f.host);
  Check(released.database_uuid==state.database_uuid&&released.primary_filespace_uuid==state.primary_filespace_uuid&&
      released.initial_route_owner_retained,"backing retirement preserves actual adopted identity and original OS custody");
  server::DatabaseOwnershipRequest contender; contender.database_path = f.paths.front();
  Check(!server::AcquireDatabaseOwnership(contender).acquired, "adopted-member close cannot expose node to a second host");
  Check(!Host::TryBeginOperation(f.host, Id(61002)).ok(), "retired writer cannot admit another operation");
  f.host.reset(); Check(route.expired(), "coordinator's final custody release retires initial route owner");
  auto reopened = server::AcquireDatabaseOwnership(contender); Check(reopened.acquired, "new owner can acquire OS custody only after actual host lifetime ends");
}

Fixture* callback_fixture = nullptr;
Host::Operation* callback_operation = nullptr;
HE callback_reentrant = HE::none;
void DropRouteDuringBegin() { callback_fixture->route.lock.reset(); }
void DropHostDuringOperation() {
  std::array<db::NativeWriterMemberCloseReceipt,3> receipts{};
  callback_reentrant = Host::RetireWriter(callback_fixture->host, receipts).error;
  callback_fixture->host.reset();
}
void DropOperationDuringRead() {
  callback_operation->Reset(); callback_fixture->host.reset();
}
void CallbackPins() {
  { Fixture f; callback_fixture = &f; const auto route = std::weak_ptr(f.route.lock);
    host_allocation_callback = DropRouteDuringBegin; auto begun = Host::Begin(f.route.lock); host_allocation_callback = nullptr;
    Check(begun.ok() && !f.route.lock && !route.expired(), "factory pins actual initial owner before allocation callback resets caller's route handle"); }
  { Fixture f; f.Mount(); callback_fixture = &f; const auto route = std::weak_ptr(f.route.lock); f.route.lock.reset();
    host_allocation_callback = DropHostDuringOperation; auto operation = Host::TryBeginOperation(f.host, Id(61001)); host_allocation_callback = nullptr;
    Check(operation.ok() && !f.host && !route.expired() && callback_reentrant == HE::busy, "operation factory pins whole host and refuses reentrant close during allocation");
    operation.operation.Reset(); Check(route.expired(), "operation retirement releases callback-retained initial owner"); }
  { Fixture f; f.Mount(); callback_fixture = &f; auto operation = Host::TryBeginOperation(f.host, Id(61001)); Check(operation.ok(), "callback read operation");
    callback_operation = &operation.operation; const auto route = std::weak_ptr(f.route.lock); f.route.lock.reset();
    host_allocation_callback = DropOperationDuringRead;
    auto reader = operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024}, f.reading.memory, f.reading.binding); host_allocation_callback = nullptr;
    Check(reader.ok() && !operation.operation && !f.host && !route.expired(), "read call pins original host across callback resetting both external owner and operation");
    reader.lease.Reset(); Check(route.expired(), "callback-retained original route retires only after actual selected read cleanup"); }
  callback_fixture = nullptr; callback_operation = nullptr;
}
void ProcessAndThreadChecks() {
  Fixture f; f.Mount();
  HE foreign = HE::none; std::thread wrong([&] { foreign = Host::ProbeBootstrap(f.host).error; }); wrong.join();
  Check(foreign == HE::wrong_thread, "opening-thread source operations reject foreign threads before native access");
  const auto child = ::fork(); Check(child >= 0, "actual inherited host child");
  if (!child) {
    allocation_budget = 0; std::array<db::NativeWriterMemberCloseReceipt,3> receipts{};
    const bool refused = Host::TryBeginOperation(f.host, Id(61002)).error == HE::wrong_process &&
        Host::RetireWriter(f.host, receipts).error == HE::wrong_process && Host::Observe(f.host).error == HE::wrong_process;
    allocation_budget = -1; f.host.reset(); f.route.lock.reset(); ::_exit(refused ? 0 : 79);
  }
  int status = 0; Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "inherited host rejects before allocation/locks and permits underlying descriptor-only abandonment");
  auto operation = Host::TryBeginOperation(f.host, Id(61001)); Check(operation.ok(), "parent custody remains usable after child abandonment");
}
void ThreadMisuse(unsigned mode) {
  Fixture f; f.Mount(); auto operation = Host::TryBeginOperation(f.host, Id(61001)); Check(operation.ok(), "actual host operation before foreign-thread misuse");
  auto reader = operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024}, f.reading.memory, f.reading.binding);
  Check(reader.ok(), "actual host reader before foreign-thread misuse");
  if (mode >= 4) { reader.lease.Reset(); operation.operation.Reset(); }
  std::set_terminate([] { ::_exit(73); });
  std::thread wrong([&] {
    switch (mode % 4) {
      case 0: (void)bool(reader.lease); break;
      case 1: { auto moved = std::move(reader.lease); (void)moved; break; }
      case 2: (void)bool(operation.operation); break;
      case 3: { auto moved = std::move(operation.operation); (void)moved; break; }
    }
  });
  wrong.join(); throw std::runtime_error("foreign host custody unexpectedly returned");
}
void ThreadControls(const char* executable) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    const auto option = std::to_string(mode); const auto child = ::fork(); Check(child >= 0, "fresh host thread-affinity child");
    if (!child) { ::execl(executable, executable, "--thread-death", option.c_str(), nullptr); ::_exit(79); }
    int status = 0; Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
        "foreign-thread live and reset host operation/reader access and move reject before custody");
  }
}

db::NativePublicationIntent WatermarkIntent(){
  db::NativePublicationIntent intent;intent.initiator_uuid=Id(62001);intent.request_context_uuid=Id(62002);
  intent.policy_snapshot_uuid=Id(62003);intent.normalized_request_sha256.fill(71);
  intent.initiator_kind=4;intent.recovery_profile=2;return intent;
}
struct WatermarkHeapProbe {
  explicit WatermarkHeapProbe(const std::string& path){struct stat identity{};
    Check(!::stat(path.c_str(),&identity),"actual native inode for publication lock probe");
    watermark_device=identity.st_dev;watermark_inode=identity.st_ino;watermark_mutexes={};
    watermark_guarded_allocations=watermark_guarded_frees=0;watermark_mutex_overflow=false;
    watermark_select_mutex=watermark_heap_probe=true;
  }
  ~WatermarkHeapProbe(){watermark_heap_probe=false;}
  void Verify(){const auto selected=!watermark_select_mutex;const auto depth=WatermarkGuardDepth();watermark_heap_probe=false;
    Check(selected&&!depth&&!watermark_mutex_overflow&&!watermark_guarded_allocations&&!watermark_guarded_frees,
        "actual native device guards contain no ordinary/aligned allocation or free in observation/publication");}
};

void OwnedObservation() {
  for(unsigned profile=0;profile<5;++profile){
    Fixture f(profile);f.Mount();
    auto operation=Host::TryBeginOperation(f.host,Id(61001));Check(operation.ok(),"actual source observation operation");
    auto observed=operation.operation.ObserveWithMemory({f.physical.budget,16*1024*1024},f.reading.memory,f.reading.binding);
    Check(observed.ok()&&observed.observation.operation_uuid()==Id(61001)&&
        observed.observation.selection().selection->header.database_uuid==Id(1)&&
        observed.observation.directory().directory.ok(),"immutable observation retains complete actual selected source");
    std::array<byte,8> bytes{};
    Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,bytes).ok(),
        "same original writer performs bounded I/O after observation guards retire");
    auto reader=operation.operation.ReadWithMemory({f.physical.budget,16*1024*1024},f.reading.memory,f.reading.binding);
    Check(reader.ok(),"new fenced reader can coexist with immutable old observation");
    observed.observation.Reset();
    Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,bytes).error==WE::read_handle_live,
        "retiring an old observation cannot release a later read reservation");
    reader.lease.Reset();
    const auto route=std::weak_ptr(f.route.lock);f.route.lock.reset();
    callback_fixture=&f;callback_operation=&operation.operation;host_allocation_callback=DropOperationDuringRead;
    auto detached=operation.operation.ObserveWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
    const auto called=!host_allocation_callback;host_allocation_callback=nullptr;
    Check(called&&detached.ok()&&!operation.operation&&!f.host&&!route.expired()&&
        detached.observation.selection().selection->header.database_uuid==Id(1),
        "immutable observation pins full original host through callback reset and guard retirement");
    detached.observation.Reset();Check(route.expired(),"last immutable source observation retires original host custody");
    callback_fixture=nullptr;callback_operation=nullptr;
  }
}

Bytes StartupFixtureState(const db::NativeManagementOperation& record) {
  auto bytes=VerificationFixtureState(record,1);
  Put(bytes,128,record.initiator_uuid);
  std::fill(bytes.begin()+160,bytes.begin()+176,byte{0});
  Num(bytes,208,8,1000);
  return bytes;
}
void OwnedInventoryGraph() {
  for(unsigned scenario=0;scenario<30;++scenario){
    const unsigned profile=scenario%5;const bool startup=scenario>=5&&scenario<15;
    const bool invalid_verification_origin=scenario>=10&&scenario<15;
    const bool policy=scenario>=15,invalid_policy=scenario>=20,orphan_policy=scenario>=25;
    Fixture f(profile);f.Mount();
    auto operation=Host::TryBeginOperation(f.host,Id(62000));Check(operation.ok(),"actual inventory graph owner");
    auto record=f.physical.t.operation;
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),record.policy_snapshot_uuid},{{},0,1000});
    const auto tx=issuer.Issue(UuidKind::transaction);Check(tx.ok(),"node issuer allocates actual transaction identity for physical fixture");
    std::optional<db::NativeStartupBinding> startup_binding;
    if(startup){
      auto initial=operation.operation.ObserveWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
      Check(initial.ok(),"startup fixture reads actual next local transaction number");
      const auto next_local=initial.observation.selection().checkpoint_inventory.inventory.next_local_transaction_id;
      startup_binding=db::NativeStartupBinding{record.uuid,record.request_context_uuid,tx.value->value,next_local,19};
      record.normalized_request_bytes=Bytes(64);
      Put(record.normalized_request_bytes,0,record.uuid);Put(record.normalized_request_bytes,16,record.request_context_uuid);
      Put(record.normalized_request_bytes,32,tx.value->value);Num(record.normalized_request_bytes,48,8,next_local);
      Num(record.normalized_request_bytes,56,8,19);
      const auto initial_state=StartupFixtureState(record);
      record.normalized_request_bytes.insert(record.normalized_request_bytes.end(),initial_state.begin(),initial_state.end());
      record.normalized_request_sha256=Sha(record.normalized_request_bytes);
      record.operation_state_bytes=invalid_verification_origin?VerificationFixtureState(record,3):initial_state;
    }
    if(policy){
      auto initial=operation.operation.ObserveWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
      Check(initial.ok(),"policy publication fixture reads actual next transaction number");
      const auto next_local=initial.observation.selection().checkpoint_inventory.inventory.next_local_transaction_id;
      record.boundary_uuid=Id(62101);record.target_uuid=Id(62102);record.evidence_required=true;
      Bytes raw(256);std::copy_n("SBPA0001",8,raw.begin());Num(raw,8,2,1);Num(raw,10,2,256);Num(raw,12,4,256);
      Put(raw,16,record.uuid);Put(raw,32,record.database_uuid);Put(raw,48,record.target_uuid);Put(raw,64,Id(62103));
      Put(raw,80,Id(62104));Put(raw,96,record.boundary_uuid);Put(raw,112,tx.value->value);
      Num(raw,160,8,1);Num(raw,168,8,1);Num(raw,176,8,1);Num(raw,184,8,next_local);
      Num(raw,200,2,invalid_policy&&!orphan_policy?2:1);Num(raw,202,2,3);
      if(orphan_policy){Put(raw,128,Id(62105));Num(raw,176,8,2);}
      record.operation_state_bytes=record.normalized_request_bytes=std::move(raw);
      record.normalized_request_sha256=Sha(record.normalized_request_bytes);
    }
    Grant publication(2*1024*1024);
    auto workspace=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,
        publication.memory,publication.binding);
    Check(workspace.ok(),"governed physical publication workspace");
    const auto observed=workspace.workspace.Inspect(operation.operation,{f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding,startup_binding);
    Check(observed.ok(),"actual selected base for bounded inventory construction");
    db::NativePublicationIntent intent;intent.recovery_profile=2;intent.initiator_uuid=record.initiator_uuid;
    intent.request_context_uuid=record.request_context_uuid;intent.policy_snapshot_uuid=record.policy_snapshot_uuid;
    intent.normalized_request_sha256=record.normalized_request_sha256;intent.initiator_kind=record.initiator_kind;intent.startup_binding=startup_binding;
    auto reserved=workspace.workspace.Reserve(operation.operation,*observed.snapshot,intent);
    Check(reserved.ok(),"actual durable profile2 generation before graph construction");
    auto source=operation.operation.ObserveWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
    Check(source.ok(),"original writer retains immutable inventory, allocation and directory inputs");
    const auto& selected=source.observation.selection();const auto& base=selected.checkpoint_inventory;
    const auto size=f.physical.t.fixture.size;
    Bytes zero_bytes(size);Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,zero_bytes).ok(),"actual primary body for graph");
    std::array<d::FilespaceRootReference,32> zero_roots;
    auto zero=d::DecodeFilespacePageZeroInto(zero_bytes,zero_roots);Check(zero.ok(),"actual primary graph binding");
    const auto encoded_record=db::EncodeNativeManagementOperation(record,1024*1024);Check(encoded_record.ok(),"canonical physical operation fixture");
    std::vector<db::NativeManagementStepView> steps(record.steps.size());std::vector<Uuid> ids(record.steps.size()*2+7);
    const auto decoded=db::DecodeNativeManagementOperationInto(encoded_record.bytes,1024*1024,{steps,ids});Check(decoded.ok(),"immutable operation view");
    std::vector<mga::TransactionInventoryEntry> entries(base.inventory.entries.begin(),base.inventory.entries.end());
    mga::TransactionInventoryEntry next;next.identity={mga::MakeLocalTransactionId(base.inventory.next_local_transaction_id),*tx.value,mga::TransactionScope::local_node};
    next.state=mga::TransactionState::created;next.begin_unix_epoch_millis=1789357072000ULL;
    next.begin_visible_through_local_transaction_id=base.inventory.next_local_transaction_id-1;
    next.begin_visible_through_commit_sequence=base.inventory.next_commit_sequence-1;entries.push_back(next);
    const page::NativeTransactionInventoryConstView inventory{base.inventory.next_local_transaction_id+1,base.inventory.next_commit_sequence,entries};
    const u64 allowance=64*1024*1024;
    Grant graph_grant(allowance);auto graph_memory=graph_grant.memory.CreateArena(graph_grant.binding,allowance,alignof(std::max_align_t));
    Check(graph_memory.ok(),"real bounded graph construction grant");auto block=graph_memory.arena.Allocate(allowance,alignof(std::max_align_t));Check(block.ok(),"graph byte region");
    const std::span<byte> backing{static_cast<byte*>(block.pointer),block.bytes};
    const db::NativeInventoryGraphInputs input{selected,source.observation.directory().directory,*zero.record,*reserved.snapshot,*decoded.record,inventory,{},allowance};
    allocation_budget=0;const auto built=db::BuildNativeInventoryGraphInto(input,issuer,backing);const auto heap=allocation_budget;allocation_budget=-1;
    if(!built.ok())std::cerr<<"owned inventory graph error="<<int(built.error)<<" profile="<<profile<<'\n';
    Check(built.ok()&&heap==0&&!built.graph.checkpoint.empty()&&!built.graph.inventory.empty()&&
        built.graph.plan.reserved_generation==reserved.snapshot->watermark.watermark,
        "governed builder constructs complete profile2 inventory graph without heap fallback");
    std::vector<Bytes> old_inventory,zeros;std::vector<std::span<const byte>> old_inventory_views,zero_views,map_views,directory_views;
    for(const auto& p:base.inventory_pages){Bytes raw(p.header.page_size_bytes);
      Check(operation.operation.ReadAtBounded(p.header.filespace_uuid,p.header.page_size_profile_uuid,p.header.page_number*u64{p.header.page_size_bytes},raw).ok(),"actual base inventory image");old_inventory.push_back(std::move(raw));}
    for(const auto& p:old_inventory)old_inventory_views.push_back(p);
    for(const auto& m:selected.allocation.pages)map_views.push_back(m.image);
    for(const auto& p:source.observation.directory().directory.pages){directory_views.push_back(p.image);
      for(const auto& member:p.directory.records){if(member.bootstrap.filespace_uuid!=Id(2))continue;Bytes raw(member.bootstrap.page_size_bytes);
        Check(operation.operation.ReadAtBounded(member.bootstrap.filespace_uuid,member.bootstrap.page_size_profile_uuid,0,raw).ok(),"actual directory member body");zeros.push_back(std::move(raw));}}
    for(const auto& raw:zeros)zero_views.push_back(raw);
    Bytes base_checkpoint(size);Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),selected.selection->checkpoint.page_number*size,base_checkpoint).ok(),"actual original checkpoint bytes");
    const db::NativeManagementDirectoryBaseView directory_base{directory_views,zero_views};
    const db::NativeManagementControlAllocationInputs proof{base_checkpoint,built.graph.checkpoint,built.graph.plan_image,
      built.graph.extent,map_views,built.graph.allocation,built.graph.bundle,old_inventory_views,&directory_base,allowance};
    Bytes validation(allowance);allocation_budget=0;
    const auto verified=db::ValidateNativeManagementControlAllocationInto(proof,validation);const auto verify_heap=allocation_budget;allocation_budget=-1;
    if(!verified.ok())std::cerr<<"owned inventory delta error="<<int(verified.error)<<" profile="<<profile<<'\n';
    Check(verified.ok()&&verify_heap==0,"complete independently decoded directory/control/inventory delta validates constructed graph");
    const auto unchanged=operation.operation.ObserveWithMemory({f.physical.budget,32*1024*1024},f.transient.memory,f.transient.binding);
    Check(unchanged.ok()&&unchanged.observation.selection().selection->checkpoint_sha256==selected.selection->checkpoint_sha256,
        "graph construction never selects or writes the candidate inventory");
    Grant publishing(512*1024*1024);
    db::NativeOwnedInventoryPublication publisher;
    const db::NativeOwnedInventoryPublicationLimits limits{{allowance,64*1024*1024},{allowance,64*1024*1024},
      {allowance,256*1024*1024},64*1024*1024,4*1024*1024,64*1024*1024,allowance};
    const auto orphan_offset=built.graph.plan.header.page_number*u64{size}+128;
    const std::array<byte,1> orphan{0x59};
    if(!profile)Check(operation.operation.WriteAtBounded(Id(2),f.profiles.front(),orphan_offset,orphan).ok()&&
      operation.operation.Sync(Id(2),f.profiles.front()).ok(),"actual prior unselected bytes in an otherwise free slot");
    db::NativeOwnedInventoryPublicationResult prepared;
    if(scenario==0){
      const auto before=operation.operation.effects();allocation_budget=0;
      const auto failed=publisher.Prepare(operation.operation,std::move(workspace.workspace),*decoded.record,inventory,
          issuer,limits,publishing.memory,publishing.binding);allocation_budget=-1;
      Check(!failed.ok()&&failed.phase==db::NativeOwnedInventoryPublicationPhase::preparation_failed&&
          !failed.mutation_started&&before.write_attempts==operation.operation.effects().write_attempts,
          "failed source preparation retains its durable reservation before any graph write");
      prepared=publisher.RetryPreparation(*decoded.record,inventory,issuer);
    }else prepared=publisher.Prepare(operation.operation,std::move(workspace.workspace),*decoded.record,inventory,
        issuer,limits,publishing.memory,publishing.binding);
    if(!prepared.ok())std::cerr<<"owned publication prepare="<<int(prepared.error)<<" control="<<int(prepared.control_error)
      <<" history="<<int(prepared.history_error)<<" delta="<<int(prepared.allocation_error)<<" profile="<<profile<<'\n';
    Check(prepared.ok()&&!prepared.mutation_started,"actual owner validates full graph and history before installation");
    reads=writes=syncs=0;
    read_fault=profile==3?1:0;write_fault=profile==1?1:profile==4?4:0;sync_fault=profile==2?1:0;
    torn_bytes=write_fault?size/3:0;io_counting=profile!=0;
    auto published=publisher.Publish();
    io_counting=false;const bool injected=read_fault&&reads>=read_fault||write_fault&&writes>=write_fault||sync_fault&&syncs>=sync_fault;
    read_fault=write_fault=sync_fault=0;torn_bytes=0;
    if(profile){Check(injected&&!published.ok()&&!published.selected_graph_verified&&!published.snapshot,
        "physical read/write/sync faults never report a selected transaction");
      const auto partial_identity=operation.operation.identity();const auto partial_effects=operation.operation.effects();
      Check(publisher.Advance(operation.operation,Id(62002))==WE::invalid_operation&&
          operation.operation.identity()==partial_identity&&operation.operation.effects()==partial_effects,
          "interrupted physical publication cannot hand off or discard its original retry effects");
      Check(Host::TryBeginOperation(f.host,Id(62001)).error==HE::writer_failure,
        "failed publication keeps its exact original writer reservation");
      published=publisher.Resume();}
    if(!published.ok())std::cerr<<"owned publication error="<<int(published.error)<<" phase="<<int(published.phase)
      <<" control="<<int(published.control_error)<<" profile="<<profile<<'\n';
    Check(published.ok()&&published.selected_graph_verified&&published.installed_graph_verified&&published.snapshot&&
      published.snapshot->selection.checkpoint_generation==reserved.snapshot->watermark.watermark,
      "actual owned writer durably selects and independently verifies complete inventory graph");
    const auto complete=publisher.Resume();
    Check(complete.ok()&&complete.effects==published.effects,"completed physical publication cannot repeat writes or reserve a new attempt");
    if(!profile){std::array<byte,1> retained{};
      Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),orphan_offset,retained).ok()&&retained==orphan,
        "fresh owned publication skips prior unselected bytes without erasing or reclaiming them");}
    auto actual=operation.operation.ObserveWithMemory({f.physical.budget,256*1024*1024},publishing.memory,publishing.binding);
    if(!actual.ok())std::cerr<<"owned final source="<<int(actual.error)<<" writer="<<int(actual.writer_error)
      <<" memory="<<int(actual.memory_source_error)<<" selected="<<int(actual.selection_error)<<" checkpoint="<<int(actual.checkpoint_error)
      <<" inventory="<<int(actual.inventory_error)<<" allocation="<<int(actual.allocation_error)
      <<" directory="<<int(actual.directory_error)<<" profile="<<profile<<'\n';
    Check(actual.ok()&&actual.observation.selection().checkpoint_inventory.inventory.next_local_transaction_id==inventory.next_local_transaction_id&&
      actual.observation.selection().checkpoint_inventory.inventory.entries.back().identity.transaction_uuid.value==tx.value->value&&
      actual.observation.selection().checkpoint_inventory.inventory.entries.back().state==mga::TransactionState::created,
      "independent source observes the durable starting transaction and advanced inventory counter");
    actual.observation.Reset();
    auto retained=operation.operation.ReadHistoryWithMemory({f.physical.budget,64*1024*1024},publishing.memory,publishing.binding);
    Check(retained.ok()&&!retained.history.entries.empty(),"original custody reads installed operation state");
    const auto& retained_state=retained.history.entries.back().record.operation_state_bytes;
    Check(std::equal(record.operation_state_bytes.begin(),record.operation_state_bytes.end(),retained_state.begin(),retained_state.end()),
      "governed graph builder and installed history retain every typed-state byte after write retry");
    std::array<byte,4096> verification_scratch;
    const auto family_verified=scratchbird::engine::internal_api::VerifyNativeVerificationHistory(retained.history,f.physical.budget,verification_scratch);
    if(invalid_verification_origin){
      Check(!family_verified.ok()&&!family_verified.selection&&
          family_verified.error==scratchbird::engine::internal_api::NativeVerificationHistoryError::invalid_state&&
          family_verified.failed_operation_uuid==record.uuid&&family_verified.failed_revision==1,
          "a physically intact retained aggregate cannot declare identity verified before its verification operation was admitted");
    }else Check(family_verified.ok()&&family_verified.verified_operations==(startup?1u:0u)&&family_verified.verified_revisions==(startup?1u:0u)&&
        family_verified.selection->checkpoint_sha256==retained.history.selection->checkpoint_sha256,
        "actual retained original-custody history also obeys the verification-family origin and phase rules");
    const auto policy_verified=scratchbird::engine::internal_api::VerifyNativePolicyActivationHistory(retained.history,f.physical.budget,verification_scratch);
    if(invalid_policy)Check(!policy_verified.ok()&&!policy_verified.selection&&policy_verified.failed_operation_uuid==record.uuid&&
        (!orphan_policy||policy_verified.error==scratchbird::engine::internal_api::NativePolicyActivationHistoryError::invalid_predecessor),
        "physical publication cannot turn genesis revocation or an unresolved predecessor into valid activation intent");
    else Check(policy_verified.ok()&&policy_verified.verified_operations==(policy?1u:0u)&&policy_verified.verified_revisions==(policy?1u:0u)&&
        policy_verified.selection->checkpoint_sha256==retained.history.selection->checkpoint_sha256,
        "actual retained policy history preserves canonical intent through publication and I/O retries");
  }
}

std::shared_ptr<Host>* handoff_host=nullptr;
HE handoff_competing_error=HE::none;
void ProbePublicationHandoff() {
  handoff_competing_error=Host::TryBeginOperation(*handoff_host,Id(62191)).error;
}
void OwnedInventoryActivation() {
  for(const unsigned profile:{0u,4u})for(const bool startup:{false,true}){
    Fixture f(profile);f.Mount();const auto budget=f.physical.budget;
    auto record=f.physical.t.operation;
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),record.policy_snapshot_uuid},{{},0,1000});
    const auto tx=issuer.Issue(UuidKind::transaction);Check(tx.ok(),"physical activation fixture transaction identity");
    std::optional<db::NativeStartupBinding> binding;
    std::optional<db::NativePublicationSnapshot> final_publication;
    u64 local=0;
    Host::OperationResult operation;
    for(unsigned phase=0;phase<3;++phase){
      if(!phase||!startup)operation=Host::TryBeginOperation(f.host,Id(62100+phase));
      Check(operation.ok()&&operation.operation.identity()==Id(62100+phase),
          "next physical operation retains actual exclusive ownership through each startup phase");
      Grant reading(256*1024*1024);
      auto before=operation.operation.ObserveWithMemory({budget,256*1024*1024},reading.memory,reading.binding);
      Check(before.ok(),"actual current inventory before starting or activation");
      const auto& actual=before.observation.selection().checkpoint_inventory.inventory;
      namespace api=scratchbird::engine::internal_api;
      std::vector<mga::TransactionInventoryEntry> entries(actual.entries.size()+1);
      std::vector<std::size_t> indices(actual.entries.size());std::vector<byte> markers(actual.entries.size());
      if(!phase){
        local=actual.next_local_transaction_id;
        if(startup){binding=db::NativeStartupBinding{record.uuid,record.request_context_uuid,tx.value->value,local,19};
          record.normalized_request_bytes=Bytes(64);
          Put(record.normalized_request_bytes,0,record.uuid);Put(record.normalized_request_bytes,16,record.request_context_uuid);
          Put(record.normalized_request_bytes,32,tx.value->value);Num(record.normalized_request_bytes,48,8,local);
          Num(record.normalized_request_bytes,56,8,19);record.normalized_request_sha256=Sha(record.normalized_request_bytes);}
      }else{
        Check(actual.entries.back().identity.transaction_uuid.value==tx.value->value&&actual.entries.back().identity.local_id.value==local&&
          actual.entries.back().state==(phase==1?mga::TransactionState::created:
            profile?mga::TransactionState::read_only_active:mga::TransactionState::active),
          "each engine candidate starts from the actual separately selected preceding state");
        const auto clock_id=issuer.Issue(UuidKind::object);Check(clock_id.ok(),"fresh admitted revision event identity");
        ++record.revision;record.updated_at=clock_id.value->value;
      }
      api::NativeLocalTransactionCandidateRequest request;
      request.transaction_uuid=*tx.value;request.local_transaction_id=local;
      request.change=!phase?api::NativeLocalTransactionChange::allocate_starting:phase==2?
        api::NativeLocalTransactionChange::start_rollback:profile?
        api::NativeLocalTransactionChange::activate_read_only:api::NativeLocalTransactionChange::activate;
      if(!phase)request.begin_unix_epoch_millis=1789357072000ULL;
      const page::NativeTransactionInventoryConstView current{actual.next_local_transaction_id,actual.next_commit_sequence,actual.entries};
      allocation_budget=0;
      const auto transformed=api::BuildNativeLocalTransactionCandidate(current,request,{entries,indices,markers});
      const auto heap=allocation_budget;allocation_budget=-1;
      Check(transformed.ok()&&heap==0,"engine constructs complete native starting, activation or abort candidate without heap fallback");
      const auto candidate=transformed.inventory;
      before.observation.Reset();
      Grant publication(2*1024*1024);
      auto watermark=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
      Check(watermark.ok(),"next publication uses actual admitted watermark workspace");
      const auto observed=watermark.workspace.Inspect(operation.operation,{budget,256*1024*1024},reading.memory,reading.binding,binding);
      Check(observed.ok(),"next publication reads the actual stable duplex generation");
      db::NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
        record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;intent.startup_binding=binding;
      const auto reserved=watermark.workspace.Reserve(operation.operation,*observed.snapshot,intent);
      Check(reserved.ok(),"starting and activation consume distinct physical generations");
      const auto encoded=db::EncodeNativeManagementOperation(record,1024*1024);Check(encoded.ok(),"canonical activation management revision");
      std::vector<db::NativeManagementStepView> steps(record.steps.size());std::vector<Uuid> ids(record.steps.size()*2+7);
      const auto decoded=db::DecodeNativeManagementOperationInto(encoded.bytes,1024*1024,{steps,ids});Check(decoded.ok(),"bounded activation management view");
      Grant publishing(512*1024*1024);db::NativeOwnedInventoryPublication publisher;
      const db::NativeOwnedInventoryPublicationLimits limits{{budget,256*1024*1024},{budget,64*1024*1024},
        {budget,256*1024*1024},64*1024*1024,4*1024*1024,64*1024*1024,budget};
      if(phase&&!startup)watermark_reset_on_read=&operation.operation;
      const auto prepared=publisher.Prepare(operation.operation,std::move(watermark.workspace),*decoded.record,candidate,issuer,
        limits,publishing.memory,publishing.binding);
      watermark_reset_on_read=nullptr;
      if(!prepared.ok())std::cerr<<"activation prepare="<<int(prepared.error)<<" history="<<int(prepared.history_error)
        <<" control="<<int(prepared.control_error)<<" allocation="<<int(prepared.allocation_error)<<" phase="<<phase<<" profile="<<profile<<'\n';
      Check(prepared.ok(),"full selected ancestry and exact next management revision prepare for activation");
      if(phase&&!startup)Check(!operation.operation&&Host::TryBeginOperation(f.host,Id(62190)).error==HE::writer_failure,
        "publisher retains the original operation after the external facade retires during acquisition");
      if(startup)Check(publisher.Advance(operation.operation,Id(62101+phase))==WE::invalid_operation,
          "prepared or partially published inventory cannot hand off its original operation");
      const auto published=publisher.Publish();
      if(!published.ok())std::cerr<<"activation publish="<<int(published.error)<<" control="<<int(published.control_error)
        <<" stage="<<int(published.phase)<<" phase="<<phase<<" profile="<<profile<<'\n';
      Check(published.ok()&&published.selected_graph_verified&&published.snapshot&&
        published.snapshot->selection.checkpoint_generation==phase+2,"separate actual starting, activation and rollback-entry graph publications");
      final_publication=published.snapshot;
      if(startup&&phase<2){
        const auto original=operation.operation.identity();
        const auto effects=operation.operation.effects();
        Check(publisher.Advance(operation.operation,original)==WE::invalid_request&&
            publisher.Advance(operation.operation,Uuid{})==WE::invalid_request,
            "successor requires a distinct valid physical attempt identity");
        auto pin=operation.operation.RetainContinuity();
        allocation_budget=0;
        const auto pinned=publisher.Advance(operation.operation,Id(62101+phase));
        const auto pin_heap=allocation_budget;allocation_budget=-1;
        Check(pinned==WE::busy&&pin_heap==0&&operation.operation.identity()==original,
            "retained original publication pin refuses successor before allocation");
        pin={};
        allocation_budget=0;
        const auto refused=publisher.Advance(operation.operation,Id(62101+phase));
        allocation_budget=-1;
        Check(refused==WE::resource_exhausted&&operation.operation.identity()==original&&
            operation.operation.effects()==effects&&publisher.Resume().ok(),
            "successor allocation failure preserves exact completed publisher and original operation");
        handoff_host=&f.host;handoff_competing_error=HE::none;
        host_allocation_callback=ProbePublicationHandoff;
        const auto advanced=publisher.Advance(operation.operation,Id(62101+phase));
        const bool probed=!host_allocation_callback;host_allocation_callback=nullptr;handoff_host=nullptr;
        Check(advanced==WE::none&&probed&&handoff_competing_error==HE::writer_failure&&
            operation.operation.identity()==Id(62101+phase)&&operation.operation.effects()==db::NativeWriterPhysicalEffects{},
            "completed publisher transfers custody without admitting a reentrant competitor or inheriting prior I/O counters");
        Check(!publisher.Resume().ok()&&publisher.Resume().effects==effects&&
            publisher.Advance(operation.operation,Id(62195))==WE::invalid_operation&&
            Host::TryBeginOperation(f.host,Id(62196)).error==HE::writer_failure,
            "retired publisher cannot reuse its proof and the successor retains exclusive admission");
      }
      if(!startup)operation.operation.Reset();
    }
    operation.operation.Reset();
    auto inspection=Host::TryBeginOperation(f.host,Id(62199));Check(inspection.ok(),"retired publication owners release actual next-operation admission");
    Grant reading(512*1024*1024);
    auto selected=inspection.operation.ObserveWithMemory({budget,256*1024*1024},reading.memory,reading.binding);
    Check(selected.ok()&&selected.observation.selection().checkpoint_inventory.inventory.entries.back().identity.transaction_uuid.value==tx.value->value&&
      selected.observation.selection().checkpoint_inventory.inventory.entries.back().state==
        mga::TransactionState::rolling_back&&
      selected.observation.selection().checkpoint_inventory.inventory.entries.back().rollback_only&&
      !selected.observation.selection().checkpoint_inventory.inventory.entries.back().evidence_record_written&&
      !selected.observation.selection().checkpoint_inventory.inventory.entries.back().commit_sequence,
      "independent native read observes the exact original transaction entering rollback without inventing terminal evidence");
    auto history=inspection.operation.ReadHistoryWithMemory({budget,64*1024*1024},reading.memory,reading.binding);
    Check(history.ok(),"independent native startup history retains each actual publication revision");
    const auto& actual=selected.observation.selection().checkpoint_inventory.inventory;
    const page::NativeTransactionInventoryConstView inventory{actual.next_local_transaction_id,actual.next_commit_sequence,actual.entries};
    std::array<byte,16384> scratch;
    allocation_budget=0;
    const auto joined=scratchbird::engine::internal_api::InspectNativeStartupTransactionHistory(history.history,
        *selected.observation.selection().selection,inventory,*final_publication,scratch);
    const auto heap=allocation_budget;allocation_budget=-1;
    Check(joined.ok()&&heap==0&&joined.transactions.size()==std::size_t(startup)&&!joined.pending_binding,
        "actual native history and separately selected inventory join without heap or synthetic recovery state");
    if(startup)Check(joined.transactions[0].binding==*binding&&joined.transactions[0].management_revision==3&&
      joined.transactions[0].inventory_state==mga::TransactionState::rolling_back&&
      joined.transactions[0].outcome==scratchbird::engine::internal_api::NativeStartupInventoryOutcome::unresolved,
      "retained startup intent still requires owning recovery after a real rollback-entry publication");
  }
}

void OwnedHistory() {
  for(unsigned profile=0;profile<5;++profile){
    Fixture f(profile,false,d::RouteStorageAccess::read_write,true);f.Mount();
    auto operation=Host::TryBeginOperation(f.host,Id(61001));Check(operation.ok(),"actual history writer operation");
    const db::NativeManagementHistoryMemoryLimits limits{f.physical.budget,32*1024*1024};
    auto wrong=f.reading.binding;wrong.database_uuid=Id(61999);
    auto refused=operation.operation.ReadHistoryWithMemory(limits,f.reading.memory,wrong);
    Check(!refused.ok()&&refused.writer_error==WE::source_failure&&
        refused.memory_error==db::NativeStorageMemoryError::invalid_binding,
        "history checks the actual binary storage grant before native I/O");
    WatermarkHeapProbe probe(f.paths.front());
    auto history=operation.operation.ReadHistoryWithMemory(limits,f.reading.memory,f.reading.binding);
    probe.Verify();
    Check(history.ok()&&history.operation_uuid==Id(61001)&&history.history.entries.size()==1&&
        history.history.entries.front().record.uuid==f.physical.t.operation.uuid&&
        history.history.entries.front().plan_sha256==Sha(f.physical.t.plan_image)&&
        history.history.latest.size()==1&&history.history.latest.front().entry==0&&
        history.history.latest.front().operation_uuid==f.physical.t.operation.uuid&&
        history.history.selection->checkpoint_sha256==Sha(f.physical.t.target_cp),
        "retained writer reads the complete actual selected publication history and exact indexes");
    history={};
    const db::NativeManagementControlAuthorityMemoryLimits control_limits{f.physical.budget,32*1024*1024};
    auto wrong_control=operation.operation.ReadControlWithMemory(control_limits,f.reading.memory,wrong);
    Check(!wrong_control.ok()&&wrong_control.memory_error==db::NativeStorageMemoryError::invalid_binding,
        "full control verification requires its actual database-bound storage grant");
    WatermarkHeapProbe control_probe(f.paths.front());
    auto control=operation.operation.ReadControlWithMemory(control_limits,f.reading.memory,f.reading.binding);
    control_probe.Verify();
    if(!control.ok())std::cerr<<"owned control error="<<int(control.error)<<" authority="<<int(control.authority_error)<<'\n';
    Check(control.ok()&&control.operation_uuid==Id(61001)&&!control.authority.publications.empty()&&
        !control.authority.allocations.empty()&&control.authority.selection&&
        control.authority.selection->checkpoint_sha256==Sha(f.physical.t.target_cp)&&
        control.physical_bytes_read>0,
        "owned control verification checks actual nonempty mixed-profile allocation ancestry");
    control={};
    const auto route=std::weak_ptr(f.route.lock);f.route.lock.reset();
    callback_fixture=&f;callback_operation=&operation.operation;
    host_allocation_callback=DropOperationDuringRead;
    if(profile%2){
      auto detached=operation.operation.ReadControlWithMemory(control_limits,f.reading.memory,f.reading.binding);
      const auto called=!host_allocation_callback;host_allocation_callback=nullptr;
      Check(called&&detached.ok()&&!operation.operation&&!f.host&&route.expired()&&
          !detached.authority.allocations.empty()&&detached.authority.selection->checkpoint_sha256==Sha(f.physical.t.target_cp),
          "control read retains original cohort through callback resets and immutable backing after close");
    }else{
      auto detached=operation.operation.ReadHistoryWithMemory(limits,f.reading.memory,f.reading.binding);
      const auto called=!host_allocation_callback;host_allocation_callback=nullptr;
      Check(called&&detached.ok()&&!operation.operation&&!f.host&&route.expired()&&
          detached.history.entries.size()==1&&detached.history.entries.front().record.uuid==f.physical.t.operation.uuid,
          "history read pins original cohort through callback resets and retains immutable bytes after close");
    }
    callback_fixture=nullptr;callback_operation=nullptr;
  }
}

Bytes RecoveryRequest(bool startup) {
  Bytes bytes(64);Put(bytes,0,Id(63005));Put(bytes,16,Id(63002));Put(bytes,32,Id(63004));
  Num(bytes,48,8,2);Num(bytes,56,8,19);
  if(startup){db::NativeManagementOperation record;record.uuid=Id(63005);record.database_uuid=Id(1);record.initiator_uuid=Id(63001);
    const auto initial=StartupFixtureState(record);bytes.insert(bytes.end(),initial.begin(),initial.end());}
  return bytes;
}
db::NativePublicationIntent RecoveryIntent(bool startup) {
  db::NativePublicationIntent intent{Id(63001),Id(63002),Id(63003),Sha(RecoveryRequest(startup)),3};
  intent.recovery_profile=2;
  if(startup)intent.startup_binding=db::NativeStartupBinding{Id(63005),Id(63002),Id(63004),2,19};
  return intent;
}
void InventoryRecoveryChild(char** argv) {
  const auto budget=std::stoull(argv[5]);const bool startup=std::string_view(argv[6])=="1";
  const auto fault=std::stoul(argv[7]);
  Grant persistent(64*1024),transient(256*1024*1024),reading(256*1024*1024),publication(2*1024*1024),recovery_memory(512*1024*1024);
  server::DatabaseOwnershipRequest request;request.database_path=argv[2];request.storage_access=d::RouteStorageAccess::read_write;
  auto route=server::AcquireDatabaseOwnership(request);Check(route.acquired,"cold inventory recovery owns original native OS lock");
  auto begun=Host::Begin(route.lock);Check(begun.ok(),"cold inventory recovery enters original route custody");
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  for(unsigned n=3;n<=4;++n){auto file=std::make_unique<d::FileDevice>();
    Check(file->Open(argv[n],d::FileOpenMode::open_existing).ok(),"cold inventory recovery opens exact secondary cohort");secondary.push_back(std::move(file));}
  Check(Host::AdoptWriter(begun.custody,Id(1),Id(2),secondary,{64*1024,{budget,256*1024*1024}},
    persistent.memory,persistent.binding,transient.memory,transient.binding).ok(),"cold inventory recovery adopts complete original cohort");
  auto operation=Host::TryBeginOperation(begun.custody,Id(63099));Check(operation.ok(),"cold inventory recovery has a new custodial operation");
  auto watermark=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
  Check(watermark.ok(),"cold inventory recovery bounded watermark workspace");
  auto observed=watermark.workspace.Inspect(operation.operation,{budget,256*1024*1024},reading.memory,reading.binding);
  Check(observed.ok()&&observed.snapshot->watermark.operation_uuid==Id(63000)&&
    observed.snapshot->watermark.intent==RecoveryIntent(startup),"cold recovery observes the exact persisted original attempt and request");
  const bool already_selected=observed.snapshot->selection.selection_generation==2;
  const auto before=operation.operation.effects();
  db::NativeOwnedInventoryRecovery recovery;
  const db::NativeOwnedInventoryRecoveryLimits limits{{budget,256*1024*1024},{budget,64*1024*1024},
    {budget,256*1024*1024},4*1024*1024};
  auto wrong=RecoveryIntent(startup);wrong.request_context_uuid=Id(63999);
  auto rejected=recovery.Prepare(operation.operation,std::move(watermark.workspace),Id(63000),wrong,limits,recovery_memory.memory,recovery_memory.binding);
  Check(!rejected.ok()&&!rejected.mutation_started&&operation.operation.effects()==before,
    "cold recovery cannot substitute a different request for durable intent");
  rejected=recovery.Prepare(operation.operation,std::move(watermark.workspace),Id(63098),RecoveryIntent(startup),limits,recovery_memory.memory,recovery_memory.binding);
  Check(!rejected.ok()&&!rejected.mutation_started&&operation.operation.effects()==before,
    "recovery operation identity cannot replace the original publication attempt");
  // The held operation must survive even if a callback retires its public facade.
  if(fault==4)watermark_reset_on_read=&operation.operation;
  WatermarkHeapProbe heap(argv[2]);
  const auto prepared=recovery.Prepare(operation.operation,std::move(watermark.workspace),Id(63000),RecoveryIntent(startup),
    limits,recovery_memory.memory,recovery_memory.binding);
  watermark_reset_on_read=nullptr;
  if(!prepared.ok())std::cerr<<"owned recovery prepare="<<int(prepared.error)<<" history="<<int(prepared.history_error)
    <<" control="<<int(prepared.control_error)<<'\n';
  Check(prepared.ok()&&prepared.installed_graph_verified&&!prepared.mutation_started,
    "cold original-custody recovery verifies complete installed history and control graph before selector writes");
  if(fault==4)Check(!operation.operation&&Host::TryBeginOperation(begun.custody,Id(63097)).writer_error==WE::busy,
    "cold recovery retains the original writer after callback retirement of the facade");
  io_counting=true;reads=writes=syncs=0;read_fault=fault==1?1:0;write_fault=fault==2?1:0;sync_fault=fault==3?1:0;
  torn_bytes=fault==2?17:0;
  auto result=recovery.Publish();io_counting=false;read_fault=write_fault=sync_fault=0;torn_bytes=0;
  if(fault>=1&&fault<=3&&!already_selected){Check(!result.ok()&&!result.snapshot&&!result.selected_graph_verified,
      "cold recovery read/write/sync failure cannot publish completion");result=recovery.Resume();}
  if(!result.ok())std::cerr<<"owned recovery publish="<<int(result.error)<<" phase="<<int(result.phase)<<" control="<<int(result.control_error)<<'\n';
  Check(result.ok()&&result.snapshot&&result.installed_graph_verified&&result.selected_graph_verified&&
      result.snapshot->watermark.watermark==2&&result.snapshot->watermark.operation_uuid==Id(63000)&&
      result.snapshot->selection.publication_uuid==Id(63000)&&result.snapshot->selection.selection_generation==2,
    "cold recovery selects only the original anchored inventory with no new publication generation");
  const auto again=recovery.Resume();Check(again.ok()&&again.effects==result.effects,"completed recovery is I/O-idempotent");
  if(already_selected)Check(!result.effects.write_attempts&&!result.mutation_started&&result.effects.successful_syncs>=5,
    "a fresh process observing completed recovery performs no selector rewrite");
  heap.Verify();
}
void OwnedInventoryColdRecovery(const char* executable) {
  for(unsigned profile=0;profile<5;++profile)for(const bool startup:{false,true}){
    Fixture f(profile);f.physical.budget=std::max<u64>(f.physical.budget,64*1024*1024);f.Mount();
    {
      auto operation=Host::TryBeginOperation(f.host,Id(63000));Check(operation.ok(),"original pre-crash inventory operation");
      Grant reading(256*1024*1024),publication(2*1024*1024),publishing(512*1024*1024);
      auto source=operation.operation.ObserveWithMemory({f.physical.budget,256*1024*1024},reading.memory,reading.binding);
      Check(source.ok(),"original pre-crash actual selected inventory");
      const auto& inventory=source.observation.selection().checkpoint_inventory.inventory;
      Check(inventory.next_local_transaction_id==2,"cold recovery fixture has actual next transaction number two");
      std::vector<mga::TransactionInventoryEntry> entries(inventory.entries.begin(),inventory.entries.end());
      mga::TransactionInventoryEntry entry;entry.identity={mga::MakeLocalTransactionId(2),{UuidKind::transaction,Id(63004)},mga::TransactionScope::local_node};
      entry.state=mga::TransactionState::created;entry.begin_unix_epoch_millis=1789357072000ULL;
      entry.begin_visible_through_local_transaction_id=1;entry.begin_visible_through_commit_sequence=inventory.next_commit_sequence-1;entries.push_back(entry);
      const page::NativeTransactionInventoryConstView candidate{3,inventory.next_commit_sequence,entries};
      const auto first_selector=d::DecodeNativeCommonPageHeader(source.observation.selection().slots[0].data(),128);
      Check(first_selector.ok(),"actual first selector offset for interrupted publication");
      const auto stop_offset=first_selector.header->page_number*u64{first_selector.header->page_size_bytes};
      source.observation.Reset();
      auto record=f.physical.t.operation;record.uuid=Id(63005);record.initiator_uuid=Id(63001);record.request_context_uuid=Id(63002);
      record.policy_snapshot_uuid=Id(63003);record.initiator_kind=3;record.normalized_request_bytes=RecoveryRequest(startup);
      record.normalized_request_sha256=Sha(record.normalized_request_bytes);
      if(startup)record.operation_state_bytes=StartupFixtureState(record);
      const auto encoded=db::EncodeNativeManagementOperation(record,1024*1024);Check(encoded.ok(),"cold recovery original operation bytes");
      std::vector<db::NativeManagementStepView> steps(record.steps.size());std::vector<Uuid> ids(record.steps.size()*2+7);
      const auto view=db::DecodeNativeManagementOperationInto(encoded.bytes,1024*1024,{steps,ids});Check(view.ok(),"cold recovery complete original operation view");
      const auto intent=RecoveryIntent(startup);
      scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),record.policy_snapshot_uuid},{{},0,1000});
      auto watermark=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
      Check(watermark.ok(),"original pre-crash watermark workspace");
      const auto observed=watermark.workspace.Inspect(operation.operation,{f.physical.budget,256*1024*1024},reading.memory,reading.binding,intent.startup_binding);
      Check(observed.ok()&&watermark.workspace.Reserve(operation.operation,*observed.snapshot,intent).ok(),"original exact intent durably reserved");
      db::NativeOwnedInventoryPublication publisher;
      const db::NativeOwnedInventoryPublicationLimits limits{{f.physical.budget,256*1024*1024},{f.physical.budget,64*1024*1024},
        {f.physical.budget,256*1024*1024},64*1024*1024,4*1024*1024,64*1024*1024,f.physical.budget};
      Check(publisher.Prepare(operation.operation,std::move(watermark.workspace),*view.record,candidate,issuer,limits,
        publishing.memory,publishing.binding).ok(),"original actual graph prepared before simulated process retirement");
      recovery_stop_offset=stop_offset;recovery_stop_hit=false;
      const auto stopped=publisher.Publish();recovery_stop_offset=-1;
      Check(recovery_stop_hit&&!stopped.ok()&&stopped.installed_graph_verified&&!stopped.selected_graph_verified&&
        stopped.phase==db::NativeOwnedInventoryPublicationPhase::selector_write,
        "interrupt after the original graph is durable and verified, before any selector byte changes");
    }
    std::array<db::NativeWriterMemberCloseReceipt,3> receipts;
    Check(Host::RetireWriter(f.host,receipts).ok()&&Host::RetireWriterBacking(f.host)==HE::none,
      "interrupted publisher and all original devices retire before an independent recovery process");
    f.host.reset();f.route.lock.reset();
    const auto allowance=std::to_string(f.physical.budget);const auto mode=startup?"1":"0";
    const auto run=[&](unsigned fault){const auto fault_text=std::to_string(fault);const auto child=::fork();Check(child>=0,"cold inventory recovery subprocess");
      if(!child){::execl(executable,executable,"--inventory-recover",f.paths[0].c_str(),f.paths[1].c_str(),f.paths[2].c_str(),
          allowance.c_str(),mode,fault_text.c_str(),nullptr);::_exit(79);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
        "fresh process completes the original native inventory publication and retains all recovery obligations");};
    run(profile);run(0);
  }
}

// Actual full-cohort physical publication, including uncertain partial effects
// and exact in-process continuation. This is not an authenticated BEGIN test.
void OwnedWatermark(unsigned profile,unsigned fault=0,unsigned site=0,bool startup=false,bool reset_on_read=false,bool readonly_member=false){
  using E=db::NativePublicationError;Fixture f(profile,readonly_member);f.Mount();
  std::optional<db::NativePublicationSnapshot> completed;
  {
    Grant publication(2*1024*1024);
    auto preparation=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
    Check(preparation.ok(),"publication backing admitted before actual selected guards");
    auto& workspace=preparation.workspace;
    auto operation=Host::TryBeginOperation(f.host,Id(62000));Check(operation.ok(),"original owned publication reservation");
    std::optional<db::NativeStartupBinding> binding;
    if(startup){
      auto selected=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
      Check(selected.ok(),"actual inventory for physical startup-tuple test");
      const auto next=selected.lease.source().selection().checkpoint_inventory.inventory.next_local_transaction_id;
      binding=db::NativeStartupBinding{Id(62010),Id(62011),Id(62012),next,9};
    }
    WatermarkHeapProbe heap(f.paths.front());
    const auto observed=workspace.Inspect(operation.operation,{f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding,binding);
    Check(observed.ok()&&observed.snapshot->watermark.watermark==1,"actual selected checkpoint/allocated watermark observation");
    auto stale=*observed.snapshot;stale.state_sha256[0]^=1;
    Check(workspace.Reserve(operation.operation,stale,WatermarkIntent()).error==E::stale_base,"stale expected source refuses before publication");
    auto intent=WatermarkIntent();intent.startup_binding=binding;
    auto wrong=intent;wrong.startup_binding=binding?std::nullopt:std::optional{db::NativeStartupBinding{Id(62010),Id(62011),Id(62012),2,9}};
    Check(workspace.Reserve(operation.operation,*observed.snapshot,wrong).error==E::request_mismatch,
        "only exact startup tuple verified against retained inventory may reach physical reservation");
    io_counting=true;reads=writes=syncs=0;
    if(fault==1){write_fault=site;torn_bytes=7;}if(fault==2)sync_fault=site;if(fault==3)read_fault=site;
    if(reset_on_read)watermark_reset_on_read=&operation.operation;
    auto reserved=workspace.Reserve(operation.operation,*observed.snapshot,intent);
    io_counting=false;read_fault=write_fault=sync_fault=0;torn_bytes=0;
    if(fault){
      Check(!reserved.ok()&&!reserved.snapshot&&reserved.io&&reserved.error==E::io_failure,
          "actual physical failure withholds reservation completion and retains native receipt");
      Check(workspace.Reserve(operation.operation,*observed.snapshot,intent).error!=E::none,
          "interrupted attempt cannot be silently rebuilt as a fresh reservation");
      auto resumed=workspace.Resume(operation.operation);
      Check(resumed.ok()&&resumed.snapshot->watermark.operation_uuid==Id(62000)&&resumed.snapshot->watermark.watermark==2,
          "same retained original operation resumes exact images without another generation");
      reserved=std::move(resumed);
    }
    Check(reserved.ok()&&reserved.snapshot->watermark.watermark==2&&reserved.snapshot->watermark.intent==intent&&
        reserved.snapshot->selection.checkpoint_generation==1,"both synchronized replicas reserve one generation without selecting a checkpoint");
    completed=reserved.snapshot;
    if(reset_on_read)Check(!watermark_reset_on_read&&!operation.operation&&reserved.effects.write_attempts==2&&reserved.effects.successful_syncs==5,
        "actual I/O callback Reset cannot retire original internal publication operation or lose its physical effects");
    else {
      const auto before=operation.operation.effects();
      Check(workspace.Resume(operation.operation).ok()&&operation.operation.effects()==before,"completed exact continuation is idempotent without new I/O");
      Check(workspace.Reserve(operation.operation,*observed.snapshot,intent).error==E::operation_pending,"same memory cannot displace completed pending intent");
    }
    heap.Verify();
    // UUID reuse cannot replace an actual reservation. Full host/OS custody
    // survives the outer handle and every external host reference being reset.
    const auto route=std::weak_ptr(f.route.lock);const auto host=std::weak_ptr(f.host);
    operation.operation.Reset();
    Check(Host::TryBeginOperation(f.host,Id(62000)).writer_error==WE::busy,"continuity pins actual operation after outer Reset, including same UUID");
    Check(workspace.Resume(operation.operation).error==E::request_mismatch,"empty outer operation cannot consume retained publication state");
    f.route.lock.reset();f.host.reset();Check(!host.expired()&&!route.expired(),"prepared publication retains full original host custody after outer owners reset");
  }
  // Independently reopen the physical cohort after its original host releases.
  for(usize n=0;n<f.paths.size();++n)Check(f.physical.t.fixture.devices[n].device->Open(f.paths[n],d::FileOpenMode::open_existing).ok(),"fresh device reopen after native owner retirement");
  const auto cold=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.physical.t.fixture.devices,Id(2),f.physical.budget);
  Check(cold.ok()&&cold.snapshot->state_sha256==completed->state_sha256&&cold.snapshot->watermark.intent==completed->watermark.intent&&
      cold.snapshot->selection.checkpoint_sha256==completed->selection.checkpoint_sha256,"independent existing native reader validates durable exact counter and unchanged selected inventory/checkpoint");
}

void OwnedWatermarkChecks(){
  for(unsigned profile=0;profile<5;++profile){OwnedWatermark(profile);OwnedWatermark(profile,0,0,true);}
  for(unsigned site=1;site<=2;++site)OwnedWatermark(0,1,site);
  for(unsigned site=1;site<=5;++site)OwnedWatermark(0,2,site);
  for(unsigned site=1;site<=4;++site)OwnedWatermark(0,3,site);
  OwnedWatermark(0,0,0,false,true);
  OwnedWatermark(4,0,0,false,false,true);
}

void WatermarkChild(char** argv,bool crash){
  Grant persistent(64*1024),transient(32*1024*1024),reading(32*1024*1024),publication(2*1024*1024);
  server::DatabaseOwnershipRequest request;request.database_path=argv[2];request.storage_access=d::RouteStorageAccess::read_write;
  auto route=server::AcquireDatabaseOwnership(request);Check(route.acquired,"fresh process acquires original native OS custody");
  auto begun=Host::Begin(route.lock);Check(begun.ok(),"fresh process creates native custody");
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  for(unsigned n=3;n<=4;++n){auto file=std::make_unique<d::FileDevice>();
    Check(file->Open(argv[n],d::FileOpenMode::open_existing).ok(),"fresh process opens exact supplied secondary member");secondary.push_back(std::move(file));}
  const auto allowance=std::stoull(argv[5]);
  Check(Host::AdoptWriter(begun.custody,Id(1),Id(2),secondary,{64*1024,{allowance,32*1024*1024}},
      persistent.memory,persistent.binding,transient.memory,transient.binding).ok(),"fresh process verifies complete native cohort");
  auto preparation=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
  Check(preparation.ok(),"fresh process admits publication backing");
  auto operation=Host::TryBeginOperation(begun.custody,Id(crash?62000:62030));Check(operation.ok(),"fresh process actual exclusive operation");
  auto observed=preparation.workspace.Inspect(operation.operation,{allowance,32*1024*1024},reading.memory,reading.binding);
  Check(observed.ok(),"fresh process independently reads selected native watermark pair");
  if(!crash){Check(observed.snapshot->watermark.watermark==2&&observed.snapshot->watermark.operation_uuid==Id(62000)&&
      observed.snapshot->watermark.intent==WatermarkIntent()&&observed.snapshot->selection.checkpoint_generation==1,
      "fresh process cold restart validates original durable pending request without selecting inventory");return;}
  io_counting=true;reads=writes=syncs=0;kill_write=2;
  (void)preparation.workspace.Reserve(operation.operation,*observed.snapshot,WatermarkIntent());
  ::_exit(79);
}

void OwnedWatermarkColdRecovery(const char* executable){
  Fixture f;
  // The crashing child starts after exec, with no inherited native host or
  // parent-held device/route ownership. Actual fixture files survive its exit.
  for(auto& file:f.secondary)Check(file->Close().ok(),"retire parent secondary before fresh native host");
  f.secondary.clear();f.route.lock.reset();
  const auto allowance=std::to_string(f.physical.budget);
  const auto run=[&](const char* mode,int expected){const auto child=::fork();Check(child>=0,"fresh native publication subprocess");
    if(!child){::execl(executable,executable,mode,f.paths[0].c_str(),f.paths[1].c_str(),f.paths[2].c_str(),allowance.c_str(),nullptr);::_exit(79);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==expected,
        "fresh native host exits at the exact requested publication/restart boundary");};
  run("--watermark-crash",86);
  server::DatabaseOwnershipRequest request;request.database_path=f.paths.front();request.storage_access=d::RouteStorageAccess::read_write;
  f.route=server::AcquireDatabaseOwnership(request);Check(f.route.acquired,"OS releases dead native host ownership");
  for(usize n=1;n<f.paths.size();++n){auto file=std::make_unique<d::FileDevice>();
    Check(file->Open(f.paths[n],d::FileOpenMode::open_existing).ok(),"original secondary cohort after process death");f.secondary.push_back(std::move(file));}
  f.Mount();
  {
    Grant publication(2*1024*1024);
    auto preparation=db::PrepareNativeOwnedPublicationWatermark(d::kCanonicalFilespacePageProfiles[4].uuid,3,publication.memory,publication.binding);
    Check(preparation.ok(),"new recovery owner admits exact native publication backing");
    auto operation=Host::TryBeginOperation(f.host,Id(62020));Check(operation.ok(),"new recovery operation owns original physical cohort");
    WatermarkHeapProbe heap(f.paths.front());
    const auto pending=preparation.workspace.Inspect(operation.operation,{f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
    Check(!pending.ok()&&!pending.snapshot&&pending.error==db::NativePublicationError::repair_required,
        "ordinary observation cannot silently repair ordered partially published replicas");
    io_counting=true;reads=writes=syncs=0;
    const auto recovered=preparation.workspace.Recover(operation.operation,{f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
    io_counting=false;
    Check(recovered.ok()&&writes==1&&recovered.snapshot->watermark.watermark==2&&recovered.snapshot->watermark.operation_uuid==Id(62000)&&
        recovered.snapshot->watermark.intent==WatermarkIntent(),"explicit recovery repairs only stale replica and preserves original consumed generation/intent");
    heap.Verify();
  }
  f.host.reset();f.route.lock.reset();
  run("--watermark-cold-verify",0);
}
}
int main(int argc, char** argv) {
  using namespace hosted_custody_test;
  try {
    Artifacts artifacts;
    if (argc == 3 && std::string_view(argv[1]) == "--thread-death") {
      const auto mode = std::stoul(argv[2]); Check(mode < 8, "host thread-affinity mode"); ThreadMisuse(mode);
    } else if(argc==6&&(std::string_view(argv[1])=="--watermark-crash"||std::string_view(argv[1])=="--watermark-cold-verify")){
      WatermarkChild(argv,std::string_view(argv[1])=="--watermark-crash");
    } else if(argc==8&&std::string_view(argv[1])=="--inventory-recover"){
      InventoryRecoveryChild(argv);
    } else {
      Check(argc == 1, "unknown host custody test mode"); ThreadControls(argv[0]);
      InitialCustody(); for (unsigned p = 0; p < 5; ++p) CompletePins(p);
      CloseObligations(); CallbackPins(); ProcessAndThreadChecks(); OwnedObservation();OwnedInventoryGraph();OwnedInventoryActivation();OwnedHistory();OwnedWatermarkChecks();OwnedWatermarkColdRecovery(argv[0]);OwnedInventoryColdRecovery(argv[0]);
    }
    artifacts.Cleanup();
    std::cout << "native hosted database custody PASS checks=" << checks << " physical_custody_only=true\n"; return 0;
  } catch (const std::exception& error) {
    allocation_budget = -1; read_fault = write_fault = sync_fault = 0; host_allocation_callback = nullptr;
    std::cerr << "native hosted database custody FAIL " << error.what() << " checks=" << checks << '\n'; return 1;
  }
}
