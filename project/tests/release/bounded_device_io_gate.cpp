// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "disk_device.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace d = scratchbird::storage::disk;
using E = d::BoundedIoError;
thread_local bool deny = false;
thread_local unsigned read_mode = 0, write_mode = 0, read_calls = 0, write_calls = 0;
thread_local unsigned sync_mode = 0, size_mode = 0;
thread_local unsigned allocations = 0;
std::recursive_mutex* probe_mutex = nullptr;
unsigned probes = 0, locked_probes = 0, checks = 0;
thread_local bool probing = false;
void Allocation() {
  if (deny) { ++allocations; throw std::bad_alloc(); }
  if (probe_mutex && !probing) {
    probing = true;
    const auto* saved = probe_mutex;
    probe_mutex = nullptr;
    bool acquired = false;
    std::thread worker([&]{auto* mutex=const_cast<std::recursive_mutex*>(saved);
      acquired=mutex->try_lock();if(acquired)mutex->unlock();});
    worker.join();
    probe_mutex = const_cast<std::recursive_mutex*>(saved);
    ++probes;if(!acquired)++locked_probes;
    probing = false;
  }
}
void* operator new(std::size_t n) { Allocation();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n,std::align_val_t a) {
  Allocation();void* p=nullptr;if(!posix_memalign(&p,static_cast<std::size_t>(a),n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void operator delete(void* p,std::align_val_t) noexcept {std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept {std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept {std::free(p);}
extern "C" {
ssize_t __real_pread(int,void*,size_t,off_t);
ssize_t __real_pwrite(int,const void*,size_t,off_t);
int __real_fsync(int);
int __real_fstat(int,struct stat*);
ssize_t __wrap_pread(int fd,void* p,size_t n,off_t offset) {
  ++read_calls;
  if(read_mode==1 || (read_mode==2&&read_calls>1)){errno=EIO;return -1;}
  if(read_mode==3&&read_calls==1){errno=EINTR;return -1;}
  if(read_mode==2)n=std::min<size_t>(n,7);
  return __real_pread(fd,p,n,offset);
}
ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t offset) {
  ++write_calls;
  if(write_mode==1 || (write_mode==2&&write_calls>1)){errno=ENOSPC;return -1;}
  if(write_mode==3&&write_calls==1){errno=EINTR;return -1;}
  if(write_mode==4)return 0;
  if(write_mode==2)n=std::min<size_t>(n,7);
  return __real_pwrite(fd,p,n,offset);
}
int __wrap_fsync(int fd){if(sync_mode){errno=EIO;return -1;}return __real_fsync(fd);}
int __wrap_fstat(int fd,struct stat* st){if(size_mode==1){errno=EIO;return -1;}
  const int rc=__real_fstat(fd,st);if(!rc&&size_mode==2)st->st_size=-1;return rc;}
}
void Check(bool condition,const char* text) {
  ++checks;if(!condition){deny=false;throw std::runtime_error(text);}
}
struct Denial { Denial(){deny=true;}~Denial(){deny=false;} };
struct Fixture {
  std::string root, path;
  d::FileDevice device;
  Fixture(){char pattern[]="/tmp/sb-01e-bounded-io-XXXXXX";const auto* p=::mkdtemp(pattern);
    Check(p,"temporary fixture");root=p;path=root+"/data";
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"open actual fixture");}
  ~Fixture(){deny=false;probe_mutex=nullptr;read_mode=write_mode=sync_mode=size_mode=0;
    if(device.is_open())(void)device.Close();std::error_code ec;std::filesystem::remove_all(root,ec);}
};
void CheckNoEffect(const d::BoundedIoResult& r,E error){
  Check(r.error==error&&!r.native_attempted&&!r.bytes_transferred&&!r.synchronized&&!r.read_only_noop,"refusal has no fabricated effect");
}
void TestProfile(unsigned bytes) {
  Fixture f;
  std::vector<unsigned char> source(bytes), target(bytes);
  for(unsigned i=0;i<bytes;++i)source[i]=static_cast<unsigned char>(i*37+11);
  std::recursive_mutex* mutex=nullptr;{auto guard=f.device.AcquireOperationGuard();mutex=guard.mutex();}
  d::BoundedIoResult partial;
  {
    d::FileDevice::BoundedIoBatch batch(f.device);
    const auto guard=f.device.AcquireOperationGuard();
    Denial no_heap;
    const auto write=batch.WriteAt(0,source.data(),bytes);
    Check(write.ok()&&write.bytes_transferred==bytes&&write.native_attempted&&!write.synchronized,"actual complete write receipt");
    const auto sync=batch.Sync();Check(sync.ok()&&sync.synchronized&&sync.native_attempted&&!sync.read_only_noop,"real durable sync");
    const auto size=batch.Size();Check(size.ok()&&size.size_bytes==bytes,"actual extent");
    const auto read=batch.ReadAt(0,target.data(),bytes);Check(read.ok()&&read.bytes_transferred==bytes&&target==source,"actual read content");
    CheckNoEffect(batch.WriteAt(0,nullptr,1),E::null_buffer);
    CheckNoEffect(batch.ReadAt(0,nullptr,1),E::null_buffer);
    CheckNoEffect(batch.WriteAt(std::numeric_limits<d::u64>::max(),source.data(),1),E::offset_overflow);
    CheckNoEffect(batch.ReadAt(0,target.data(),std::numeric_limits<d::usize>::max()),E::count_overflow);
    CheckNoEffect(batch.WriteAt(static_cast<d::u64>(std::numeric_limits<std::streamoff>::max()),source.data(),1),E::extent_overflow);
    const auto zero=batch.WriteAt(0,nullptr,0);Check(zero.ok()&&!zero.native_attempted&&!zero.bytes_transferred,"zero length no-op");
    Check(batch.ReadAt(0,nullptr,0).ok(),"zero read no-op");
    read_mode=2;read_calls=0;std::fill(target.begin(),target.end(),0);
    partial=batch.ReadAt(0,target.data(),bytes);read_mode=0;
    Check(partial.error==E::native_failure&&partial.native_error==EIO&&partial.bytes_transferred==7&&partial.native_attempted,"partial read exact effects");
    Check(std::equal(source.begin(),source.begin()+7,target.begin())&&target[7]==0,"partial read leaves untouched suffix");
    write_mode=2;write_calls=0;partial=batch.WriteAt(bytes,source.data(),bytes);write_mode=0;
    Check(partial.error==E::native_failure&&partial.native_error==ENOSPC&&partial.bytes_transferred==7&&!partial.synchronized,"partial physical write retained on failure");
    sync_mode=1;const auto failed_sync=batch.Sync();sync_mode=0;
    Check(failed_sync.error==E::native_failure&&failed_sync.native_error==EIO&&failed_sync.native_attempted&&!failed_sync.synchronized,"failed sync not durable");
    size_mode=1;const auto failed_size=batch.Size();size_mode=0;
    Check(failed_size.error==E::native_failure&&failed_size.native_error==EIO&&failed_size.size_bytes==0,"failed size not zero-success");
    // Exactly 16 captures; publication (and all allocation probes) is outside
    // BOTH the denial and outer retained-device fence.
    size_mode=2;const auto negative=batch.Size();size_mode=0;
    Check(negative.error==E::invalid_size&&!negative.size_bytes&&!negative.native_error,"negative size rejected");
    probe_mutex=mutex;
  }
  probe_mutex=nullptr;
  Check(probes&&!locked_probes,"all observation allocations after compound guard release");
  const auto rendered=d::RenderBoundedIoResult(partial,f.path);
  Check(!rendered.ok()&&rendered.bytes_transferred==7&&rendered.diagnostic.diagnostic_code=="SB-STORAGE-DISK-WRITE-FAILED","late diagnostic preserves effects and code");
  {
    d::FileDevice::BoundedIoBatch batch(f.device);const auto guard=f.device.AcquireOperationGuard();Denial no_heap;
    const auto extent=batch.Size();Check(extent.ok()&&extent.size_bytes==bytes+7,"physical partial extension visible");
    read_mode=write_mode=1;
    const auto refused_read=batch.ReadAt(0,target.data(),bytes);
    const auto refused_write=batch.WriteAt(0,source.data(),bytes);
    read_mode=write_mode=0;
    Check(refused_read.error==E::native_failure&&refused_read.native_error==EIO&&!refused_read.bytes_transferred&&refused_read.native_attempted,"first read syscall failure");
    Check(refused_write.error==E::native_failure&&refused_write.native_error==ENOSPC&&!refused_write.bytes_transferred&&refused_write.native_attempted,"first write syscall failure");
    write_mode=4;const auto zero=batch.WriteAt(0,source.data(),bytes);write_mode=0;
    Check(zero.error==E::short_transfer&&!zero.native_error&&!zero.bytes_transferred&&zero.native_attempted,"zero native write not success or stale errno");
    read_mode=write_mode=3;read_calls=write_calls=0;
    Check(batch.WriteAt(0,source.data(),bytes).ok()&&write_calls==2,"write EINTR retry");
    Check(batch.ReadAt(0,target.data(),bytes).ok()&&read_calls==2,"read EINTR retry");
    read_mode=write_mode=0;
    const auto eof=batch.ReadAt(bytes,target.data(),bytes);
    Check(eof.error==E::short_transfer&&eof.bytes_transferred==7&&!eof.native_error,"EOF partial read distinct from native error");
    Check(batch.Sync().synchronized,"retry sync actually durable");
  }
  Check(f.device.Close().ok(),"close before independent reopen");
  Check(f.device.Open(f.path,d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");
  {
    d::FileDevice::BoundedIoBatch batch(f.device);const auto guard=f.device.AcquireOperationGuard();Denial no_heap;
    Check(batch.ReadAt(0,target.data(),bytes).ok()&&target==source,"reopen persisted bytes");
    CheckNoEffect(batch.WriteAt(0,source.data(),bytes),E::read_only);
    CheckNoEffect(batch.WriteAt(0,nullptr,0),E::read_only);
    const auto sync=batch.Sync();Check(sync.ok()&&sync.read_only_noop&&!sync.native_attempted&&!sync.synchronized,"readonly sync is not durability evidence");
  }
  Check(f.device.Close().ok(),"final close");
  {
    d::FileDevice::BoundedIoBatch batch(f.device);const auto guard=f.device.AcquireOperationGuard();Denial no_heap;
    CheckNoEffect(batch.ReadAt(0,target.data(),bytes),E::not_open);
    CheckNoEffect(batch.WriteAt(0,source.data(),bytes),E::not_open);
    CheckNoEffect(batch.Size(),E::not_open);CheckNoEffect(batch.Sync(),E::not_open);
  }
}
void TestOverflow() {
  Fixture f;unsigned char byte=42;
  const auto before=f.device.rejected_io_latency_observations();
  {
    d::FileDevice::BoundedIoBatch batch(f.device);const auto guard=f.device.AcquireOperationGuard();Denial no_heap;
    for(unsigned i=0;i<17;++i)Check(batch.WriteAt(i,&byte,1).bytes_transferred==1,"overflow preserves every physical write");
    Check(f.device.rejected_io_latency_observations()==before+1,"fixed batch saturation counted immediately");
  }
  std::array<unsigned char,17> data{};
  Check(f.device.ReadAt(0,data.data(),data.size()).ok()&&std::all_of(data.begin(),data.end(),[](auto x){return x==42;}),"overflow does not lose bytes");
}
void TestObservationFailure() {
  Fixture f;unsigned char byte=91;
  const auto before=f.device.failed_io_latency_observations();
  const auto attempts=allocations;
  {
    // Deliberately fail publication allocations AFTER the fence is gone.
    // Those failures must be counted without changing the completed write.
    Denial no_heap;
    d::FileDevice::BoundedIoBatch batch(f.device);
    const auto guard=f.device.AcquireOperationGuard();
    Check(batch.WriteAt(0,&byte,1).bytes_transferred==1,"real effect before telemetry failure");
    CheckNoEffect(batch.WriteAt(0,nullptr,1),E::null_buffer);
    Check(allocations==attempts,"no heap attempted in effects or fixed refusals");
  }
  Check(allocations>attempts&&f.device.failed_io_latency_observations()>=before+2,"post-guard error and latency publication exceptions counted");
  unsigned char actual=0;
  Check(f.device.ReadAt(0,&actual,1).ok()&&actual==byte,"failed observations did not undo physical effect");
}
int main() {
  try {for(unsigned size:{8192u,16384u,32768u,65536u,131072u})TestProfile(size);
    TestOverflow();Check(!allocations,"no attempted heap allocation in bounded I/O success OR failure");
    TestObservationFailure();
    std::cout<<"PASS bounded device I/O "<<checks<<" checks\n";return 0;
  } catch(const std::exception& e){deny=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
