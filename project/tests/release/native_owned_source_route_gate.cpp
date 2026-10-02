// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_ownership.hpp"
#include "native_owned_checkpoint_source.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
  std::cout<<"native owned source route isolation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
