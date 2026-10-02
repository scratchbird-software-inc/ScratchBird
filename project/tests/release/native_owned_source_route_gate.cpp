// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_ownership.hpp"
#include "native_owned_checkpoint_source.hpp"
#include "route_ownership_lease.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <new>
#include <thread>
#include <type_traits>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace disk=scratchbird::storage::disk;
namespace db=scratchbird::storage::database;
namespace server=scratchbird::server;
unsigned reads=0;
bool count_reads=false;
thread_local bool fail_borrow_allocation=false;
thread_local int wait_allocation_budget=-1;
thread_local bool fail_native_wait=false,fail_condition_init=false,forbid_inherited_sync=false;
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*,pthread_mutex_t*,const timespec*);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c,pthread_mutex_t* m,const timespec* t){
  if(forbid_inherited_sync)::_exit(77);
  if(fail_native_wait){fail_native_wait=false;return EINVAL;}
  return __real_pthread_cond_timedwait(c,m,t);
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* m){
  if(forbid_inherited_sync)::_exit(77);return __real_pthread_mutex_lock(m);
}
extern "C" int __real_pthread_cond_destroy(pthread_cond_t*);
extern "C" int __wrap_pthread_cond_destroy(pthread_cond_t* c){
  if(forbid_inherited_sync)::_exit(77);return __real_pthread_cond_destroy(c);
}
extern "C" int __real_pthread_cond_init(pthread_cond_t*,const pthread_condattr_t*);
extern "C" int __wrap_pthread_cond_init(pthread_cond_t* c,const pthread_condattr_t* a){
  if(fail_condition_init){fail_condition_init=false;return EAGAIN;}
  return __real_pthread_cond_init(c,a);
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* buffer,size_t size,off_t offset){
  if(count_reads)++reads;
  return __real_pread(fd,buffer,size,offset);
}
void Check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct Fixture {
  std::filesystem::path root;
  Fixture(){char pattern[]="/tmp/sb_owned_source_route.XXXXXX";auto* made=::mkdtemp(pattern);
    Check(made!=nullptr,"create isolated source route fixture");root=made;}
  ~Fixture(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
};
void CountedBorrowers(const std::string& path){
  static_assert(!std::is_default_constructible_v<disk::RouteSourceTransition>);
  static_assert(!std::is_copy_constructible_v<disk::RouteSourceTransition>);
  Check(!disk::RouteSourceTransitionResult{}.ok(),"default result does not fabricate a transition");
  server::DatabaseOwnershipRequest request;request.database_path=path;
  auto route=server::AcquireDatabaseOwnership(request);
  Check(route.acquired&&route.lock,"actual owner for borrower accounting");
  const auto lock_path=path+".sb.route.owner.lock";
  auto observation=disk::RouteOwnershipLease::Borrow(lock_path);
  Check(observation&&observation->ObserveLegacyBorrowers()==1,"one actual counted borrow");
  {auto copy=observation;Check(copy->ObserveLegacyBorrowers()==1,"copies retain one admission rather than recounting references");
    auto another=disk::RouteOwnershipLease::Borrow(lock_path);
    Check(another&&another->ObserveLegacyBorrowers()==2,"separate admissions are counted separately");}
  Check(observation->ObserveLegacyBorrowers()==1,"last copy retires exactly its admission");
  bool allocation_failed=false;fail_borrow_allocation=true;
  try{auto unused=disk::RouteOwnershipLease::Borrow(lock_path);}
  catch(const std::bad_alloc&){allocation_failed=true;}
  fail_borrow_allocation=false;
  Check(allocation_failed&&observation->ObserveLegacyBorrowers()==1,"failed pin allocation cannot leak or undercount an admission");
  auto first=std::make_unique<disk::FileDevice>();
  Check(first->Open(path,disk::FileOpenMode::open_existing_read_only).ok()&&
    observation->ObserveLegacyBorrowers()==2,"actual open retains counted admission");
  const auto alias=path+".alias";std::filesystem::create_hard_link(path,alias);
  {disk::FileDevice aliased;
    Check(aliased.Open(alias,disk::FileOpenMode::open_existing_read_only).ok()&&
      observation->ObserveLegacyBorrowers()==3,"inode alias borrows the same real route owner");
    Check(aliased.Close().ok()&&observation->ObserveLegacyBorrowers()==2,"alias close retires its admission");}
  {disk::FileDevice failed;
    Check(!failed.Open(path,disk::FileOpenMode::create_new).ok()&&
      observation->ObserveLegacyBorrowers()==2,"failed actual open retires admission without closing active reader");}
  bool worker_ok=false;
  std::thread waiter([&]{try{
    disk::FileDevice reader;const auto opened=reader.Open(path,disk::FileOpenMode::open_existing_read_only);
    unsigned char value=0;
    worker_ok=opened.ok()&&reader.ReadAt(0,&value,1).ok()&&value==0x5a&&reader.Close().ok();
  }catch(...){worker_ok=false;}});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  bool counted_waiter=false;
  while(std::chrono::steady_clock::now()<deadline){
    if(observation->ObserveLegacyBorrowers()==3){counted_waiter=true;break;}
    std::this_thread::yield();
  }
  // Never throw while a waiter needs this thread's opening-thread path guard.
  route.lock->release();
  const bool late_borrow_refused=!disk::RouteOwnershipLease::Borrow(lock_path);
  bool late_alias_refused=false;
  {disk::FileDevice late;late_alias_refused=!late.Open(alias,disk::FileOpenMode::open_existing_read_only).ok();}
  const bool closed=first->Close().ok();first.reset();waiter.join();
  Check(counted_waiter&&closed&&worker_ok,"admitted waiting open stays counted and usable through owner withdrawal");
  Check(late_borrow_refused&&late_alias_refused&&observation->ObserveLegacyBorrowers()==1,
    "withdrawal refuses later borrowers while retaining existing counted pins");
  const auto child=::fork();Check(child>=0,"fork inherited counted borrower");
  if(child==0){const bool refused=!observation->ObserveLegacyBorrowers();observation.reset();::_exit(refused?0:7);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&
    observation->ObserveLegacyBorrowers()==1,"inherited observation and destruction never touch parent borrower accounting");
  const int probe=::open(lock_path.c_str(),O_RDWR|O_CLOEXEC);Check(probe>=0,"actual retained route-lock probe");
  const int locked=::flock(probe,LOCK_EX|LOCK_NB);const int error=errno;::close(probe);
  Check(locked<0&&(error==EWOULDBLOCK||error==EAGAIN),"counted pin retains actual route OS ownership after owner release and child destruction");
  observation.reset();
  disk::FileDevice reopened;
  Check(reopened.Open(path,disk::FileOpenMode::open_existing_read_only).ok()&&reopened.Close().ok(),
    "last admission release permits independent ownership");
}
bool AwaitParked(const std::shared_ptr<disk::RouteSourceDrainWait>& wait,std::uint64_t count){
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(std::chrono::steady_clock::now()<deadline){
    if(wait->Observe().parked_waiters==count)return true;
    std::this_thread::yield();
  }
  return false;
}
void DrainWaits(const std::string& path){
  using S=disk::RouteSourceDrainWaitState;
  using C=disk::RouteSourceDrainCancelResult;
  using E=disk::RouteSourceTransitionError;
  using Clock=std::chrono::steady_clock;
  static_assert(!std::is_default_constructible_v<disk::RouteSourceDrainWait>);
  static_assert(!std::is_copy_constructible_v<disk::RouteSourceDrainWait>);
  server::DatabaseOwnershipRequest request;request.database_path=path;
  auto route=server::AcquireDatabaseOwnership(request);
  Check(route.acquired&&route.lock,"actual owner for bounded drain waits");
  auto reader=std::make_unique<disk::FileDevice>();
  Check(reader->Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"retain actual legacy reader during wait");
  auto transition=route.lock->BeginNativeSourceTransition();
  Check(transition.ok(),"actual public owner issues retained transition");
  const auto begin=[&]{return transition.transition->BeginDrainWait(Clock::now()+std::chrono::seconds(10));};
  Check(transition.transition->BeginDrainWait(Clock::time_point::max()).error==E::invalid_deadline,
    "unbounded deadline is refused");
  fail_condition_init=true;auto condition_failure=begin();
  Check(!fail_condition_init&&condition_failure.error==E::lock_failure&&!condition_failure.wait,
    "native condition construction failure is typed and has no wait handle");
  fail_borrow_allocation=true;auto allocation_failure=begin();fail_borrow_allocation=false;
  Check(allocation_failure.error==E::resource_exhausted&&!allocation_failure.wait,
    "wait allocation failure cannot publish a wait or reopen admission");
  auto expired=transition.transition->BeginDrainWait(Clock::time_point::min());
  Check(expired.ok()&&expired.wait->Wait().state==S::deadline_expired&&
    expired.wait->Observe().legacy_borrowers==1,"past deadline retains outstanding borrower count");
  for(int point:{0,1}){
    wait_allocation_budget=point;auto failed=begin();const auto remaining=wait_allocation_budget;
    wait_allocation_budget=-1;
    Check(remaining==-1&&failed.error==E::resource_exhausted&&!failed.wait&&
      transition.transition->ObserveDrain().legacy_borrowers==1&&
      !disk::RouteOwnershipLease::Borrow(path+".sb.route.owner.lock"),
      "wait object/control-block allocation failures retain the same fenced owner");
  }
  auto native_failure=begin();Check(native_failure.ok(),"admit native wait failure attempt");
  fail_native_wait=true;auto failed=native_failure.wait->Wait();
  Check(!fail_native_wait&&failed.state==S::lock_failure&&failed.legacy_borrowers==1&&
    !failed.parked_waiters,"native wait error is not success and balances parked registration");
  auto cancelled=begin();Check(cancelled.ok(),"admit independent cancellation attempt");
  Check(cancelled.wait->Cancel()==C::requested&&cancelled.wait->Wait().state==S::cancelled&&
    cancelled.wait->Cancel()==C::already_completed,"cancellation before waiting is latched and idempotent");
  auto timed=transition.transition->BeginDrainWait(Clock::now()+std::chrono::milliseconds(15));
  Check(timed.ok()&&timed.wait->Wait().state==S::deadline_expired&&
    timed.wait->Observe().legacy_borrowers==1,"actual timed condition wait expires without retiring borrower");
  auto active=begin(),other=begin();Check(active.ok()&&other.ok(),"two independent retained wait attempts");
  S first=S::pending,second=S::pending,third=S::pending;
  std::thread one([w=active.wait,&first]{first=w->Wait().state;});
  std::thread two([w=active.wait,&second]{second=w->Wait().state;});
  std::thread three([w=other.wait,&third]{third=w->Wait().state;});
  const bool parked=AwaitParked(active.wait,2)&&AwaitParked(other.wait,1);
  const auto cancel=active.wait->Cancel();
  one.join();two.join();
  const bool unaffected=other.wait->Observe().state==S::pending;
  unsigned char value=0;
  const bool readable=reader->ReadAt(0,&value,1).ok()&&value==0x5a;
  const bool closed=reader->Close().ok();reader.reset();three.join();
  Check(parked&&cancel==C::requested&&first==S::cancelled&&second==S::cancelled&&unaffected,
    "cancellation wakes all callers of one handle without cancelling another attempt");
  Check(readable&&closed&&third==S::drained&&other.wait->Observe().legacy_borrowers==0&&
    !other.wait->Observe().parked_waiters,"actual reader close wakes drain and retires all wait registrations");
  Check(expired.wait->Wait().state==S::deadline_expired&&active.wait->Wait().state==S::cancelled&&
    active.wait->Observe().legacy_borrowers==1,"completed outcomes and selected counts remain immutable after later drain");
  Check(!disk::RouteOwnershipLease::Borrow(path+".sb.route.owner.lock"),"wait completion never reopens admission");
  route.lock->release();
  Check(other.wait->Wait().state==S::drained&&begin().error==E::withdrawn,
    "past drain result is not new authority after issuer withdrawal");
  for(const auto* suffix:{".sb.route.owner.lock",".sb.owner.lock"}){
    const int fd=::open((path+suffix).c_str(),O_RDWR|O_CLOEXEC);Check(fd>=0,"probe retained wait owner");
    const int got=::flock(fd,LOCK_EX|LOCK_NB),error=errno;::close(fd);
    Check(got<0&&(error==EWOULDBLOCK||error==EAGAIN),"completed wait still pins actual ownership locks");
  }
}
void WithdrawWaiting(const std::string& path){
  using S=disk::RouteSourceDrainWaitState;using E=disk::RouteSourceTransitionError;
  server::DatabaseOwnershipRequest request;request.database_path=path;
  auto route=server::AcquireDatabaseOwnership(request);Check(route.acquired&&route.lock,"owner for withdrawn wait");
  auto borrow=disk::RouteOwnershipLease::Borrow(path+".sb.route.owner.lock");Check(bool(borrow),"real legacy pin");
  auto transition=route.lock->BeginNativeSourceTransition();Check(transition.ok(),"issue transition for withdrawal");
  auto waiting=transition.transition->BeginDrainWait(std::chrono::steady_clock::now()+std::chrono::seconds(10));
  Check(waiting.ok(),"retain waiting attempt");
  S outcome=S::pending;
  // Reference capture ensures the child can destroy every copied handle even
  // though the parent's worker is parked inside a real native condition wait.
  std::thread worker([&]{outcome=waiting.wait->Wait().state;});
  const bool parked=AwaitParked(waiting.wait,1);
  const auto child=::fork();
  if(child==0){
    forbid_inherited_sync=true;
    const bool refused=waiting.wait->Wait().state==S::wrong_process&&
      waiting.wait->Observe().state==S::wrong_process&&
      waiting.wait->Cancel()==disk::RouteSourceDrainCancelResult::wrong_process&&
      transition.transition->BeginDrainWait(std::chrono::steady_clock::now()).error==E::wrong_process;
    route.lock.reset();borrow.reset();transition.transition.reset();waiting.wait.reset();
    ::_exit(refused?0:7);
  }
  int status=0;const bool child_ok=child>0&&::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
  route.lock->release();worker.join();
  Check(parked&&child_ok,"inherited live wait refuses and destructs without inherited synchronization access");
  Check(outcome==S::withdrawn&&waiting.wait->Observe().legacy_borrowers==1&&
    !waiting.wait->Observe().parked_waiters,"issuer withdrawal wakes actual wait without falsely retiring borrower");
  borrow.reset();Check(waiting.wait->Wait().state==S::withdrawn,"withdrawal completion remains immutable");
}
void* operator new(std::size_t n){
  if(fail_borrow_allocation){fail_borrow_allocation=false;throw std::bad_alloc();}
  if(wait_allocation_budget>=0&&wait_allocation_budget--==0)throw std::bad_alloc();
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){try{
  Fixture fixture;const auto path=(fixture.root/"source").string();
  {disk::FileDevice seed;const unsigned char value=0x5a;
    Check(seed.Open(path,disk::FileOpenMode::create_new).ok()&&seed.WriteAt(0,&value,1).ok()&&seed.Close().ok(),"create actual owned fixture");}
  {auto independent=std::make_unique<disk::FileDevice>();
    Check(independent->Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"retain independent owner across fork");
    const auto child=::fork();Check(child>=0,"fork independent storage owner");
    if(child==0){independent.reset();::_exit(0);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"destroy inherited device in child");
    const int probe=::open((path+".sb.owner.lock").c_str(),O_RDWR|O_CLOEXEC);
    Check(probe>=0,"open actual owner lock probe");const int locked=::flock(probe,LOCK_EX|LOCK_NB);const int error=errno;::close(probe);
    Check(locked<0&&(error==EWOULDBLOCK||error==EAGAIN),"child destruction must not unlock parent's independent OS owner lock");
    Check(independent->Close().ok(),"parent closes independent owner");}
  server::DatabaseOwnershipRequest request;request.database_path=path;
  auto route=server::AcquireDatabaseOwnership(request);Check(route.acquired&&route.lock&&route.lock->valid(),"retain real server route ownership");
  std::vector<std::unique_ptr<disk::FileDevice>> files;files.push_back(std::make_unique<disk::FileDevice>());
  Check(files[0]->Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"open actual route-borrowed file");
  const scratchbird::core::platform::Uuid database{{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,1}};
  auto primary=database;primary.bytes[15]=2;
  count_reads=true;const auto refused=db::NativeOwnedCheckpointSource::Adopt(database,primary,files,1048576);count_reads=false;
  Check(refused.error==db::NativeOwnedSourceError::device_ownership&&!refused.owner&&files.size()==1&&files[0]->is_open()&&!reads,
        "route-borrowed source refused before reading or taking ownership");
  route.lock->release();
  unsigned char value=0;Check(files[0]->ReadAt(0,&value,1).ok()&&value==0x5a,"original borrower remains usable after route withdrawal");
  {disk::FileDevice late;Check(!late.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"borrower retains real ownership after refused adoption");}
  Check(files[0]->Close().ok(),"borrower closes normally on its opening thread");files.clear();
  {disk::FileDevice reopened;Check(reopened.Open(path,disk::FileOpenMode::open_existing_read_only).ok()&&
      reopened.ReadAt(0,&value,1).ok()&&value==0x5a&&reopened.Close().ok(),"independent owner may open after actual borrower release");}
  CountedBorrowers(path);
  DrainWaits(path);WithdrawWaiting(path);
  std::cout<<"native owned source route isolation, counted borrowing and bounded drain waits passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
