// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_armor.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace c=scratchbird::core::crypto;
using Bytes=std::vector<std::uint8_t>;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
c::PgpInput In(const Bytes& b){return {b.data(),b.size()};}
c::PgpInput In(const std::string& b){return {reinterpret_cast<const std::uint8_t*>(b.data()),b.size()};}
c::PgpOutput Out(Bytes& b){return {b.data(),b.size()};}
bool Zero(const Bytes& b){return std::all_of(b.begin(),b.end(),[](auto x){return !x;});}
Bytes Encode(const Bytes& raw){const auto size=c::ArmorEncodedSize(raw.size());Check(size.code==c::PgpCode::ok,"encode sizing");
  Bytes out(size.bytes);Check(c::EncodeArmor(In(raw),Out(out))==c::PgpCode::ok,"encode");return out;}
std::string Wrap(std::string body){return "-----BEGIN PGP MESSAGE-----\n\n"+body+"-----END PGP MESSAGE-----\n";}
struct Probe {
  unsigned calls=0,stop=0;bool throwing=false;
  static bool Poll(void* raw){auto& self=*static_cast<Probe*>(raw);if(++self.calls!=self.stop)return false;
    if(self.throwing)throw self.stop;return true;}
  c::PgpCancellation callback(){return {Poll,this};}
};
int main() try {
  const Bytes raw(513,0x57),body=Encode(raw);
  Check(c::ArmorDecodedSize(In(body)).bytes==raw.size(),"decode inspection exact");
  for(bool decode:{false,true}){
    Bytes output(decode?raw.size():body.size(),0xa5);
    const auto run=[&](Probe& probe){return decode?c::DecodeArmor(In(body),Out(output),probe.callback()):
      c::EncodeArmor(In(raw),Out(output),probe.callback());};
    Probe normal;Check(run(normal)==c::PgpCode::ok,"baseline codec");
    for(bool throwing:{false,true})for(unsigned stop=1;stop<=normal.calls;++stop){
      std::fill(output.begin(),output.end(),0xa5);Probe probe{0,stop,throwing};bool caught=false;
      try{Check(run(probe)==c::PgpCode::cancelled,"cancelled codec result");}catch(unsigned n){caught=n==stop;}
      Check(caught==throwing&&probe.calls==stop&&Zero(output),"every codec cancellation/exception clears full unpublished output");
    }
  }
  // Receiver supports UTF8 metadata; it never controls decoded byte semantics.
  for(const char* key:{"Comment","Unknown-Extension","Charset"}) {
    const auto text="-----BEGIN PGP MESSAGE-----\n"+std::string(key)+": caf\xc3\xa9\n\nZm9v\n-----END PGP MESSAGE-----\n";
    Bytes out(3,0xa5);Check(c::DecodeArmor(In(text),Out(out))==c::PgpCode::ok&&out==Bytes({'f','o','o'}),"UTF8 header values ignored");
  }
  for(auto input:{std::string(65536,'\n')+Wrap("Zm9v\n"),Wrap("Zm9v\n")+std::string(65536,'\n'),
      "-----BEGIN PGP MESSAGE-----\n\n"+std::string(65536,'\n')+"Zm9v\n-----END PGP MESSAGE-----\n",
      "-----BEGIN PGP MESSAGE-----\n"+std::string(65536,'X')+": value\n\nZm9v\n-----END PGP MESSAGE-----\n",
      "-----BEGIN PGP MESSAGE-----\nComment: "+std::string(65536,'X')+"\n\nZm9v\n-----END PGP MESSAGE-----\n"}) {
    Probe probe{0,7,false};Bytes out(3,0xa5);
    Check(c::DecodeArmor(In(input),Out(out),probe.callback())==c::PgpCode::cancelled&&probe.calls==7&&Zero(out),"newline/header work is cancellable while scanning");
  }
  const auto complete=Wrap("Zm9v\n");
  for(std::size_t n=0;n<complete.size()-1;++n){
    Bytes out(3,0xa5);Check(c::DecodeArmor({In(complete).data,n},Out(out))!=c::PgpCode::ok&&Zero(out),"every truncated boundary clears all output");
  }
  // Final newline is optional on receive, never omitted by canonical output.
  Bytes out(3);Check(c::DecodeArmor({In(complete).data,complete.size()-1},Out(out))==c::PgpCode::ok,"unterminated final boundary line accepted");
  for(const auto& invalid:{Wrap("Zh==\n"),Wrap("Zm9=\n"),Wrap("Zm9v\n")+"injected",
      std::string("-----BEGIN PGP MESSAGE-----\nComment: \xc0\x80\n\nZm9v\n-----END PGP MESSAGE-----\n")}) {
    out.assign(3,0xa5);Check(c::DecodeArmor(In(invalid),Out(out))==c::PgpCode::invalid_packet&&Zero(out),"malformed input erases already decoded prefixes");
  }
  for(std::size_t size:{0u,1u,2u,4u,128u}) {
    Bytes wrong(size,0xa5);Check(c::DecodeArmor(In(complete),Out(wrong))==c::PgpCode::invalid_extent&&Zero(wrong),"incorrect output sizes cannot overflow or publish prefixes");
  }
  out.assign(3,0xa5);const auto saved=out;
  Check(c::DecodeArmor({nullptr,1},Out(out))==c::PgpCode::invalid_extent&&out==saved,"invalid borrowed extent untouched");
  Check(c::DecodeArmor({reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX-1),3},Out(out))==c::PgpCode::invalid_extent&&out==saved,"overflowing input untouched");
  auto mutable_body=body;const auto original=mutable_body;
  Check(c::DecodeArmor(In(mutable_body),{mutable_body.data()+1,raw.size()})==c::PgpCode::invalid_extent&&mutable_body==original,"overlapping private output refused before effects");
  // CRC24 cannot be used as an integrity oracle. Ignore both valid and malformed.
  for(const char* crc:{"=twTO","=xxxx","=not a checksum","="}) {
    const auto text=Wrap(std::string("Zm9v\n")+crc+"\n");out.assign(3,0);
    Check(c::DecodeArmor(In(text),Out(out))==c::PgpCode::ok&&out==Bytes({'f','o','o'}),"RFC9580 CRC footer never rejects a valid payload");
  }
  std::cout<<"PASS armor codec checks="<<checks<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
