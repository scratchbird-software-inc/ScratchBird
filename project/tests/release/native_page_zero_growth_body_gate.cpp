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
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {
std::atomic<bool> armed{false};
bool count_allocations=false;
unsigned reads=0,writes=0,syncs=0,stats=0,hashes=0;
unsigned fail_read=0,fail_sync=0,fail_stat=0,fail_hash=0,write_fault=0;
std::size_t torn_bytes=97;
unsigned long allocations=0;
long allocation_budget=-1;
bool change_probe_prefix=false;
off_t probe_extend_to=0;
std::atomic<unsigned> probe_pause{0};
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
  if(armed&&++reads==fail_read){errno=EIO;return -1;}
  if(n==4096){unsigned expected=1;if(probe_pause.compare_exchange_strong(expected,2))while(probe_pause==2)std::this_thread::yield();}
  const auto r=__real_pread(fd,b,n,at);
  if(armed&&n>4096&&r>0){if(change_probe_prefix){change_probe_prefix=false;static_cast<unsigned char*>(b)[0]^=1;}
    if(probe_extend_to){const auto size=probe_extend_to;probe_extend_to=0;if(ftruncate(fd,size)){errno=EIO;return -1;}}}
  return r;}
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
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t size,unsigned char* out,unsigned int* length,const EVP_MD* md,ENGINE* engine){
  if(armed&&++hashes==fail_hash)return 0;return __real_EVP_Digest(data,size,out,length,md,engine);}

namespace {
namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=d::FilespacePageZeroBodyError;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
 d::FilespacePageZero Metadata(unsigned profile,u32 flags,u16 role){
    d::FilespacePageZero original;
    const auto& p=d::kCanonicalFilespacePageProfiles[profile];auto& b=original.bootstrap;
    b.database_uuid=Id(1);b.filespace_uuid=Id(2);b.page_size_profile_uuid=p.uuid;
    b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.page_size_bytes=p.page_size_bytes;
    b.filespace_role=role;b.lifecycle_state=1;b.flags=flags;
    if(flags&1)b.encryption_profile_uuid=Id(7);
    original.page_uuid=Id(3);original.creation_operation_uuid=Id(4);original.writer_identity_uuid=Id(5);
    original.page_generation=3;original.root_set_generation=5;original.total_pages=32;original.free_pages=18;
    if(role<=4){constexpr u32 types[]{0,8,5,3,769,9,10,11,5,768};
      for(u16 kind=1;kind<=9;++kind)original.roots.push_back({kind,types[kind],Id(2),kind,1,p.uuid,Id(30+kind)});
      original.roots.push_back({18,0x30e,Id(2),18,1,p.uuid,Id(79)});
      original.roots.push_back({19,0x30e,Id(2),19,1,p.uuid,Id(79)});
      original.roots.push_back({20,0x500,Id(2),20,1,p.uuid,Id(80)});
      original.roots.push_back({21,0x500,Id(2),21,1,p.uuid,Id(80)});
    }else original.roots.push_back({3,3,Id(2),1,1,p.uuid,Id(6)});
    return original;
}
struct Fixture {
  std::filesystem::path root;d::FileDevice device;std::string path;
  d::FilespacePageZero original,target;d::FilespaceBootstrapBinding binding;
  Bytes before,after,whole;u64 budget=0;
  Fixture(unsigned profile,u32 flags,u16 role){char name[]="/tmp/sb-growth-body.XXXXXX";const auto created=::mkdtemp(name);
    Check(created,"owned growth body fixture");root=created;path=(root/"member").string();
    const auto& p=d::kCanonicalFilespacePageProfiles[profile];original=Metadata(profile,flags,role);
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
void RecoveryCandidates(unsigned profile){Fixture f(profile,0,1);using R=d::FilespaceRecoveryRootError;
  const u64 budget=f.before.size()+4096;
  const auto run=[&](u64 limit=0){armed=true;auto r=d::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(f.device,f.binding,limit?limit:budget);armed=false;return r;};
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.bootstrap&&r.roots.empty()&&!r.observed_size_bytes,"no partial recovery candidates");};
  const auto good=[&](const auto& r){Check(r.ok()&&r.bootstrap->filespace_uuid==Id(2)&&r.roots.size()==f.original.roots.size()&&r.observed_size_bytes==f.whole.size(),"complete candidate locations and observed bytes");
    for(std::size_t i=0;i<r.roots.size();++i){const auto& a=r.roots[i];const auto& b=f.original.roots[i];Check(a.kind==b.kind&&a.page_type==b.page_type&&a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid,"exact original candidate tuple");}};
  count_allocations=true;allocations=0;const auto initial=run();count_allocations=false;good(initial);
  const unsigned sites[]{reads,stats,hashes,static_cast<unsigned>(allocations)};
  Check(sites[0]==2&&sites[1]==2&&sites[2]==1&&!writes&&!syncs,"bounded read-only candidate probe");
  for(unsigned mode=0;mode<4;++mode)for(unsigned site=0;site<sites[mode];++site){f.Reset();const auto loss=f.device.failed_io_latency_observations();
    if(mode==0)fail_read=site+1;if(mode==1)fail_stat=site+1;if(mode==2)fail_hash=site+1;if(mode==3)allocation_budget=site;
    const auto r=run();allocation_budget=-1;
    if(r.ok())Check(mode==3&&f.device.failed_io_latency_observations()>loss,"measured optional probe telemetry loss");
    else {Check(r.error==(mode==2?R::hash_failure:mode==3?R::resource_exhausted:R::io_failure),"exact probe backend error class");empty(r);}
    Check(!writes&&!syncs&&f.Actual()==f.whole,"probe failures preserve all actual bytes");
  }
  f.Reset();empty(run(budget-1));
  for(unsigned variant=0;variant<3;++variant){f.Reset();auto torn=f.before;
    if(variant==0)std::fill(torn.begin()+4096,torn.begin()+4480,0);
    if(variant==1)torn[4224+12]=255; // root count is mutable and must be ignored
    if(variant==2)torn[4448]^=1; // invalid full-image seal is not a root signature
    Check(f.device.WriteAt(0,torn.data(),torn.size()).ok()&&f.device.Sync().ok(),"torn body root discovery fixture");good(run());
    Check(f.Actual()!=f.whole&&!writes&&!syncs,"probe leaves actual torn body untouched");
  }
  for(unsigned offset:{0u,4480u,4482u,4484u,4494u,4504u,4552u,static_cast<unsigned>(f.before.size()-1)}){f.Reset();auto wrong=f.before;wrong[offset]^=0xff;
    Check(f.device.WriteAt(0,wrong.data(),wrong.size()).ok(),"bad candidate directory fixture");empty(run());Check(!writes&&!syncs,"bad directory cannot authorize repair");}
  for(unsigned mutation=0;mutation<13;++mutation){f.Reset();auto wrong=f.before;
    auto* first=wrong.data()+4480;auto* selector=first+9*80;auto* watermark=first+11*80;
    if(mutation==0)std::fill(first,wrong.data()+wrong.size(),0);
    if(mutation==1)std::fill(first,first+80,0); // a hole cannot hide later entries
    if(mutation==2)std::fill(watermark+80,watermark+160,0);
    if(mutation==3){StoreLittle16(first+80,1);StoreLittle32(first+84,8);}
    if(mutation==4)StoreLittle64(selector+24,19);
    if(mutation==5)StoreLittle64(watermark+24,21);
    if(mutation==6){const auto id=Id(79);std::copy(id.bytes.begin(),id.bytes.end(),watermark+56);std::copy(id.bytes.begin(),id.bytes.end(),watermark+136);}
    if(mutation==7){const auto id=Id(81);std::copy(id.bytes.begin(),id.bytes.end(),watermark+56);}
    if(mutation==8){const auto id=Id(9);std::copy(id.bytes.begin(),id.bytes.end(),watermark+8);}
    if(mutation==9){const auto id=d::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid;std::copy(id.bytes.begin(),id.bytes.end(),first+40);}
    if(mutation==10)StoreLittle64(first+24,~u64{0});
    if(mutation==11)StoreLittle64(first+32,0);
    if(mutation==12){std::copy(wrong.begin()+4480+80,wrong.begin()+4480+13*80,wrong.begin()+4480);std::fill(first+12*80,first+13*80,0);}
    Check(f.device.WriteAt(0,wrong.data(),wrong.size()).ok(),"structurally invalid candidate set");empty(run());
    auto expected=f.whole;std::copy(wrong.begin(),wrong.end(),expected.begin());Check(!writes&&!syncs&&f.Actual()==expected,"invalid candidate set leaves actual bytes unchanged");
  }
  f.Reset();auto changed=f.original;changed.roots[11].page_number=22;changed.roots[12].page_number=23;
  const auto moved=d::EncodeFilespacePageZero(changed);Check(moved.ok()&&f.device.WriteAt(0,moved.bytes->data(),moved.bytes->size()).ok(),"structurally valid untrusted redirection fixture");
  const auto candidate=run();Check(candidate.ok()&&candidate.roots[11].page_number==22,"a structural candidate is deliberately NOT original-publication authority");
  f.Reset();change_probe_prefix=true;const auto changed_prefix=run();Check(!change_probe_prefix&&changed_prefix.error==R::changed_observation,"changed probe prefix rejected");empty(changed_prefix);
  f.Reset();probe_extend_to=f.whole.size()+1;const auto changed_extent=run();Check(!probe_extend_to&&changed_extent.error==R::changed_observation,"actual mid-probe extension rejected");empty(changed_extent);
  std::filesystem::resize_file(f.path,f.whole.size());f.Reset();
  probe_pause=1;auto reader=std::async(std::launch::async,[&]{return run();});struct Release {~Release(){probe_pause=3;}} release;
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(probe_pause!=2&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();Check(probe_pause==2,"probe pauses with retained device ownership");
  std::atomic<bool> entered=false;auto closer=std::async(std::launch::async,[&]{entered=true;return f.device.Close();});
  while(!entered&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
  const bool blocked=closer.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;probe_pause=3;
  const auto read_result=reader.get();const auto closed=closer.get();Check(entered&&blocked&&closed.ok(),"Close waits until complete root discovery finishes");good(read_result);probe_pause=0;
  Check(f.device.Open(f.path,d::FileOpenMode::open_existing).ok(),"probe concurrency reopen");auto torn=f.before;std::fill(torn.begin()+4096,torn.begin()+4480,0);
  Check(f.device.WriteAt(0,torn.data(),torn.size()).ok()&&f.device.Sync().ok(),"torn body retained for cold root discovery");
  Check(f.device.Close().ok()&&f.device.Open(f.path,d::FileOpenMode::open_existing_read_only).ok(),"candidate readonly reopen");good(run());
  Check(f.device.Close().ok(),"candidate independent-process release");const auto profile_arg=std::to_string(profile);const auto child=fork();Check(child>=0,"candidate child process");
  if(!child){execl("/proc/self/exe","native_page_zero_growth_body_gate","--probe-recovery-roots",f.path.c_str(),profile_arg.c_str(),nullptr);_exit(127);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent structural root discovery");
  for(u32 flags:{1u,2u,3u}){Fixture protected_file(profile,flags,1);const auto r=d::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(protected_file.device,protected_file.binding,budget);
    Check(r.error==((flags&1)?R::encrypted_requires_authority:R::cluster_requires_authority),"protected roots require their real authority path");empty(r);}
  Fixture secondary(profile,0,5);const auto r=d::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(secondary.device,secondary.binding,budget);Check(r.error==R::invalid_bootstrap,"secondary is not a primary recovery anchor");empty(r);
  for(u16 role:{u16{2},u16{3},u16{4}}){Fixture primary(profile,0,role);const auto r=d::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(primary.device,primary.binding,budget);
    Check(r.ok()&&r.bootstrap->filespace_role==role&&r.roots.size()==13,"every eligible primary role has the same bounded discovery contract");}
}
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
  const auto profile_arg=std::to_string(profile),flags_arg=std::to_string(flags),role_arg=std::to_string(role);
  const pid_t child=::fork();Check(child>=0,"start independent repair process");
  if(!child){execl("/proc/self/exe","native_page_zero_growth_body_gate","--repair-growth-body",f.path.c_str(),profile_arg.c_str(),flags_arg.c_str(),role_arg.c_str(),nullptr);_exit(127);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process repairs original torn body");
  Check(f.device.Open(f.path,d::FileOpenMode::open_existing_read_only).ok()&&f.Actual()==f.Expected(),"readonly independent final image");
  r=f.Run();Check(!r.ok()&&r.error==E::invalid_device&&!r.write_attempted,"readonly handle is not a mutation capability");
  std::cout<<"growth body profile="<<f.before.size()<<" flags="<<flags<<" role="<<role<<" reads="<<nr<<" syncs="<<ns<<" stats="<<nt<<" hashes="<<nh<<" allocations="<<na<<'\n';
}
}
int main(int argc,char** argv){try{
  if(argc!=1){const bool probe=argc==4&&std::string_view(argv[1])=="--probe-recovery-roots";
    const bool repair=argc==6&&std::string_view(argv[1])=="--repair-growth-body";Check(probe||repair,"known cold process mode");
    const auto profile=std::stoul(argv[3]);const auto flags=repair?std::stoul(argv[4]):0;const auto role=repair?std::stoul(argv[5]):1;
    Check(profile<5&&flags<4&&(role==1||role==5),"cold process fixture parameters");
    const auto original=Metadata(profile,flags,role);const auto& p=d::kCanonicalFilespacePageProfiles[profile];const d::FilespaceBootstrapBinding binding{Id(1),Id(2),p.uuid};
    d::FileDevice owned;Check(owned.Open(argv[2],probe?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"cold process exclusive file open");
    if(probe){const auto result=d::ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(owned,binding,p.page_size_bytes+4096);
      Check(result.ok()&&result.roots.size()==original.roots.size(),"cold root candidates");
      for(std::size_t i=0;i<result.roots.size();++i){const auto& a=result.roots[i];const auto& b=original.roots[i];
        Check(a.kind==b.kind&&a.page_type==b.page_type&&a.filespace_uuid==b.filespace_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.object_uuid==b.object_uuid,"cold exact root tuple");}
    }else{auto target=original;++target.page_generation;++target.root_set_generation;target.total_pages+=2;target.free_pages+=2;
      const auto a=d::EncodeFilespacePageZero(original),b=d::EncodeFilespacePageZero(target);Check(a.ok()&&b.ok(),"cold exact retained images");
      const auto result=d::RepairFilespacePageZeroGrowthBodyFromOpenDevice(owned,binding,*a.bytes,*b.bytes,4*u64{p.page_size_bytes});
      Check(result.ok()&&result.damaged_body_observed,"cold exact torn-body repair");}
    return 0;
  }
  for(unsigned profile=0;profile<5;++profile){RecoveryCandidates(profile);for(u32 flags=0;flags<4;++flags)for(u16 role:{u16{1},u16{5}})Test(profile,flags,role);}std::cout<<"PASS growth body checks="<<checks<<" not_native_growth_completion=true\n";return 0;}
  catch(const std::exception& error){armed=false;allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}}
