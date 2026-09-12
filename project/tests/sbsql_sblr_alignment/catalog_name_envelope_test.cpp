// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_envelope.hpp"
#include "catalog_page.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
namespace c=scratchbird::core::catalog;
namespace page=scratchbird::storage::page;
using namespace scratchbird::core::platform;
unsigned checks=0,failures=0;
void Check(bool ok,const char* message) {
  ++checks;if(!ok && ++failures<16)std::cerr<<"FAIL "<<message<<'\n';
}
template<class Result>void Refused(const Result& r) {
  Check(!r.ok() && r.error!=c::CatalogNameEnvelopeError::none,"invalid envelope accepted");
  if constexpr(requires{r.record;})Check(!r.record,"partial record returned");
  else Check(r.bytes.empty(),"partial bytes returned");
}
std::vector<byte> Hex(const std::string& text) {
  std::vector<byte> out;int high=-1;
  for(char ch:text) {
    if(ch==' ')continue;
    int nibble=ch<='9'?ch-'0':ch-'a'+10;
    if(high<0)high=nibble;else{out.push_back(high*16+nibble);high=-1;}
  }return out;
}
TypedUuid Id(UuidKind kind,byte seed) {
  return {kind,Uuid{{0x01,0x92,0x13,0x24,0x35,0x46,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,seed}}};
}
c::CatalogNameVector Vector() {
  c::CatalogNameVector v;
  v.name_vector_uuid=Id(UuidKind::object,1);v.object_uuid=Id(UuidKind::object,2);
  v.object_class="table";v.owning_schema_uuid=Id(UuidKind::schema,4);
  v.default_language_tag="fr-CA";v.default_name_entry_uuid=Id(UuidKind::object,6);
  v.name_collision_policy_uuid=Id(UuidKind::object,7);v.catalog_generation_id=0x0102030405060708;
  v.security_policy_uuid=Id(UuidKind::object,9);v.lifecycle_state=c::CatalogNameLifecycle::active;
  return v;
}
c::CatalogNameEnvelope Fixture(bool entry=false) {
  c::CatalogNameEnvelope r;
  auto& b=r.binding;
  b.database_uuid=Id(UuidKind::database,101);b.filespace_uuid=Id(UuidKind::filespace,102);
  b.row_uuid=Id(UuidKind::row,103);b.version_uuid=Id(UuidKind::row,104);
  b.catalog_object_uuid=Id(UuidKind::object,1);b.creating_transaction_uuid=Id(UuidKind::transaction,106);
  b.page_id=9;b.slot_id=3;b.storage_generation=4;b.version_sequence=5;b.creating_transaction_number=6;
  b.catalog_generation=0x0102030405060708;
  r.payload=Vector();
  if(entry) {
    c::CatalogNameEntry e;
    e.name_entry_uuid=b.catalog_object_uuid;e.name_vector_uuid=Id(UuidKind::object,2);
    e.object_uuid=Id(UuidKind::object,3);e.object_class="table";e.scope_uuid=Id(UuidKind::object,5);
    e.language_tag="en";e.dialect_profile_uuid=Id(UuidKind::object,11);
    e.identifier_profile_uuid=Id(UuidKind::object,12);e.raw_name_text="a";e.display_name="a";
    e.catalog_generation_id=b.catalog_generation;e.created_transaction_uuid=b.creating_transaction_uuid;
    e.security_policy_uuid=Id(UuidKind::object,30);e.resource_epoch=1;e.name_resolution_epoch=2;
    r.payload=e;
  }return r;
}
std::vector<byte> Golden() {
  // Independent Core byte oracle: header and payload both literal, not generated
  // with a production serializer. The owning version metadata is a test fixture.
  auto out=Hex(
    "53424352 0100 b000 92010000 e2000000 01000500 0100 0500 "
    "019213243546778899aabbccddeeff65 019213243546778899aabbccddeeff66 "
    "019213243546778899aabbccddeeff67 019213243546778899aabbccddeeff68 "
    "019213243546778899aabbccddeeff01 019213243546778899aabbccddeeff6a "
    "0900000000000000 03000000 00000000 0400000000000000 0500000000000000 "
    "0600000000000000 0807060504030201 0000000000000000 "
    "53424356 0100 1800 e2000000 0a000000 01000500 01000000 "
    "0100 05 00 10000000 019213243546778899aabbccddeeff01 "
    "0200 05 00 10000000 019213243546778899aabbccddeeff02 "
    "0300 03 00 05000000 7461626c65 "
    "0400 05 00 10000000 019213243546778899aabbccddeeff04 "
    "0500 03 00 05000000 66722d4341 "
    "0600 05 00 10000000 019213243546778899aabbccddeeff06 "
    "0700 05 00 10000000 019213243546778899aabbccddeeff07 "
    "0800 01 00 08000000 0807060504030201 "
    "0900 05 00 10000000 019213243546778899aabbccddeeff09 "
    "0a00 01 00 08000000 0200000000000000");
  Check(out.size()==402,"golden envelope size");return out;
}
void Roundtrip(const c::CatalogNameEnvelope& r) {
  const auto encoded=c::EncodeCatalogNameEnvelope(r);
  Check(encoded.ok(),"valid envelope refused");if(!encoded.ok())return;
  const auto decoded=c::DecodeCatalogNameEnvelope(encoded.bytes,r.binding);
  Check(decoded.ok(),"valid envelope decode refused");if(!decoded.ok())return;
  const auto again=c::EncodeCatalogNameEnvelope(*decoded.record);
  Check(again.ok() && again.bytes==encoded.bytes,"envelope changed on roundtrip");
}
void HeaderAndIdentity() {
  const auto base=Fixture();const auto golden=Golden();
  const auto encoded=c::EncodeCatalogNameEnvelope(base);
  Check(encoded.ok() && encoded.bytes==golden,"complete independent golden mismatch");
  auto decoded=c::DecodeCatalogNameEnvelope(golden,base.binding);
  Check(decoded.ok(),"golden decode refused");
  if(decoded.ok()) {
    const auto& v=std::get<c::CatalogNameVector>(decoded.record->payload);
    Check(v.default_language_tag=="fr-CA" && v.name_vector_uuid.value==Id(UuidKind::object,1).value &&
          decoded.record->binding.version_uuid.value==Id(UuidKind::row,104).value,"golden values changed");
  }
  for(unsigned offset=0;offset<176;++offset)for(unsigned value=0;value<256;++value) {
    if(golden[offset]==value)continue;
    auto bad=golden;bad[offset]=value;Refused(c::DecodeCatalogNameEnvelope(bad,base.binding));
  }
  for(std::size_t n=0;n<golden.size();++n)
    Refused(c::DecodeCatalogNameEnvelope({golden.begin(),golden.begin()+n},base.binding));
  auto trailing=golden;trailing.push_back(0);Refused(c::DecodeCatalogNameEnvelope(trailing,base.binding));
  using B=c::CatalogNameVersionBinding;
  const std::array<TypedUuid B::*,6> members={&B::database_uuid,&B::filespace_uuid,&B::row_uuid,
      &B::version_uuid,&B::catalog_object_uuid,&B::creating_transaction_uuid};
  for(unsigned slot=0;slot<members.size();++slot) {
    const auto member=members[slot];const auto expected=(base.binding.*member).kind;
    for(unsigned kind=0;kind<256;++kind) {
      auto r=base;(r.binding.*member).kind=static_cast<UuidKind>(kind);
      if(kind==static_cast<unsigned>(expected))Roundtrip(r);
      else{Refused(c::EncodeCatalogNameEnvelope(r));Refused(c::DecodeCatalogNameEnvelope(golden,r.binding));}
    }
    for(unsigned version=0;version<16;++version)for(unsigned variant=0;variant<4;++variant) {
      auto r=base;auto& value=(r.binding.*member).value;
      value.bytes[6]=(version<<4)|7;value.bytes[8]=(variant<<6)|25;
      if(version==7 && variant==2)Roundtrip(r);
      else {
        Refused(c::EncodeCatalogNameEnvelope(r));
        auto bad=golden;bad[24+16*slot+6]=value.bytes[6];bad[24+16*slot+8]=value.bytes[8];
        Refused(c::DecodeCatalogNameEnvelope(bad,base.binding));
      }
    }
    auto r=base;(r.binding.*member).value={};Refused(c::EncodeCatalogNameEnvelope(r));
    auto expected_other=base.binding;(expected_other.*member).value.bytes[15]++;
    const auto mismatch=c::DecodeCatalogNameEnvelope(golden,expected_other);
    Refused(mismatch);Check(mismatch.error==c::CatalogNameEnvelopeError::binding_mismatch,"valid foreign binding category");
  }
}
void Set32(std::vector<byte>& bytes,std::size_t at,u32 value) {
  for(unsigned n=0;n<4;++n){bytes[at+n]=value%256;value/=256;}
}
std::vector<byte> Repack(const c::CatalogNameEnvelope& original,const std::vector<byte>& payload) {
  auto bytes=c::EncodeCatalogNameEnvelope(original).bytes;bytes.resize(176);
  bytes.insert(bytes.end(),payload.begin(),payload.end());
  Set32(bytes,8,bytes.size());Set32(bytes,12,payload.size());return bytes;
}
void BindingAndPayload() {
  const auto base=Fixture();const auto golden=Golden();
  using B=c::CatalogNameVersionBinding;
  for(auto member:{&B::page_id,&B::storage_generation,&B::version_sequence,
                   &B::creating_transaction_number,&B::catalog_generation}) {
    auto wrong=base.binding;wrong.*member+=1;
    auto result=c::DecodeCatalogNameEnvelope(golden,wrong);Refused(result);
    Check(result.error==c::CatalogNameEnvelopeError::binding_mismatch,"scalar binding mismatch category");
    auto invalid=base;invalid.binding.*member=0;Refused(c::EncodeCatalogNameEnvelope(invalid));
    Refused(c::DecodeCatalogNameEnvelope(golden,invalid.binding));
  }
  auto wrong_slot=base.binding;wrong_slot.slot_id++;
  Refused(c::DecodeCatalogNameEnvelope(golden,wrong_slot));
  auto r=base;r.binding.slot_id=0;Roundtrip(r);
  r.binding.slot_id=std::numeric_limits<u32>::max();Roundtrip(r);
  r=base;r.binding.page_id=std::numeric_limits<u64>::max();Roundtrip(r);
  for(bool entry:{false,true}) {
    r=Fixture(entry);Roundtrip(r);
    auto changed=r;
    std::visit([](auto& p){p.catalog_generation_id++;},changed.payload);
    Refused(c::EncodeCatalogNameEnvelope(changed));
    auto payload=entry?c::EncodeCatalogNameEntry(std::get<c::CatalogNameEntry>(changed.payload)):
                       c::EncodeCatalogNameVector(std::get<c::CatalogNameVector>(changed.payload));
    Refused(c::DecodeCatalogNameEnvelope(Repack(r,payload.bytes),r.binding));
    changed=r;
    if(entry)std::get<c::CatalogNameEntry>(changed.payload).name_entry_uuid.value.bytes[15]++;
    else std::get<c::CatalogNameVector>(changed.payload).name_vector_uuid.value.bytes[15]++;
    Refused(c::EncodeCatalogNameEnvelope(changed));
    payload=entry?c::EncodeCatalogNameEntry(std::get<c::CatalogNameEntry>(changed.payload)):
                  c::EncodeCatalogNameVector(std::get<c::CatalogNameVector>(changed.payload));
    Refused(c::DecodeCatalogNameEnvelope(Repack(r,payload.bytes),r.binding));
    changed=r;std::visit([](auto& p){p.object_class.clear();},changed.payload);
    Refused(c::EncodeCatalogNameEnvelope(changed));
  }
  r=Fixture(true);auto changed=r;
  std::get<c::CatalogNameEntry>(changed.payload).created_transaction_uuid.value.bytes[15]++;
  Refused(c::EncodeCatalogNameEnvelope(changed));
  auto payload=c::EncodeCatalogNameEntry(std::get<c::CatalogNameEntry>(changed.payload));
  Refused(c::DecodeCatalogNameEnvelope(Repack(r,payload.bytes),r.binding));
  auto bad=golden;bad[16]=2;Refused(c::DecodeCatalogNameEnvelope(bad,base.binding));
  bad=golden;bad[176+16]=2;Refused(c::DecodeCatalogNameEnvelope(bad,base.binding));
  const std::string legacy="kind=5\nrow_uuid=01921324-3546-7788-99aa-bbccddeeff67\n";
  Refused(c::DecodeCatalogNameEnvelope({legacy.begin(),legacy.end()},base.binding));
  Check(!c::CatalogNameEnvelopeEncodeResult{}.ok() && !c::CatalogNameEnvelopeDecodeResult{}.ok(),
        "default constructed result asserts success");
}
void SizeAndPageContainer() {
  auto r=Fixture();auto& v=std::get<c::CatalogNameVector>(r.payload);
  v.object_class=std::string(130675,'x');
  auto encoded=c::EncodeCatalogNameEnvelope(r);
  Check(encoded.ok() && encoded.bytes.size()==131072,"exact envelope size maximum");
  Roundtrip(r);v.object_class.push_back('x');
  Refused(c::EncodeCatalogNameEnvelope(r));
  encoded.bytes.push_back(0);Refused(c::DecodeCatalogNameEnvelope(encoded.bytes,r.binding));
  r=Fixture();encoded=c::EncodeCatalogNameEnvelope(r);
  page::CatalogPageRow row;
  row.kind=page::CatalogPageRowKind::typed_catalog_record;row.ordinal=3;
  row.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
  auto pages=page::BuildCatalogPageSet({row},8192,9,10);
  Check(pages.ok() && pages.pages.size()==1,"real catalog page packing failed");
  if(!pages.ok() || pages.pages.empty())return;
  auto parsed=page::ParseCatalogPageBody(pages.pages[0].body,9);
  Check(parsed.ok() && parsed.body.rows.size()==1 && parsed.body.rows[0].payload==row.payload,
        "page container altered binary envelope");
  if(parsed.ok() && parsed.body.rows.size()==1) {
    const auto& bytes=parsed.body.rows[0].payload;
    Check(c::DecodeCatalogNameEnvelope({bytes.begin(),bytes.end()},r.binding).ok(),
          "page recovered envelope did not bind");
  }
  // Envelope structural validation is NOT a checksum or visibility oracle.
  auto changed=encoded.bytes;const std::vector<byte> needle{'t','a','b','l','e'};
  auto at=std::search(changed.begin()+176,changed.end(),needle.begin(),needle.end());
  Check(at!=changed.end(),"fixture class not located");if(at==changed.end())return;
  *at='c';Check(c::DecodeCatalogNameEnvelope(changed,r.binding).ok(),"codec invents metadata authority");
  auto damaged=pages.pages[0].body;
  auto record_at=std::search(damaged.begin(),damaged.end(),encoded.bytes.begin(),encoded.bytes.end());
  Check(record_at!=damaged.end(),"packed record bytes not found");if(record_at==damaged.end())return;
  record_at[at-changed.begin()]='c';
  Check(!page::ParseCatalogPageBody(damaged,9).ok(),"owning page checksum accepted altered payload");
  // This is page-container integration, not a live MGA version or disk commit.
}
int main() {
  HeaderAndIdentity();BindingAndPayload();SizeAndPageContainer();
  std::cout<<"catalog name envelope: checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
