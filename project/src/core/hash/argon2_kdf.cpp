// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "argon2_kdf.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
extern "C" {
#include "argon2_reference/src/core.h"
#include "argon2_reference/src/blake2/blake2.h"
#include "argon2_reference/src/blake2/blake2-impl.h"
}

namespace scratchbird::core::crypto {
namespace {
bool Extent(Argon2Input in) noexcept {
  return (!in.size || in.data) && in.size <= UINTPTR_MAX-reinterpret_cast<std::uintptr_t>(in.data);
}
bool Overlap(Argon2Input a,Argon2Input b) noexcept {
  if (!a.size || !b.size) return false;
  const auto x=reinterpret_cast<std::uintptr_t>(a.data),y=reinterpret_cast<std::uintptr_t>(b.data);
  return x<y+b.size && y<x+a.size;
}
template<class T> struct Scratch {
  T value{};
  ~Scratch(){OPENSSL_cleanse(&value,sizeof(value));}
};
struct Ownership {
  Argon2Output workspace,output;
  bool cleared=false,accepted=false;
  void Clear() noexcept {
    if (cleared) return;
    for (std::size_t at=0;at<workspace.size;) {
      const auto n=std::min(std::size_t{65536},workspace.size-at);
      OPENSSL_cleanse(workspace.data+at,n);at+=n;
    }
    cleared=true;
  }
  ~Ownership(){Clear();if(!accepted)OPENSSL_cleanse(output.data,output.size);}
};
struct Probe {
  Argon2Cancellation source;
  std::exception_ptr exception;
  bool Poll(){return source.requested&&source.requested(source.context);}
  static int FromC(void* opaque) noexcept {
    auto& self=*static_cast<Probe*>(opaque);
    try{return self.Poll()?1:0;}catch(...){self.exception=std::current_exception();return 1;}
  }
};
Argon2Code Compute(Argon2idParameters p,Argon2Input password,Argon2Input salt,
    Argon2Input secret,Argon2Input associated,Argon2Output workspace,Argon2Output output,Probe& probe) {
  Scratch<blake2b_state> hash;
  Scratch<std::array<std::uint8_t,72>> seed;
  Scratch<std::array<std::uint8_t,1024>> bytes;
  Scratch<block> final_block;
  std::array<std::uint8_t,4> scalar{};
  const auto number=[&](std::uint32_t n){store32(scalar.data(),n);return blake2b_update(&hash.value,scalar.data(),4)==0;};
  const auto input=[&](Argon2Input in){
    if(!number(static_cast<std::uint32_t>(in.size)))return Argon2Code::provider_failure;
    for(std::size_t at=0;at<in.size;){
      if(probe.Poll())return Argon2Code::cancelled;
      const auto n=std::min(std::size_t{4096},in.size-at);
      if(blake2b_update(&hash.value,in.data+at,n)!=0)return Argon2Code::provider_failure;
      at+=n;
    }
    return Argon2Code::ok;
  };
  if(blake2b_init(&hash.value,64)!=0 || !number(p.lanes) || !number(output.size) ||
     !number(p.memory_kib) || !number(p.passes) || !number(0x13) || !number(2))return Argon2Code::provider_failure;
  for(auto in:{password,salt,secret,associated}){const auto code=input(in);if(code!=Argon2Code::ok)return code;}
  if(blake2b_final(&hash.value,seed.value.data(),64)!=0)return Argon2Code::provider_failure;
  argon2_instance_t instance{};
  instance.memory=reinterpret_cast<block*>(workspace.data);
  instance.version=ARGON2_VERSION_13;instance.passes=p.passes;instance.lanes=p.lanes;instance.threads=1;
  instance.type=Argon2_id;instance.memory_blocks=static_cast<std::uint32_t>(workspace.size/1024);
  instance.segment_length=instance.memory_blocks/(p.lanes*4);instance.lane_length=instance.segment_length*4;
  for(std::uint32_t lane=0;lane<p.lanes;++lane){
    for(std::uint32_t first=0;first<2;++first){
      if(probe.Poll())return Argon2Code::cancelled;
      store32(seed.value.data()+64,first);store32(seed.value.data()+68,lane);
      if(blake2b_long(bytes.value.data(),1024,seed.value.data(),72)!=0)return Argon2Code::provider_failure;
      auto& b=instance.memory[lane*instance.lane_length+first];
      for(unsigned word=0;word<128;++word)b.v[word]=load64(bytes.value.data()+word*8);
    }
  }
  for(std::uint32_t pass=0;pass<p.passes;++pass)for(std::uint8_t slice=0;slice<4;++slice)
    for(std::uint32_t lane=0;lane<p.lanes;++lane){
      if(probe.Poll())return Argon2Code::cancelled;
      if(fill_segment_interruptible(&instance,{pass,lane,slice,0},Probe::FromC,&probe)){
        if(probe.exception)std::rethrow_exception(probe.exception);
        return Argon2Code::cancelled;
      }
    }
  copy_block(&final_block.value,instance.memory+instance.lane_length-1);
  for(std::uint32_t lane=1;lane<p.lanes;++lane){
    if(probe.Poll())return Argon2Code::cancelled;
    xor_block(&final_block.value,instance.memory+(lane+1)*instance.lane_length-1);
  }
  for(unsigned word=0;word<128;++word)store64(bytes.value.data()+word*8,final_block.value.v[word]);
  if(probe.Poll())return Argon2Code::cancelled;
  return blake2b_long(output.data,output.size,bytes.value.data(),1024)==0?Argon2Code::ok:Argon2Code::provider_failure;
}
} // namespace
Argon2Workspace Argon2idWorkspaceBytes(Argon2idParameters p) noexcept {
  if(!p.passes || !p.lanes || p.lanes>0xffffff || std::uint64_t(p.memory_kib)<8ULL*p.lanes)
    return {Argon2Code::invalid_parameters,0};
  const std::uint64_t divisor=4ULL*p.lanes;
  const auto bytes=(p.memory_kib/divisor)*divisor*1024;
  if(bytes>std::numeric_limits<std::size_t>::max())return {Argon2Code::size_overflow,0};
  return {Argon2Code::ok,bytes};
}
std::size_t Argon2idFixedScratchBytes() noexcept {return 16384;}
Argon2Code DeriveArgon2idKey(Argon2idParameters p,Argon2Input password,Argon2Input salt,
    Argon2Output workspace,Argon2Output output,Argon2Cancellation cancellation,
    Argon2Input secret,Argon2Input associated_data) {
  const auto size=Argon2idWorkspaceBytes(p);
  if(size.code!=Argon2Code::ok)return size.code;
  if(output.size<4 || output.size>65535)return Argon2Code::invalid_parameters;
  const Argon2Input work{workspace.data,workspace.size},out{output.data,output.size};
  if(!Extent(work)||!Extent(out)||workspace.size!=size.bytes||reinterpret_cast<std::uintptr_t>(workspace.data)%alignof(block)||
     Overlap(work,out))return Argon2Code::invalid_extent;
  for(auto in:{password,salt,secret,associated_data})
    if(!Extent(in)||in.size>UINT32_MAX||Overlap(work,in)||Overlap(out,in))return Argon2Code::invalid_extent;
  Ownership owner{workspace,output};Probe probe{cancellation,{}};
  if(probe.Poll())return Argon2Code::cancelled;
  const auto code=Compute(p,password,salt,secret,associated_data,workspace,output,probe);
  owner.Clear();
  if(code!=Argon2Code::ok)return code;
  if(probe.Poll())return Argon2Code::cancelled;
  owner.accepted=true;return Argon2Code::ok;
}
} // namespace scratchbird::core::crypto
