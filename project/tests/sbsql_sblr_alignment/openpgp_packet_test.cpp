// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_packet.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace c = scratchbird::core::crypto;
using Bytes = std::vector<std::uint8_t>;
using Code = c::PgpCode;
thread_local bool forbid_heap = false;
void* operator new(std::size_t n) {
  if (forbid_heap) throw std::bad_alloc();
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
unsigned checks = 0;
void Check(bool b, const char* why) { ++checks; if (!b) throw std::runtime_error(why); }
c::PgpInput In(const Bytes& v) { return {v.data(), v.size()}; }
c::PgpOutput Out(Bytes& v) { return {v.data(), v.size()}; }
bool Filled(const Bytes& v, unsigned char x) {
  return std::all_of(v.begin(), v.end(), [x](auto b) { return x == b; });
}
Bytes Packet(unsigned type, const Bytes& body) {
  const auto size = c::PacketEncodedSize(type, body.size());
  Check(size.code == Code::ok, "encoded size");
  Bytes result(size.bytes, 0xa5);
  Check(c::EncodePacket(type, In(body), Out(result)) == Code::ok, "encode packet");
  return result;
}
void Lengths() {
  // Independent RFC9580 section4.2.3 length-header examples and thresholds.
  struct Vector { std::size_t size; Bytes header; };
  const Vector vectors[]{{0,{0xd2,0}},{191,{0xd2,191}},
    {192,{0xd2,192,0}},{8383,{0xd2,223,255}},
    {8384,{0xd2,255,0,0,32,192}},{100,{0xd2,0x64}},
    {1723,{0xd2,0xc5,0xfb}},{100000,{0xd2,0xff,0,1,0x86,0xa0}}};
  for (const auto& vector : vectors) {
    Bytes body(vector.size);
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = i*79;
    auto packet = Packet(18, body);
    Check(packet.size() == body.size()+vector.header.size() &&
        std::equal(vector.header.begin(), vector.header.end(), packet.begin()), "independent length oracle");
    packet.insert(packet.end(), {0xc3,0}); // A following packet is not part of the body.
    const auto info = c::InspectPacket(In(packet));
    Check(info.code == Code::ok && info.type == 18 && info.body_bytes == body.size() &&
        info.packet_bytes == packet.size()-2 && !info.partial, "exact first-packet extent");
    Bytes decoded(body.size(), 0xa5);
    forbid_heap = true;
    const auto code = c::CopyPacketBody(In(packet), Out(decoded));
    forbid_heap = false;
    Check(code == Code::ok && decoded == body, "zero-allocation body decode");
    for (std::size_t prefix = 0; prefix < vector.header.size()+body.size(); ++prefix)
      Check(c::InspectPacket({packet.data(), prefix}).code == Code::invalid_packet,
          "every truncated header and body prefix rejected");
  }
  // Non-shortest, but RFC-valid definite lengths remain admissible on input.
  Check(c::InspectPacket(In(Bytes{0xc3,255,0,0,0,0})).code == Code::ok, "five-octet zero length");
  Check(c::PacketEncodedSize(0, 0).code == Code::invalid_profile &&
      c::PacketEncodedSize(64, 0).code == Code::invalid_profile, "reserved or oversized packet type");
  Check(c::PacketEncodedSize(18, std::numeric_limits<std::size_t>::max()).code == Code::size_overflow &&
      c::LiteralPacketEncodedSize(std::numeric_limits<std::size_t>::max()).code == Code::size_overflow,
      "size addition overflow");
  if (sizeof(std::size_t) > 4) {
    Check(c::PacketEncodedSize(18, 0xffffffffULL).bytes == 0x100000005ULL, "uint32 maximum body size query");
    Check(c::PacketEncodedSize(18, 0x100000000ULL).code == Code::size_overflow, "definite length cannot truncate");
  }
}
void Partial() {
  // 512 bytes, a one-byte partial segment, then a two-byte definite segment.
  Bytes wire{0xd2,233}; wire.insert(wire.end(),512,0x39);
  wire.insert(wire.end(), {224,0x71,2,0,0xff});
  Bytes expected(512,0x39); expected.insert(expected.end(),{0x71,0,0xff});
  auto info = c::InspectPacket(In(wire));
  Check(info.code == Code::ok && info.partial && info.packet_bytes == wire.size() &&
      info.body_bytes == expected.size(), "mixed partial and definite lengths");
  Bytes decoded(expected.size());
  Check(c::CopyPacketBody(In(wire), Out(decoded)) == Code::ok && decoded == expected, "partial body concatenation");
  for (std::size_t n = 0; n < wire.size(); ++n)
    Check(c::InspectPacket({wire.data(),n}).code == Code::invalid_packet,"partial chain truncation");
  for (unsigned type = 1; type < 64; ++type) {
    wire[0] = 0xc0|type;
    const bool permitted = type == 8 || type == 9 || type == 11 || type == 18;
    Check((c::InspectPacket(In(wire)).code == Code::ok) == permitted,"partial type restrictions");
  }
  for (unsigned exponent = 0; exponent < 9; ++exponent) {
    Bytes short_first{0xcb, static_cast<std::uint8_t>(224+exponent)};
    short_first.insert(short_first.end(), std::size_t{1}<<exponent,0); short_first.push_back(0);
    Check(c::InspectPacket(In(short_first)).code == Code::invalid_packet,"first partial minimum512");
  }
  Bytes empty_final{0xcb,233}; empty_final.insert(empty_final.end(),512,0x7a); empty_final.push_back(0);
  Check(c::InspectPacket(In(empty_final)).code == Code::ok,"empty definite final segment");
  // Every legal partial exponent is decoded safely, including huge truncated extents.
  for (unsigned exponent = 9; exponent <= 30; ++exponent)
    Check(c::InspectPacket(In(Bytes{0xcb,static_cast<std::uint8_t>(224+exponent),0})).code == Code::invalid_packet,
        "large partial lengths cannot read beyond input");
}
void Literals() {
  const Bytes data{'a',0,0xff,'\n','\r','\n'};
  Bytes output(c::LiteralPacketEncodedSize(data.size()).bytes);
  const Bytes expected{0xcb,12,'b',0,0,0,0,0,'a',0,0xff,'\n','\r','\n'};
  forbid_heap = true;
  const auto code = c::EncodeLiteralPacket(In(data), Out(output));
  forbid_heap = false;
  Check(code == Code::ok && output == expected,"literal packet exact bytes, no newline or charset rewrite");
  Bytes body(c::InspectPacket(In(output)).body_bytes);
  Check(c::CopyPacketBody(In(output), Out(body)) == Code::ok,"literal body copy");
  auto literal = c::InspectLiteralBody(In(body));
  Check(literal.code == Code::ok && literal.format == 'b' && literal.data.size == data.size() &&
      std::equal(data.begin(),data.end(),literal.data.data),"literal bytes view");
  Bytes empty(c::LiteralPacketEncodedSize(0).bytes);
  Check(c::EncodeLiteralPacket({},Out(empty)) == Code::ok && empty == Bytes({0xcb,6,'b',0,0,0,0,0}),"empty literal is not NULL");
  for (unsigned length = 0; length <= 255; ++length) {
    Bytes named(6+length,0xff); named[0]='u'; named[1]=length;
    named.insert(named.end(),data.begin(),data.end());
    literal = c::InspectLiteralBody(In(named));
    Check(literal.code == Code::ok && literal.data.size == data.size() &&
        std::equal(data.begin(),data.end(),literal.data.data),"metadata ignored at every filename length");
    for (std::size_t n = 0; n < 6+length; ++n)
      Check(c::InspectLiteralBody({named.data(),n}).code == Code::invalid_packet,"truncated literal metadata");
  }
  for (unsigned format = 0; format < 256; ++format) {
    body[0]=format;
    Check((c::InspectLiteralBody(In(body)).code == Code::ok) ==
        (format == 'b' || format == 't' || format == 'u'),"literal format vocabulary");
  }
}
struct Interrupt {};
struct Probe {
  unsigned calls=0,stop=0; bool throwing=false;
  static bool Poll(void* context) {
    auto& p=*static_cast<Probe*>(context);
    if (++p.calls != p.stop) return false;
    if (p.throwing) throw Interrupt{};
    return true;
  }
  c::PgpCancellation Callback() { return {Poll,this}; }
};
void Cancellation() {
  Bytes body(100000,0x3a); const auto wire=Packet(18,body);
  for (unsigned operation=0;operation<3;++operation) {
    Bytes output(operation==0?wire.size():operation==1?body.size():c::LiteralPacketEncodedSize(body.size()).bytes,0xa5);
    auto run=[&](Probe& p) {
      return operation==0?c::EncodePacket(18,In(body),Out(output),p.Callback()):
          operation==1?c::CopyPacketBody(In(wire),Out(output),p.Callback()):
          c::EncodeLiteralPacket(In(body),Out(output),p.Callback());
    };
    Probe complete; Check(run(complete)==Code::ok,"complete cancel-probed operation");
    Probe inspection; Check(c::InspectPacket(In(wire),inspection.Callback()).code==Code::ok,"preflight probe count");
    for (bool throwing:{false,true}) for (unsigned stop=1;stop<=complete.calls;++stop) {
      std::fill(output.begin(),output.end(),0xa5); Probe p{0,stop,throwing};
      bool exception=false; Code code=Code::ok;
      try {code=run(p);} catch (const Interrupt&) {exception=true;}
      Check(throwing?exception:code==Code::cancelled,"every cancellation/exception point observed");
      const bool preflight=operation==1 && stop<=inspection.calls;
      Check(Filled(output,preflight?0xa5:0),"unaccepted output unchanged, accepted output fully erased");
    }
  }
  // Bounded cancellation during a malicious many-small-segment inspection.
  Bytes chain{0xd2,233}; chain.insert(chain.end(),512,0);
  for(unsigned i=0;i<10000;++i) chain.insert(chain.end(),{224,0x7f});
  chain.push_back(0);
  Probe p{0,25};
  Check(c::InspectPacket(In(chain),p.Callback()).code==Code::cancelled && p.calls==25,"partial-header work is cancellable");
}
void Extents() {
  Bytes body(1024,0x2a), out(1030,0xa5);
  auto packet=Packet(18,body); const auto original=packet;
  Check(c::EncodePacket(18,In(body),Out(out))==Code::invalid_extent && Filled(out,0xa5),"exact output size required");
  Check(c::CopyPacketBody(In(packet),{packet.data()+1,body.size()})==Code::invalid_extent && packet==original,"decode overlap rejected before writes");
  Check(c::EncodePacket(18,{packet.data()+3,body.size()},Out(packet))==Code::invalid_extent && packet==original,"encode overlap rejected");
  Check(c::InspectPacket({nullptr,1}).code==Code::invalid_extent &&
      c::InspectLiteralBody({nullptr,6}).code==Code::invalid_extent,"null extent rejected");
  const auto* end=reinterpret_cast<const std::uint8_t*>(std::numeric_limits<std::uintptr_t>::max()-3);
  Check(c::InspectPacket({end,8}).code==Code::invalid_extent,"pointer arithmetic overflow rejected");
  for (unsigned tag=0;tag<256;++tag) {
    const Bytes wire{static_cast<std::uint8_t>(tag),0};
    Check((c::InspectPacket(In(wire)).code==Code::ok)==(tag>192),"native packet header bits and reserved zero type");
  }
  Bytes invalid{0xd2,255,0xff,0xff,0xff,0xff}; Bytes untouched(16,0xa5);
  Check(c::CopyPacketBody(In(invalid),Out(untouched))==Code::invalid_packet && Filled(untouched,0xa5),"malformed shape leaves output untouched");
}
int main() {
  try {Lengths();Partial();Literals();Cancellation();Extents();}
  catch(const std::exception& e){forbid_heap=false;std::cerr<<e.what()<<'\n';return 1;}
  std::cout<<"openpgp_packet checks="<<checks<<'\n';
}
