// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_error_vector_runtime.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>
#include <type_traits>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace e = scratchbird::engine::sblr;
using B = std::vector<std::uint8_t>;
using D = e::SblrErrorVectorDescriptorV1;
using Q = e::SblrErrorVectorIssueRequestV1;
using R = e::SblrErrorVectorIssueResultV1;
using E = e::SblrErrorVectorEntryV1;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && ++failures <= 24) std::cerr << message << '\n';
}
void Put(B& b, std::size_t at, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) b[at+i] = std::uint8_t(n >> (8*i));
}
template<class A> void Copy(B& b, std::size_t at, const A& a) {
  std::copy(a.begin(), a.end(), b.begin()+at);
}
e::SblrErrorUuidV1 Id(unsigned n) {
  e::SblrErrorUuidV1 id{1,0x9e,0x44,0x55,0x66,0x77,0x70,0,0x80,0,0,0,0,0,0,0};
  id[14] = std::uint8_t(n >> 8); id[15] = std::uint8_t(n);
  return id;
}
e::SblrErrorSha256V1 Hash(std::string_view domain, const B& bytes) {
  B material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  e::SblrErrorSha256V1 digest{};
  Check(SHA256(material.data(), material.size(), digest.data()) != nullptr,
        "independent SHA256 oracle failed");
  return digest;
}
B RawEntries(std::vector<E>* entries) {
  B all;
  for (auto& entry : *entries) {
    B b(80);
    Put(b,0,entry.occurrence_ordinal,8);Copy(b,8,entry.diagnostic_uuid);
    Put(b,24,entry.diagnostic_generation,8);Put(b,32,entry.precedence_ordinal,4);
    b[36]=entry.severity_code;b[37]=entry.redaction_class;
    Put(b,40,entry.safe_field_count,4);Copy(b,48,entry.safe_fields_sha256);
    entry.entry_sha256=Hash("ScratchBird.SblrErrorVectorEntry.V1",b);
    b.resize(128);Copy(b,80,entry.entry_sha256);
    all.insert(all.end(),b.begin(),b.end());
  }
  return all;
}
B Raw(D* d) {
  B entries=RawEntries(&d->entries),b(152);
  std::copy_n("ERVD",4,b.begin());Put(b,4,1,2);Put(b,6,152,2);
  Put(b,8,152+entries.size(),4);Copy(b,16,d->descriptor_uuid);
  Put(b,32,d->descriptor_generation,8);Copy(b,40,d->registry_snapshot_uuid);
  Put(b,56,d->registry_generation,8);Copy(b,64,d->statement_receipt_uuid);
  Copy(b,80,d->diagnostic_registry_snapshot_uuid);Put(b,96,d->diagnostic_registry_generation,8);
  Put(b,104,d->entries.size(),4);Put(b,108,128,4);
  d->vector_sha256=Hash("ScratchBird.SblrErrorVectorDescriptorVector.V1",entries);
  Copy(b,112,d->vector_sha256);b.insert(b.end(),entries.begin(),entries.end());return b;
}
B Raw(Q* q) {
  B entries=RawEntries(&q->entries),b(120);
  std::copy_n("EVRQ",4,b.begin());Put(b,4,1,2);Put(b,6,120,2);
  Put(b,8,120+entries.size(),4);Copy(b,16,q->statement_receipt_uuid);
  Copy(b,32,q->registry_snapshot_uuid);Put(b,48,q->registry_generation,8);
  Copy(b,56,q->diagnostic_registry_snapshot_uuid);Put(b,72,q->diagnostic_registry_generation,8);
  Put(b,80,q->entries.size(),4);Put(b,84,128,4);
  q->entries_sha256=Hash("ScratchBird.SblrErrorVectorIssueRequest.V1",entries);
  Copy(b,88,q->entries_sha256);b.insert(b.end(),entries.begin(),entries.end());return b;
}
B Raw(const R& r) {
  B b(64);std::copy_n("EVRS",4,b.begin());Put(b,4,1,2);Put(b,6,64,2);
  Put(b,8,64+r.canonical_ervd.size(),4);Copy(b,16,r.descriptor_uuid);
  Put(b,32,r.descriptor_generation,8);Put(b,40,r.registry_generation,8);
  Put(b,48,r.canonical_ervd.size(),4);
  b.insert(b.end(),r.canonical_ervd.begin(),r.canonical_ervd.end());return b;
}
bool Equal(const E& a,const E& b) {
  return a.occurrence_ordinal==b.occurrence_ordinal && a.diagnostic_uuid==b.diagnostic_uuid &&
    a.diagnostic_generation==b.diagnostic_generation && a.precedence_ordinal==b.precedence_ordinal &&
    a.severity_code==b.severity_code && a.redaction_class==b.redaction_class &&
    a.safe_field_count==b.safe_field_count && a.safe_fields_sha256==b.safe_fields_sha256 &&
    a.entry_sha256==b.entry_sha256;
}
bool EntriesEqual(const std::vector<E>& a,const std::vector<E>& b) {
  if(a.size()!=b.size()) return false;
  for(std::size_t i=0;i<a.size();++i) if(!Equal(a[i],b[i])) return false;
  return true;
}
bool Equal(const D& a,const D& b) {
  return a.descriptor_uuid==b.descriptor_uuid && a.descriptor_generation==b.descriptor_generation &&
    a.registry_snapshot_uuid==b.registry_snapshot_uuid && a.registry_generation==b.registry_generation &&
    a.statement_receipt_uuid==b.statement_receipt_uuid &&
    a.diagnostic_registry_snapshot_uuid==b.diagnostic_registry_snapshot_uuid &&
    a.diagnostic_registry_generation==b.diagnostic_registry_generation &&
    a.vector_sha256==b.vector_sha256 && EntriesEqual(a.entries,b.entries);
}
bool Equal(const Q& a,const Q& b) {
  return a.registry_snapshot_uuid==b.registry_snapshot_uuid && a.registry_generation==b.registry_generation &&
    a.statement_receipt_uuid==b.statement_receipt_uuid &&
    a.diagnostic_registry_snapshot_uuid==b.diagnostic_registry_snapshot_uuid &&
    a.diagnostic_registry_generation==b.diagnostic_registry_generation &&
    a.entries_sha256==b.entries_sha256 && EntriesEqual(a.entries,b.entries);
}
bool Equal(const R& a,const R& b) {
  return a.descriptor_uuid==b.descriptor_uuid && a.descriptor_generation==b.descriptor_generation &&
    a.registry_generation==b.registry_generation && a.canonical_ervd==b.canonical_ervd;
}
B SafeField(unsigned type,std::string_view key,const B& value) {
  B bytes(8);Put(bytes,0,key.size(),2);Put(bytes,2,type,2);Put(bytes,4,value.size(),4);
  bytes.insert(bytes.end(),key.begin(),key.end());
  bytes.insert(bytes.end(),value.begin(),value.end());
  bytes.resize((bytes.size()+3)&~std::size_t(3),0);return bytes;
}
e::SblrErrorSha256V1 SafeHash(const B& fields,unsigned count) {
  B preimage(4,0);Put(preimage,0,count,4);
  preimage.insert(preimage.end(),fields.begin(),fields.end());
  return Hash("ScratchBird.SblrErrorVectorSafeFields.V1",preimage);
}
void SafeFields() {
  const auto empty=SafeHash({},0);
  Check(e::SblrErrorVectorEmptySafeFieldsHashV1()==empty,"empty safe-field constant differs from SHA256 oracle");
  e::SblrErrorSha256V1 output{};
  Check(e::ComputeSblrErrorVectorSafeFieldsHashV1({},0,&output)&&output==empty,
        "empty safe-field hash mismatch");
  const auto reject=[&](const B& bytes,unsigned count) {
    output.fill(0x5a);const auto prior=output;
    Check(!e::ComputeSblrErrorVectorSafeFieldsHashV1(bytes,count,&output),"malformed safe fields hashed");
    Check(output==prior,"failed safe-field hash changed output");
  };
  B all;
  for(unsigned type=1;type<=8;++type) {
    B value;
    if(type==1)value={'s','a','f','e'};
    if(type==2)value={1};
    if(type==3||type==4)value=B(8,0xff);
    if(type==5||type==8){const auto id=Id(type);value.assign(id.begin(),id.end());}
    if(type==6)value={0,0xff,0x7f};
    if(type==8){auto user=Id(500);user[6]=0x40;value.insert(value.end(),user.begin(),user.end());}
    const std::string key(1,char('a'+type));const auto field=SafeField(type,key,value);
    const auto expected=SafeHash(field,1);
    Check(e::ComputeSblrErrorVectorSafeFieldsHashV1(field,1,&output)&&output==expected,
          "scalar safe-field preimage differs from independent oracle");
    all.insert(all.end(),field.begin(),field.end());
    for(std::size_t n=0;n<field.size();++n)reject(B(field.begin(),field.begin()+n),1);
  }
  Check(e::ComputeSblrErrorVectorSafeFieldsHashV1(all,8,&output)&&output==SafeHash(all,8),
        "ordered mixed scalar safe-field preimage mismatch");
  reject(all,7);reject(all,9);reject(all,0);reject({},1);reject({},146);
  auto trailing=all;trailing.push_back(0);reject(trailing,8);
  auto duplicate=SafeField(7,"same",{});const auto same=duplicate;
  duplicate.insert(duplicate.end(),same.begin(),same.end());
  reject(duplicate,2);
  auto reversed=SafeField(7,"z",{});const auto first=SafeField(7,"a",{});
  reversed.insert(reversed.end(),first.begin(),first.end());reject(reversed,2);
  reject(SafeField(2,"b",{2}),1);reject(SafeField(7,"n",{0}),1);
  reject(SafeField(1,"bad",{0xc0,0x80}),1);reject(SafeField(1,"bad",{0}),1);
  reject(SafeField(8,"bad",B(16,0)),1);reject(SafeField(5,"bad",B(36,'a')),1);
  reject(SafeField(256,"bad",{}),1);
  auto pad=SafeField(7,"a",{});pad.back()=1;reject(pad,1);
  B maximum;
  for(unsigned i=0;i<145;++i) {
    const std::string key{char('a'+i/10),char('0'+i%10)};
    const auto field=SafeField(7,key,{});maximum.insert(maximum.end(),field.begin(),field.end());
  }
  Check(e::ComputeSblrErrorVectorSafeFieldsHashV1(maximum,145,&output)&&output==SafeHash(maximum,145),
        "maximum safe-field count failed");
  const auto max_bytes=SafeField(6,"a",B(262132,0x7f));
  Check(max_bytes.size()==262144 && e::ComputeSblrErrorVectorSafeFieldsHashV1(max_bytes,1,&output)&&
        output==SafeHash(max_bytes,1),"maximum safe-field byte extent failed");
  auto oversized=max_bytes;oversized.push_back(0);reject(oversized,1);
  Check(!e::ComputeSblrErrorVectorSafeFieldsHashV1({},0,nullptr),"null safe-field output accepted");
  unsigned failures_injected=0;bool completed=false;
  for(long index=0;index<256;++index) {
    output.fill(0x5a);const auto prior=output;
    fail_after=index;const bool ok=e::ComputeSblrErrorVectorSafeFieldsHashV1(all,8,&output);fail_after=-1;
    if(ok){Check(output==SafeHash(all,8),"allocation sweep produced wrong safe-field digest");completed=true;break;}
    ++failures_injected;Check(output==prior,"safe-field allocation failure published partial hash");
  }
  Check(completed&&failures_injected>0,"safe-field allocation sweep did not finish actual failure sites");
  std::cout<<"safe_field_allocation_points="<<failures_injected<<'\n';
}
D Base(unsigned count=2) {
  D d;d.descriptor_uuid=Id(1);d.registry_snapshot_uuid=Id(2);
  d.statement_receipt_uuid=Id(3);d.diagnostic_registry_snapshot_uuid=Id(4);
  d.descriptor_generation=5;d.registry_generation=6;d.diagnostic_registry_generation=7;
  for(unsigned i=1;i<=count;++i) {
    E entry;entry.occurrence_ordinal=i;entry.precedence_ordinal=i;
    entry.diagnostic_uuid=Id(10+i);entry.diagnostic_generation=8;
    entry.severity_code=3;entry.redaction_class=1;entry.safe_field_count=1;
    entry.safe_fields_sha256=SafeHash(SafeField(4,"count",B(8,0)),1);
    d.entries.push_back(entry);
  }
  return d;
}
Q Request(const D& d) {
  Q q;q.statement_receipt_uuid=d.statement_receipt_uuid;q.registry_snapshot_uuid=d.registry_snapshot_uuid;
  q.registry_generation=d.registry_generation;q.diagnostic_registry_snapshot_uuid=d.diagnostic_registry_snapshot_uuid;
  q.diagnostic_registry_generation=d.diagnostic_registry_generation;q.entries=d.entries;return q;
}
R Response(const D& d,const B& bytes) {
  R r;r.descriptor_uuid=d.descriptor_uuid;r.descriptor_generation=d.descriptor_generation;
  r.registry_generation=d.registry_generation;r.canonical_ervd=bytes;return r;
}
void RejectD(D d) {
  const D before=d;
  Check(e::EncodeSblrErrorVectorDescriptorV1(&d).empty(),"invalid ERVD encoded");
  Check(Equal(d,before),"failed ERVD encode mutated source");
  auto bytes=Raw(&d);D output=before;
  Check(!e::DecodeSblrErrorVectorDescriptorV1(bytes.data(),bytes.size(),&output,nullptr),
        "resealed invalid ERVD decoded");
  Check(Equal(output,before),"failed ERVD decode mutated output");
}
void RejectQ(Q q) {
  const Q before=q;
  Check(e::EncodeSblrErrorVectorIssueRequestV1(&q).empty(),"invalid EVRQ encoded");
  Check(Equal(q,before),"failed EVRQ encode mutated source");
  auto bytes=Raw(&q);Q output=before;
  Check(!e::DecodeSblrErrorVectorIssueRequestV1(bytes.data(),bytes.size(),&output,nullptr),
        "resealed invalid EVRQ decoded");
  Check(Equal(output,before),"failed EVRQ decode mutated output");
}
void RejectR(const R& r) {
  Check(e::EncodeSblrErrorVectorIssueResultV1(r).empty(),"invalid EVRS encoded");
  auto bytes=Raw(r);R output=r;
  Check(!e::DecodeSblrErrorVectorIssueResultV1(bytes.data(),bytes.size(),&output,nullptr),
        "invalid EVRS decoded");
  Check(Equal(output,r),"failed EVRS decode mutated output");
}
template<class T,class F> void RejectBytes(const B& bytes,const T& sentinel,F decode) {
  T output=sentinel;
  Check(!decode(bytes.data(),bytes.size(),&output,nullptr),"corrupted/truncated bytes decoded");
  Check(Equal(output,sentinel),"byte refusal mutated output");
}
template<class T,class F> void ByteFaults(const B& bytes,const T& sentinel,F decode,
                                         std::size_t protected_start) {
  for(std::size_t n=0;n<bytes.size();++n)
    RejectBytes(B(bytes.begin(),bytes.begin()+n),sentinel,decode);
  // These formats hash entries, not every header identity. A changed but
  // structurally valid unbound header UUID is an admission-layer refusal,
  // not something a standalone structural decoder can infer.
  for(std::size_t n=protected_start;n<bytes.size();++n) {
    B bad=bytes;bad[n]^=1;RejectBytes(bad,sentinel,decode);
  }
  B extra=bytes;extra.push_back(0);RejectBytes(extra,sentinel,decode);
  Check(!decode(nullptr,bytes.size(),static_cast<T*>(nullptr),nullptr),"null codec input/output accepted");
  Check(!decode(bytes.data(),bytes.size(),static_cast<T*>(nullptr),nullptr),"null output accepted");
}
template<class T,class F,class Expected> void Allocations(const T& initial,F operation,Expected expected) {
  bool completed=false;unsigned points=0;
  for(long i=0;i<512;++i) {
    T object=initial;fail_after=i;bool ok=false;bool threw=false;
    try {ok=operation(&object);}catch(...){threw=true;}
    fail_after=-1;
    Check(!threw,"allocation failure escaped codec");
    if(ok) {Check(expected(object),"successful sweep changed data");completed=true;break;}
    ++points;Check(Equal(object,initial),"allocation failure mutated caller");
  }
  Check(completed && points>0,"allocation sweep incomplete");
  std::cout<<"allocation_points="<<points<<'\n';
}
}
int main() {
  SafeFields();
  // Preserve the existing diagnostic-record ceiling, including counts above
  // the separate 64-parameter MessageVector section limit.
  for(unsigned count:{0u,65u,145u}) {
    B fields;
    for(unsigned i=0;i<count;++i) {
      const std::string key{char('a'+i/10),char('0'+i%10)};
      const auto field=SafeField(7,key,{});fields.insert(fields.end(),field.begin(),field.end());
    }
    D descriptor=Base(1);descriptor.entries[0].safe_field_count=count;
    descriptor.entries[0].safe_fields_sha256=SafeHash(fields,count);
    D canonical=descriptor;const auto bytes=Raw(&canonical);
    Check(e::EncodeSblrErrorVectorDescriptorV1(&descriptor)==bytes,
          "admitted safe-field count rejected by ERVD");
    Q request=Request(descriptor),expected_request=request;
    Check(e::EncodeSblrErrorVectorIssueRequestV1(&request)==Raw(&expected_request),
          "admitted safe-field count rejected by EVRQ");
    const auto response=Response(canonical,bytes);
    Check(e::EncodeSblrErrorVectorIssueResultV1(response)==Raw(response),
          "admitted safe-field count rejected by EVRS");
  }
  for(unsigned malformed=0;malformed<2;++malformed) {
    D empty=Base(1);empty.entries[0].safe_field_count=0;
    empty.entries[0].safe_fields_sha256=Hash("ScratchBird.SblrErrorVectorSafeFields.V1",B(4,0));
    if(malformed)empty.entries[0].safe_fields_sha256[0]^=1;
    else empty.entries[0].safe_field_count=146;
    RejectD(empty);RejectQ(Request(empty));RejectR(Response(empty,Raw(&empty)));
  }
  static_assert(std::is_nothrow_move_assignable_v<D> && std::is_nothrow_move_assignable_v<Q>);
  D base=Base(),sealed=base;B db=Raw(&sealed);
  D actual=base,decoded;
  Check(e::EncodeSblrErrorVectorDescriptorV1(&actual)==db && Equal(actual,sealed),"ERVD exact oracle mismatch");
  Check(e::DecodeSblrErrorVectorDescriptorV1(db.data(),db.size(),&decoded,nullptr) && Equal(decoded,sealed),
        "ERVD roundtrip lost fields");
  Q q=Request(base),qs=q;B qb=Raw(&qs);Q qa=q,qd;
  Check(e::EncodeSblrErrorVectorIssueRequestV1(&qa)==qb && Equal(qa,qs),"EVRQ exact oracle mismatch");
  Check(e::DecodeSblrErrorVectorIssueRequestV1(qb.data(),qb.size(),&qd,nullptr) && Equal(qd,qs),
        "EVRQ roundtrip lost fields");
  R r=Response(sealed,db),rd;B rb=Raw(r);
  Check(e::EncodeSblrErrorVectorIssueResultV1(r)==rb,"EVRS exact oracle mismatch");
  Check(e::DecodeSblrErrorVectorIssueResultV1(rb.data(),rb.size(),&rd,nullptr) && Equal(rd,r),
        "EVRS roundtrip lost fields");
  for(unsigned severity=1;severity<=13;++severity) {
    D x=base;for(auto& entry:x.entries) entry.severity_code=severity;
    D sx=x;const B expected=Raw(&sx);
    Check(e::EncodeSblrErrorVectorDescriptorV1(&x)==expected && Equal(x,sx),"canonical severity lost in ERVD");
    D dx;Check(e::DecodeSblrErrorVectorDescriptorV1(expected.data(),expected.size(),&dx,nullptr) &&
      Equal(dx,sx),"canonical severity rejected by ERVD decoder");
    Q y=Request(x),sy=y;const B yb=Raw(&sy);
    Check(e::EncodeSblrErrorVectorIssueRequestV1(&y)==yb && Equal(y,sy),"canonical severity lost in EVRQ");
    Q dy;Check(e::DecodeSblrErrorVectorIssueRequestV1(yb.data(),yb.size(),&dy,nullptr) &&
      Equal(dy,sy),"canonical severity rejected by EVRQ decoder");
    const R response=Response(sx,expected);const B response_bytes=Raw(response);R parsed;
    Check(e::EncodeSblrErrorVectorIssueResultV1(response)==response_bytes,
          "canonical severity lost in EVRS");
    Check(e::DecodeSblrErrorVectorIssueResultV1(response_bytes.data(),response_bytes.size(),&parsed,nullptr) &&
          Equal(parsed,response),"canonical severity rejected by EVRS decoder");
  }
  for(unsigned severity=0;severity<256;++severity) if(severity==0||severity>13) {
    D d=base;d.entries.back().severity_code=severity;RejectD(d);RejectQ(Request(d));
  }
  for(unsigned slot=0;slot<5;++slot) for(unsigned variant=0;variant<4;++variant) {
    D d=base;auto* id=slot==0?&d.descriptor_uuid:slot==1?&d.registry_snapshot_uuid:
      slot==2?&d.statement_receipt_uuid:slot==3?&d.diagnostic_registry_snapshot_uuid:&d.entries[0].diagnostic_uuid;
    if(variant==0) *id={};else if(variant==1) (*id)[6]=0x40;
    else if(variant==2) (*id)[8]=0;else {*id={};(*id)[0]=1;}
    RejectD(d);if(slot!=0) RejectQ(Request(d));
  }
  for(unsigned slot=0;slot<3;++slot) {D d=base;
    if(slot==0)d.descriptor_generation=0;else if(slot==1)d.registry_generation=0;else d.diagnostic_registry_generation=0;
    RejectD(d);if(slot!=0)RejectQ(Request(d));
  }
  {auto d=base;d.entries.clear();RejectD(d);RejectQ(Request(d));}
  for(unsigned field=0;field<5;++field) {
    auto d=base;auto& x=d.entries.back();
    if(field==0)x.occurrence_ordinal=3;else if(field==1)x.precedence_ordinal=0;
    else if(field==2)x.diagnostic_generation=0;else if(field==3)x.redaction_class=5;
    else x.safe_fields_sha256={};
    RejectD(d);RejectQ(Request(d));
  }
  // Independent ERROR_VECTOR_REGISTRY_SUBSET_ORDER_V1 oracles: rank is a
  // registry identity attribute, never the vector-local occurrence number.
  const auto accept_order=[](D d) {
    D sealed=d;const auto db=Raw(&sealed);D decoded;
    Check(e::EncodeSblrErrorVectorDescriptorV1(&d)==db,"registry-rank ERVD encode failed");
    Check(e::DecodeSblrErrorVectorDescriptorV1(db.data(),db.size(),&decoded,nullptr) &&
          Equal(decoded,sealed),"registry-rank ERVD decode failed");
    Q q=Request(sealed),qs=q,qd;const auto qb=Raw(&qs);
    Check(e::EncodeSblrErrorVectorIssueRequestV1(&q)==qb,"registry-rank EVRQ encode failed");
    Check(e::DecodeSblrErrorVectorIssueRequestV1(qb.data(),qb.size(),&qd,nullptr) &&
          Equal(qd,qs),"registry-rank EVRQ decode failed");
    const auto r=Response(sealed,db);const auto rb=Raw(r);R rd;
    Check(e::EncodeSblrErrorVectorIssueResultV1(r)==rb,"registry-rank EVRS encode failed");
    Check(e::DecodeSblrErrorVectorIssueResultV1(rb.data(),rb.size(),&rd,nullptr) &&
          Equal(rd,r),"registry-rank EVRS decode failed");
  };
  for(unsigned rank=1;rank<=4096;++rank) {
    auto d=Base(1);d.entries[0].precedence_ordinal=rank;accept_order(d);
  }
  {
    auto d=Base(4);d.entries[0].precedence_ordinal=7;
    d.entries[1].precedence_ordinal=91;
    d.entries[2]=d.entries[1];d.entries[2].occurrence_ordinal=3;
    d.entries[2].safe_fields_sha256=SafeHash(SafeField(4,"count",B(8,1)),1);
    d.entries[3].precedence_ordinal=4096;accept_order(d);
    for(unsigned field=0;field<5;++field) {
      auto bad=d;auto& repeated=bad.entries[2];
      if(field==0)repeated.diagnostic_uuid=Id(900);
      else if(field==1)++repeated.diagnostic_generation;
      else if(field==2)++repeated.severity_code;
      else if(field==3)++repeated.redaction_class;
      else repeated.precedence_ordinal=90;
      RejectD(bad);RejectQ(Request(bad));RejectR(Response(bad,Raw(&bad)));
    }
    auto repeated=d.entries[1];d.entries.assign(4096,repeated);
    for(unsigned i=0;i<4096;++i)d.entries[i].occurrence_ordinal=i+1;
    accept_order(d);
  }
  for(unsigned fault=0;fault<4;++fault) {
    auto bad=Base(2);
    if(fault==0)bad.entries[1].precedence_ordinal=4097;
    else if(fault==1)bad.entries[1].precedence_ordinal=1;
    else if(fault==2)bad.entries[1].diagnostic_uuid=bad.entries[0].diagnostic_uuid;
    else {bad.entries[0].precedence_ordinal=91;bad.entries[1].precedence_ordinal=7;}
    RejectD(bad);RejectQ(Request(bad));RejectR(Response(bad,Raw(&bad)));
  }
  for(unsigned fault=0;fault<6;++fault) {
    R bad=r;if(fault==0)bad.descriptor_uuid=Id(900);else if(fault==1)++bad.descriptor_generation;
    else if(fault==2)++bad.registry_generation;else if(fault==3)bad.canonical_ervd={1};
    else if(fault==4)bad.canonical_ervd[112]^=1;else bad.descriptor_uuid[6]=0x40;
    RejectR(bad);
  }
  ByteFaults(db,base,e::DecodeSblrErrorVectorDescriptorV1,112);
  ByteFaults(qb,q,e::DecodeSblrErrorVectorIssueRequestV1,88);
  ByteFaults(rb,r,e::DecodeSblrErrorVectorIssueResultV1,176);
  for(unsigned at:{0u,4u,6u,8u,12u,104u,108u,144u}) {
    B bad=db;bad[at]^=1;RejectBytes(bad,base,e::DecodeSblrErrorVectorDescriptorV1);
  }
  for(unsigned at:{0u,4u,6u,8u,12u,80u,84u}) {
    B bad=qb;bad[at]^=1;RejectBytes(bad,q,e::DecodeSblrErrorVectorIssueRequestV1);
  }
  for(unsigned at:{0u,4u,6u,8u,12u,48u,52u,56u}) {
    B bad=rb;bad[at]^=1;RejectBytes(bad,r,e::DecodeSblrErrorVectorIssueResultV1);
  }
  Check(e::EncodeSblrErrorVectorDescriptorV1(nullptr).empty(),"null descriptor encoded");
  Check(e::EncodeSblrErrorVectorIssueRequestV1(nullptr).empty(),"null request encoded");
  {D maximum=Base(4096),copy=maximum;auto expected=Raw(&copy);
    Check(e::EncodeSblrErrorVectorDescriptorV1(&maximum)==expected,"maximum exact ERVD failed");
    D maximum_decoded;
    Check(e::DecodeSblrErrorVectorDescriptorV1(expected.data(),expected.size(),&maximum_decoded,nullptr) &&
          Equal(maximum_decoded,copy),"maximum ERVD decode failed");
    Q maximum_q=Request(copy),maximum_q_sealed=maximum_q;
    const auto maximum_q_bytes=Raw(&maximum_q_sealed);
    Check(e::EncodeSblrErrorVectorIssueRequestV1(&maximum_q)==maximum_q_bytes,"maximum EVRQ encode failed");
    Q maximum_q_decoded;
    Check(e::DecodeSblrErrorVectorIssueRequestV1(maximum_q_bytes.data(),maximum_q_bytes.size(),&maximum_q_decoded,nullptr) &&
          Equal(maximum_q_decoded,maximum_q_sealed),"maximum EVRQ decode failed");
    const R maximum_r=Response(copy,expected);const auto maximum_r_bytes=Raw(maximum_r);R maximum_r_decoded;
    Check(e::EncodeSblrErrorVectorIssueResultV1(maximum_r)==maximum_r_bytes,"maximum EVRS encode failed");
    Check(e::DecodeSblrErrorVectorIssueResultV1(maximum_r_bytes.data(),maximum_r_bytes.size(),&maximum_r_decoded,nullptr) &&
          Equal(maximum_r_decoded,maximum_r),"maximum EVRS decode failed");
    maximum=Base(4097);RejectD(maximum);RejectQ(Request(maximum));}
  Allocations(base,[](D* x){return !e::EncodeSblrErrorVectorDescriptorV1(x).empty();},
    [&](const D& x){return Equal(x,sealed);});
  Allocations(q,[](Q* x){return !e::EncodeSblrErrorVectorIssueRequestV1(x).empty();},
    [&](const Q& x){return Equal(x,qs);});
  Allocations(r,[](R* x){return !e::EncodeSblrErrorVectorIssueResultV1(*x).empty();},
    [&](const R& x){return Equal(x,r);});
  Allocations(base,[&](D* x){return e::DecodeSblrErrorVectorDescriptorV1(db.data(),db.size(),x,nullptr);},
    [&](const D& x){return Equal(x,sealed);});
  Allocations(q,[&](Q* x){return e::DecodeSblrErrorVectorIssueRequestV1(qb.data(),qb.size(),x,nullptr);},
    [&](const Q& x){return Equal(x,qs);});
  Allocations(r,[&](R* x){return e::DecodeSblrErrorVectorIssueResultV1(rb.data(),rb.size(),x,nullptr);},
    [&](const R& x){return Equal(x,r);});
  std::cout<<"error_vector checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
