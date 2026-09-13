// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "scrypt_kdf.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::core::crypto {
namespace {
using Byte=std::uint8_t;
using Word=std::uint32_t;
using Wide=std::uint64_t;

// Algorithm constants and equations from FIPS180-4 sections4.2.2/5.3.3/6.2
// and RFC7914. No external implementation source is embedded here.
constexpr std::array<Word,64> sha_constants{
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
struct ShaState {
  std::array<Word,8> h{};
  std::array<Byte,64> block{};
  Wide bytes=0;
  std::size_t used=0;
};
struct Scratch {
  ShaState sha,inner_seed,outer_seed;
  std::array<Word,64> schedule{};
  std::array<Word,8> sha_work{};
  std::array<Byte,64> key{};
  std::array<Byte,32> digest{},inner{};
  std::array<Word,16> mix{},salsa{};
  std::array<Byte,4> counter{};
  Word sum1=0,sum2=0;
};
struct SecretScratch {
  Scratch value{};
  bool cleared=false;
  void Clear() noexcept{if(!cleared){OPENSSL_cleanse(&value,sizeof(value));cleared=true;}}
  ~SecretScratch(){Clear();}
};
struct Execution {
  ScryptCancellationProbe cancellation;
  unsigned units=0;
  bool Poll() const {return !cancellation.requested||!cancellation.requested(cancellation.context);}
  bool Unit(){if(++units<64)return true;units=0;return Poll();}
};
Word Ror(Word x,unsigned n){return (x>>n)|(x<<(32-n));}
Word Rol(Word x,unsigned n){return (x<<n)|(x>>(32-n));}
Word Big32(const Byte* p){return (Word(p[0])<<24)|(Word(p[1])<<16)|(Word(p[2])<<8)|p[3];}
Word Little32(const Byte* p){return Word(p[0])|(Word(p[1])<<8)|(Word(p[2])<<16)|(Word(p[3])<<24);}
void PutBig32(Byte* p,Word word){for(unsigned i=0;i<4;++i)p[i]=static_cast<Byte>(word>>(24-8*i));}
void PutLittle32(Byte* p,Word word){for(unsigned i=0;i<4;++i)p[i]=static_cast<Byte>(word>>(8*i));}
void ShaReset(ShaState& state){
  state={};state.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
}
bool ShaCompress(Scratch& s,Execution& execution){
  if(!execution.Unit())return false;
  auto& w=s.schedule;auto& a=s.sha_work;
  for(unsigned i=0;i<16;++i)w[i]=Big32(s.sha.block.data()+4*i);
  for(unsigned i=16;i<64;++i){
    const auto x=w[i-15],y=w[i-2];
    w[i]=w[i-16]+(Ror(x,7)^Ror(x,18)^(x>>3))+w[i-7]+(Ror(y,17)^Ror(y,19)^(y>>10));
  }
  a=s.sha.h;
  for(unsigned i=0;i<64;++i){
    s.sum1=a[7]+(Ror(a[4],6)^Ror(a[4],11)^Ror(a[4],25))+((a[4]&a[5])^(~a[4]&a[6]))+sha_constants[i]+w[i];
    s.sum2=(Ror(a[0],2)^Ror(a[0],13)^Ror(a[0],22))+((a[0]&a[1])^(a[0]&a[2])^(a[1]&a[2]));
    for(unsigned j=7;j>0;--j)a[j]=a[j-1];
    a[4]+=s.sum1;a[0]=s.sum1+s.sum2;
  }
  for(unsigned i=0;i<8;++i)s.sha.h[i]+=a[i];
  return true;
}
bool ShaUpdate(Scratch& s,ScryptInput input,Execution& execution){
  // The outer extent check guarantees SHA256's uint64 bit-length bound.
  s.sha.bytes+=input.size;
  while(input.size){
    const auto count=std::min(input.size,64-s.sha.used);
    std::memcpy(s.sha.block.data()+s.sha.used,input.data,count);
    input.data+=count;input.size-=count;s.sha.used+=count;
    if(s.sha.used==64){if(!ShaCompress(s,execution))return false;s.sha.used=0;}
  }
  return true;
}
bool ShaFinish(Scratch& s,Byte* output,Execution& execution){
  const Wide bits=s.sha.bytes*8;
  s.sha.block[s.sha.used++]=0x80;
  if(s.sha.used>56){
    std::fill(s.sha.block.begin()+s.sha.used,s.sha.block.end(),0);
    if(!ShaCompress(s,execution))return false;
    s.sha.used=0;
  }
  std::fill(s.sha.block.begin()+s.sha.used,s.sha.block.begin()+56,0);
  for(unsigned i=0;i<8;++i)s.sha.block[56+i]=static_cast<Byte>(bits>>(56-8*i));
  if(!ShaCompress(s,execution))return false;
  for(unsigned i=0;i<8;++i)PutBig32(output+4*i,s.sha.h[i]);
  return true;
}
bool HmacPrepare(Scratch& s,ScryptInput password,Execution& execution){
  if(password.size>64){
    ShaReset(s.sha);
    if(!ShaUpdate(s,password,execution)||!ShaFinish(s,s.digest.data(),execution))return false;
    std::copy(s.digest.begin(),s.digest.end(),s.key.begin());
  }else if(password.size)std::memcpy(s.key.data(),password.data,password.size);
  for(auto& b:s.key)b^=0x36;
  ShaReset(s.sha);if(!ShaUpdate(s,{s.key.data(),s.key.size()},execution))return false;
  s.inner_seed=s.sha;
  for(auto& b:s.key)b^=0x36^0x5c;
  ShaReset(s.sha);if(!ShaUpdate(s,{s.key.data(),s.key.size()},execution))return false;
  s.outer_seed=s.sha;
  return true;
}
bool Pbkdf2(Scratch& s,ScryptInput salt,ScryptOutput output,Execution& execution){
  Wide block=1;
  while(output.size){
    PutBig32(s.counter.data(),static_cast<Word>(block++));
    s.sha=s.inner_seed;
    if(!ShaUpdate(s,salt,execution)||!ShaUpdate(s,{s.counter.data(),4},execution)||
        !ShaFinish(s,s.inner.data(),execution))return false;
    s.sha=s.outer_seed;
    if(!ShaUpdate(s,{s.inner.data(),32},execution)||!ShaFinish(s,s.digest.data(),execution))return false;
    const auto count=std::min<std::size_t>(32,output.size);
    std::memcpy(output.data,s.digest.data(),count);output.data+=count;output.size-=count;
  }
  return true;
}
void Quarter(std::array<Word,16>& a,unsigned i,unsigned j,unsigned k,unsigned l){
  a[j]^=Rol(a[i]+a[l],7);a[k]^=Rol(a[j]+a[i],9);
  a[l]^=Rol(a[k]+a[j],13);a[i]^=Rol(a[l]+a[k],18);
}
bool Salsa(Scratch& s,Execution& execution){
  if(!execution.Unit())return false;
  s.salsa=s.mix;
  for(unsigned round=0;round<4;++round){
    for(unsigned lane=0;lane<4;++lane){
      const unsigned i=5*lane;Quarter(s.salsa,i,(i+4)%16,(i+8)%16,(i+12)%16);
    }
    for(unsigned lane=0;lane<4;++lane){
      const unsigned base=4*lane;Quarter(s.salsa,base+lane,base+(lane+1)%4,base+(lane+2)%4,base+(lane+3)%4);
    }
  }
  for(unsigned i=0;i<16;++i)s.mix[i]+=s.salsa[i];
  return true;
}
bool BlockMix(Scratch& s,const Byte* input,Byte* output,std::size_t r,Execution& execution){
  const auto last=input+128*r-64;
  for(unsigned i=0;i<16;++i)s.mix[i]=Little32(last+4*i);
  for(std::size_t block=0;block<2*r;++block){
    for(unsigned i=0;i<16;++i)s.mix[i]^=Little32(input+64*block+4*i);
    if(!Salsa(s,execution))return false;
    const auto offset=64*((block%2)*r+block/2);
    for(unsigned i=0;i<16;++i)PutLittle32(output+offset+4*i,s.mix[i]);
  }
  return true;
}
bool Copy(Byte* to,const Byte* from,std::size_t bytes,Execution& execution,bool xor_bytes=false){
  while(bytes){
    if(!execution.Poll())return false;
    const auto count=std::min<std::size_t>(bytes,65536);
    if(xor_bytes){for(std::size_t i=0;i<count;++i)to[i]^=from[i];}
    else std::memcpy(to,from,count);
    to+=count;from+=count;bytes-=count;
  }
  return true;
}
bool Valid(ScryptInput bytes){
  return (!bytes.size||bytes.data)&&bytes.size<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(bytes.data);
}
bool Overlap(ScryptInput a,ScryptInput b){
  if(!a.size||!b.size)return false;
  const auto first=reinterpret_cast<std::uintptr_t>(a.data),second=reinterpret_cast<std::uintptr_t>(b.data);
  return first<second+b.size&&second<first+a.size;
}
} // namespace

std::size_t ScryptFixedScratchBytes() noexcept {return sizeof(Scratch);}

ScryptExecutionCode DeriveScryptKey(ScryptInput password,ScryptInput salt,
    Wide n,std::uint32_t r,std::uint32_t p,ScryptOutput workspace,ScryptOutput output,
    ScryptCancellationProbe cancellation){
  ScryptWorkEstimate cost;
  const auto estimated=EstimateScryptWork(password.size,salt.size,n,r,p,output.size,cost);
  if(estimated==ScryptEstimateCode::invalid_parameters)return ScryptExecutionCode::invalid_parameters;
  constexpr Wide sha_max_bytes=std::numeric_limits<Wide>::max()/8;
  if(estimated!=ScryptEstimateCode::ok||cost.workspace_bytes>std::numeric_limits<std::size_t>::max()||
      password.size>sha_max_bytes||salt.size>sha_max_bytes-68)return ScryptExecutionCode::cost_overflow;
  const ScryptInput work{workspace.data,workspace.size},result{output.data,output.size};
  if(workspace.size!=cost.workspace_bytes||!Valid(password)||!Valid(salt)||!Valid(work)||!Valid(result)||
      Overlap(work,result)||Overlap(password,work)||Overlap(password,result)||Overlap(salt,work)||Overlap(salt,result))
    return ScryptExecutionCode::invalid_buffers;
  struct Erase {
    ScryptOutput workspace,output;
    bool success=false;
    bool workspace_cleared=false;
    void ClearWorkspace() noexcept{if(!workspace_cleared){OPENSSL_cleanse(workspace.data,workspace.size);workspace_cleared=true;}}
    ~Erase(){ClearWorkspace();if(!success)OPENSSL_cleanse(output.data,output.size);}
  } erase{workspace,output};
  SecretScratch scratch;
  auto& s=scratch.value;Execution execution{cancellation};
  if(!execution.Poll()||!HmacPrepare(s,password,execution))return ScryptExecutionCode::cancelled;
  const auto row=std::size_t{128}*r;
  const auto b_size=row*p;
  auto* const b=workspace.data;
  auto* const v=b+b_size;
  auto* x=v+row*static_cast<std::size_t>(n);
  auto* y=x+row;
  if(!Pbkdf2(s,salt,{b,b_size},execution))return ScryptExecutionCode::cancelled;
  for(std::size_t lane=0;lane<p;++lane){
    if(!Copy(x,b+row*lane,row,execution))return ScryptExecutionCode::cancelled;
    for(Wide i=0;i<n;++i){
      if(!Copy(v+static_cast<std::size_t>(i)*row,x,row,execution)||!BlockMix(s,x,y,r,execution))return ScryptExecutionCode::cancelled;
      std::swap(x,y);
    }
    for(Wide i=0;i<n;++i){
      const auto last=x+row-64;
      const auto j=(Wide(Little32(last))|(Wide(Little32(last+4))<<32))&(n-1);
      if(!Copy(x,v+static_cast<std::size_t>(j)*row,row,execution,true)||!BlockMix(s,x,y,r,execution))return ScryptExecutionCode::cancelled;
      std::swap(x,y);
    }
    if(!Copy(b+row*lane,x,row,execution))return ScryptExecutionCode::cancelled;
  }
  if(!Pbkdf2(s,{b,b_size},output,execution))return ScryptExecutionCode::cancelled;
  // Erasure can be substantial. It must precede the last cancellation fence,
  // while the output guard still owns the complete but unpublished key.
  scratch.Clear();erase.ClearWorkspace();
  if(!execution.Poll())return ScryptExecutionCode::cancelled;
  erase.success=true;
  return ScryptExecutionCode::ok;
}
} // namespace scratchbird::core::crypto
