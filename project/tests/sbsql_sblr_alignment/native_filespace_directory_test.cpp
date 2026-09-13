// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_directory.hpp"
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
namespace {
long allocation_budget=-1;
bool count_allocations=false;
unsigned long allocations=0;
unsigned reads=0,fail_read=0,hash_calls=0,fail_hash=0;
}
void* operator new(std::size_t n){if(count_allocations)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;if(fail_read==reads){errno=EIO;return -1;}return __real_pread(fd,p,n,o);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hash_calls;if(fail_hash==hash_calls)return nullptr;return __real_EVP_MD_CTX_new();}
namespace {
namespace p=scratchbird::storage::page;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=p::NativeDirectoryError;
std::size_t checks=0;
void Check(bool good,const char* message,std::source_location at=std::source_location::current()){
  ++checks;if(!good)throw std::runtime_error(std::string(message)+" line="+std::to_string(at.line()));}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(i*8));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
std::array<byte,32> Hash(const Bytes& b){std::array<byte,32> out{};Check(SHA256(b.data(),b.size(),out.data())!=nullptr,"independent hash");return out;}
void Seal(Bytes& b){std::fill(b.begin()+296,b.begin()+328,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+296);}
Bytes Oracle(const p::NativeFilespaceDirectory& value){const auto& h=value.header;Bytes b(h.page_size_bytes,0);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n("SBFDIR01",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+192*value.records.size());
  Put(b,144,value.object_uuid);Num(b,160,8,value.directory_generation);Put(b,168,value.creator_transaction_uuid);Num(b,184,8,value.creator_local_transaction_id);
  Num(b,192,8,value.total_records);Num(b,200,8,value.first_record);Num(b,208,4,value.records.size());if(value.next)Ref(b,216,*value.next);
  std::copy(value.next_sha256.begin(),value.next_sha256.end(),b.begin()+264);
  for(std::size_t i=0;i<value.records.size();++i){const auto at=384+i*192;const auto& r=value.records[i];const auto& a=r.bootstrap;
    Put(b,at,a.filespace_uuid);Put(b,at+16,a.page_size_profile_uuid);Put(b,at+32,a.checksum_profile_uuid);Put(b,at+48,a.encryption_profile_uuid);
    Put(b,at+64,r.locator_uuid);Put(b,at+80,r.page_zero_uuid);Num(b,at+96,8,r.page_zero_generation);Num(b,at+104,8,r.root_set_generation);
    Num(b,at+112,8,r.total_pages);Num(b,at+120,8,r.verification_epoch);Num(b,at+128,2,a.filespace_role);Num(b,at+130,2,a.lifecycle_state);
    Num(b,at+132,4,a.flags);Num(b,at+136,4,a.page_size_bytes);Num(b,at+140,4,a.durable_format_generation);if(r.operation)Ref(b,at+144,*r.operation);
  }Seal(b);return b;
}
d::FilespacePageZero Zero(unsigned profile,byte fs){d::FilespacePageZero z;auto& b=z.bootstrap;const auto& registered=d::kCanonicalFilespacePageProfiles[profile];
  b.database_uuid=Id(1);b.filespace_uuid=Id(fs);b.page_size_profile_uuid=registered.uuid;b.page_size_bytes=registered.page_size_bytes;
  b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.filespace_role=1;b.lifecycle_state=1;
  z.page_uuid=Id(30+fs);z.creation_operation_uuid=Id(40+fs);z.writer_identity_uuid=Id(50+fs);z.page_generation=3;z.root_set_generation=5;z.total_pages=64;
  constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768};
  for(unsigned k=1;k<=9;++k)z.roots.push_back({static_cast<u16>(k),types[k],Id(fs),10+k,100+k,registered.uuid,Id(60+k)});
  return z;
}
p::NativeFilespaceDirectory Example(unsigned profile=0){auto zero=Zero(profile,2);p::NativeFilespaceDirectory value;
  value.header={zero.bootstrap.page_size_bytes,9,Id(1),Id(2),Id(20),15,105,0,zero.bootstrap.page_size_profile_uuid};
  value.object_uuid=Id(65);value.directory_generation=8;value.creator_transaction_uuid=Id(80);value.creator_local_transaction_id=9;value.total_records=1;
  value.records.push_back({zero.bootstrap,Id(90),zero.page_uuid,zero.page_generation,zero.root_set_generation,zero.total_pages,0,{}});return value;
}
void Empty(const p::NativeFilespaceDirectoryResult& r){Check(!r.ok()&&!r.directory&&r.bytes.empty(),"no failed directory prefix");}
void Empty(const p::NativeFilespaceDirectoryChainResult& r){Check(!r.ok()&&r.pages.empty()&&!r.retained_image_bytes,"no failed chain prefix");}
void Invalid(const p::NativeFilespaceDirectory& v){Empty(p::EncodeNativeFilespaceDirectory(v));Empty(p::DecodeNativeFilespaceDirectory(Oracle(v)));}
void Codecs(){for(unsigned profile=0;profile<5;++profile){auto v=Example(profile);
    for(unsigned role=1;role<=14;++role)for(unsigned state=1;state<=15;++state){v.records[0].bootstrap.filespace_role=role;v.records[0].bootstrap.lifecycle_state=state;
      const auto b=Oracle(v);auto r=p::EncodeNativeFilespaceDirectory(v);Check(r.ok()&&r.bytes==b,"independent all-profile directory encoding");
      r=p::DecodeNativeFilespaceDirectory(b);Check(r.ok()&&Oracle(*r.directory)==b,"independent all-state directory decoding");}
    v=Example(profile);v.records[0].bootstrap.flags=3;v.records[0].bootstrap.encryption_profile_uuid=Id(99);
    v.records[0].operation=d::NativePageReference{Id(2),25,3,v.header.page_size_profile_uuid};
    Check(p::DecodeNativeFilespaceDirectory(Oracle(v)).ok(),"encrypted cluster declaration and operation structurally representable");
  }
  const auto good=Oracle(Example());for(std::size_t i=0;i<good.size();++i){auto b=good;b[i]^=1;Empty(p::DecodeNativeFilespaceDirectory(b));}
  for(unsigned at:{128u,136u,138u,140u,212u,328u,383u,8191u}){auto b=good;b[at]^=1;Seal(b);Empty(p::DecodeNativeFilespaceDirectory(b));}
  for(unsigned change=0;change<22;++change){auto v=Example();auto& r=v.records.front();
    switch(change){case 0:v.header.page_type=5;break;case 1:v.header.flags=1;break;case 2:v.object_uuid={};break;
      case 3:v.creator_transaction_uuid.bytes[6]=0x40;break;case 4:v.directory_generation=0;break;case 5:v.creator_local_transaction_id=0;break;
      case 6:v.total_records=0;break;case 7:v.first_record=1;break;case 8:r.locator_uuid={};break;case 9:r.page_zero_uuid=v.header.page_uuid;break;
      case 10:r.page_zero_generation=0;break;case 11:r.root_set_generation=0;break;case 12:r.total_pages=0;break;
      case 13:r.total_pages=std::numeric_limits<u64>::max();break;case 14:r.bootstrap.filespace_role=15;break;case 15:r.bootstrap.lifecycle_state=0;break;
      case 16:r.bootstrap.flags=4;break;case 17:r.bootstrap.encryption_profile_uuid=Id(8);break;case 18:r.bootstrap.page_size_bytes=4096;break;
      case 19:r.bootstrap.database_uuid=Id(9);Empty(p::EncodeNativeFilespaceDirectory(v));continue;
      case 20:v.total_records=2;v.records.push_back(r);break;case 21:r.operation=d::NativePageReference{Id(2),0,1,v.header.page_size_profile_uuid};break;}
    Invalid(v);
  }
  hash_calls=0;fail_hash=1;Empty(p::DecodeNativeFilespaceDirectory(good));fail_hash=0;
  for(bool encode:{false,true}){count_allocations=true;allocations=0;auto r=encode?p::EncodeNativeFilespaceDirectory(Example()):p::DecodeNativeFilespaceDirectory(good);count_allocations=false;
    Check(r.ok(),"measure codec allocations");const auto total=allocations;
    for(unsigned long n=0;n<total;++n){auto v=Example();allocation_budget=n;r=encode?p::EncodeNativeFilespaceDirectory(v):p::DecodeNativeFilespaceDirectory(good);allocation_budget=-1;
      if(!r.ok())Empty(r);else Check(r.bytes==good,"allocation recovery has exact image");}}
}
struct Fixture{std::filesystem::path root;Fixture(){char pattern[]="/tmp/sb-directory-test-XXXXXX";const auto p=mkdtemp(pattern);Check(p,"owned temporary fixture");root=p;}
  ~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}};
void Chains(){for(unsigned profile=0;profile<5;++profile){Fixture fixture;d::FileDevice first,second;auto z1=Zero(profile,2),z2=Zero((profile+1)%5,3);
    const auto path1=(fixture.root/"first").string(),path2=(fixture.root/"second").string();Check(first.Open(path1,d::FileOpenMode::create_new).ok()&&second.Open(path2,d::FileOpenMode::create_new).ok(),"owned mixed-profile filespaces");
    const auto prepare=[&](auto& device,const auto& z){const auto b=d::EncodeFilespacePageZero(z);Check(b.ok(),"page-zero fixture encoding");const byte pad=0;
      Check(device.WriteAt(z.total_pages*z.bootstrap.page_size_bytes-1,&pad,1).ok()&&device.WriteAt(0,b.bytes->data(),b.bytes->size()).ok()&&device.Sync().ok(),"actual filespace header and capacity");};prepare(first,z1);prepare(second,z2);
    auto head=Example(profile),tail=Example((profile+1)%5);tail.header.filespace_uuid=Id(3);tail.header.page_uuid=Id(21);
    tail.records={{z2.bootstrap,Id(91),z2.page_uuid,z2.page_generation,z2.root_set_generation,z2.total_pages,4,{}}};tail.total_records=2;tail.first_record=1;
    head.total_records=2;head.next=d::NativePageReference{Id(3),15,105,z2.bootstrap.page_size_profile_uuid};head.next_sha256=Hash(Oracle(tail));
    const auto put=[&](auto& device,const auto& image){const auto b=Oracle(image);const auto io=device.WriteAt(image.header.page_number*image.header.page_size_bytes,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size()&&device.Sync().ok(),"actual directory page persistence");};
    const auto persist=[&](auto h,const auto& t,bool reseal=true){if(reseal)h.next_sha256=Hash(Oracle(t));put(first,h);put(second,t);};persist(head,tail);
    const std::vector<d::NativeFilespaceDevice> devices{{Id(3),z2.bootstrap.page_size_profile_uuid,&second},{Id(2),z1.bootstrap.page_size_profile_uuid,&first}};
    const d::FilespaceRootReference ref{5,9,Id(2),15,105,z1.bootstrap.page_size_profile_uuid,Id(65)};const u64 limit=z1.bootstrap.page_size_bytes+z2.bootstrap.page_size_bytes;
    const auto read=[&](u64 budget){return p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),devices,ref,budget);};
    reads=hash_calls=0;allocations=0;count_allocations=true;auto r=read(limit);count_allocations=false;const auto nr=reads,nh=hash_calls;const auto na=allocations;
    Check(r.ok()&&r.pages.size()==2&&r.retained_image_bytes==limit&&r.pages[0].bytes==Oracle(head)&&r.pages[1].bytes==Oracle(tail),"actual complete directory and headers");Empty(read(limit-1));
    for(unsigned fault=1;fault<=nr;++fault){reads=0;fail_read=fault;r=read(limit);fail_read=0;Empty(r);}
    for(unsigned fault=1;fault<=nh;++fault){hash_calls=0;fail_hash=fault;r=read(limit);fail_hash=0;Empty(r);Check(r.error==E::hash_failure,"exact nested directory hash failure");}
    if(profile==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;r=read(limit);allocation_budget=-1;
        if(r.ok())Check(r.pages[0].bytes==Oracle(head)&&r.pages[1].bytes==Oracle(tail),"allocation sweep valid complete chain");else Empty(r);
        if(n==na)Check(r.ok(),"allocation sweep reaches measured terminal success");}std::cout<<"directory allocation sites="<<na<<'\n';}
    for(unsigned change=0;change<16;++change){auto t=tail;auto h=head;switch(change){
      case 0:t.directory_generation++;break;case 1:t.creator_transaction_uuid=Id(82);break;case 2:t.creator_local_transaction_id++;break;
      case 3:t.records[0].page_zero_generation++;break;case 4:t.records[0].root_set_generation++;break;case 5:t.records[0].total_pages++;break;
      case 6:t.records[0].bootstrap.lifecycle_state=2;break;case 7:t.records[0].bootstrap.filespace_role=2;break;
      case 8:t.records[0].page_zero_uuid=Id(34);break;case 9:t.records[0].bootstrap.filespace_uuid=Id(4);break;
      case 10:t.records[0].page_zero_uuid=h.records[0].page_zero_uuid;break;case 11:t.header.page_uuid=h.header.page_uuid;break;
      case 12:t.records[0].operation=d::NativePageReference{Id(2),64,3,z1.bootstrap.page_size_profile_uuid};break;
      case 13:t.records[0].bootstrap.filespace_uuid=Id(2);break;
      case 14:t.header.database_uuid=Id(5);t.records[0].bootstrap.database_uuid=Id(5);break;
      case 15:t.header.page_generation++;break;}
      Check(p::DecodeNativeFilespaceDirectory(Oracle(t)).ok(),"wrong chain fact independently valid image");persist(h,t);Empty(read(limit));}
    auto t=tail;t.records[0].verification_epoch++;persist(head,t,false);r=read(limit);Empty(r);Check(r.error==E::invalid_integrity,"exact successor hash binding");
    auto h=head;t=tail;h.total_records=t.total_records=3;
    t.next=d::NativePageReference{Id(2),15,105,z1.bootstrap.page_size_profile_uuid};t.next_sha256.fill(1);
    persist(h,t);r=read(limit+z1.bootstrap.page_size_bytes);Empty(r);Check(r.error==E::chain_mismatch,"actual continuation cycle rejected");
    t.first_record=0;persist(h,t);r=read(limit);Empty(r);Check(r.error==E::chain_mismatch,"actual discontinuous ordinal rejected");
    h=head;t=tail;h.total_records=t.total_records=3;
    auto closed=t.records[0];closed.bootstrap.filespace_uuid=Id(4);closed.bootstrap.filespace_role=12;closed.bootstrap.lifecycle_state=3;
    closed.page_zero_uuid=Id(34);closed.locator_uuid=Id(92);t.records.push_back(closed);
    persist(h,t);r=read(limit);Check(r.ok()&&r.pages.back().directory->records.size()==2,"closed directory entry retained without opening locator");
    for(const auto& alias:{head.records[0].page_zero_uuid,head.header.page_uuid}){
      auto duplicate_zero=t;duplicate_zero.records.back().page_zero_uuid=alias;
      Check(p::DecodeNativeFilespaceDirectory(Oracle(duplicate_zero)).ok(),"cross-image closed alias is individually valid");
      persist(h,duplicate_zero);r=read(limit);Empty(r);Check(r.error==E::chain_mismatch,"closed entry cannot alias retained page or page-zero identity");
    }
    auto missing=devices;missing.erase(missing.begin());Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),missing,ref,limit));
    auto duplicate=devices;duplicate.push_back(devices.front());Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),duplicate,ref,limit));
    persist(head,tail);Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,d::FileOpenMode::open_existing_read_only).ok(),"reopen owned read-only filespaces");
    Check(read(limit).ok()&&first.read_only()&&second.read_only(),"directory read-only reopen");
  }}
}
int main(){try{Codecs();Chains();std::cout<<"PASS directory checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
