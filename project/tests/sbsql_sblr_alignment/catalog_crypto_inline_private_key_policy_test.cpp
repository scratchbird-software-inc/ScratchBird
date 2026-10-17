// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include "bootstrap_policy_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

thread_local bool deny_heap=false;
thread_local unsigned allocations=0;
void* operator new(std::size_t n){++allocations;if(deny_heap)throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;
using Body=c::CatalogCryptoInlinePrivateKeyPolicy;
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value){deny_heap=false;throw std::runtime_error(why);}}
struct HeapGuard {
  unsigned before=allocations;
  HeapGuard(){deny_heap=true;}
  ~HeapGuard(){deny_heap=false;}
  bool untouched()const{return allocations==before;}
};
Body Value(){Body v;v.body_uuid=f::Id(4);v.generation=1;v.database_uuid=f::Id(1);
  v.origin_transaction_uuid={p::UuidKind::transaction,f::Id(5)};v.origin_local_transaction_id=1;return v;}
std::string Golden(const Body& v){
  // Independent normative SBCV construction; none of the product family/schema
  // size/encode/decode functions produce this oracle.
  f::Golden g(65620);g.Identity(1,v.body_uuid);g.Integer(2,v.generation);g.Identity(3,v.database_uuid);
  g.Integer(4,v.purpose);g.Boolean(5,v.inline_private_key_use_allowed);g.Identity(6,v.origin_transaction_uuid.value);
  g.Integer(7,v.origin_local_transaction_id);return g.Finish();
}
std::string Pinned(){
  // Independently derived from the seven normative field headers/scalars with
  // Python struct.pack, not from an implementation result. Baseline deny.
  constexpr std::string_view h="534243560100180099000000070000005400010001000000010005001000000001000000000070008000000000000004020001000800000001000000000000000300050010000000010000000000700080000000000000010400010008000000010000000000000005000200010000000006000500100000000100000000007000800000000000000507000100080000000100000000000000";
  std::string out(h.size()/2,0);auto nib=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<char>(16*nib(h[2*i])+nib(h[2*i+1]));return out;
}
c::CatalogMetadataVersion Metadata(const Body& v){
  auto m=f::Metadata(0,v.origin_transaction_uuid,f::Id(60),Golden(v));
  m.record.header.object_uuid={p::UuidKind::object,v.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(100)};
  m.object_subtype="crypto_inline_private_key";m.definition_version=v.generation;
  m.creator_local_transaction_id=v.origin_local_transaction_id;return m;
}
bool Filled(const std::array<p::byte,256>& b,p::byte value=0x5a){return std::all_of(b.begin(),b.end(),[=](auto v){return v==value;});}
void FirstUse(){
  const auto v=Value();const auto wire=Pinned();const auto m=Metadata(v);
  Check(Golden(v)==wire && wire.size()==153,"independent fixed full deny oracle");
  std::array<p::byte,256> output;output.fill(0x5a);
  HeapGuard guard;
  const auto size=c::CatalogCryptoInlinePrivateKeyPolicyEncodedBytes(v);
  const auto encoded=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(v,output);
  const auto decoded=c::DecodeCatalogCryptoInlinePrivateKeyPolicy(wire);
  const auto schema=c::CatalogCryptoInlinePrivateKeyPolicySchemaView();
  const auto common=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m));
  const auto header=c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(m.record));
  const auto matches=c::CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(m,v.database_uuid);
  const auto continuity=c::CatalogMetadataPreservesFamilyOrigin(m,m);
  Check(guard.untouched() && size && *size==153 && schema.id==65620 && schema.version==1 && schema.fields.size()==7 &&
      encoded.ok() && encoded.bytes.size()==153 && decoded.ok() && common && header.ok() && matches && continuity,
      "first-use size/Into/decode/shared metadata/header/origin registration all heap-free");
  Check(std::memcmp(encoded.bytes.data(),wire.data(),153)==0 && !decoded.record->inline_private_key_use_allowed,
      "exact first-use native bytes retain explicit false permission");
  Check(std::all_of(output.begin()+153,output.end(),[](auto b){return b==0x5a;}),"successful encode leaves extra backing unchanged");
}
void Roundtrip(const Body& v){
  const auto wire=Golden(v);std::array<p::byte,256> out;out.fill(0x5a);
  HeapGuard guard;const auto size=c::CatalogCryptoInlinePrivateKeyPolicyEncodedBytes(v);
  const auto encoded=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(v,out);const auto decoded=c::DecodeCatalogCryptoInlinePrivateKeyPolicy(wire);
  Check(guard.untouched() && size && *size==153 && encoded.ok() && decoded.ok() && encoded.bytes.size()==wire.size() &&
      std::memcmp(encoded.bytes.data(),wire.data(),wire.size())==0 && decoded.record->body_uuid==v.body_uuid &&
      decoded.record->database_uuid==v.database_uuid && decoded.record->generation==v.generation && decoded.record->purpose==v.purpose &&
      decoded.record->origin_transaction_uuid.kind==p::UuidKind::transaction && decoded.record->origin_transaction_uuid.value==v.origin_transaction_uuid.value &&
      decoded.record->origin_local_transaction_id==v.origin_local_transaction_id &&
      decoded.record->inline_private_key_use_allowed==v.inline_private_key_use_allowed,"independent exact native definition roundtrip, not runtime authority");
}
void Invalid(const Body& v,bool wire=true){
  const auto bytes=wire?Golden(v):std::string{};std::array<p::byte,256> out;out.fill(0x5a);
  HeapGuard guard;const auto size=c::CatalogCryptoInlinePrivateKeyPolicyEncodedBytes(v);
  const auto encoded=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(v,out);
  const auto decoded=wire?c::DecodeCatalogCryptoInlinePrivateKeyPolicy(bytes):c::CatalogCryptoInlinePrivateKeyPolicyResult{};
  Check(guard.untouched() && !size && !encoded.ok() && encoded.bytes.empty() && !decoded.ok() && !decoded.record && Filled(out),
      "invalid native definition refuses atomically without output or heap");
}
void InvalidWire(std::string_view bytes){HeapGuard guard;const auto r=c::DecodeCatalogCryptoInlinePrivateKeyPolicy(bytes);
  Check(guard.untouched() && !r.ok() && !r.record,"invalid/trailing/missing/duplicate/wrong-kind-or-width wire refuses without partial value or heap");}
void ValuesAndFrames(){
  Check(!Body{}.inline_private_key_use_allowed,"creation field default is false, not permission by absence");
  auto v=Value();Roundtrip(v);v.inline_private_key_use_allowed=true;Roundtrip(v);
  auto max=v;max.generation=max.origin_local_transaction_id=UINT64_MAX;Roundtrip(max);
  for(unsigned i=0;i<11;++i){auto bad=v;
    if(i==0)bad.body_uuid={};if(i==1)bad.database_uuid={};if(i==2)bad.origin_transaction_uuid.value={};
    if(i==3)bad.generation=0;if(i==4)bad.origin_local_transaction_id=0;if(i==5)bad.purpose=0;if(i==6)bad.purpose=UINT64_MAX;
    if(i==7)bad.body_uuid=bad.database_uuid;if(i==8)bad.body_uuid=bad.origin_transaction_uuid.value;
    if(i==9)bad.database_uuid=bad.origin_transaction_uuid.value;if(i==10)bad.origin_transaction_uuid.kind=p::UuidKind::object;
    Invalid(bad,i!=10);
  }
  for(unsigned which=0;which<3;++which)for(unsigned version=0;version<16;++version)if(version!=7){
    auto bad=v;auto& id=which==0?bad.body_uuid:which==1?bad.database_uuid:bad.origin_transaction_uuid.value;
    id.bytes[6]=version<<4;Invalid(bad);
  }
  for(unsigned which=0;which<3;++which)for(unsigned variant:{0u,0x40u,0xc0u}){
    auto bad=v;auto& id=which==0?bad.body_uuid:which==1?bad.database_uuid:bad.origin_transaction_uuid.value;
    id.bytes[8]=variant;Invalid(bad);
  }
  const auto wire=Pinned();std::array<p::byte,256> out;
  for(std::size_t n=0;n<153;++n){out.fill(0x5a);HeapGuard guard;
    const auto e=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(Value(),std::span(out).first(n));
    const auto d=c::DecodeCatalogCryptoInlinePrivateKeyPolicy(std::string_view(wire).substr(0,n));
    Check(guard.untouched() && !e.ok() && e.bytes.empty() && !d.ok() && !d.record && Filled(out),"every short input/output refuses before writes");}
  auto trailing=wire;trailing.push_back(0);InvalidWire(trailing);
  for(unsigned byte:{0u,4u,6u,8u,12u,16u,20u,22u}){auto bad=wire;bad[byte]^=1;InvalidWire(bad);}
  constexpr std::array<std::size_t,7> starts{24,48,64,88,104,113,137};
  for(unsigned field=0;field<7;++field){const auto at=starts[field];const auto length=p::LoadLittle32(wire.data()+at+4);
    for(unsigned change=0;change<7;++change){auto bad=wire;
      if(change==0)f::Put(bad,at,0xffff,2);if(change==1)f::Put(bad,at,field?field:2,2);
      if(change==2)bad[at+2]=static_cast<char>(99);if(change==3)bad[at+3]=1;
      if(change==4)f::Put(bad,at+4,length-1,4);if(change==5)f::Put(bad,at+4,length+1,4);
      if(change==6){bad.erase(at,8+length);f::Put(bad,8,bad.size(),4);f::Put(bad,12,6,4);}InvalidWire(bad);
    }
  }
  for(unsigned b=2;b<=255;++b){auto bad=wire;bad[112]=static_cast<char>(b);InvalidWire(bad);}
  // Extra repeated field with otherwise reconciled header remains malformed.
  auto duplicate=wire;duplicate.append(wire.substr(137));f::Put(duplicate,8,duplicate.size(),4);f::Put(duplicate,12,8,4);InvalidWire(duplicate);
  {HeapGuard guard;const auto d=c::DecodeCatalogCryptoInlinePrivateKeyPolicy({nullptr,153});
    Check(guard.untouched() && !d.ok() && !d.record,"null claimed input rejected before reading");}
  {HeapGuard guard;Check(!c::IsCatalogCryptoInlinePrivateKeyPolicyPayload({nullptr,153}),"null classifier input rejected before reading");}
  auto object=Value();std::array<unsigned char,sizeof(object)> before{},after{};std::memcpy(before.data(),&object,sizeof(object));
  for(std::size_t offset=0;offset<sizeof(object);++offset){HeapGuard guard;
    const auto e=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(object,{reinterpret_cast<p::byte*>(&object)+offset,1});
    std::memcpy(after.data(),&object,sizeof(object));Check(guard.untouched() && !e.ok() && e.bytes.empty() && before==after,"whole source object overlap refuses unchanged");}
  // Unlike a one-byte destination this has sufficient output capacity; refusal
  // must be due to WHOLE source-object overlap, including its native padding.
  alignas(Body) std::array<p::byte,768> region;region.fill(0x5a);
  auto* embedded=new(region.data()+256) Body(Value());const auto retained=region;
  for(std::size_t at=104;at<256+sizeof(Body);++at){HeapGuard guard;
    const auto e=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(*embedded,std::span(region).subspan(at,153));
    Check(guard.untouched() && !e.ok() && e.bytes.empty() && region==retained,"sufficient-capacity partial/complete/padding source overlap refuses atomically");}
  embedded->~Body();
  {HeapGuard guard;const auto e=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(object,{static_cast<p::byte*>(nullptr),153});
    Check(guard.untouched() && !e.ok() && e.bytes.empty(),"null destination rejected before writes");}
  {HeapGuard guard;const auto e=c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(object,{reinterpret_cast<p::byte*>(UINTPTR_MAX-2),153});
    Check(guard.untouched() && !e.ok() && e.bytes.empty(),"wrapping output extent rejected before writes");}
  {HeapGuard guard;Check(!c::IsCatalogCryptoInlinePrivateKeyPolicyPayload({reinterpret_cast<const char*>(UINTPTR_MAX-2),153}),"wrapping classifier input rejected before reading");}
}
void MetadataAndContinuity(){
  const auto v=Value();const auto m=Metadata(v);const auto view=c::BorrowCatalogMetadataVersion(m);
  for(unsigned n=0;n<20;++n){auto bad=m;
    if(n==0)bad.record.header.kind=c::CatalogRecordKind::config_profile;if(n==1)bad.object_subtype="other";
    if(n==2)bad.record.header.object_uuid.value=f::Id(8);if(n==3)bad.record.header.object_uuid.kind=p::UuidKind::principal;
    if(n==4)bad.record.header.row_uuid.value=v.body_uuid;if(n==5)bad.record.header.row_uuid.kind=p::UuidKind::object;
    if(n==6)bad.definition_version=2;if(n==7)bad.owning_schema_uuid={};if(n==8)bad.record.header.parent_uuid.value=f::Id(61);
    if(n==9)bad.owner_uuid={};if(n==10)bad.owner_uuid.kind=p::UuidKind::transaction;if(n==11)bad.audit_uuid={};
    if(n==12)bad.creator_transaction_uuid.kind=p::UuidKind::object;if(n==13)bad.creator_transaction_uuid.value=f::Id(6);
    if(n==14)bad.creator_local_transaction_id=2;if(n==15)bad.creator_local_transaction_id=0;
    if(n==16)f::Put(bad.record.payload,16,65619,4);if(n==17)bad.record.payload="inline_private_key_use_allowed=true";
    if(n==18)bad.record.payload[112]=2;if(n==19)bad.record.header.parent_uuid.kind=p::UuidKind::schema;
    const auto bad_view=c::BorrowCatalogMetadataVersion(bad);HeapGuard guard;
    const auto size=c::CatalogMetadataVersionEncodedBytes(bad_view);
    Check(guard.untouched() && !size && !c::CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(bad_view),
      "shared common metadata cannot bypass corrupted subtype/schema/kind/binding/origin/boolean");
  }
  {HeapGuard guard;Check(c::CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(view,v.database_uuid) &&
      !c::CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(view,f::Id(2)) &&
      !c::CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(view,{}),"actual owning database cannot be selected by payload label");}
  // An increased definition generation may hold an explicitly authored true
  // field. This is still no activation, permission or transaction-success proof.
  auto next=v;next.generation=2;next.inline_private_key_use_allowed=true;
  auto successor=Metadata(next);successor.creator_transaction_uuid.value=f::Id(6);successor.creator_local_transaction_id=2;
  {HeapGuard guard;Check(c::CatalogMetadataPreservesFamilyOrigin(m,successor) &&
      c::CatalogCryptoInlinePrivateKeyPolicyPreservesOrigin(m,successor),"higher semantic generation may change permission while retaining exact immutable origin");}
  for(unsigned n=0;n<7;++n){auto value=next;auto bad=successor;
    if(n==0)value.body_uuid=f::Id(9);if(n==1)value.database_uuid=f::Id(9);if(n==2)value.origin_transaction_uuid.value=f::Id(9);
    if(n==3)value.origin_local_transaction_id=2;if(n==4)value.purpose=2;
    if(n==5){value=v;value.inline_private_key_use_allowed=true;}if(n==6)value.generation=0;
    bad.record.payload=Golden(value);bad.record.header.object_uuid.value=value.body_uuid;bad.definition_version=value.generation;
    HeapGuard guard;Check(guard.untouched() && !c::CatalogMetadataPreservesFamilyOrigin(m,bad),"common continuity rejects changed identity/database/purpose/origin/same-generation permission");
  }
  auto rollback=successor;auto older=v;rollback.record.payload=Golden(older);rollback.definition_version=1;
  rollback.creator_transaction_uuid=v.origin_transaction_uuid;rollback.creator_local_transaction_id=1;
  {HeapGuard guard;Check(!c::CatalogMetadataPreservesFamilyOrigin(successor,rollback),"semantic generation cannot decrease");}
  auto prior_origin=next;prior_origin.origin_local_transaction_id=3;auto bad=Metadata(prior_origin);bad.creator_local_transaction_id=2;
  {HeapGuard guard;Check(!c::CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(bad),"current creator cannot precede immutable origin");}
  auto retired=successor;retired.record.header.deleted=true;retired.lifecycle=c::CatalogObjectLifecycle::dropped;
  retired.status=c::CatalogObjectStatus::retired;retired.retired_transaction_uuid=retired.creator_transaction_uuid;
  {HeapGuard guard;Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(retired)).has_value() &&
      c::CatalogMetadataPreservesFamilyOrigin(m,retired),"logical retirement retains a valid definition/origin; eligibility belongs to owning lookup");}
  auto erased=successor;erased.object_subtype="unrelated";erased.record.payload="unrelated";
  {HeapGuard guard;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,erased),"family cannot vanish during successor mutation");}
  auto changed=v;changed.inline_private_key_use_allowed=true;const auto same=Metadata(changed);
  {HeapGuard guard;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,same),"same semantic generation cannot change permission bytes");}
  for(bool allowed:{false,true}){auto b=Value();b.inline_private_key_use_allowed=allowed;auto definition=Metadata(b);HeapGuard guard;
    Check(c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(definition.record)).ok() &&
      c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(definition)).has_value(),"both boolean definitions structurally encodable, neither is a runtime grant");}
}
int main()try{
  FirstUse();ValuesAndFrames();MetadataAndContinuity();
  std::cout<<"catalog_crypto_inline_private_key_policy "<<checks<<" PASS\n";return 0;
}catch(const std::exception& e){deny_heap=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
