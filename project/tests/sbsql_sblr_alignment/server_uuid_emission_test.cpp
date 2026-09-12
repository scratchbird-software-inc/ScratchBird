// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbps.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {
using Id=std::array<std::uint8_t,16>;
unsigned failures=0;
void Check(bool value,const char* message) {
  if(!value) {++failures;std::cerr<<message<<'\n';}
}
bool Transfer(int fd,void* bytes,std::size_t count,bool write) {
  auto* data=static_cast<std::uint8_t*>(bytes);
  while(count) {
    const auto n=write ? ::write(fd,data,count) : ::read(fd,data,count);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) return false;
    count-=std::size_t(n);data+=n;
  }
  return true;
}
bool Valid(const Id& id) {return (id[6]&0xf0)==0x70 && (id[8]&0xc0)==0x80;}
std::vector<Id> Generate(std::size_t count) {
  std::vector<Id> ids;ids.reserve(count);
  for(std::size_t i=0;i<count;++i) ids.push_back(scratchbird::server::sbps::MakeUuidV7Bytes());
  return ids;
}
}
int main() {
  namespace sbps=scratchbird::server::sbps;
  constexpr unsigned count=4096,threads=8;
  const auto initial=sbps::MakeUuidV7Bytes();Check(Valid(initial),"initial server identity not binary UUIDv7");
  int descriptors[2];
  if(::pipe(descriptors)!=0) {std::cerr<<"pipe failed\n";return 2;}
  const auto child=::fork();
  if(child<0) {::close(descriptors[0]);::close(descriptors[1]);std::cerr<<"fork failed\n";return 2;}
  if(child==0) {
    ::close(descriptors[0]);
    auto ids=Generate(count);
    const bool written=Transfer(descriptors[1],ids.data(),ids.size()*sizeof(Id),true);
    ::close(descriptors[1]);::_exit(written?0:2);
  }
  ::close(descriptors[1]);
  const auto parent_ids=Generate(count);
  std::vector<Id> child_ids(count);
  const bool received=Transfer(descriptors[0],child_ids.data(),child_ids.size()*sizeof(Id),false);
  ::close(descriptors[0]);int status=0;
  const bool waited=::waitpid(child,&status,0)==child;
  Check(received && waited && WIFEXITED(status) && WEXITSTATUS(status)==0,"child generation/read failed");
  std::set<Id> parent_set(parent_ids.begin(),parent_ids.end());
  unsigned duplicates=0,cloned_suffixes=0;
  for(unsigned i=0;i<count;++i) {
    duplicates+=parent_set.contains(child_ids[i]);
    cloned_suffixes+=std::equal(parent_ids[i].begin()+6,parent_ids[i].end(),child_ids[i].begin()+6);
    Check(Valid(parent_ids[i]) && Valid(child_ids[i]),"fork emitted non-v7 identity");
  }
  Check(duplicates==0,"forked server reused an actual binary UUID");
  // This additional state-cloning check isolates fork safety from scheduler
  // timing. A shifted clock can mask actual collisions in one particular run.
  Check(cloned_suffixes==0,"fork cloned the server identity generator stream");
  std::barrier ready(threads);
  std::array<std::vector<Id>,threads> batches;
  std::vector<std::thread> workers;
  for(unsigned i=0;i<threads;++i) workers.emplace_back([&,i] {
    ready.arrive_and_wait();batches[i]=Generate(count);
  });
  for(auto& worker:workers) worker.join();
  std::set<Id> all;
  for(const auto& batch:batches) for(const auto& id:batch) {
    Check(Valid(id),"concurrent server emission not binary UUIDv7");all.insert(id);
  }
  const auto concurrent_duplicates=threads*count-all.size();
  Check(concurrent_duplicates==0,"concurrent server reused binary UUID identities");
  std::cout<<"server_uuid_emission fork_duplicates="<<duplicates<<" cloned_suffixes="<<cloned_suffixes
           <<" concurrent_duplicates="<<concurrent_duplicates<<" samples="<<(threads+2)*count
           <<" failures="<<failures<<'\n';
  return failures?1:0;
}
