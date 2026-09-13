// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_system_state.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <unistd.h>
namespace {long allocation_budget=-1;bool count_allocations=false;unsigned long allocations=0;unsigned reads=0,fail_read=0,hash_calls=0,fail_hash=0;}
void* operator new(std::size_t n){if(count_allocations)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t at){++reads;if(fail_read==reads){errno=EIO;return -1;}return __real_pread(fd,p,n,at);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hash_calls;if(fail_hash==hash_calls)return nullptr;return __real_EVP_MD_CTX_new();}
namespace {
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=db::NativeSystemStateError;using L=db::NativeSystemLifecycle;using R=db::NativeSystemRecovery;
std::size_t checks=0;
void Check(bool good,const char* message,std::source_location at=std::source_location::current()){++checks;if(!good)throw std::runtime_error(std::string(message)+" line="+std::to_string(at.line()));}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
void Seal(Bytes& b){std::fill(b.begin()+408,b.begin()+440,0);std::array<byte,32> out{};Check(SHA256(b.data(),b.size(),out.data())!=nullptr,"independent system-state hash");std::copy(out.begin(),out.end(),b.begin()+408);}
Bytes Oracle(const db::NativeSystemState& s){const auto& h=s.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBSYS001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);
  Num(b,160,8,s.state_generation);Num(b,168,8,s.restart_generation);Num(b,176,8,s.startup_counter);Put(b,184,s.creator_transaction_uuid);Num(b,200,8,s.creator_local_transaction_id);
  Num(b,208,2,static_cast<u16>(s.lifecycle));Num(b,210,2,static_cast<u16>(s.recovery));Num(b,212,4,s.flags);Num(b,216,8,s.checkpoint_generation);
  if(s.checkpoint)Ref(b,224,*s.checkpoint);Put(b,272,s.checkpoint_object_uuid);Put(b,288,s.clean_transaction_uuid);Num(b,304,8,s.clean_local_transaction_id);Put(b,312,s.transition_operation_uuid);
  if(s.predecessor)Ref(b,328,*s.predecessor);std::copy(s.predecessor_sha256.begin(),s.predecessor_sha256.end(),b.begin()+376);Seal(b);return b;
}
db::NativeSystemState Example(unsigned profile=0){const auto& p=d::kCanonicalFilespacePageProfiles[profile];db::NativeSystemState s;
  s.header={p.page_size_bytes,8,Id(1),Id(2),Id(70),11,101,0,p.uuid};s.object_uuid=Id(41);s.state_generation=1;s.restart_generation=2;s.startup_counter=3;
  s.creator_transaction_uuid=Id(90);s.creator_local_transaction_id=17;s.lifecycle=L::opening;s.recovery=R::checkpoint_rebuild;
  s.checkpoint_generation=5;s.checkpoint=d::NativePageReference{Id(2),19,109,p.uuid};s.checkpoint_object_uuid=Id(49);
  s.clean_transaction_uuid=Id(91);s.clean_local_transaction_id=16;s.transition_operation_uuid=Id(92);return s;
}
void Empty(const db::NativeSystemStateResult& r){Check(!r.ok()&&!r.state&&r.bytes.empty(),"system-state failure returns no prefix");}
void Invalid(const db::NativeSystemState& s){Empty(db::EncodeNativeSystemState(s));Empty(db::DecodeNativeSystemState(Oracle(s)));}
void Codecs(){for(unsigned profile=0;profile<5;++profile)for(unsigned state=1;state<=10;++state)for(unsigned recovery=1;recovery<=7;++recovery)for(unsigned generation:{1u,2u}){
    auto s=Example(profile);s.lifecycle=static_cast<L>(state);s.recovery=static_cast<R>(recovery);s.flags=state==2?5:6;s.state_generation=generation;
    if(state==4&&recovery<4)s.flags=2;if(generation==2){s.predecessor=d::NativePageReference{Id(2),10,100,s.header.page_size_profile_uuid};s.predecessor_sha256.fill(7);}
    const auto expected=Oracle(s);const auto e=db::EncodeNativeSystemState(s);Check(e.ok()&&e.bytes==expected,"independent profile state recovery generation encoding");
    const auto r=db::DecodeNativeSystemState(expected);Check(r.ok()&&Oracle(*r.state)==expected,"independent native system-state decoding");}
  auto initial=Example();initial.checkpoint_generation=0;initial.checkpoint.reset();initial.checkpoint_object_uuid={};initial.clean_transaction_uuid={};initial.clean_local_transaction_id=0;initial.lifecycle=L::creating;
  Check(db::DecodeNativeSystemState(Oracle(initial)).ok(),"pre-checkpoint creation image is not clean shutdown");
  auto clustered=Example();clustered.flags|=db::NativeSystemFlag::cluster;
  const auto cluster_bytes=Oracle(clustered);const auto cluster_encoded=db::EncodeNativeSystemState(clustered);
  Check(cluster_encoded.ok()&&cluster_encoded.bytes==cluster_bytes&&db::DecodeNativeSystemState(cluster_bytes).ok(),"cluster observation bit is representable without provider authority");
  const auto good=Oracle(Example());for(std::size_t i=0;i<good.size();++i){auto b=good;b[i]^=1;Empty(db::DecodeNativeSystemState(b));}
  for(unsigned at:{128u,136u,138u,140u,440u,511u,8191u}){auto b=good;b[at]^=1;Seal(b);Empty(db::DecodeNativeSystemState(b));}
  for(unsigned change=0;change<25;++change){auto s=Example();switch(change){
    case 0:s.header.page_type=5;break;case 1:s.header.flags=2;break;case 2:s.object_uuid={};break;case 3:s.creator_transaction_uuid.bytes[6]=0x40;break;
    case 4:s.transition_operation_uuid={};break;case 5:s.state_generation=0;break;case 6:s.restart_generation=0;break;case 7:s.startup_counter=1;break;
    case 8:s.creator_local_transaction_id=0;break;case 9:s.lifecycle=static_cast<L>(11);break;case 10:s.recovery=static_cast<R>(0);break;case 11:s.flags=16;break;
    case 12:s.flags=7;break;case 13:s.flags=4;break;case 14:s.flags=5;break;case 15:s.lifecycle=L::closed;break;case 16:s.flags=2;break;
    case 17:s.clean_transaction_uuid={};break;case 18:s.clean_local_transaction_id=0;break;case 19:s.checkpoint_generation=0;break;
    case 20:s.checkpoint.reset();break;case 21:s.checkpoint_object_uuid={};break;case 22:s.state_generation=2;break;
    case 23:s.predecessor_sha256[0]=1;break;case 24:s.checkpoint->page_number=11;break;}Invalid(s);}
  auto s=Example();s.lifecycle=L::closed;s.flags=5;s.clean_local_transaction_id=0;s.clean_transaction_uuid={};Invalid(s);
  s=Example();s.lifecycle=L::ready;s.recovery=R::corruption_stop;s.flags=2;Invalid(s);
  s=Example();s.state_generation=2;s.predecessor=s.checkpoint;s.predecessor_sha256.fill(1);Invalid(s);
  s=Example();s.checkpoint->page_generation=0;Invalid(s);s=Example();s.checkpoint->page_number=std::numeric_limits<u64>::max();Invalid(s);
  s=Example();s.checkpoint->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;Invalid(s);
  for(bool encode:{false,true}){s=Example();allocations=0;count_allocations=true;auto r=encode?db::EncodeNativeSystemState(s):db::DecodeNativeSystemState(good);count_allocations=false;Check(r.ok(),"measure system codec allocations");const auto count=allocations;
    for(unsigned long n=0;n<=count;++n){allocation_budget=n;r=encode?db::EncodeNativeSystemState(s):db::DecodeNativeSystemState(good);allocation_budget=-1;if(r.ok())Check(r.bytes==good,"recovered codec allocation exact bytes");else Empty(r);if(n==count)Check(r.ok(),"codec allocation sweep terminal success");}
    hash_calls=0;fail_hash=1;r=encode?db::EncodeNativeSystemState(s):db::DecodeNativeSystemState(good);fail_hash=0;Empty(r);Check(r.error==E::hash_failure,"codec hash failure cause");}
}
struct Fixture{std::filesystem::path root;Fixture(){char pattern[]="/tmp/sb-system-state-XXXXXX";const auto p=mkdtemp(pattern);Check(p,"owned system-state fixture");root=p;}~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}};
void Files(){for(unsigned profile=0;profile<5;++profile){Fixture fixture;auto s=Example(profile);d::FilespacePageZero z;auto& b=z.bootstrap;b.database_uuid=Id(1);b.filespace_uuid=Id(2);b.page_size_profile_uuid=s.header.page_size_profile_uuid;
    b.page_size_bytes=s.header.page_size_bytes;b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.filespace_role=1;b.lifecycle_state=1;
    z.page_uuid=Id(3);z.creation_operation_uuid=Id(4);z.writer_identity_uuid=Id(5);z.page_generation=7;z.root_set_generation=8;z.total_pages=64;
    constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768};for(unsigned k=1;k<=9;++k)z.roots.push_back({static_cast<u16>(k),types[k],Id(2),10+k,100+k,b.page_size_profile_uuid,Id(40+k)});
    const auto zero=d::EncodeFilespacePageZero(z);Check(zero.ok(),"valid actual page-zero fixture");d::FileDevice device;const auto path=(fixture.root/"node").string();Check(device.Open(path,d::FileOpenMode::create_new).ok(),"own actual system-state device");
    const byte pad=0;Check(device.WriteAt(z.total_pages*b.page_size_bytes-1,&pad,1).ok()&&device.WriteAt(0,zero.bytes->data(),zero.bytes->size()).ok(),"actual page-zero capacity");
    const auto put=[&](const auto& state){const auto bytes=Oracle(state);const auto io=device.WriteAt(11*b.page_size_bytes,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"persist independent system-state bytes");};put(s);
    const auto ref=z.roots.front();const auto read=[&]{return db::ReadNativeSystemStateFromOpenDevice(device,Id(1),ref);};reads=hash_calls=0;allocations=0;count_allocations=true;auto r=read();count_allocations=false;const auto nr=reads,nh=hash_calls;const auto na=allocations;
    Check(r.ok()&&r.bytes==Oracle(s),"actual referenced native system-state image");
    for(unsigned n=1;n<=nr;++n){reads=0;fail_read=n;r=read();fail_read=0;Empty(r);}
    for(unsigned n=1;n<=nh;++n){hash_calls=0;fail_hash=n;r=read();fail_hash=0;Empty(r);Check(r.error==E::hash_failure,"actual system-state hash fault cause");}
    if(profile==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;r=read();allocation_budget=-1;if(r.ok())Check(r.bytes==Oracle(s),"actual allocation sweep exact image");else Empty(r);if(n==na)Check(r.ok(),"actual allocation sweep terminal success");}std::cout<<"system-state allocation sites="<<na<<'\n';}
    for(unsigned n=0;n<5;++n){auto changed=s;if(n==0)changed.object_uuid=Id(99);if(n==1)changed.header.database_uuid=Id(9);if(n==2)changed.header.page_number=12;if(n==3)changed.header.page_generation++;if(n==4)changed.header.filespace_uuid=Id(9);
      Check(db::DecodeNativeSystemState(Oracle(changed)).ok(),"mismatched owner is structurally valid image");put(changed);r=read();Empty(r);Check(r.error==E::binding_mismatch,"exact actual owner reference binding");}
    auto changed=s;changed.checkpoint->page_number=64;put(changed);r=read();Empty(r);Check(r.error==E::invalid_reference,"observed local checkpoint must fit actual capacity");
    put(s);auto bad=ref;bad.kind=2;Empty(db::ReadNativeSystemStateFromOpenDevice(device,Id(1),bad));
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"read-only system-state reopen");Check(read().ok()&&device.read_only(),"native state preserves read-only ownership");
  }}
void Histories(){for(unsigned profile=0;profile<5;++profile){const unsigned other=(profile+1)%5;Fixture fixture;d::FileDevice first,second;
    const auto path1=(fixture.root/"history-primary").string(),path2=(fixture.root/"history-secondary").string();
    const auto make_zero=[&](unsigned p,byte fs,byte page_id){const auto example=Example(p);d::FilespacePageZero z;auto& b=z.bootstrap;
      b.database_uuid=Id(1);b.filespace_uuid=Id(fs);b.page_size_profile_uuid=example.header.page_size_profile_uuid;b.page_size_bytes=example.header.page_size_bytes;
      b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.filespace_role=fs==2?1:2;b.lifecycle_state=1;
      z.page_uuid=Id(page_id);z.creation_operation_uuid=Id(4);z.writer_identity_uuid=Id(5);z.page_generation=7;z.root_set_generation=8;z.total_pages=64;
      constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768};for(unsigned k=1;k<=9;++k)z.roots.push_back({static_cast<u16>(k),types[k],Id(fs),10+k,100+k,b.page_size_profile_uuid,Id(40+k)});return z;};
    const auto z1=make_zero(profile,2,3),z2=make_zero(other,7,8);
    Check(first.Open(path1,d::FileOpenMode::create_new).ok()&&second.Open(path2,d::FileOpenMode::create_new).ok(),"own mixed-profile system-state history devices");
    const auto prepare=[&](auto& device,const auto& z){const auto bytes=d::EncodeFilespacePageZero(z);Check(bytes.ok(),"history page-zero fixture");const byte pad=0;
      Check(device.WriteAt(z.total_pages*z.bootstrap.page_size_bytes-1,&pad,1).ok()&&device.WriteAt(0,bytes.bytes->data(),bytes.bytes->size()).ok()&&device.Sync().ok(),"actual history headers and capacity");};prepare(first,z1);prepare(second,z2);
    std::array<db::NativeSystemState,3> states{Example(profile),Example(other),Example(profile)};
    states[1].header.filespace_uuid=Id(7);
    const auto page_ref=[](const auto& s){return d::NativePageReference{s.header.filespace_uuid,s.header.page_number,s.header.page_generation,s.header.page_size_profile_uuid};};
    const auto root_ref=[&](const auto& s){const auto p=page_ref(s);return d::FilespaceRootReference{1,8,p.filespace_uuid,p.page_number,p.page_generation,p.page_size_profile_uuid,s.object_uuid};};
    for(unsigned i=0;i<3;++i){auto& s=states[i];s.header.page_uuid=Id(70+i);s.header.page_number=11+i;s.header.page_generation=101+i;s.state_generation=1+i;
      s.restart_generation=i?3:2;s.startup_counter=3+i;s.checkpoint=states[0].checkpoint;
      if(i){s.checkpoint_generation=6;s.checkpoint->page_number=20;s.checkpoint->page_generation=110;s.predecessor=page_ref(states[i-1]);}}
    const auto persist=[&](auto values){std::array<Bytes,3> bytes;
      for(unsigned i=0;i<3;++i){if(i)Check(SHA256(bytes[i-1].data(),bytes[i-1].size(),values[i].predecessor_sha256.data())!=nullptr,"independent predecessor complete hash");
        bytes[i]=Oracle(values[i]);auto& device=i==1?second:first;const auto io=device.WriteAt((11+i)*values[i].header.page_size_bytes,bytes[i].data(),bytes[i].size());
        Check(io.ok()&&io.bytes_transferred==bytes[i].size()&&device.Sync().ok(),"persist exact retained system-state image");}
      return bytes;};
    const auto images=persist(states);
    const std::vector<d::NativeFilespaceDevice> devices{{Id(7),z2.bootstrap.page_size_profile_uuid,&second},{Id(2),z1.bootstrap.page_size_profile_uuid,&first}};
    const auto head=root_ref(states[2]),terminal=root_ref(states[0]);const u64 limit=2*z1.bootstrap.page_size_bytes+z2.bootstrap.page_size_bytes;
    const auto read=[&](u64 budget){return db::ReadNativeSystemStateHistoryFromOpenDevices(Id(1),devices,head,terminal,budget);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&r.pages.empty()&&!r.retained_image_bytes,"system-state history failure returns no prefix");};
    reads=hash_calls=0;allocations=0;count_allocations=true;auto r=read(limit);count_allocations=false;const auto nr=reads,nh=hash_calls;const auto na=allocations;
    Check(r.ok()&&r.pages.size()==3&&r.retained_image_bytes==limit&&r.pages[0].bytes==images[2]&&r.pages[1].bytes==images[1]&&r.pages[2].bytes==images[0],"actual immutable mixed-profile system-state history");
    auto same=db::ReadNativeSystemStateHistoryFromOpenDevices(Id(1),devices,head,head,z1.bootstrap.page_size_bytes);
    Check(same.ok()&&same.pages.size()==1&&same.pages[0].bytes==images[2],"exact one-image supplied history range");
    r=read(limit-1);empty(r);Check(r.error==E::resource_exhausted,"history shared image budget");
    for(unsigned n=1;n<=nr;++n){reads=0;fail_read=n;r=read(limit);fail_read=0;empty(r);}
    for(unsigned n=1;n<=nh;++n){hash_calls=0;fail_hash=n;r=read(limit);fail_hash=0;empty(r);Check(r.error==E::hash_failure,"history image and predecessor hash faults");}
    if(profile==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;r=read(limit);allocation_budget=-1;if(r.ok())Check(r.pages.size()==3&&r.retained_image_bytes==limit&&r.pages[0].bytes==images[2]&&r.pages[1].bytes==images[1]&&r.pages[2].bytes==images[0],"history allocation recovery exact images");else empty(r);if(n==na)Check(r.ok(),"history allocation sweep terminal success");}std::cout<<"system-history allocation sites="<<na<<'\n';}
    for(unsigned mutation=0;mutation<9;++mutation){auto changed=states;auto& older=changed[1];auto& newer=changed[2];
      switch(mutation){case 0:newer.state_generation=4;break;case 1:newer.restart_generation=2;break;case 2:newer.startup_counter=3;break;
        case 3:newer.checkpoint_generation=5;break;case 4:newer.checkpoint->page_number=21;break;case 5:newer.checkpoint_object_uuid=Id(96);break;
        case 6:newer.clean_transaction_uuid=Id(94);break;case 7:newer.clean_transaction_uuid={};newer.clean_local_transaction_id=0;break;
        case 8:older.header.page_uuid=newer.header.page_uuid;break;}
      persist(changed);r=read(limit);empty(r);Check(r.error==E::history_mismatch,"resealed inconsistent history refused");}
    auto changed=states;changed[2].lifecycle=L::closed;changed[2].flags=5;changed[2].clean_transaction_uuid=Id(94);changed[2].clean_local_transaction_id=18;
    changed[2].creator_transaction_uuid=Id(94);changed[2].creator_local_transaction_id=18;persist(changed);Check(read(limit).ok(),"closed transition records a new clean transaction observation");
    persist(states);auto altered=images[1];altered[312]^=1;Seal(altered);Check(db::DecodeNativeSystemState(altered).ok(),"resealed altered predecessor remains valid alone");
    Check(second.WriteAt(12*z2.bootstrap.page_size_bytes,altered.data(),altered.size()).ok()&&second.Sync().ok(),"persist altered actual predecessor");r=read(limit);empty(r);Check(r.error==E::invalid_integrity,"complete predecessor digest is authoritative");
    changed=states;changed[1].predecessor=page_ref(states[2]);persist(changed);r=read(limit);empty(r);Check(r.error==E::history_mismatch,"physical history cycle refused");
    persist(states);auto missing=terminal;missing.page_number=14;empty(db::ReadNativeSystemStateHistoryFromOpenDevices(Id(1),devices,head,missing,limit));
    auto duplicates=devices;duplicates.push_back(devices.front());empty(db::ReadNativeSystemStateHistoryFromOpenDevices(Id(1),duplicates,head,terminal,limit));
    auto omitted=devices;omitted.erase(omitted.begin());empty(db::ReadNativeSystemStateHistoryFromOpenDevices(Id(1),omitted,head,terminal,limit));
    Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,d::FileOpenMode::open_existing_read_only).ok(),"read-only history reopen");
    Check(read(limit).ok()&&first.read_only()&&second.read_only(),"reopened owned system-state history");
  }}
}
int main(){try{Codecs();Files();Histories();std::cout<<"PASS system-state checks="<<checks<<" not_SQL_E2E=true\n";return 0;}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
