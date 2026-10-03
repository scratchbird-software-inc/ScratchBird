// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_value_codec.hpp"
#include "catalog_runtime_authority_binding.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>

namespace {
bool deny_heap = false;
std::size_t attempted_allocations = 0, checks = 0;
void Allocation() {
  if (deny_heap) { ++attempted_allocations; throw std::bad_alloc(); }
}
}
void* operator new(std::size_t n) { Allocation(); if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) {
  Allocation(); void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1) == 0) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
using E = c::CatalogValueError;
using T = c::CatalogValueType;
using Bytes = std::vector<p::byte>;
void Check(bool ok, const char* why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) { std::cerr << at.line() << ": " << why << '\n'; throw why; }
}
template <typename F> auto NoHeap(F f) {
  deny_heap = true;
  try { auto result = f(); deny_heap = false; return result; }
  catch (...) { deny_heap = false; throw; }
}
void Num(Bytes& bytes, std::size_t at, unsigned width, p::u64 n) {
  for (unsigned i = 0; i < width; ++i) bytes[at + i] = static_cast<p::byte>(n >> (i * 8));
}
Bytes Integer(p::u64 n) { Bytes b(8); Num(b, 0, 8, n); return b; }
p::Uuid Id(unsigned n) {
  p::Uuid id;
  for (unsigned i = 0; i < 16; ++i) id.bytes[i] = static_cast<p::byte>(n + 11 * i);
  id.bytes[6] = (id.bytes[6] & 15) | 0x70; id.bytes[8] = (id.bytes[8] & 63) | 0x80;
  return id;
}
Bytes Identity(unsigned n) { const auto id = Id(n); return {id.bytes.begin(), id.bytes.end()}; }
struct WireField { p::u16 id; T type; Bytes value; };
// Independent Core offsets; deliberately never call the production encoder.
Bytes Wire(std::span<const WireField> fields, p::u32 schema = 123, p::u16 version = 1) {
  Bytes b(24, 0); b[0]='S'; b[1]='B'; b[2]='C'; b[3]='V';
  Num(b, 4, 2, 1); Num(b, 6, 2, 24); Num(b, 12, 4, fields.size());
  Num(b, 16, 4, schema); Num(b, 20, 2, version);
  for (const auto& f : fields) {
    const auto at = b.size(); b.resize(at + 8 + f.value.size());
    Num(b, at, 2, f.id); b[at + 2] = static_cast<p::byte>(f.type);
    Num(b, at + 4, 4, f.value.size()); std::copy(f.value.begin(), f.value.end(), b.begin() + at + 8);
  }
  Num(b, 8, 4, b.size()); return b;
}
Bytes TextList() {
  // Three elements: empty, embedded NUL/newline, U+10FFFF.
  return {3,0,0,0, 0,0,0,0, 3,0,0,0, 'a',0,10, 4,0,0,0, 0xf4,0x8f,0xbf,0xbf};
}
c::CatalogValueSchema Schema() {
  return {123, 1, {{1,T::unsigned_integer,true,8}, {2,T::boolean,true,1},
      {3,T::utf8_text,true,64}, {4,T::opaque_bytes,true,64},
      {5,T::engine_identity,true,16,p::UuidKind::object}, {6,T::user_uuid_data,true,16},
      {7,T::engine_identity_list,true,32,p::UuidKind::principal}, {8,T::utf8_text_list,true,64}}};
}
std::vector<WireField> Fields() {
  Bytes ids = Identity(3); const auto next = Identity(4); ids.insert(ids.end(), next.begin(), next.end());
  return {{1,T::unsigned_integer,Integer(0xfedcba9876543210ull)}, {2,T::boolean,{1}},
      {3,T::utf8_text,{'a',0,10,13,0xc2,0x80}}, {4,T::opaque_bytes,{0,0xff,0x80}},
      {5,T::engine_identity,Identity(1)}, {6,T::user_uuid_data,Bytes(16,0)},
      {7,T::engine_identity_list,ids}, {8,T::utf8_text_list,TextList()}};
}
c::CatalogValueDecodeViewResult Decode(c::CatalogValueSchemaView schema, std::span<const p::byte> bytes,
                                      std::span<c::CatalogValueFieldView> backing) {
  return NoHeap([&] { return c::DecodeCatalogValueBlockInto(schema, bytes, backing); });
}
void Expect(const c::CatalogValueSchema& schema, const Bytes& b, E error) {
  std::array<c::CatalogValueFieldView, 16> backing;
  for (auto& f : backing) f.id = 65535;
  std::array<unsigned char, sizeof(backing)> before;
  std::memcpy(before.data(), backing.data(), before.size());
  const auto borrowed = Decode(c::BorrowCatalogValueSchema(schema), b, backing);
  Check(borrowed.error == error, "borrowed exact diagnostic");
  const auto owning = c::DecodeCatalogValueBlock(schema, b);
  Check(owning.error == error, "owning exact diagnostic");
  if (error != E::none) {
    Check(borrowed.fields.empty() && owning.fields.empty(), "failure exposed partial fields");
    Check(std::memcmp(before.data(), backing.data(), before.size()) == 0, "failure changed backing");
  } else {
    Check(borrowed.fields.size() == owning.fields.size(), "field count mismatch");
    const auto roundtrip = c::EncodeCatalogValueBlock(schema, owning.fields);
    Check(roundtrip.ok() && roundtrip.bytes == b, "owning canonical roundtrip");
    for (const auto& f : borrowed.fields)
      Check(f.bytes.data() >= b.data() && f.bytes.data() + f.bytes.size() <= b.data() + b.size(), "payload cloned");
  }
}
void FieldAdmission() {
  const auto schema=Schema();auto values=Fields();
  const auto validate=[&](E expected){
    std::array<c::CatalogValueFieldView,8> views;
    for(std::size_t i=0;i<values.size();++i)
      views[i]={values[i].id,values[i].type,schema.fields[i].identity_kind,values[i].value};
    Check(NoHeap([&]{return c::ValidateCatalogValueFields(c::BorrowCatalogValueSchema(schema),
        {views.data(),values.size()});})==expected,"native field admission exact independent error");
  };
  validate(E::none);
  for(unsigned i=0;i<8;++i){
    values=Fields();values[i].value.resize(schema.fields[i].maximum_bytes+1);validate(E::size_limit);
  }
  for(unsigned value=2;value<256;++value){values=Fields();values[1].value[0]=value;validate(E::invalid_value);}
  for(unsigned kind=0;kind<256;++kind){
    const auto bytes=Identity(1);
    const c::CatalogValueFieldView view{5,T::engine_identity,static_cast<p::UuidKind>(kind),bytes};
    const c::CatalogValueSchemaView one{123,1,{schema.fields.data()+4,1}};
    Check(NoHeap([&]{return c::ValidateCatalogValueFields(one,{&view,1});})==
        (kind==static_cast<unsigned>(p::UuidKind::object)?E::none:E::invalid_value),"native UUID kind exact error");
  }
  values=Fields();values[2].value={0xc0,0x80};validate(E::invalid_value);
  values=Fields();values[6].value.pop_back();validate(E::invalid_value);
  values=Fields();values[7].value={0,0,0};validate(E::invalid_value);
  values=Fields();values.pop_back();validate(E::missing_field);
  values=Fields();values[1].id=1;validate(E::invalid_framing);
  values=Fields();values[2].id=65535;validate(E::missing_field);
  values=Fields();values[2].type=T::opaque_bytes;validate(E::type_mismatch);
  const c::CatalogValueSchema invalid{};
  Check(NoHeap([&]{return c::ValidateCatalogValueFields(c::BorrowCatalogValueSchema(invalid),{});})==E::invalid_schema,
      "invalid schema exact field diagnostic");
  const c::CatalogValueFieldSchema sizes[]={{1,T::opaque_bytes,true,131040},{2,T::opaque_bytes,true,131040}};
  const c::CatalogValueSchemaView big{321,1,sizes};
  const Bytes first(65536),second(65536);
  const c::CatalogValueFieldView too_large[]={{1,T::opaque_bytes,p::UuidKind::unknown,first},
      {2,T::opaque_bytes,p::UuidKind::unknown,second}};
  Check(NoHeap([&]{return c::ValidateCatalogValueFields(big,too_large);})==E::size_limit,"total field block bound");
}
void ValuesAndErrors() {
  FieldAdmission();
  const auto schema = Schema(); const auto fields = Fields(); const auto b = Wire(fields);
  Expect(schema, b, E::none);
  std::array<c::CatalogValueFieldView, 8> backing;
  const auto v = Decode(c::BorrowCatalogValueSchema(schema), b, backing);
  Check(v.ok() && v.fields.data() == backing.data(), "caller field array not used");
  for (std::size_t i = 0; i < fields.size(); ++i) {
    Check(v.fields[i].id == fields[i].id && v.fields[i].type == fields[i].type, "type/id changed");
    Check(std::equal(v.fields[i].bytes.begin(), v.fields[i].bytes.end(), fields[i].value.begin(), fields[i].value.end()), "value loss");
  }
  Check(v.fields[0].unsigned_value() == 0xfedcba9876543210ull, "native integer access");
  Check(v.fields[4].identity()->value == Id(1) && v.fields[4].identity()->kind == p::UuidKind::object, "native identity access");
  Check(!v.fields[4].unsigned_value() && !v.fields[0].identity(), "accessor accepts wrong type");
  for (std::size_t n = 0; n < b.size(); ++n) Expect(schema, {b.begin(), b.begin() + n}, E::invalid_framing);
  for (std::size_t at : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 10u, 11u, 22u, 23u}) {
    auto bad = b; bad[at] ^= 1; Expect(schema, bad, E::invalid_framing);
  }
  for (unsigned at : {4u, 5u}) { auto bad = b; bad[at] ^= 1; Expect(schema, bad, E::unsupported_version); }
  for (unsigned at : {16u, 17u, 18u, 19u, 20u, 21u}) { auto bad = b; bad[at] ^= 1; Expect(schema, bad, E::invalid_schema); }
  auto bad = b; Num(bad, 12, 4, 9); Expect(schema, bad, E::invalid_framing);
  bad = b; bad.push_back(0); Num(bad,8,4,bad.size()); Expect(schema,bad,E::invalid_framing);
  auto fs = fields; fs[1].id = 1; Expect(schema, Wire(fs), E::invalid_framing);
  fs = fields; fs.back().id = 9; Expect(schema, Wire(fs), E::missing_field);
  auto optional = schema; optional.fields.back().required = false; Expect(optional, Wire(fs), E::unknown_field);
  fs = fields; fs.pop_back(); Expect(schema, Wire(fs), E::missing_field); Expect(optional, Wire(fs), E::none);
  fs = fields; fs[0].type = T::boolean; Expect(schema, Wire(fs), E::type_mismatch);
  fs = fields; fs[0].value.resize(7); Expect(schema, Wire(fs), E::invalid_value);
  fs[0].value.resize(9); Expect(schema, Wire(fs), E::size_limit);
  fs = fields; fs[6].value.pop_back(); Expect(schema, Wire(fs), E::invalid_value);
  fs = fields; fs[7].value = {0,0,0}; Expect(schema, Wire(fs), E::invalid_value);
  for (unsigned value : {4u,255u}) { fs = fields; fs[7].value[0] = value; Expect(schema,Wire(fs),E::invalid_value); }
  fs = fields; fs[7].value[8] = 255; Expect(schema,Wire(fs),E::invalid_value);
  fs = fields; fs[7].value.push_back(0); Expect(schema,Wire(fs),E::invalid_value);
  fs = fields; fs[7].value.back() = 0xff; Expect(schema,Wire(fs),E::invalid_value);
  for (unsigned value=2; value<256; ++value) { fs=fields; fs[1].value[0]=value; Expect(schema,Wire(fs),E::invalid_value); }
  for (const Bytes text : {Bytes{0x80}, Bytes{0xc0,0x80}, Bytes{0xe0,0x80,0x80}, Bytes{0xed,0xa0,0x80},
                         Bytes{0xf4,0x90,0x80,0x80}, Bytes{0xf0,0x90,0x80}, Bytes{0xf5,0x80,0x80,0x80}}) {
    fs=fields; fs[2].value=text; Expect(schema,Wire(fs),E::invalid_value);
  }
  fs=fields; fs[2].value.clear(); fs[3].value.clear(); fs[6].value.clear(); fs[7].value={0,0,0,0};
  Expect(schema,Wire(fs),E::none);
  // Multiple simultaneous defects establish precedence, not just parity.
  bad=b; bad[4]=2; bad[22]=1; Expect(schema,bad,E::unsupported_version);
  bad=b; bad[16]^=1; bad[22]=1; Expect(schema,bad,E::invalid_framing);
  fs=fields; fs[1].id=1; fs[1].type=T::opaque_bytes; Expect(schema,Wire(fs),E::invalid_framing);
  auto invalid=schema; invalid.fields[0].id=0; Expect(invalid,{},E::invalid_schema);
  invalid=schema; invalid.fields[0].maximum_bytes=7; Expect(invalid,b,E::invalid_schema);
  invalid=schema; invalid.fields[3].identity_kind=p::UuidKind::object; Expect(invalid,b,E::invalid_schema);
  invalid=schema; invalid.fields[7].maximum_bytes=3; Expect(invalid,b,E::invalid_schema);
  invalid=schema; invalid.fields[0].type=static_cast<T>(9); Expect(invalid,b,E::invalid_schema);
}
void UuidMatrix() {
  for (unsigned kind=0;kind<256;++kind) for (unsigned version=0;version<16;++version)
    for (unsigned variant=0;variant<4;++variant) for (const auto type : {T::engine_identity,T::engine_identity_list}) {
      const c::CatalogValueSchema schema{123,1,{{1,type,true,16,static_cast<p::UuidKind>(kind)}}};
      auto raw=Identity(7); raw[6]=(version<<4)|3; raw[8]=(variant<<6)|7;
      const std::array<WireField,1> f{{{1,type,raw}}}; const auto b=Wire(f);
      const auto expected = !(kind<=7 || kind==9) ? E::invalid_schema :
          version==7 && variant==2 ? E::none : E::invalid_value;
      Expect(schema,b,expected);
    }
  const c::CatalogValueSchema users{123,1,{{1,T::user_uuid_data,true,16}}};
  for (unsigned octet=0;octet<16;++octet) for (unsigned value=0;value<256;++value) {
    auto raw=Identity(3); raw[octet]=value;
    const std::array<WireField,1> f{{{1,T::user_uuid_data,raw}}}; Expect(users,Wire(f),E::none);
  }
}
void BackingAndBounds() {
  const auto schema=Schema(); const auto b=Wire(Fields()); std::array<c::CatalogValueFieldView,9> backing;
  auto view=c::BorrowCatalogValueSchema(schema);
  for (std::size_t n=0;n<8;++n) Check(Decode(view,b,std::span(backing).first(n)).error==E::invalid_backing,"short backing admitted");
  backing.back().id=1234; Check(Decode(view,b,backing).ok() && backing.back().id==1234,"extra backing changed");
  auto bad=b; bad[0]=0; Check(Decode(view,bad,{}).error==E::invalid_framing,"backing obscured wire error");
  Check(Decode(view,b,{static_cast<c::CatalogValueFieldView*>(nullptr),8}).error==E::invalid_backing,"null backing");
  const auto max=std::numeric_limits<std::uintptr_t>::max();
  Check(Decode(view,b,{backing.data(),std::numeric_limits<std::size_t>::max()}).error==E::invalid_backing,"size overflow backing");
  Check(Decode(view,b,{reinterpret_cast<c::CatalogValueFieldView*>(max-7),8}).error==E::invalid_backing,"address overflow backing");
  Check(Decode(view,b,{reinterpret_cast<c::CatalogValueFieldView*>(reinterpret_cast<p::byte*>(backing.data())+1),8}).error==E::invalid_backing,"misaligned backing");
  // Valid source storage objects, intentionally overlapping destination only.
  std::array<c::CatalogValueFieldView,16> overlap;
  std::memcpy(overlap.data(),b.data(),b.size());
  Check(Decode(view,{reinterpret_cast<const p::byte*>(overlap.data()),b.size()},overlap).error==E::invalid_backing,"input alias");
  Check(std::memcmp(overlap.data(),b.data(),b.size())==0,"alias refusal changed input");
  Check(Decode(view,b,{reinterpret_cast<c::CatalogValueFieldView*>(const_cast<c::CatalogValueFieldSchema*>(view.fields.data())),8}).error==E::invalid_backing,"schema alias");
  Check(Decode(view,{static_cast<const p::byte*>(nullptr),24},backing).error==E::invalid_backing,"null input");
  Check(Decode(view,{reinterpret_cast<const p::byte*>(max-3),24},backing).error==E::invalid_backing,"overflow input");
  view.fields={static_cast<const c::CatalogValueFieldSchema*>(nullptr),8};
  Check(Decode(view,b,backing).error==E::invalid_backing,"null schema");
  const c::CatalogValueSchema empty{123,1,{}}; Expect(empty,Wire({}),E::none);
  // Largest legal block: 24 header + 8 field header + 131040 payload.
  for (const auto type : {T::opaque_bytes,T::utf8_text,T::engine_identity_list,T::utf8_text_list}) {
    const auto kind=type==T::engine_identity_list?p::UuidKind::object:p::UuidKind::unknown;
    const c::CatalogValueSchema s{123,1,{{1,type,true,131040,kind}}}; Bytes payload(131040,0);
    if(type==T::engine_identity_list) for(std::size_t n=0;n<payload.size();n+=16) {
      const auto id=Identity(static_cast<unsigned>(n)); std::copy(id.begin(),id.end(),payload.begin()+n);
    }
    if(type==T::utf8_text_list) Num(payload,0,4,(payload.size()-4)/4); // 32759 empty elements
    const std::array<WireField,1> f{{{1,type,payload}}}; const auto bytes=Wire(f); Expect(s,bytes,E::none);
    auto over=bytes; over.push_back(0); Num(over,8,4,over.size()); Expect(s,over,E::size_limit);
  }
}
Bytes RuntimeWire(bool credential, unsigned mode, p::u64 generation=1) {
  std::vector<WireField> f{{1,T::engine_identity,Identity(1)}, {2,T::unsigned_integer,Integer(generation)},
    {3,T::engine_identity,Identity(3)}, {4,T::engine_identity,Identity(4)},
    {5,T::engine_identity,Identity(mode==1?3:5)}, {6,T::engine_identity,Identity(6)},
    {7,T::engine_identity,Identity(7)}};
  if(credential) f.push_back({8,T::engine_identity,Identity(8)});
  for(unsigned n=9;n<=13;++n) f.push_back({static_cast<p::u16>(n),T::unsigned_integer,Integer(n==9?mode:41+n)});
  f.push_back({14,T::engine_identity,Identity(14)}); f.push_back({15,T::unsigned_integer,Integer(56)});
  return Wire(f,65587);
}
auto RuntimeDecode(const Bytes& bytes) {
  return NoHeap([&]{return c::DecodeCatalogRuntimeAuthorityBinding(
      {reinterpret_cast<const char*>(bytes.data()),bytes.size()});});
}
void RuntimeFirstUse() {
  // Run before any owning schema initialization or metadata/record codec call.
  for(bool credential:{false,true}) for(unsigned mode=1;mode<=3;++mode)
    for(p::u64 generation:{p::u64{1},std::numeric_limits<p::u64>::max()}) {
      const auto bytes=RuntimeWire(credential,mode,generation); const auto result=RuntimeDecode(bytes);
      Check(result.ok(),"runtime first-use decode"); const auto& r=*result.record;
      Check(r.binding_uuid==Id(1) && r.database_uuid==Id(3) && r.service_principal_uuid==Id(4) &&
        r.security_authority_uuid==Id(mode==1?3:5) && r.provider_uuid==Id(6) && r.policy_uuid==Id(7),"runtime native identities");
      Check(r.generation==generation && static_cast<p::u64>(r.authority_mode)==mode && r.security_epoch==51 &&
        r.policy_epoch==52 && r.provider_generation==53 && r.catalog_generation==54 && r.origin_local_transaction_id==56,
        "runtime native counters");
      Check(r.origin_transaction_uuid.kind==p::UuidKind::transaction && r.origin_transaction_uuid.value==Id(14),"runtime origin");
      Check(credential ? r.credential_reference_uuid==Id(8) : !r.credential_reference_uuid,"optional credential");
      const auto encoded=c::EncodeCatalogRuntimeAuthorityBinding(r); Check(encoded.ok() && encoded.bytes==bytes,"runtime independent wire");
      for(std::size_t n=0;n<bytes.size();++n) {
        const Bytes short_bytes(bytes.begin(),bytes.begin()+n); const auto bad=RuntimeDecode(short_bytes);
        Check(!bad.ok() && !bad.record,"runtime truncation published a record");
      }
      // Mutate every fixed-width field: zero counters, invalid identities, invalid mode.
      for(std::size_t at=24;at<bytes.size();) {
        const unsigned tag=bytes[at+2], size=bytes[at+4]; auto bad=bytes;
        std::fill_n(bad.begin()+at+8,size,0); const auto rejected=RuntimeDecode(bad);
        Check(rejected.error==E::invalid_value && !rejected.record,"runtime zero field admitted");
        if(tag==5) for(unsigned octet=0;octet<16;++octet) {
          bad=bytes; bad[at+8+octet]^=1;
          // UUID bytes remain full width; local authority must still match its database.
          const bool valid= !(mode==1 && (bytes[at]==3 || bytes[at]==5));
          const auto changed=RuntimeDecode(bad); Check(changed.ok()==valid,"runtime exact identity binding");
        }
        at+=8+size;
      }
    }
  for(unsigned mode:{0u,4u,255u}) Check(!RuntimeDecode(RuntimeWire(false,mode)).ok(),"invalid runtime mode");
  // Owning metadata itself is set up before allocation denial; all family checks
  // must then stay allocation-free and preserve subtype/header/origin checks.
  const auto bytes=RuntimeWire(true,1); c::CatalogMetadataVersion m;
  m.record.payload.assign(bytes.begin(),bytes.end()); m.record.header.kind=c::CatalogRecordKind::config_profile;
  m.record.header.object_uuid={p::UuidKind::object,Id(1)}; m.record.header.parent_uuid={p::UuidKind::object,Id(20)};
  m.owning_schema_uuid={p::UuidKind::schema,Id(20)}; m.owner_uuid={p::UuidKind::principal,Id(4)};
  m.audit_uuid={p::UuidKind::object,Id(21)}; m.security_policy_uuid={p::UuidKind::object,Id(7)};
  m.creator_transaction_uuid={p::UuidKind::transaction,Id(14)}; m.creator_local_transaction_id=56;
  m.object_subtype="agent_runtime_authority"; m.definition_version=1; m.catalog_generation=54; m.security_epoch=51;
  Check(NoHeap([&]{return c::CatalogRuntimeAuthorityBindingMatchesMetadata(m);}),"runtime metadata match");
  auto next=m; next.definition_version=2; auto next_bytes=RuntimeWire(true,1,2);
  next.record.payload.assign(next_bytes.begin(),next_bytes.end()); next.creator_transaction_uuid.value=Id(30);
  Check(NoHeap([&]{return c::CatalogRuntimeAuthorityBindingPreservesOrigin(m,next);}),"runtime origin continuity");
  auto invalid=m; invalid.object_subtype="different";
  Check(!NoHeap([&]{return c::CatalogRuntimeAuthorityBindingMatchesMetadata(invalid);}),"subtype erased");
  invalid=m; invalid.record.header.kind=c::CatalogRecordKind::schema;
  Check(!NoHeap([&]{return c::CatalogRuntimeAuthorityBindingMatchesMetadata(invalid);}),"header erased");
  invalid=m; invalid.owning_schema_uuid.value=Id(31);
  Check(!NoHeap([&]{return c::CatalogRuntimeAuthorityBindingMatchesMetadata(invalid);}),"schema binding erased");
  invalid=m; invalid.creator_local_transaction_id++;
  Check(!NoHeap([&]{return c::CatalogRuntimeAuthorityBindingMatchesMetadata(invalid);}),"genesis origin erased");
  next_bytes.back()=1; next.record.payload.assign(next_bytes.begin(),next_bytes.end());
  Check(!NoHeap([&]{return c::CatalogRuntimeAuthorityBindingPreservesOrigin(m,next);}),"origin change admitted");
}
}
int main() {
  try {
    RuntimeFirstUse(); ValuesAndErrors(); UuidMatrix(); BackingAndBounds();
    Check(attempted_allocations==0,"borrowed decode attempted allocation");
    std::cout<<"native_catalog_value_memory_gate: "<<checks<<" checks, zero borrowed allocations\n";
    return 0;
  } catch (...) { deny_heap=false; std::cerr<<"failed after "<<checks<<" checks, allocation attempts="<<attempted_allocations<<'\n'; return 1; }
}
