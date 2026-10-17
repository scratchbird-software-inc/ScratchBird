// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual three-member/mixed-profile selected native graph and fault oracles.
#define main UnusedNativeWriterFixtureMain
#include "native_management_control_bundle_test.cpp"
#undef main
#include "native_owned_checkpoint_writer.hpp"
#include "../../src/server/database_ownership.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "../support/owned_temp_directory.hpp"

namespace {bool writer_late_close_fault=false;int writer_reused_fd=-1,writer_retired_fd=-1;}
namespace {void (*writer_allocation_callback)()=nullptr;}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t bytes){
  if(auto callback=writer_allocation_callback){writer_allocation_callback=nullptr;callback();}
  return __real_malloc(bytes);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd){
  if(!writer_late_close_fault)return __real_close(fd);
  writer_late_close_fault=false;const int status=__real_close(fd);if(status)return status;
  writer_retired_fd=fd;writer_reused_fd=::open("/dev/null",O_RDONLY|O_CLOEXEC);
  errno=EINTR;return -1;
}
namespace {
namespace server=scratchbird::server;
using Writer=db::NativeOwnedCheckpointWriter;
using WE=db::NativeWriterError;
using Grant=checkpoint_inventory_memory::Grant;
template<class T>concept ExposesDevices=requires(T value){value.devices();};
template<class T>concept ExposesSelectedHandle=requires(T value){value.selected;};
static_assert(!ExposesDevices<Writer>&&!ExposesDevices<Writer::Operation>&&!ExposesDevices<Writer::ReadLease>);
static_assert(!ExposesSelectedHandle<Writer::ReadLease>);
static_assert(!std::is_default_constructible_v<Writer>&&!std::is_copy_constructible_v<Writer::Operation>);
struct ParentOwnedArtifacts {
  scratchbird::tests::OwnedTempDirectory directory;
  std::optional<std::string> previous;
  ParentOwnedArtifacts(){if(const auto* p=std::getenv("TMPDIR"))previous=p;
    Check(!::setenv("TMPDIR",directory.path().c_str(),1),"private parent-owned writer test temporary namespace");}
  ~ParentOwnedArtifacts(){Restore();}
  void Restore()noexcept{if(previous)(void)::setenv("TMPDIR",previous->c_str(),1);else(void)::unsetenv("TMPDIR");}
  void Cleanup(){Restore();directory.Cleanup();}
};

struct WriterFixture {
  DirectoryHistoryFixture physical;
  Grant persistent{64*1024},transient{32*1024*1024},reading{32*1024*1024};
  std::vector<std::string> paths;
  std::vector<Uuid> identities,profiles;
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  server::DatabaseOwnershipResult route;
  d::RouteSourceTransitionResult transition;
  std::shared_ptr<Writer> writer;
  explicit WriterFixture(unsigned profile=0,unsigned secondary_profile=5):physical(profile,secondary_profile<5?secondary_profile:(profile+1)%5,false,2,false,false,true,false){
    for(const auto& file:physical.t.fixture.devices){paths.push_back(file.device->path());identities.push_back(file.filespace_uuid);profiles.push_back(file.page_size_profile_uuid);
      Check(file.device->Close().ok(),"close initial independent fixture members before route custody");}
    Start();
  }
  ~WriterFixture(){writer.reset();secondary.clear();transition.transition.reset();route.lock.reset();}
  void Start(d::RouteStorageAccess access=d::RouteStorageAccess::read_write,bool readonly_member=false,
      std::shared_ptr<d::RouteOwnershipLease>* legacy=nullptr){
    writer.reset();secondary.clear();transition.transition.reset();route.lock.reset();
    server::DatabaseOwnershipRequest request;request.database_path=paths.front();request.storage_access=access;
    route=server::AcquireDatabaseOwnership(request);Check(route.acquired&&route.lock,"actual initial route/storage/data ownership");
    if(legacy){*legacy=d::RouteOwnershipLease::Borrow(paths.front()+".sb.route.owner.lock");Check(bool(*legacy),"actual legacy admission before native fence");}
    for(std::size_t n=1;n<paths.size();++n){auto file=std::make_unique<d::FileDevice>();
      Check(file->Open(paths[n],readonly_member&&n+1==paths.size()?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),
          "actual independently owned cohort member with its requested unchanged OS capability");secondary.push_back(std::move(file));}
    transition=route.lock->BeginNativeSourceTransition();Check(transition.ok(),"actual single-consumption transition fences legacy admission");
  }
  db::NativeOwnedWriterResult Adopt(Uuid database=Id(1),const db::NativeStorageMemoryBinding* supplied=nullptr){
    return Writer::AdoptRouteWithMemory(transition.transition,database,Id(2),secondary,
        {64*1024,{physical.budget,32*1024*1024}},persistent.memory,supplied?*supplied:persistent.binding,transient.memory,transient.binding);
  }
  void Accept(){auto adopted=Adopt();if(!adopted.ok())std::cerr<<"writer adoption="<<int(adopted.error)<<" route="<<int(adopted.route_error)
      <<" source="<<int(adopted.memory_source_error)<<" checkpoint="<<int(adopted.checkpoint_error)<<'\n';
    Check(adopted.ok()&&secondary.empty()&&!persistent.memory.Matches(persistent.binding),"actual full cohort and persistent grant consumed exactly once");writer=std::move(adopted.writer);}
  u64 ScratchPage()const {const auto& maps=physical.t.before.at(Id(2));
    for(const auto& map:maps)for(std::size_t n=map.states.size();n--;)if(map.states[n]==page::NativeAllocationState::free)return map.first_page+n;
    throw std::runtime_error("actual native fixture has no free physical scratch page");}
};
void FailedAdoption(WriterFixture& f,const db::NativeOwnedWriterResult& result){
  Check(!result.ok()&&!result.writer&&f.secondary.size()+1==f.paths.size()&&f.persistent.memory.Matches(f.persistent.binding),
      "failed writer adoption leaves every original member/grant owned and publishes no writer");
  for(const auto& file:f.secondary)Check(file&&file->is_open(),"failed writer adoption preserves independent physical handles");
}
void InitialRefusals(){
  WriterFixture f;
  std::shared_ptr<d::RouteOwnershipLease> legacy;f.Start(d::RouteStorageAccess::read_write,false,&legacy);
  reads=0;io_counting=true;auto undrained=f.Adopt();io_counting=false;FailedAdoption(f,undrained);
  Check(undrained.route_error==d::RouteSourceTransitionError::not_drained&&!reads,
      "actual counted borrower must drain before writable transfer or native reads");legacy.reset();
  for(unsigned field=0;field<4;++field){auto bad=f.persistent.binding;const std::array<Uuid*,4> fields{&bad.database_uuid,&bad.operation_uuid,&bad.owner_uuid,&bad.context_uuid};
    *fields[field]=Id(61000);reads=0;io_counting=true;auto result=f.Adopt(Id(1),&bad);io_counting=false;
    FailedAdoption(f,result);Check(result.error==db::NativeOwnedSourceError::memory_failure&&!reads,"every actual binary grant dimension checked before native I/O");}
  FailedAdoption(f,f.Adopt(Id(61000)));
  {auto missing=std::move(f.secondary.back());f.secondary.pop_back();auto result=f.Adopt();
    Check(result.error==db::NativeOwnedSourceError::incomplete_cohort&&!result.writer&&f.secondary.size()==1,
        "valid but incomplete cohort cannot acquire writer ownership");f.secondary.push_back(std::move(missing));}
  {auto duplicate=std::make_unique<d::FileDevice>();const auto path=(f.physical.t.fixture.path.parent_path()/"duplicate-member").string();
    const auto size=f.secondary.front()->Size();Check(size.ok(),"duplicate fixture source physical size");Bytes raw(size.size_bytes);
    Check(f.secondary.front()->ReadAt(0,raw.data(),raw.size()).ok()&&duplicate->Open(path,d::FileOpenMode::create_new).ok()&&
        duplicate->WriteAt(0,raw.data(),raw.size()).ok()&&duplicate->Sync().ok(),"actual separate inode with duplicate native member identity");
    f.secondary.push_back(std::move(duplicate));auto result=f.Adopt();
    Check(!result.ok()&&!result.writer&&f.secondary.size()==3&&f.persistent.memory.Matches(f.persistent.binding),"duplicate member cannot become a writer cohort");f.secondary.pop_back();}
  {byte original=0;Check(f.secondary.front()->ReadAt(0,&original,1).ok(),"actual secondary bootstrap before corruption");
    const byte wrong=original^1;Check(f.secondary.front()->WriteAt(0,&wrong,1).ok()&&f.secondary.front()->Sync().ok(),"actual malformed member bootstrap");
    auto result=f.Adopt();FailedAdoption(f,result);Check(result.error==db::NativeOwnedSourceError::bootstrap_failure,"invalid member bootstrap not silently replaced by caller identity");
    Check(f.secondary.front()->WriteAt(0,&original,1).ok()&&f.secondary.front()->Sync().ok(),"restore actual bootstrap before next admission");}
  db::NativeOwnedSourceError foreign=db::NativeOwnedSourceError::none;
  std::thread other([&]{foreign=f.Adopt().error;});other.join();
  Check(foreign==db::NativeOwnedSourceError::device_ownership,"foreign opening-thread cohort refuses before storage/governor access");
  // Final owner revalidation is distinct from initial drain observation. The
  // helper withdraws while the admitting thread is paused at an actual pread.
  historical_read_pause=1;bool actually_paused=false;
  std::thread withdrawer([&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(historical_read_pause.load()!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    actually_paused=historical_read_pause.load()==2;f.route.lock->release();historical_read_pause=3;});
  auto withdrawn=f.Adopt();withdrawer.join();historical_read_pause=0;FailedAdoption(f,withdrawn);
  Check(actually_paused&&withdrawn.route_error==d::RouteSourceTransitionError::withdrawn,
      "withdrawal during actual selected-source verification prevents final physical transfer");
  f.Start(d::RouteStorageAccess::read_only);reads=0;io_counting=true;auto readonly=f.Adopt();io_counting=false;
  FailedAdoption(f,readonly);Check(readonly.route_error==d::RouteSourceTransitionError::writable_not_requested&&!reads,
      "initial readonly physical capability cannot become writable by source selection");
  f.Start();
  {struct rlimit original{};Check(!::getrlimit(RLIMIT_NOFILE,&original),"observe actual descriptor limit");
    auto exhausted=original;exhausted.rlim_cur=0;Check(!::setrlimit(RLIMIT_NOFILE,&exhausted),"bounded real duplication failure");
    auto failure=f.Adopt();const auto restored=::setrlimit(RLIMIT_NOFILE,&original);
    Check(!restored,"restore descriptor limit before diagnostic/test work");FailedAdoption(f,failure);
    Check(failure.route_error==d::RouteSourceTransitionError::io_failure,"actual retained writable descriptor duplication failure preserves source inputs");}
  f.Start();auto read=db::NativeOwnedCheckpointSource::AdoptRouteWithMemory(f.transition.transition,Id(1),Id(2),f.secondary,
      {64*1024,{f.physical.budget,32*1024*1024}},f.persistent.memory,f.persistent.binding,f.transient.memory,f.transient.binding);
  Check(read.ok(),"actual read source single-consumption under initially RW route");
  auto upgrade=f.Adopt();Check(!upgrade.ok()&&!upgrade.writer&&upgrade.route_error==d::RouteSourceTransitionError::already_transferred,
      "committed read source cannot be upgraded even when original route retained writable capability");read.owner.reset();f.persistent.Empty();
}
void PhysicalOperations(unsigned profile,unsigned secondary_profile){
  WriterFixture f(profile,secondary_profile);f.Start(d::RouteStorageAccess::read_write,true);
  const auto scratch=f.ScratchPage()*f.physical.t.fixture.size;
  f.Accept();const auto second=f.Adopt();Check(!second.ok()&&second.route_error==d::RouteSourceTransitionError::already_transferred,"writer transfer consumes exact transition once");
  auto begun=Writer::TryBeginOperation(f.writer,Id(61001));Check(begun.ok(),"actual atomic writer operation reservation");
  Check(Writer::TryBeginOperation(f.writer,Id(61002)).error==WE::busy,"concurrent/reentrant operation cannot reconstruct physical custody");
  std::array<byte,32> payload{},actual{};for(unsigned n=0;n<payload.size();++n)payload[n]=n+17;
  auto selected=begun.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  Check(selected.ok()&&selected.lease.source().directory().directory.pages.front().directory.total_records==3,
      "restricted source reads exact complete retained native cohort");
  const auto no_heap=[&](auto&& call){allocation_budget=0;auto result=call();const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0,"live-read refusal uses no memory/observation callback under device guards");return result;};
  Check(no_heap([&]{return begun.operation.WriteAtBounded(Id(2),f.profiles.front(),scratch,payload);}).error==WE::read_handle_live&&
      no_heap([&]{return begun.operation.ReadAtBounded(Id(2),f.profiles.front(),scratch,actual);}).error==WE::read_handle_live&&
      no_heap([&]{return begun.operation.Sync(Id(2),f.profiles.front());}).error==WE::read_handle_live,
      "every bounded physical primitive refuses while restricted selected source remains live");
  allocation_budget=0;auto competing=Writer::TryBeginOperation(f.writer,Id(61002));const auto left=allocation_budget;allocation_budget=-1;
  Check(left==0&&competing.error==WE::busy,"busy operation refuses before control allocation under selected source guard");
  Check(begun.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding).writer_error==WE::read_handle_live,
      "second selected handle cannot recursively reenter native guards");
  selected.lease.Reset();Check(Writer::TryBeginOperation(f.writer,Id(61002)).error==WE::busy,"outer operation survives actual selected handle Reset");
  std::array<byte,1> readonly_byte{};
  Check(begun.operation.ReadAtBounded(f.identities.back(),f.profiles.back(),0,readonly_byte).ok()&&readonly_byte[0]=='S',
      "read-only member remains present and genuinely readable in complete writable-node cohort");
  const auto prior=begun.operation.effects();
  const auto readonly=begun.operation.WriteAtBounded(f.identities.back(),f.profiles.back(),0,payload);
  const auto readonly_sync=begun.operation.Sync(f.identities.back(),f.profiles.back());
  Check(readonly.error==WE::read_only_member&&!readonly.io&&!readonly.size_observation&&readonly.effects==prior&&
      readonly_sync.error==WE::read_only_member&&!readonly_sync.io&&readonly_sync.effects==prior,
      "primary writable custody never upgrades a read-only secondary or certifies a no-op sync");
  auto bad=begun.operation.WriteAtBounded(Id(61010),f.profiles.front(),scratch,payload);Check(bad.error==WE::filespace_absent&&!bad.io,"foreign filespace has no native write attempt");
  bad=begun.operation.WriteAtBounded(Id(2),Id(61010),scratch,payload);Check(bad.error==WE::profile_mismatch&&!bad.io,"actual bound page profile cannot be caller-replaced");
  bad=begun.operation.WriteAtBounded(Id(2),f.profiles.front(),UINT64_MAX,payload);Check(bad.error==WE::extent_invalid&&!bad.io,"overflow refused before physical access");
  bad=begun.operation.WriteAtBounded(Id(2),f.profiles.front(),UINT64_C(1)<<40,payload);
  Check(bad.error==WE::extent_outside_file&&bad.size_observation&&bad.size_observation->ok()&&!bad.io,"bounded writes cannot extend or allocate filespace storage");
  const auto primary=f.paths.front(),retained=primary+".retained";std::filesystem::rename(primary,retained);
  const int replacement=::open(primary.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);const byte unrelated=0xda;
  Check(replacement>=0&&::write(replacement,&unrelated,1)==1&&::close(replacement)==0,"replace diagnostic pathname with unrelated inode");
  auto write=begun.operation.WriteAtBounded(Id(2),f.profiles.front(),scratch,payload);
  Check(write.ok()&&write.io->native_attempted&&write.io->bytes_transferred==payload.size()&&write.effects.write_attempts==1&&write.effects.write_bytes==payload.size(),
      "actual initially retained writable inode accepts exact physical bytes, not acceptance-only success");
  const auto sync=begun.operation.Sync(Id(2),f.profiles.front());Check(sync.ok()&&sync.io->native_attempted&&sync.io->synchronized,"actual native synchronization receipt, not MGA durable completion");
  Check(begun.operation.ReadAtBounded(Id(2),f.profiles.front(),scratch,actual).ok()&&actual==payload,"same-inode physical readback under actual retained owner");
  // Real partial native effects survive failure; no refusal-as-success or
  // silent replacement of the operation identity after an uncertain write.
  auto changed=payload;changed.fill(0xa5);io_counting=true;reads=writes=syncs=0;write_fault=1;torn_bytes=7;
  auto partial=begun.operation.WriteAtBounded(Id(2),f.profiles.front(),scratch,changed);io_counting=false;write_fault=0;torn_bytes=0;
  Check(!partial.ok()&&partial.error==WE::io_failure&&partial.io&&partial.io->native_attempted&&partial.effects.uncertain_write&&
      partial.operation_uuid==Id(61001)&&partial.effects.write_attempts==2,"partial failed write retains actual native cause/cumulative uncertain effects and operation identity");
  Check(begun.operation.ReadAtBounded(Id(2),f.profiles.front(),scratch,actual).ok()&&
      std::equal(actual.begin(),actual.begin()+7,changed.begin())&&std::equal(actual.begin()+7,actual.end(),payload.begin()+7),
      "independent physical read proves retained partial bytes despite failed write receipt");
  io_counting=true;syncs=0;sync_fault=1;const auto failed_sync=begun.operation.Sync(Id(2),f.profiles.front());io_counting=false;sync_fault=0;
  Check(!failed_sync.ok()&&failed_sync.io&&failed_sync.io->native_attempted&&!failed_sync.io->synchronized&&failed_sync.io->native_error==EIO&&
      failed_sync.effects.uncertain_write,"failed sync cannot erase preceding real effects");
  const auto accepted=begun.operation.effects();f.writer->Withdraw();f.route.lock->release();
  Check(Writer::TryBeginOperation(f.writer,Id(61002)).error==WE::withdrawn,"actual owner withdrawal closes new physical admission");
  auto weak=std::weak_ptr(f.writer);f.writer.reset();
  Check(!weak.expired()&&begun.operation.WriteAtBounded(Id(2),f.profiles.front(),scratch,payload).ok()&&
      begun.operation.Sync(Id(2),f.profiles.front()).ok()&&begun.operation.effects().write_attempts==accepted.write_attempts+1,
      "accepted operation retains real owner and effects after external owner withdrawal");
  begun.operation.Reset();Check(weak.expired(),"last actual operation releases source/route custody after guards");f.transition.transition.reset();f.route.lock.reset();
  d::FileDevice reopened;Check(reopened.Open(retained,d::FileOpenMode::open_existing_read_only).ok(),"independent reopen of original retained inode");
  Check(reopened.ReadAt(scratch,actual.data(),actual.size()).ok()&&actual==payload&&reopened.Close().ok(),"independent reopen proves physical write persistence");
  const int untouched=::open(primary.c_str(),O_RDONLY|O_CLOEXEC);byte value=0;struct stat state{};
  Check(untouched>=0&&::fstat(untouched,&state)==0&&state.st_size==1&&::pread(untouched,&value,1,0)==1&&value==unrelated&&::close(untouched)==0,
      "mutable diagnostic path never redirected retained native I/O");
  std::filesystem::remove(primary);std::filesystem::rename(retained,primary);
  f.persistent.Empty();f.transient.memory={};f.reading.memory={};f.transient.Empty();f.reading.Empty();
}
void ReaderRetainsReservation(){WriterFixture f;f.Accept();auto op=Writer::TryBeginOperation(f.writer,Id(61001));Check(op.ok(),"actual reader-lifetime operation");
  auto reader=op.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);Check(reader.ok(),"actual restricted reader");
  op.operation.Reset();allocation_budget=0;auto busy=Writer::TryBeginOperation(f.writer,Id(61002));const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0&&busy.error==WE::busy,"dropping external operation cannot release reservation behind a retained read guard");
  auto weak=std::weak_ptr(f.writer);f.writer->Withdraw();f.writer.reset();Check(!weak.expired(),"reader retains actual writer and source owner");
  reader.lease.Reset();Check(weak.expired(),"last reader resets native guards before retiring actual operation/custody");f.persistent.Empty();
}
void OperationAllocationFailure(){WriterFixture f;f.Accept();allocation_budget=0;
  auto failed=Writer::TryBeginOperation(f.writer,Id(61001));const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining<0&&failed.error==WE::resource_exhausted&&!failed.operation,
      "actual operation control allocation failure publishes no reservation");
  auto retry=Writer::TryBeginOperation(f.writer,Id(61001));Check(retry.ok(),"failed control allocation leaves genuine next operation admissible");
  retry.operation.Reset();f.route.lock->release();auto revoked=Writer::TryBeginOperation(f.writer,Id(61002));
  Check(revoked.error==WE::withdrawn&&revoked.route_error==d::RouteSourceTransitionError::withdrawn,
      "actual route withdrawal independently closes new operations without a writer-local flag");
}
void ReaderFailureRetry(){WriterFixture f;f.Accept();auto operation=Writer::TryBeginOperation(f.writer,Id(61001));
  Check(operation.ok(),"actual operation retained across selected-reader preparation failures");
  const auto baseline=f.reading.memory.Snapshot();
  const auto preserved=[&]{const auto current=f.reading.memory.Snapshot();
    Check(f.reading.memory.CheckBinding(f.reading.binding)==db::NativeStorageMemoryError::none&&current.active&&
        current.reserved_bytes==baseline.reserved_bytes&&current.allocated_bytes==baseline.allocated_bytes,
        "failed selected read preserves exact admitted grant and releases every attempted backing allocation");
    Check(operation.operation&&operation.operation.identity()==Id(61001)&&Writer::TryBeginOperation(f.writer,Id(61002)).error==WE::busy,
        "failed selected read never releases or replaces the outer actual operation reservation");
    std::array<byte,1> value{};Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,value).ok()&&value[0]=='S',
        "reader preparation failure releases its flags and actual device guards before bounded reuse");
  };
  allocation_budget=0;auto allocation=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining<0&&!allocation.ok()&&!allocation.lease&&allocation.writer_error==WE::resource_exhausted&&
      allocation.error==db::NativeOwnedSourceError::resource_exhausted,
      "first actual ReaderState allocation failure publishes no source and preserves its typed cause");preserved();
  reads=0;read_fault=1;io_counting=true;
  auto failed=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  io_counting=false;read_fault=0;
  Check(reads==1&&!failed.ok()&&!failed.lease&&failed.writer_error==WE::source_failure&&
      failed.error==db::NativeOwnedSourceError::source_failure&&!failed.io_status.ok(),
      "actual selected-source pread failure after reservation retains exact failed I/O status, no usable reader");preserved();
  auto retried=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  Check(retried.ok()&&retried.lease.source().directory().directory.pages.front().directory.total_records==3&&
      operation.operation.identity()==Id(61001),"same actual operation successfully retries full selected-source acquisition after both failures");
  retried.lease.Reset();preserved();
}
WriterFixture* callback_fixture=nullptr;
Writer::Operation* callback_operation=nullptr;
bool callback_ran=false;
WE callback_read_error=WE::none,callback_io_error=WE::none;
void ResetOwnerCallback(){callback_ran=true;callback_fixture->writer.reset();}
void ResetOperationCallback(){callback_ran=true;auto& f=*callback_fixture;
  const auto read=callback_operation->ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  callback_read_error=read.writer_error;std::array<byte,1> bytes{};
  callback_io_error=callback_operation->ReadAtBounded(Id(2),f.profiles.front(),0,bytes).error;
  callback_operation->Reset();f.writer.reset();
}
void CallbackCustody(){
  {WriterFixture f;f.Accept();callback_fixture=&f;callback_ran=false;auto weak=std::weak_ptr(f.writer);
    writer_allocation_callback=ResetOwnerCallback;auto operation=Writer::TryBeginOperation(f.writer,Id(61001));writer_allocation_callback=nullptr;
    Check(callback_ran&&!f.writer&&operation.ok()&&!weak.expired(),"operation factory pins actual owner across allocation callback resetting caller's shared pointer");
    operation.operation.Reset();Check(weak.expired(),"operation factory callback owner releases on actual operation retirement");}
  {WriterFixture f;f.Accept();auto operation=Writer::TryBeginOperation(f.writer,Id(61001));Check(operation.ok(),"actual read callback operation");
    callback_fixture=&f;callback_operation=&operation.operation;callback_ran=false;auto weak=std::weak_ptr(f.writer);
    writer_allocation_callback=ResetOperationCallback;
    auto read=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);writer_allocation_callback=nullptr;
    Check(callback_ran&&callback_read_error==WE::read_handle_live&&callback_io_error==WE::read_handle_live&&read.ok()&&!operation.operation&&!f.writer&&!weak.expired(),
        "read reserves before allocation callback, refuses recursive I/O, and survives external operation/owner Reset");
    read.lease.Reset();Check(weak.expired(),"last actual read releases callback-retained reservation and owner");}
  {WriterFixture f;f.Accept();auto operation=Writer::TryBeginOperation(f.writer,Id(61001));Check(operation.ok(),"actual bounded I/O callback operation");
    callback_fixture=&f;callback_operation=&operation.operation;callback_ran=false;auto weak=std::weak_ptr(f.writer);std::array<byte,1> actual{};
    writer_allocation_callback=ResetOperationCallback;
    const auto read=operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,actual);writer_allocation_callback=nullptr;
    Check(callback_ran&&callback_read_error==WE::busy&&callback_io_error==WE::busy&&read.ok()&&actual[0]=='S'&&
        !operation.operation&&!f.writer&&weak.expired(),
        "real I/O reporting callback cannot reenter or invalidate active device/batch cleanup after external Reset");}
  callback_fixture=nullptr;callback_operation=nullptr;
}
void CloseControls(){
  {WriterFixture f;f.Accept();std::array<db::NativeWriterMemberCloseReceipt,3> members{};
    reads=writes=syncs=0;io_counting=true;const auto invalid=Writer::Close(f.writer,std::span(members).first(2));io_counting=false;
    Check(invalid.error==WE::invalid_request&&!invalid.member_count&&!reads&&!writes&&!syncs,
        "short fixed close receipt backing refuses before withdrawal or effects");
    auto operation=Writer::TryBeginOperation(f.writer,Id(61001));Check(operation.ok(),"invalid close backing leaves genuine physical admission open");
    auto reader=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);Check(reader.ok(),"real selected read before bounded drain");
    allocation_budget=0;auto busy=Writer::Close(f.writer,members);const auto remaining=allocation_budget;allocation_budget=-1;
    Check(busy.error==WE::busy&&remaining==0&&!busy.member_count&&Writer::TryBeginOperation(f.writer,Id(61002)).error==WE::withdrawn,
        "busy close withdraws new operations without waiting, allocating or disturbing live reader/accepted operation");
    operation.operation.Reset();Check(Writer::Close(f.writer,members).error==WE::busy,"reader alone retains true close/drain obligation");
    reader.lease.Reset();auto closed=Writer::Close(f.writer,members);
    Check(closed.ok()&&closed.member_count==3&&!closed.historical_close_uncertainty,"actual synchronized complete cohort closes after last read release");
    for(const auto& member:members)Check(member.synchronization&&member.synchronization->native_attempted&&member.synchronization->synchronized&&
        member.data.attempted&&member.data.disposition==db::NativeWriterHandleCloseDisposition::released,
        "each writable native member has its own actual synchronization and physical release receipt");
    io_counting=true;syncs=0;const auto duplicate=Writer::Close(f.writer,members);io_counting=false;
    Check(duplicate.ok()&&!syncs&&std::all_of(members.begin(),members.end(),[](const auto& m){return !m.synchronization&&!m.data.attempted&&!m.ownership.attempted;}),
        "complete close retry cannot resynchronize or reclose retired descriptors");
  }
  {WriterFixture f;f.Accept();std::array<db::NativeWriterMemberCloseReceipt,3> members{};
    io_counting=true;syncs=0;sync_fault=1;auto failed=Writer::Close(f.writer,members);io_counting=false;sync_fault=0;
    Check(!failed.ok()&&failed.error==WE::io_failure&&!failed.all_members_retired&&members[0].synchronization&&
        members[0].synchronization->native_error==EIO&&!members[0].synchronization->synchronized&&!members[0].data.attempted,
        "actual failed close-time synchronization preserves failed member and exact cause for retry");
    io_counting=true;syncs=0;auto retried=Writer::Close(f.writer,members);io_counting=false;
    Check(retried.ok()&&syncs==1&&members[0].synchronization&&members[0].synchronization->synchronized,
        "actual close retry synchronizes and retires only the genuinely retained failed member");
  }
  {WriterFixture f;f.Accept();std::array<db::NativeWriterMemberCloseReceipt,3> members{};
    writer_reused_fd=writer_retired_fd=-1;writer_late_close_fault=true;
    const auto failed=Writer::Close(f.writer,members);
    Check(!writer_late_close_fault&&writer_reused_fd==writer_retired_fd&&writer_reused_fd>=0&&
        failed.error==WE::close_failure&&failed.historical_close_uncertainty&&failed.first_uncertain_filespace==Id(2)&&
        failed.first_uncertain_native_error==EINTR&&members[0].data.native_error==EINTR&&
        members[0].data.disposition==db::NativeWriterHandleCloseDisposition::released_after_error,
        "real Linux descriptor release followed by late EINTR is retained as uncertain error, not retried");
    const auto retry=Writer::Close(f.writer,members);Check(!retry.ok()&&retry.historical_close_uncertainty&&
        !members[0].data.attempted&&::fcntl(writer_reused_fd,F_GETFD)>=0,
        "close retry never targets reused descriptor or erases the original uncertain release");
    f.writer.reset();Check(::fcntl(writer_reused_fd,F_GETFD)>=0,"writer destruction never recloses a retired/reused native descriptor");
    Check(!::close(writer_reused_fd),"release test-owned replacement descriptor");writer_reused_fd=-1;
  }
}
void ThreadAffinityDeath(unsigned mode){WriterFixture f;f.Accept();auto result=Writer::TryBeginOperation(f.writer,Id(61001));
  Check(result.ok(),"actual operation before thread-affinity misuse");
  auto selected=result.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);
  Check(selected.ok(),"actual guarded source before thread-affinity misuse");
  std::set_terminate([]{::_exit(73);});
  std::thread wrong([&]{if(mode==0)(void)result.operation.identity();else if(mode==1){auto moved=std::move(result.operation);}
    else if(mode==2)(void)selected.lease.source();else {auto moved=std::move(selected.lease);}});wrong.join();
  throw std::runtime_error("thread-affine native writer misuse unexpectedly returned");
}
void ThreadControls(const char* executable){for(unsigned mode=0;mode<4;++mode){
  const auto argument=std::to_string(mode);const auto pid=::fork();Check(pid>=0,"fork owned fresh-process thread control");
  if(!pid){::execl(executable,executable,"--thread-death",argument.c_str(),nullptr);::_exit(79);}
  int status=0;pid_t waited;do{waited=::waitpid(pid,&status,0);}while(waited<0&&errno==EINTR);
  Check(waited==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==73,
      "real foreign-thread operation/read access and move terminate without returning forged custody");
}}
void ForkControls(){WriterFixture f;
  auto child=::fork();Check(child>=0,"fork before native writer transfer");
  if(!child){allocation_budget=0;auto result=f.Adopt();::_exit(result.route_error==d::RouteSourceTransitionError::wrong_process&&allocation_budget==0?0:78);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"inherited transition refuses before native mutex/governor/allocation");
  f.Accept();auto operation=Writer::TryBeginOperation(f.writer,Id(61001));Check(operation.ok(),"actual operation before fork");
  auto reader=operation.operation.ReadWithMemory({f.physical.budget,32*1024*1024},f.reading.memory,f.reading.binding);Check(reader.ok(),"actual retained source before fork");
  child=::fork();Check(child>=0,"fork inherited native writer/source owners");
  if(!child){auto refused=Writer::TryBeginOperation(f.writer,Id(61002));std::array<byte,1> byte{};
    auto io=operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,byte);
    reader.lease.Reset();operation.operation.Reset();f.writer.reset();f.transition.transition.reset();f.route.lock.reset();
    ::_exit(refused.error==WE::wrong_process&&io.error==WE::wrong_process&&!io.io?0:79);}
  Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fork rejects source use and abandons inherited guards with close-only child cleanup");
  reader.lease.Reset();std::array<byte,1> byte{};Check(operation.operation.ReadAtBounded(Id(2),f.profiles.front(),0,byte).ok(),"child cleanup never unlocks/closes parent custody");
}
void AdmissionFaults(unsigned route,unsigned shard,unsigned shards){
  unsigned long maximum=0;unsigned physical_reads=0;
  {WriterFixture f;allocations=reads=0;counting=io_counting=true;auto measured=f.Adopt();counting=io_counting=false;
    maximum=allocations;physical_reads=reads;Check(measured.ok(),"measure complete actual writer transfer");measured.writer.reset();f.persistent.Empty();}
  const auto count=route==0?physical_reads:maximum+1;
  for(unsigned long at=route==0?1:0;at<count+(route==0?1:0);++at){if(at%shards!=shard)continue;
    WriterFixture f;const auto before=f.persistent.manager.Snapshot().telemetry_truncation_count+f.transient.manager.Snapshot().telemetry_truncation_count;
    reads=writes=syncs=0;io_counting=true;if(route==0)read_fault=at;else allocation_budget=at;
    auto result=f.Adopt();const auto remaining=allocation_budget;allocation_budget=-1;io_counting=false;read_fault=0;
    Check(!writes&&!syncs,"physical source adoption never performs mutation or synchronization");
    if(route==0){Check(reads==at,"each native writer admission fault reaches its exact actual read site");FailedAdoption(f,result);}
    else {Check(at==maximum?remaining>=0:remaining<0,"complete measured writer-control/backing allocation sweep");
      if(!result.ok())FailedAdoption(f,result);
      else {if(remaining<0)Check(f.persistent.manager.Snapshot().telemetry_truncation_count+f.transient.manager.Snapshot().telemetry_truncation_count>before||result.writer->failed_io_latency_observations()>0,
          "only recorded optional telemetry failure can coexist with complete writer transfer");result.writer.reset();f.persistent.Empty();}}
  }
}
}
int main(int argc,char** argv){try{ParentOwnedArtifacts artifacts;
  if(argc==3&&std::string_view(argv[1])=="--thread-death"){const auto mode=std::stoul(argv[2]);Check(mode<4,"thread control mode");ThreadAffinityDeath(mode);}
  else if(argc==5&&std::string_view(argv[1])=="--faults"){const auto route=std::stoul(argv[2]),shard=std::stoul(argv[3]),shards=std::stoul(argv[4]);
    Check(route<2&&shards&&shard<shards,"bounded writer admission fault selection");AdmissionFaults(route,shard,shards);}
  else {Check(argc==1,"unknown native writer test mode");ThreadControls(argv[0]);InitialRefusals();for(unsigned p=0;p<5;++p)for(unsigned q=0;q<5;++q)PhysicalOperations(p,q);ReaderRetainsReservation();OperationAllocationFailure();ReaderFailureRetry();CallbackCustody();CloseControls();ForkControls();}
  artifacts.Cleanup();
  std::cout<<"native owned checkpoint writer PASS checks="<<checks<<" physical_only_not_startup_admission=true\n";return 0;
}catch(const std::exception& e){allocation_budget=-1;counting=io_counting=false;read_fault=write_fault=sync_fault=0;
  writer_allocation_callback=nullptr;writer_late_close_fault=false;if(writer_reused_fd>=0)(void)::close(writer_reused_fd);
  std::cerr<<"native owned writer FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
