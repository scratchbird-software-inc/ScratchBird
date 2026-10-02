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
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace disk=scratchbird::storage::disk;
namespace db=scratchbird::storage::database;
namespace server=scratchbird::server;
unsigned reads=0;
bool count_reads=false;
thread_local bool fail_borrow_allocation=false;
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
void* operator new(std::size_t n){
  if(fail_borrow_allocation){fail_borrow_allocation=false;throw std::bad_alloc();}
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
  std::cout<<"native owned source route isolation and counted borrowing passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
