// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_horizon_root.hpp"
#include "disk_device.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <unistd.h>

namespace {long allocation_budget=-1;unsigned long allocations=0;bool count_allocations=false;
unsigned reads=0,fail_read=0,hash_calls=0,fail_hash=0,full_calls=0,fail_full=0;}
void* operator new(std::size_t n){if(count_allocations)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t offset){++reads;if(fail_read==reads){errno=EIO;return -1;}return __real_pread(fd,p,n,offset);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hash_calls;if(fail_hash==hash_calls)return nullptr;return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* b,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* e){++full_calls;if(fail_full==full_calls)return 0;return __real_EVP_Digest(b,n,out,count,md,e);}
namespace {
namespace p=scratchbird::storage::page;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=p::NativeHorizonError;
std::size_t checks=0;
void Check(bool good,const char* message,std::source_location at=std::source_location::current()){++checks;if(!good)throw std::runtime_error(std::string(message)+" line="+std::to_string(at.line()));}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(i*8));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
std::array<byte,32> Hash(const Bytes& b){std::array<byte,32> out{};Check(SHA256(b.data(),b.size(),out.data())!=nullptr,"independent complete digest");return out;}
void Seal(Bytes& b){std::fill(b.begin()+408,b.begin()+440,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+408);}
Bytes Oracle(const p::NativeHorizonRoot& v){const auto& h=v.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBHOR001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512+160*v.records.size());Put(b,144,v.object_uuid);Num(b,160,8,v.epoch);Put(b,168,v.creator_transaction_uuid);Num(b,184,8,v.creator_local_transaction_id);
  Num(b,192,8,v.flags);Num(b,200,8,v.total_records);Num(b,208,8,v.first_record);Num(b,216,4,v.records.size());Ref(b,224,v.retention);Put(b,272,v.retention_object_uuid);std::copy(v.retention_sha256.begin(),v.retention_sha256.end(),b.begin()+288);
  if(v.next)Ref(b,320,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),b.begin()+368);Num(b,400,8,v.minimum_blocker);
  for(std::size_t i=0;i<v.records.size();++i){const auto at=512+i*160;const auto& r=v.records[i];Num(b,at,2,static_cast<u16>(r.kind));Num(b,at+2,2,static_cast<u16>(r.owner_kind));Num(b,at+4,4,r.flags);Num(b,at+8,8,r.local_boundary);
    Put(b,at+16,r.owner_uuid);Put(b,at+32,r.pin_uuid);Put(b,at+48,r.checkpoint_object_uuid);Num(b,at+64,8,r.checkpoint_generation);Put(b,at+72,r.diagnostic_uuid);if(r.checkpoint)Ref(b,at+88,*r.checkpoint);
  }Seal(b);return b;
}
d::FilespacePageZero Zero(unsigned profile,byte fs){d::FilespacePageZero z;auto& b=z.bootstrap;const auto& registered=d::kCanonicalFilespacePageProfiles[profile];
  b.database_uuid=Id(1);b.filespace_uuid=Id(fs);b.page_size_profile_uuid=registered.uuid;b.page_size_bytes=registered.page_size_bytes;b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.filespace_role=1;b.lifecycle_state=1;
  z.page_uuid=Id(30+fs);z.creation_operation_uuid=Id(40+fs);z.writer_identity_uuid=Id(50+fs);z.page_generation=3;z.root_set_generation=5;z.total_pages=64;
  constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768};for(unsigned k=1;k<=9;++k)z.roots.push_back({static_cast<u16>(k),types[k],Id(fs),10+k,100+k,registered.uuid,Id(60+k)});return z;
}
p::NativeHorizonRoot Example(unsigned profile=0){const auto z=Zero(profile,2);p::NativeHorizonRoot v;
  v.header={z.bootstrap.page_size_bytes,0x302,Id(1),Id(2),Id(20),24,124,0,z.bootstrap.page_size_profile_uuid};v.object_uuid=Id(81);v.epoch=8;v.creator_transaction_uuid=Id(80);v.creator_local_transaction_id=9;
  v.retention={Id(2),23,123,z.bootstrap.page_size_profile_uuid};v.retention_object_uuid=Id(82);v.retention_sha256.fill(0x61);v.total_records=1;v.minimum_blocker=8;
  p::NativeHorizonRecord r;r.flags=1;r.local_boundary=8;r.owner_uuid=Id(90);r.pin_uuid=Id(92);r.checkpoint_object_uuid=Id(69);r.checkpoint_generation=2;r.diagnostic_uuid=Id(93);r.checkpoint=d::NativePageReference{Id(2),19,109,z.bootstrap.page_size_profile_uuid};v.records.push_back(r);return v;
}
void Empty(const p::NativeHorizonResult& r){Check(!r.ok()&&!r.root&&r.bytes.empty(),"failed horizon image has no prefix");}
void Empty(const p::NativeHorizonChainResult& r){Check(!r.ok()&&r.pages.empty()&&!r.retained_image_bytes,"failed horizon chain has no prefix");}
void Images(){
  for(unsigned profile=0;profile<5;++profile)for(unsigned kind=1;kind<=18;++kind)for(unsigned owner=1;owner<=8;++owner){auto v=Example(profile);v.flags=1;auto& r=v.records[0];r.kind=static_cast<p::NativeHorizonKind>(kind);r.owner_kind=static_cast<p::NativeHorizonOwner>(owner);
    const auto encoded=p::EncodeNativeHorizonRoot(v),decoded=p::DecodeNativeHorizonRoot(Oracle(v));if(kind>=11&&owner!=5){Empty(encoded);Empty(decoded);}else Check(encoded.ok()&&decoded.ok()&&encoded.bytes==Oracle(v)&&decoded.bytes==encoded.bytes,"all profiles named kinds owners independent bytes");}
  for(unsigned profile=0;profile<5;++profile){auto v=Example(profile);v.records.clear();v.total_records=0;v.minimum_blocker=0;Check(p::EncodeNativeHorizonRoot(v).ok()&&p::DecodeNativeHorizonRoot(Oracle(v)).ok(),"represent empty image without cleanup grant");
    v=Example(profile);auto r=v.records[0];r.owner_uuid=Id(91);r.local_boundary=4;v.records.push_back(r);v.total_records=2;v.minimum_blocker=4;Check(p::EncodeNativeHorizonRoot(v).bytes==Oracle(v),"minimum blocker is independently computed");
    const unsigned capacity=(v.header.page_size_bytes-512)/160;v.records.clear();v.total_records=capacity;v.minimum_blocker=0;r.flags=0;r.diagnostic_uuid={};
    for(unsigned i=0;i<capacity;++i){r.owner_uuid=Id(0);r.owner_uuid.bytes[14]=i>>8;r.owner_uuid.bytes[15]=i&255;v.records.push_back(r);}
    const auto full=Oracle(v);Check(p::EncodeNativeHorizonRoot(v).bytes==full&&p::DecodeNativeHorizonRoot(full).ok(),"exact maximum record capacity at each profile");
    v.records.push_back(r);v.total_records++;Empty(p::EncodeNativeHorizonRoot(v));auto overflow=full;Num(overflow,216,4,capacity+1);Seal(overflow);Empty(p::DecodeNativeHorizonRoot(overflow));}
  const auto good=Oracle(Example());for(std::size_t i=0;i<good.size();++i){auto b=good;b[i]^=1;Empty(p::DecodeNativeHorizonRoot(b));}
  for(unsigned variant=0;variant<32;++variant){auto v=Example();auto& r=v.records[0];switch(variant){
    case 0:v.object_uuid={};break;case 1:v.epoch=0;break;case 2:v.creator_transaction_uuid.bytes[6]=0x40;break;case 3:v.creator_local_transaction_id=0;break;
    case 4:v.flags=2;break;case 5:v.retention.page_number=0;break;case 6:v.retention_object_uuid={};break;case 7:v.retention_sha256.fill(0);break;
    case 8:v.total_records=0;break;case 9:v.first_record=2;break;case 10:v.total_records=2;break;case 11:v.minimum_blocker=9;break;
    case 12:r.kind=static_cast<p::NativeHorizonKind>(0);break;case 13:r.kind=static_cast<p::NativeHorizonKind>(19);break;case 14:r.owner_kind=static_cast<p::NativeHorizonOwner>(9);break;
    case 15:r.local_boundary=0;break;case 16:r.owner_uuid={};break;case 17:r.pin_uuid.bytes[6]=0x40;break;case 18:r.flags=16;break;case 19:r.flags=4;break;
    case 20:r.diagnostic_uuid={};break;case 21:r.flags=0;v.minimum_blocker=0;break;case 22:r.checkpoint_object_uuid={};break;case 23:r.checkpoint_generation=0;break;
    case 24:r.checkpoint.reset();break;case 25:r.checkpoint->page_number=23;r.checkpoint->page_generation=123;break;
    case 26:v.retention.page_number=24;break;case 27:r.kind=p::NativeHorizonKind::cluster_transaction;r.owner_kind=p::NativeHorizonOwner::cluster;break;
    case 28:r.owner_kind=p::NativeHorizonOwner::cluster;break;case 29:v.records.push_back(r);v.total_records=2;break;
    case 30:r.checkpoint->page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[1].uuid;break;
    case 31:v.total_records=2;v.next=d::NativePageReference{Id(2),24,124,v.header.page_size_profile_uuid};v.next_sha256.fill(1);break;
  }Empty(p::EncodeNativeHorizonRoot(v));Empty(p::DecodeNativeHorizonRoot(Oracle(v)));}
  for(auto offset:{128u,136u,138u,140u,216u,220u,440u,648u,672u}){auto b=good;b[offset]^=1;Seal(b);Empty(p::DecodeNativeHorizonRoot(b));}
  for(unsigned flags=0;flags<16;++flags){auto v=Example();v.records[0].flags=flags;if(!flags)v.records[0].diagnostic_uuid={};v.minimum_blocker=(flags&1)?8:0;
    const auto r=p::DecodeNativeHorizonRoot(Oracle(v));if((flags&4)&&!(flags&1))Empty(r);else Check(r.ok(),"defined flags and reason pair");}
  auto v=Example();v.records[0].pin_uuid={};v.records[0].checkpoint.reset();v.records[0].checkpoint_generation=0;v.records[0].checkpoint_object_uuid={};Check(p::DecodeNativeHorizonRoot(Oracle(v)).ok(),"optional references entirely absent");
}
struct Fixture {std::filesystem::path root;Fixture(){std::string pattern=(std::filesystem::temp_directory_path()/"sb_horizon.XXXXXX").string();Check(::mkdtemp(pattern.data())!=nullptr,"isolated horizon fixture");root=pattern;}~Fixture(){std::error_code error;std::filesystem::remove_all(root,error);}};
void Chains(){for(unsigned profile=0;profile<5;++profile){const auto other=(profile+1)%5;Fixture fixture;d::FileDevice first,second;const auto path1=(fixture.root/"first").string(),path2=(fixture.root/"second").string();
  auto z1=Zero(profile,2),z2=Zero(other,7);Check(first.Open(path1,d::FileOpenMode::create_new).ok()&&second.Open(path2,d::FileOpenMode::create_new).ok(),"own both horizon filespaces");
  const byte pad=0;Check(first.WriteAt(z1.total_pages*z1.bootstrap.page_size_bytes-1,&pad,1).ok()&&second.WriteAt(z2.total_pages*z2.bootstrap.page_size_bytes-1,&pad,1).ok(),"allocate actual horizon capacities");
  auto head=Example(profile),tail=head;head.total_records=tail.total_records=2;tail.first_record=1;tail.header.filespace_uuid=Id(7);tail.header.page_size_profile_uuid=z2.bootstrap.page_size_profile_uuid;tail.header.page_size_bytes=z2.bootstrap.page_size_bytes;tail.header.page_uuid=Id(21);tail.header.page_number=25;tail.header.page_generation=125;tail.records[0].kind=p::NativeHorizonKind::oat;
  head.next=d::NativePageReference{Id(7),25,125,z2.bootstrap.page_size_profile_uuid};const auto reference=d::NativePageReference{Id(2),24,124,z1.bootstrap.page_size_profile_uuid};
  const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& bytes){const auto io=file.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&file.Sync().ok(),"persist actual independently encoded image");};
const auto persist=[&](auto a,auto b){const auto bytes=Oracle(b);if(a.next)a.next_sha256=Hash(bytes);else a.next_sha256.fill(0);const auto zero1=d::EncodeFilespacePageZero(z1),zero2=d::EncodeFilespacePageZero(z2);Check(zero1.ok()&&zero2.ok(),"encode enclosing bootstrap fixtures");put(first,0,z1.bootstrap.page_size_bytes,*zero1.bytes);put(second,0,z2.bootstrap.page_size_bytes,*zero2.bytes);put(first,24,z1.bootstrap.page_size_bytes,Oracle(a));put(second,25,z2.bootstrap.page_size_bytes,bytes);};
  const std::vector<d::NativeFilespaceDevice> devices{{Id(7),z2.bootstrap.page_size_profile_uuid,&second},{Id(2),z1.bootstrap.page_size_profile_uuid,&first}};
  const u64 budget=z1.bootstrap.page_size_bytes+z2.bootstrap.page_size_bytes;const auto read=[&](u64 limit){return p::ReadNativeHorizonRootFromOpenDevices(Id(1),devices,Id(81),reference,limit);};
  persist(head,tail);reads=hash_calls=full_calls=0;allocations=0;count_allocations=true;auto result=read(budget);count_allocations=false;const auto nr=reads,nh=hash_calls,nf=full_calls;const auto na=allocations;
  auto expected_head=head;expected_head.next_sha256=Hash(Oracle(tail));Check(result.ok()&&result.pages.size()==2&&result.retained_image_bytes==budget&&result.pages[0].bytes==Oracle(expected_head)&&result.pages[1].bytes==Oracle(tail),"actual mixed-profile complete horizon chain");
  Empty(read(budget-1));Empty(read(0));
  for(unsigned n=1;n<=nr;++n){reads=0;fail_read=n;result=read(budget);fail_read=0;Empty(result);Check(result.error==E::io_failure,"nested actual read failure cause");}
  for(unsigned n=1;n<=nh;++n){hash_calls=0;fail_hash=n;result=read(budget);fail_hash=0;Empty(result);Check(result.error==E::hash_failure,"actual hash failure cause");}
  for(unsigned n=1;n<=nf;++n){full_calls=0;fail_full=n;result=read(budget);fail_full=0;Empty(result);Check(result.error==E::hash_failure,"actual full-digest failure cause");}
  if(profile==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;result=read(budget);allocation_budget=-1;if(result.ok())Check(result.pages.size()==2&&result.retained_image_bytes==budget&&result.pages[0].bytes==Oracle(expected_head)&&result.pages[1].bytes==Oracle(tail),"complete allocation recovery");else Empty(result);if(n==na)Check(result.ok(),"allocation sweep reaches terminal success");}std::cout<<"horizon chain allocation sites="<<na<<" read sites="<<nr<<" hash sites="<<nh<<std::endl;}
  for(unsigned variant=0;variant<12;++variant){auto a=head,b=tail;switch(variant){case 0:b.epoch++;break;case 1:b.creator_transaction_uuid=Id(91);break;case 2:b.creator_local_transaction_id++;break;case 3:b.flags=1;break;case 4:b.retention_sha256[0]^=1;break;case 5:b.records[0].kind=p::NativeHorizonKind::oit;break;case 6:b.header.page_uuid=a.header.page_uuid;break;case 7:b.header.page_uuid=z1.page_uuid;break;case 8:b.retention.page_number=64;break;case 9:b.records[0].checkpoint->page_number=64;break;case 10:b.records[0].checkpoint_object_uuid=Id(99);break;case 11:b.header.page_generation++;break;}persist(a,b);Empty(read(budget));}
  persist(head,tail);auto changed=tail;changed.records[0].local_boundary=7;changed.minimum_blocker=7;put(second,25,z2.bootstrap.page_size_bytes,Oracle(changed));result=read(budget);Empty(result);Check(result.error==E::invalid_integrity,"resealed altered actual successor full digest");
  auto a=head,b=tail;a.total_records=b.total_records=3;b.next=reference;b.next_sha256.fill(1);persist(a,b);result=read(2*budget);Empty(result);Check(result.error==E::chain_mismatch,"actual horizon cycle");
  persist(head,tail);auto duplicate=devices;duplicate.push_back(devices[0]);Empty(p::ReadNativeHorizonRootFromOpenDevices(Id(1),duplicate,Id(81),reference,budget));
  auto missing=devices;missing.erase(missing.begin());Empty(p::ReadNativeHorizonRootFromOpenDevices(Id(1),missing,Id(81),reference,budget));
  auto single=head;single.total_records=1;single.next.reset();persist(single,tail);Check(read(budget).ok(),"single image still validates unused supplied device");
  z2.bootstrap.database_uuid=Id(4);persist(single,tail);Empty(read(budget));z2.bootstrap.database_uuid=Id(1);persist(head,tail);
  Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,d::FileOpenMode::open_existing_read_only).ok(),"reopen actual horizon files read only");Check(read(budget).ok()&&first.read_only()&&second.read_only(),"retained read-only horizon chain");
}}
}
int main(){try{Images();Chains();std::cout<<"horizon image/chain checks="<<checks<<" failures=0\n";return 0;}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
