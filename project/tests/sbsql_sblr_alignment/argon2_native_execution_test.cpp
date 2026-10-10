// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "argon2_kdf.hpp"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>
#include <thread>
#include <vector>
namespace c=scratchbird::core::crypto;
using Bytes=std::vector<unsigned char>;
thread_local bool deny=false,after_cleanup=false;
thread_local unsigned allocation_attempts=0;
thread_local c::Argon2Output observed_workspace{};
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t,std::size_t);
extern "C" void* __real_realloc(void*,std::size_t);
extern "C" void __real_OPENSSL_cleanse(void*,std::size_t);
extern "C" void* __wrap_malloc(std::size_t n){if(deny){++allocation_attempts;return nullptr;}return __real_malloc(n);}
extern "C" void* __wrap_calloc(std::size_t n,std::size_t s){if(deny){++allocation_attempts;return nullptr;}return __real_calloc(n,s);}
extern "C" void* __wrap_realloc(void* p,std::size_t n){if(deny){++allocation_attempts;return nullptr;}return __real_realloc(p,n);}
extern "C" void __wrap_OPENSSL_cleanse(void* p,std::size_t n){
  __real_OPENSSL_cleanse(p,n);
  if(observed_workspace.data&&static_cast<unsigned char*>(p)+n==observed_workspace.data+observed_workspace.size)after_cleanup=true;
}
void* operator new(std::size_t n){if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
Bytes Hex(std::string_view s){Bytes out;auto nib=[](char x){return x<='9'?x-'0':x-'a'+10;};for(std::size_t i=0;i<s.size();i+=2)out.push_back(nib(s[i])*16+nib(s[i+1]));return out;}
c::Argon2Input In(const Bytes& b){return {b.data(),b.size()};}
bool Zero(c::Argon2Output b){return std::all_of(b.data,b.data+b.size,[](auto n){return n==0;});}
struct Buffers {
  Bytes backing,key;
  c::Argon2Output workspace,output;
  Buffers(c::Argon2idParameters p,unsigned bytes):key(bytes+32,0xa5){
    const auto size=c::Argon2idWorkspaceBytes(p);Check(size.code==c::Argon2Code::ok,"fixture extent valid");
    backing.assign(size.bytes+32,0xa5);workspace={backing.data()+16,static_cast<std::size_t>(size.bytes)};output={key.data()+16,bytes};
  }
  void Arm(){observed_workspace=workspace;after_cleanup=false;allocation_attempts=0;deny=true;}
  void Done(bool success,bool accepted=true){
    deny=false;observed_workspace={};Check(!allocation_attempts,"algorithm and cleanup perform no heap allocation");
    if(accepted){Check(Zero(workspace),"workspace fully erased");if(!success)Check(Zero(output),"failed key fully erased");}
    for(const auto* v:{&backing,&key})Check(std::all_of(v->begin(),v->begin()+16,[](auto x){return x==0xa5;})&&
      std::all_of(v->end()-16,v->end(),[](auto x){return x==0xa5;}),"both redzones unchanged");
  }
};
void KnownAnswers(){
  { // Independent RFC9106 section5.3 with secret and associated data.
    const Bytes password(32,1),salt(16,2),secret(8,3),ad(12,4);Buffers b({32,3,4},32);
    b.Arm();const auto code=c::DeriveArgon2idKey({32,3,4},In(password),In(salt),b.workspace,b.output,{},In(secret),In(ad));b.Done(true);
    const auto expected=Hex("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
    Check(code==c::Argon2Code::ok&&std::equal(expected.begin(),expected.end(),b.output.data),"RFC9106 complete Argon2id known answer");
  }
  // Independently generated with installed libargon2.so.1 argon2id_hash_raw,
  // not this provider. Password[i]=i*19 mod256, salt[i]=i for16 bytes.
  struct Vector {unsigned memory,passes,lanes,password,output;const char* hex;};
  const Vector vectors[]{
    {65536,3,4,8,32,"c3c8ba005aa7551e15570f40add4fd8501de6fa8300fb0bee0b0b72f2f1d86a3"},
    {8,1,1,0,4,"5784f750"},
    {9,2,1,129,65,"6dcc808693f0933d8bacc31850b2b3965b74a0f2ed0c36f229cf2db87c52373f33c2980944c21cec1a0d07fc0214c01f5792b41b48d775a33554a375a67b6a4e0f"},
    {37,3,4,4097,96,"5e52cf5ddc8e02480efde39cef2ad21ca7bc5e8fe5a3d1d47c514d1750380ec67200ec13dba524791b0b093b259a3eca71a3d24465c8dc5101a6c25991d12ffaea2879ed0234e6cb8147c86475f8e7d5506bf47fae1fe3f44abe487e607b642a"},
    {48,2,3,128,64,"238899ab4b15e1d3382cb336d88ddc7ef5f34edb5301e1d17ef5dadd633e931ff7a88b19c55bdb0ae4eb5b16f1afddbe98b068ac95fa84df606414778beb8b62"},
    {32,1,4,1,65535,"d85b5a96fa5ee9c40d942d5a9c04e40e22c71b8a31694bfa74c05be85cbecf69"}
  };
  for(const auto& v:vectors){
    const c::Argon2idParameters p{v.memory,v.passes,v.lanes};Buffers b(p,v.output);Bytes password(v.password),salt(16);
    for(unsigned i=0;i<password.size();++i)password[i]=i*19;for(unsigned i=0;i<16;++i)salt[i]=i;
    b.Arm();const auto code=c::DeriveArgon2idKey(p,In(password),In(salt),b.workspace,b.output);b.Done(true);
    const auto expected=Hex(v.hex);std::array<unsigned char,32> digest{};
    const auto* actual=b.output.data;if(v.output==65535){Check(SHA256(actual,v.output,digest.data())!=nullptr,"independent output digest");actual=digest.data();}
    Check(code==c::Argon2Code::ok&&std::equal(expected.begin(),expected.end(),actual),"independent default/boundary answer");
  }
  Check(c::Argon2idParameters{}.memory_kib==65536&&c::Argon2idParameters{}.passes==3&&c::Argon2idParameters{}.lanes==4,"owner approved native defaults");
}
struct Probe {
  unsigned calls=0,stop=0;bool throws=false;
  static bool Poll(void* raw){auto& p=*static_cast<Probe*>(raw);if(++p.calls!=p.stop)return false;if(p.throws)throw p.stop;return true;}
};
void Cancellation(){
  const c::Argon2idParameters p{1024,2,2};const Bytes password(8193,0x39),salt(16,0x81);
  Buffers normal(p,65);Probe baseline;normal.Arm();const auto initial=c::DeriveArgon2idKey(p,In(password),In(salt),normal.workspace,normal.output,{Probe::Poll,&baseline});normal.Done(true);
  Check(initial==c::Argon2Code::ok&&baseline.calls>40,"actual memory fill has bounded cancellation probes");
  for(bool throws:{false,true})for(unsigned at=1;at<=baseline.calls;++at){
    Buffers b(p,65);Probe probe{0,at,throws};bool caught=false;c::Argon2Code code{};
    b.Arm();try{code=c::DeriveArgon2idKey(p,In(password),In(salt),b.workspace,b.output,{Probe::Poll,&probe});}catch(unsigned n){caught=n==at;}b.Done(false);
    Check(probe.calls==at&&(throws?caught:code==c::Argon2Code::cancelled),"every probe cancels/throws with full cleanup including C callbacks");
  }
  Buffers b(p,32);b.Arm();const auto code=c::DeriveArgon2idKey(p,{},In(salt),b.workspace,b.output,{[](void*){return after_cleanup;},nullptr});b.Done(false);
  Check(after_cleanup&&code==c::Argon2Code::cancelled,"cancel during erasure precedes publication");
}
void Invalid(){
  for(unsigned fault=0;fault<17;++fault){
    c::Argon2idParameters p{32,1,4};Buffers b(p,32);auto work=b.workspace,out=b.output;Bytes data(16,0x33);auto pwd=In(data),salt=In(data);
    if(fault==0)work.size--;if(fault==1)work.data++;if(fault==2)work.data=nullptr;
    if(fault==3)out.data=nullptr;if(fault==4)out.data=work.data;if(fault==5)pwd={nullptr,1};
    if(fault==6)salt={nullptr,1};if(fault==7)pwd={work.data,1};if(fault==8)salt={out.data,1};
    if(fault==9)pwd={reinterpret_cast<const unsigned char*>(UINTPTR_MAX-1),3};
    if(fault==10)p.passes=0;if(fault==11)p.lanes=0;if(fault==12)p.lanes=0x1000000;
    if(fault==13)p.memory_kib=31;if(fault==14)out.size=3;if(fault==15)out.size=65536;
    if(fault==16&&sizeof(std::size_t)>4)pwd={data.data(),std::size_t(UINT32_MAX)+1};
    if(fault==16&&sizeof(std::size_t)==4)continue;
    const auto before=b.backing,key=b.key;Probe probe;b.Arm();const auto code=c::DeriveArgon2idKey(p,pwd,salt,work,out,{Probe::Poll,&probe});b.Done(false,false);
    Check(code!=c::Argon2Code::ok&&before==b.backing&&key==b.key&&!probe.calls,"invalid parameters/extents refuse without effects or callbacks");
  }
}
void Concurrent(){
  std::atomic<bool> good=true;std::vector<std::thread> threads;
  const auto expected=Hex("5784f750");
  for(unsigned i=0;i<4;++i)threads.emplace_back([&]{
    Bytes workspace(8192),out(4),salt(16);for(unsigned n=0;n<16;++n)salt[n]=n;
    for(unsigned repeat=0;repeat<8;++repeat)if(c::DeriveArgon2idKey({8,1,1},{},In(salt),{workspace.data(),workspace.size()},{out.data(),out.size()})!=c::Argon2Code::ok||out!=expected)good=false;
  });
  for(auto& thread:threads)thread.join();Check(good,"independent concurrent operations share no mutable crypto state");
}
int main(){KnownAnswers();Cancellation();Invalid();Concurrent();std::printf("PASS Argon2id checks=%u\n",checks);}
