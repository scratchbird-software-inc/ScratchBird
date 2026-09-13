// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "scrypt_kdf.hpp"
#include "kdf_resource_governor.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace c=scratchbird::core::crypto;
namespace m=scratchbird::core::memory;
thread_local bool watch=false;
thread_local unsigned allocations=0,workspace_erases=0,output_erases=0,scratch_erases=0;
thread_local bool erasures_zero=true;
thread_local bool cancel_after_workspace_cleanup=false,cleanup_requested_cancel=false;
thread_local c::ScryptOutput watched_workspace{},watched_output{};
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t,std::size_t);
extern "C" void* __real_realloc(void*,std::size_t);
extern "C" void __real_OPENSSL_cleanse(void*,std::size_t);
extern "C" void* __wrap_malloc(std::size_t n){if(watch){++allocations;return nullptr;}return __real_malloc(n);}
extern "C" void* __wrap_calloc(std::size_t n,std::size_t s){if(watch){++allocations;return nullptr;}return __real_calloc(n,s);}
extern "C" void* __wrap_realloc(void* p,std::size_t n){if(watch){++allocations;return nullptr;}return __real_realloc(p,n);}
void* operator new(std::size_t n){if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" void __wrap_OPENSSL_cleanse(void* data,std::size_t bytes){
  __real_OPENSSL_cleanse(data,bytes);
  if(!watch)return;
  if(data==watched_workspace.data&&bytes==watched_workspace.size){++workspace_erases;if(cancel_after_workspace_cleanup)cleanup_requested_cancel=true;}
  else if(data==watched_output.data&&bytes==watched_output.size)++output_erases;
  else if(bytes==c::ScryptFixedScratchBytes())++scratch_erases;
  else {erasures_zero=false;return;}
  const auto* p=static_cast<const unsigned char*>(data);
  for(std::size_t i=0;i<bytes;++i)if(p[i])erasures_zero=false;
}
unsigned checks=0,failures=0;
void Check(bool ok,const char* message){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",message);}}
bool Zero(c::ScryptOutput bytes){return std::all_of(bytes.data,bytes.data+bytes.size,[](auto b){return b==0;});}
std::vector<unsigned char> Hex(std::string_view text){
  std::vector<unsigned char> out;auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(std::size_t i=0;i<text.size();i+=2)out.push_back(static_cast<unsigned char>(16*digit(text[i])+digit(text[i+1])));
  return out;
}
struct Buffers {
  std::vector<unsigned char> workspace,output;
  c::ScryptOutput work,key;
  Buffers(std::uint64_t n,unsigned r,unsigned p,unsigned length){
    c::ScryptWorkEstimate cost;Check(c::EstimateScryptWork(0,0,n,r,p,length,cost)==c::ScryptEstimateCode::ok,"test tuple has representable workspace");
    workspace.assign(cost.workspace_bytes+34,0xa5);output.assign(length+34,0xa5);
    work={workspace.data()+17,static_cast<std::size_t>(cost.workspace_bytes)};key={output.data()+17,length};
  }
  void Arm(){allocations=workspace_erases=output_erases=scratch_erases=0;erasures_zero=true;cleanup_requested_cancel=false;watched_workspace=work;watched_output=key;watch=true;}
  void Done(bool success,bool accepted=true){
    watch=false;
    Check(allocations==0,"native derivation and cleanup allocate neither C nor C++ heap memory");
    Check(erasures_zero&&workspace_erases==(accepted?1u:0u)&&scratch_erases==(accepted?1u:0u)&&output_erases==(!success&&accepted?1u:0u),"all owned secret buffers are fully erased on the required exits");
    if(accepted)Check(Zero(work),"workspace is zero after actual computation/cancellation");
    if(accepted&&!success)Check(Zero(key),"cancelled or exceptional output is entirely zero");
    for(const auto* v:{&workspace,&output})Check(std::all_of(v->begin(),v->begin()+17,[](auto b){return b==0xa5;})&&
      std::all_of(v->end()-17,v->end(),[](auto b){return b==0xa5;}),"unaligned workspace/output preserve both guard regions");
  }
};
void KnownAnswers(bool large=false){
  struct Vector{const char* password;const char* salt;std::uint64_t n;unsigned r,p;const char* hex;};
  const Vector vectors[]{
    {"","",16,1,1,"77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906"},
    {"password","NaCl",1024,8,16,"fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"},
    {"pleaseletmein","SodiumChloride",16384,8,1,"7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887"},
    {"pleaseletmein","SodiumChloride",1048576,8,1,"2101cb9b6a511aaeaddbbe09cf70f881ec568d574a2ffd4dabe5ee9820adaa478e56fd8f4ba5d09ffa1c6d927c40f4c337304049e8a952fbcbf45c6fa77a41a4"}
  };
  for(const auto& v:vectors){
    if((v.n>16384)!=large)continue;
    const std::string password=v.password,salt=v.salt;Buffers b(v.n,v.r,v.p,64);
    b.Arm();const auto result=c::DeriveScryptKey({reinterpret_cast<const unsigned char*>(password.data()),password.size()},
      {reinterpret_cast<const unsigned char*>(salt.data()),salt.size()},v.n,v.r,v.p,b.work,b.key);b.Done(true);
    Check(result==c::ScryptExecutionCode::ok&&std::equal(b.key.data,b.key.data+64,Hex(v.hex).begin()),"native SHA/HMAC/PBKDF2/ROMix match entire independent RFC7914 known answer");
  }
}
void Differential(){
  for(unsigned length:{1u,31u,32u,33u,63u,64u,65u,129u,1024u,65535u})for(unsigned input:{0u,1u,55u,56u,63u,64u,65u,127u,128u,129u,513u}){
    std::vector<unsigned char> password(input),salt(input+3);
    for(unsigned i=0;i<input;++i)password[i]=static_cast<unsigned char>(i*19);
    for(unsigned i=0;i<salt.size();++i)salt[i]=static_cast<unsigned char>(i*31+255);
    const auto before_password=password,before_salt=salt;
    const std::uint64_t n=std::uint64_t{1}<<(1+(input%7));const unsigned r=1+input%4,p=1+input%3;
    Buffers b(n,r,p,length);std::vector<unsigned char> expected(length);const unsigned char empty=0;
    const auto oracle=EVP_PBE_scrypt(password.empty()?"":reinterpret_cast<const char*>(password.data()),password.size(),salt.empty()?&empty:salt.data(),salt.size(),n,r,p,64*1024*1024,expected.data(),expected.size());
    b.Arm();const auto result=c::DeriveScryptKey({password.data(),password.size()},{salt.data(),salt.size()},n,r,p,b.work,b.key);b.Done(true);
    Check(oracle==1&&result==c::ScryptExecutionCode::ok&&std::equal(expected.begin(),expected.end(),b.key.data),"full native output agrees with independent OpenSSL across padding, long-key and output boundaries");
    Check(password==before_password&&salt==before_salt,"borrowed arbitrary binary inputs never mutate");
  }
  // Inputs may share storage; mutable extents remain separate.
  std::vector<unsigned char> input(4097,0x91),expected(64);Buffers b(16,2,2,64);
  Check(EVP_PBE_scrypt(reinterpret_cast<const char*>(input.data()+1),4096,input.data()+1,4096,16,2,2,1024*1024,expected.data(),64)==1,"overlapping readonly input oracle computes actual output");
  b.Arm();const auto result=c::DeriveScryptKey({input.data()+1,4096},{input.data()+1,4096},16,2,2,b.work,b.key);b.Done(true);
  Check(result==c::ScryptExecutionCode::ok&&std::equal(expected.begin(),expected.end(),b.key.data),"unaligned mutually overlapping readonly inputs are valid");
}
struct Probe {
  unsigned calls=0,stop=0;
  bool throws=false;
  static bool Call(void* p){auto& self=*static_cast<Probe*>(p);if(++self.calls!=self.stop)return false;if(self.throws)throw self.stop;return true;}
};
void Cancellation(){
  {
    Buffers b(16,1,1,64);cancel_after_workspace_cleanup=true;
    b.Arm();const auto code=c::DeriveScryptKey({}, {},16,1,1,b.work,b.key,
      {[](void*){return cleanup_requested_cancel;},nullptr});b.Done(false);cancel_after_workspace_cleanup=false;
    Check(code==c::ScryptExecutionCode::cancelled&&cleanup_requested_cancel,
          "cancellation arriving during workspace erasure precedes final key publication");
  }
  Buffers normal(64,2,3,65);Probe observed;
  normal.Arm();const auto result=c::DeriveScryptKey({}, {},64,2,3,normal.work,normal.key,{Probe::Call,&observed});normal.Done(true);
  Check(result==c::ScryptExecutionCode::ok&&observed.calls>100,"probe is polled during actual mixing, not only at entry/exit");
  for(bool throws:{false,true})for(unsigned stop=1;stop<=observed.calls;++stop){
    Buffers b(64,2,3,65);Probe probe{0,stop,throws};bool threw=false;c::ScryptExecutionCode code{};
    b.Arm();try{code=c::DeriveScryptKey({}, {},64,2,3,b.work,b.key,{Probe::Call,&probe});}catch(unsigned where){threw=where==stop;}
    b.Done(false);
    Check(probe.calls==stop&&(throws?threw:code==c::ScryptExecutionCode::cancelled),"every computation/publication probe cancels or unwinds without a partial accepted key");
  }
  // Large password, salt and row copies must themselves be interruptible.
  for(unsigned phase=0;phase<3;++phase){
    std::vector<unsigned char> input(1<<20,0x44);
    const unsigned r=phase==2?1024u:1u;Buffers b(2,r,1,64);Probe probe{0,3,false};
    b.Arm();const auto code=c::DeriveScryptKey(phase==0?c::ScryptInput{input.data(),input.size()}:c::ScryptInput{},
      phase==1?c::ScryptInput{input.data(),input.size()}:c::ScryptInput{},2,r,1,b.work,b.key,{Probe::Call,&probe});b.Done(false);
    Check(code==c::ScryptExecutionCode::cancelled&&probe.calls==3,"large input hashing or working extent computation can stop before complete derivation");
  }
  {
    std::vector<unsigned char> input(1<<20,0x44);Buffers b(2,1,1,64);Probe probe;
    b.Arm();const auto code=c::DeriveScryptKey({input.data(),input.size()},{},2,1,1,b.work,b.key,{Probe::Call,&probe});b.Done(true);
    Check(code==c::ScryptExecutionCode::ok&&probe.calls>=258,
          "complete long-password hashing supplies bounded-work cancellation opportunities");
  }
  {
    // Independently compute the initial B lane, then cancel between the two
    // 64KiB copies into X. A single uninterruptible row copy cannot pass.
    constexpr std::size_t row=128*1024;Buffers b(2,1024,1,64);
    std::vector<unsigned char> initial(row);
    Check(PKCS5_PBKDF2_HMAC("",0,nullptr,0,1,EVP_sha256(),static_cast<int>(initial.size()),initial.data())==1,
          "independent one-iteration PBKDF2 prepares exact initial lane oracle");
    struct CopyProbe {
      const unsigned char* x;const unsigned char* initial;bool reached=false;
      static bool Call(void* raw){auto& self=*static_cast<CopyProbe*>(raw);
        if(std::equal(self.x,self.x+65536,self.initial)&&std::all_of(self.x+65536,self.x+row,[](auto v){return v==0xa5;}))self.reached=true;
        return self.reached;
      }
    } probe{b.work.data+3*row,initial.data()};
    b.Arm();const auto code=c::DeriveScryptKey({}, {},2,1024,1,b.work,b.key,{CopyProbe::Call,&probe});b.Done(false);
    Check(code==c::ScryptExecutionCode::cancelled&&probe.reached,"large workspace copy is cancellable before its second64KiB extent");
  }
}
void InvalidBuffers(){
  for(unsigned fault=0;fault<13;++fault){
    Buffers b(16,1,1,64);c::ScryptInput password{},salt{};auto work=b.work,key=b.key;
    std::uint64_t n=16;unsigned r=1,p=1;
    if(fault==0)work.size--;
    if(fault==1)work.size++;
    if(fault==2)work.data=nullptr;
    if(fault==3)key.data=nullptr;
    if(fault==4)password={nullptr,1};
    if(fault==5)salt={nullptr,1};
    if(fault==6)key.data=work.data;
    if(fault==7)password={work.data+work.size-1,1};
    if(fault==8)salt={key.data,1};
    if(fault==9)password={reinterpret_cast<const unsigned char*>(std::numeric_limits<std::uintptr_t>::max()-1),4};
    if(fault==10)n=3;
    if(fault==11){n=std::uint64_t{1}<<63;r=8;}
    if(fault==12)password={reinterpret_cast<const unsigned char*>(1),std::numeric_limits<std::size_t>::max()/8+1};
    const auto before_work=b.workspace,before_key=b.output;Probe probe;
    b.Arm();const auto code=c::DeriveScryptKey(password,salt,n,r,p,work,key,{Probe::Call,&probe});b.Done(false,false);
    const auto expected=fault==10?c::ScryptExecutionCode::invalid_parameters:(fault>=11?c::ScryptExecutionCode::cost_overflow:c::ScryptExecutionCode::invalid_buffers);
    Check(code==expected&&probe.calls==0&&before_work==b.workspace&&before_key==b.output,"invalid costs/extents reject before reads, writes or borrowed callbacks");
  }
}
m::MemoryBinaryUuid Id(unsigned n){m::MemoryBinaryUuid out{};out[0]=1;out[6]=0x70;out[8]=0x80;out[15]=static_cast<unsigned char>(n);return out;}
void ResourceOwner(){
  const m::KdfResourceOwner owner{Id(1),Id(2),Id(3),Id(4),Id(5)};
  auto ledger=std::make_shared<m::HierarchicalMemoryBudgetLedger>();
  auto governor=m::KdfResourceGovernor::Create(ledger,Id(1),Id(2),{1<<20,1<<20,1<<20,1<<20,1<<20,1});
  auto receipt=governor->Issue(owner);c::ScryptWorkEstimate cost;
  Check(c::EstimateScryptWork(0,0,16,1,1,64,cost)==c::ScryptEstimateCode::ok,"native resource fixture uses shared cost calculation");
  auto admitted=receipt->Acquire(owner,{cost.workspace_bytes+64+c::ScryptFixedScratchBytes(),cost.work_units});
  Check(admitted.grant.live(),"shared Core ledger reserves actual native extents before workspace allocation");
  Buffers b(16,1,1,64);
  struct Bound {m::KdfResourceGrant* grant;m::KdfResourceReceipt* receipt;unsigned polls=0;
    static bool Call(void* raw){auto& self=*static_cast<Bound*>(raw);if(++self.polls==3)self.receipt->Revoke();return !self.grant->live();}
  } bound{&admitted.grant,receipt.get()};
  b.Arm();const auto result=c::DeriveScryptKey({}, {},16,1,1,b.work,b.key,{Bound::Call,&bound});b.Done(false);
  Check(result==c::ScryptExecutionCode::cancelled&&ledger->Snapshot().current_bytes==cost.workspace_bytes+64+c::ScryptFixedScratchBytes(),"real receipt revocation stops computation while retaining cleanup capacity");
  b.workspace.clear();b.workspace.shrink_to_fit();b.output.clear();b.output.shrink_to_fit();
  admitted.grant.Reset();
  Check(ledger->Snapshot().current_bytes==0&&receipt->consumed_work_units()==cost.work_units,"erased/freed native payload releases memory without refunding work");
}
void Concurrent(){
  const auto expected=Hex("77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
  std::atomic<bool> correct=true;std::vector<std::thread> threads;
  for(unsigned i=0;i<8;++i)threads.emplace_back([&]{
    std::vector<unsigned char> workspace(2432),output(64);
    for(unsigned repeat=0;repeat<20;++repeat){
      const auto code=c::DeriveScryptKey({}, {},16,1,1,{workspace.data(),workspace.size()},{output.data(),output.size()});
      if(code!=c::ScryptExecutionCode::ok||output!=expected||!std::all_of(workspace.begin(),workspace.end(),[](auto b){return b==0;}))correct=false;
    }
  });
  for(auto& thread:threads)thread.join();Check(correct,"concurrent calls have no shared mutable crypto state");
}
int main(int argc,char** argv){
  if(argc==2&&std::string_view(argv[1])=="--large-rfc")KnownAnswers(true);
  else if(argc==1){KnownAnswers();Differential();Cancellation();InvalidBuffers();ResourceOwner();Concurrent();}
  else return 2;
  std::printf("%u checks, %u failures\n",checks,failures);return failures?1:0;}
