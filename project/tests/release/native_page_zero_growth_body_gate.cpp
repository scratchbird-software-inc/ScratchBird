// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool armed=false,count_allocations=false;
unsigned reads=0,writes=0,syncs=0,stats=0,hashes=0;
unsigned fail_read=0,fail_sync=0,fail_stat=0,fail_hash=0,write_fault=0;
std::size_t torn_bytes=97;
unsigned long allocations=0;
long allocation_budget=-1;
}
void* operator new(std::size_t n){if(count_allocations)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t at){
  if(armed&&++reads==fail_read){errno=EIO;return -1;}return __real_pread(fd,b,n,at);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t at){
  if(armed){++writes;
    if(at!=4096||n!=384){errno=EINVAL;return -1;}
    if(write_fault){const auto mode=write_fault;write_fault=0;
      if(mode==2||mode==3){const auto done=__real_pwrite(fd,b,mode==2?torn_bytes:n,at);if(done<0)return done;}
      if(mode<=3){errno=EIO;return -1;}
      const auto done=__real_pwrite(fd,b,n,at);if(done!=static_cast<ssize_t>(n))return done;
      const auto corrupt=static_cast<unsigned char>(static_cast<const unsigned char*>(b)[100]^1);
      if(__real_pwrite(fd,&corrupt,1,at+100)!=1)return -1;return done;
    }
  }
  return __real_pwrite(fd,b,n,at);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(armed&&++syncs==fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* out){if(armed&&++stats==fail_stat){errno=EIO;return -1;}return __real_fstat(fd,out);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(armed&&++hashes==fail_hash)return nullptr;return __real_EVP_MD_CTX_new();}

namespace {
namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=d::FilespacePageZeroBodyError;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
struct Fixture {
  std::filesystem::path root;d::FileDevice device;std::string path;
  d::FilespacePageZero original,target;d::FilespaceBootstrapBinding binding;
  Bytes before,after,whole;u64 budget=0;
  Fixture(unsigned profile,u32 flags,u16 role){char name[]="/tmp/sb-growth-body.XXXXXX";const auto created=::mkdtemp(name);
    Check(created,"owned growth body fixture");root=created;path=(root/"member").string();
    const auto& p=d::kCanonicalFilespacePageProfiles[profile];auto& b=original.bootstrap;
    b.database_uuid=Id(1);b.filespace_uuid=Id(2);b.page_size_profile_uuid=p.uuid;
    b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.page_size_bytes=p.page_size_bytes;
    b.filespace_role=role;b.lifecycle_state=1;b.flags=flags;
    if(flags&1)b.encryption_profile_uuid=Id(7);
    original.page_uuid=Id(3);
    original.creation_operation_uuid=Id(4);original.writer_identity_uuid=Id(5);
    original.page_generation=3;original.root_set_generation=5;original.total_pages=32;original.free_pages=18;
    if(role<=4){constexpr u32 types[]{0,8,5,3,769,9,10,11,5,768};
      for(u16 kind=1;kind<=9;++kind)original.roots.push_back({kind,types[kind],Id(2),kind,1,p.uuid,Id(30+kind)});
      original.roots.push_back({18,0x30e,Id(2),18,1,p.uuid,Id(79)});
      original.roots.push_back({19,0x30e,Id(2),19,1,p.uuid,Id(79)});
      original.roots.push_back({20,0x500,Id(2),20,1,p.uuid,Id(80)});
      original.roots.push_back({21,0x500,Id(2),21,1,p.uuid,Id(80)});
    }else original.roots.push_back({3,3,Id(2),1,1,p.uuid,Id(6)});
    target=original;target.page_generation++;target.root_set_generation++;target.total_pages+=2;target.free_pages+=2;
    const auto a=d::EncodeFilespacePageZero(original),z=d::EncodeFilespacePageZero(target);
    Check(a.ok()&&z.ok(),"valid complete before/after inputs");before=*a.bytes;after=*z.bytes;
    binding={Id(1),Id(2),p.uuid};budget=4*u64{p.page_size_bytes};
    whole.assign(target.total_pages*p.page_size_bytes,0xa5);std::copy(before.begin(),before.end(),whole.begin());
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"own fixture file");Reset();
  }
  ~Fixture(){armed=false;allocation_budget=-1;std::error_code e;std::filesystem::remove_all(root,e);}
  void Reset(){armed=false;allocation_budget=-1;fail_read=fail_sync=fail_stat=fail_hash=write_fault=0;
    reads=writes=syncs=stats=hashes=0;
    const auto io=device.WriteAt(0,whole.data(),whole.size());Check(io.ok()&&io.bytes_transferred==whole.size()&&device.Sync().ok(),"reset exact owned extent");}
  Bytes Actual(){armed=false;const auto size=device.Size();Check(size.ok(),"actual extent");Bytes bytes(size.size_bytes);
    const auto io=device.ReadAt(0,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual bytes");return bytes;}
  Bytes Expected(){auto expected=whole;std::copy(after.begin()+4096,after.begin()+4480,expected.begin()+4096);return expected;}
  d::FilespacePageZeroBodyResult Run(bool repair=false,u64 limit=0){armed=true;
    auto r=repair?d::RepairFilespacePageZeroGrowthBodyFromOpenDevice(device,binding,before,after,limit?limit:budget):
      d::WriteFilespacePageZeroGrowthBodyFromOpenDevice(device,binding,before,after,limit?limit:budget);
    armed=false;return r;}
};
void Test(unsigned profile,u32 flags,u16 role){Fixture f(profile,flags,role);
  count_allocations=true;allocations=0;auto r=f.Run();count_allocations=false;
  const auto nr=reads,ns=syncs,nt=stats,nh=hashes;const auto na=allocations;
  Check(r.ok()&&r.original_preimage_verified&&r.write_attempted&&!r.uncertain_write&&r.confirmed_bytes==384&&
        r.sync_attempted&&r.sync_completed&&r.postimage_verified&&writes==1&&f.Actual()==f.Expected(),"exact body-only durable physical effect");
  reads=writes=syncs=stats=hashes=0;r=f.Run();
  Check(r.ok()&&r.target_already_present&&!r.write_attempted&&!writes&&syncs==1&&f.Actual()==f.Expected(),"duplicate target resynchronizes without rewriting");
  for(unsigned route=0;route<4;++route){const auto count=route==0?nr:route==1?ns:route==2?nt:nh;
    for(unsigned at=1;at<=count;++at){f.Reset();if(route==0)fail_read=at;if(route==1)fail_sync=at;if(route==2)fail_stat=at;if(route==3)fail_hash=at;
      r=f.Run();Check(!r.ok()&&r.error==(route==3?E::hash_failure:E::io_failure),"exact read/sync/size/hash failure category");
      if(r.write_attempted)Check(r.confirmed_bytes==384&&!r.uncertain_write,"confirmed write survives later failure");
      else Check(!writes&&f.Actual()==f.whole,"pre-effect failure preserves original bytes");
      Check(!r.postimage_verified,"failure is not physical completion");
    }}
  for(unsigned long at=0;at<na;++at){f.Reset();const auto lost=f.device.failed_io_latency_observations();allocation_budget=at;r=f.Run();allocation_budget=-1;
    if(r.ok())Check(f.device.failed_io_latency_observations()>lost,"only measured optional telemetry can lose an allocation");
    else Check(r.error==E::resource_exhausted&&!r.postimage_verified,"required allocation failure is not completion");
    if(!r.write_attempted)Check(f.Actual()==f.whole,"allocation refusal before write preserves bytes");}
  for(bool damaged:{false,true}){
    const auto prepare=[&]{f.Reset();auto image=damaged?f.before:f.after;
      if(damaged)image[4224+152]^=1; // valid common header, invalid full-image seal
      Check(f.device.WriteAt(0,image.data(),image.size()).ok()&&f.device.Sync().ok(),"retained-state failure fixture");};
    prepare();allocations=0;count_allocations=true;r=f.Run(damaged);count_allocations=false;
    const unsigned counts[]{reads,syncs,stats,hashes,static_cast<unsigned>(allocations)};
    Check(r.ok()&&(damaged?r.damaged_body_observed:r.target_already_present),"retained-state positive baseline");
    for(unsigned route=0;route<5;++route)for(unsigned site=0;site<counts[route];++site){
      prepare();const auto prior=f.Actual();const auto lost=f.device.failed_io_latency_observations();
      if(route==0)fail_read=site+1;if(route==1)fail_sync=site+1;if(route==2)fail_stat=site+1;if(route==3)fail_hash=site+1;
      if(route==4)allocation_budget=site;
      r=f.Run(damaged);allocation_budget=-1;
      if(r.ok())Check(route==4&&f.device.failed_io_latency_observations()>lost,"only optional telemetry loss succeeds in retained-state sweep");
      else Check(r.error==(route==4?E::resource_exhausted:route==3?E::hash_failure:E::io_failure)&&!r.postimage_verified,
                 "retained-state fault category and failure-completion separation");
      if(!r.write_attempted)Check(f.Actual()==prior,"retained-state refusal cannot rewrite an existing image");
      if(route==3)Check(!r.write_attempted&&!r.damaged_body_observed,"hash failure cannot authorize damaged-body repair");
    }
  }
  for(unsigned mode=1;mode<=4;++mode){f.Reset();write_fault=mode;r=f.Run();
    Check(!r.ok()&&r.write_attempted&&r.error==(mode==4?E::readback_mismatch:E::io_failure),"failed physical write retains exact category");
    if(mode<=3)Check(r.uncertain_write&&r.confirmed_bytes==0&&!r.sync_completed,"zero reported bytes do not mean zero effects");
    const auto actual=f.Actual();if(mode==1)Check(actual==f.whole,"failure before backend write unchanged");else Check(actual!=f.whole,"backend effects retained after failure");
    Check(std::equal(actual.begin(),actual.begin()+4096,f.whole.begin())&&
          std::equal(actual.begin()+4480,actual.end(),f.whole.begin()+4480),"failed write never changes bootstrap roots or payload");
    reads=writes=syncs=stats=hashes=0;
    if(mode==2||mode==4){r=f.Run();Check(!r.ok()&&r.error==E::preimage_changed&&!writes,"ordinary retry cannot silently repair torn body");}
    reads=writes=syncs=stats=hashes=0;r=f.Run(true);Check(r.ok()&&f.Actual()==f.Expected(),"explicit retained-image repair completes exact physical body");
    if(mode==2||mode==4)Check(r.damaged_body_observed&&r.observed_body_error!=d::FilespacePageZeroError::none,
                           "successful repair retains original image damage classification");
  }
  f.Reset();r=f.Run(false,f.budget-1);Check(!r.ok()&&r.error==E::resource_exhausted&&!writes,"four-image verification ceiling");
  for(unsigned mutation=0;mutation<6;++mutation){f.Reset();auto candidate=f.target;
    if(mutation==0)candidate.page_generation++;if(mutation==1)candidate.root_set_generation++;
    if(mutation==2)candidate.total_pages=f.original.total_pages;
    if(mutation==3)candidate.page_uuid=Id(20);if(mutation==4)candidate.roots[0].object_uuid=Id(21);
    if(mutation==5)candidate.writer_identity_uuid=Id(22);
    if(mutation==2)candidate.free_pages=f.original.free_pages;
    const auto image=d::EncodeFilespacePageZero(candidate);Check(image.ok(),"independently valid disallowed transition");
    armed=true;r=d::WriteFilespacePageZeroGrowthBodyFromOpenDevice(f.device,f.binding,f.before,*image.bytes,f.budget);armed=false;
    Check(!r.ok()&&r.error==E::invalid_transition&&!writes&&f.Actual()==f.whole,"unrelated valid metadata cannot be substituted");
  }
  f.Reset();auto other=f.target;other.page_generation++;const auto valid_other=d::EncodeFilespacePageZero(other);Check(valid_other.ok(),"foreign valid body");
  Check(f.device.WriteAt(0,valid_other.bytes->data(),valid_other.bytes->size()).ok(),"persist foreign valid body");
  r=f.Run(true);Check(!r.ok()&&r.error==E::preimage_changed&&!writes,"repair refuses unexpected valid metadata");
  f.Reset();auto damaged=f.before;damaged[4480]^=1;Check(f.device.WriteAt(0,damaged.data(),damaged.size()).ok(),"immutable root damage fixture");
  r=f.Run(true);Check(!r.ok()&&r.error==E::preimage_changed&&!writes,"repair refuses immutable root damage");
  f.Reset();damaged=f.before;damaged[100]^=1;Check(f.device.WriteAt(0,damaged.data(),damaged.size()).ok(),"immutable bootstrap damage fixture");
  r=f.Run(true);Check(!r.ok()&&r.error==E::preimage_changed&&!writes,"repair refuses immutable bootstrap damage");
  f.Reset();std::filesystem::resize_file(f.path,f.whole.size()-1);r=f.Run();
  Check(!r.ok()&&r.error==E::extent_mismatch&&!writes,"metadata update does not round up or grow a short extent");
  if(!flags){f.Reset();
    for(std::size_t prefix=1;prefix<384;++prefix){
      Check(f.device.WriteAt(0,f.before.data(),f.before.size()).ok()&&f.device.Sync().ok(),"persist original before torn-prefix case");
      torn_bytes=prefix;write_fault=2;r=f.Run();
      Check(!r.ok()&&r.uncertain_write&&r.confirmed_bytes==0,"every torn-prefix error retains unknown write effects");
      r=f.Run(true);Check(r.ok()&&r.postimage_verified,"every torn-prefix case repairs exact retained target");
    }
    torn_bytes=97;Check(f.Actual()==f.Expected(),"all torn-prefix repairs preserve immutable roots and payload");
  }
  f.Reset();write_fault=2;r=f.Run();Check(!r.ok()&&r.uncertain_write,"retain torn body for independent reopen");
  Check(f.device.Close().ok(),"release failed fixture ownership");
  const pid_t child=::fork();Check(child>=0,"start independent repair process");
  if(!child){d::FileDevice owned;const bool opened=owned.Open(f.path,d::FileOpenMode::open_existing).ok();
    const auto repaired=opened?d::RepairFilespacePageZeroGrowthBodyFromOpenDevice(owned,f.binding,f.before,f.after,f.budget):d::FilespacePageZeroBodyResult{};
    ::_exit(repaired.ok()&&repaired.damaged_body_observed?0:1);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process repairs original torn body");
  Check(f.device.Open(f.path,d::FileOpenMode::open_existing_read_only).ok()&&f.Actual()==f.Expected(),"readonly independent final image");
  r=f.Run();Check(!r.ok()&&r.error==E::invalid_device&&!r.write_attempted,"readonly handle is not a mutation capability");
  std::cout<<"growth body profile="<<f.before.size()<<" flags="<<flags<<" role="<<role<<" reads="<<nr<<" syncs="<<ns<<" stats="<<nt<<" hashes="<<nh<<" allocations="<<na<<'\n';
}
}
int main(){try{for(unsigned profile=0;profile<5;++profile)for(u32 flags=0;flags<4;++flags)for(u16 role:{u16{1},u16{5}})Test(profile,flags,role);std::cout<<"PASS growth body checks="<<checks<<" not_native_growth_completion=true\n";return 0;}
  catch(const std::exception& error){armed=false;allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}}
