// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/bulk_import_column_digest.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
using namespace scratchbird::engine::internal_api;
using Hash=std::array<std::uint8_t,32>;
static long allocation=-1;
static bool injected=false;
void* operator new(std::size_t n) {
  if(allocation==0) { allocation=-1; injected=true; throw std::bad_alloc(); }
  if(allocation>0) --allocation;
  if(void* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
static unsigned checks=0;
static void Check(bool ok,const char* why) { ++checks; if(!ok) throw std::runtime_error(why); }
static EngineUuid Id(unsigned n) {
  EngineUuid u{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0}};
  u.bytes[14]=n>>8;u.bytes[15]=n;return u;
}
static MgaRelationStorageDescriptor Fixture() {
  MgaRelationStorageDescriptor d; d.relation_uuid=Id(1);d.relation_generation=2;
  d.descriptor_uuid=Id(3);d.descriptor_generation=4;
  MgaRelationColumnStorageDescriptor a;
  a.column_uuid=Id(5);a.column_generation=6;a.ordinal=7;a.canonical_name_key="k";
  a.value_descriptor.descriptor_uuid=Id(8);a.value_descriptor.descriptor_kind="scalar";
  a.value_descriptor.canonical_type_name="int32";a.value_descriptor.encoded_descriptor="type=int32";
  auto b=a;b.column_uuid=Id(9);b.column_generation=10;b.ordinal=11;b.canonical_name_key="z";
  b.nullable=false;b.generated=true;b.identity_column=true;
  b.charset_uuid=Id(12);b.collation_uuid=Id(13);b.character_length=14;
  b.max_inline_bytes=15;b.overflow_policy="locator";
  d.columns={b,a};return d; // Native vector order is not digest order.
}
static Hash Sentinel() { Hash h{};h.fill(0x55);return h; }
static void Refuse(const MgaRelationStorageDescriptor& d) {
  auto hash=Sentinel();
  Check(!ComputeBulkImportColumnDigestV2(d,&hash),"invalid identity/structure accepted");
  Check(hash==Sentinel(),"refusal changed caller hash");
}
int main() {
 try {
  const auto d=Fixture(); Hash expected{};
  Check(ComputeBulkImportColumnDigestV2(d,&expected),"valid descriptor");
  // Independent Python hashlib/struct oracle from the exact Core field list:
  // 454 bytes, including fixed-width nil charset and collation fields.
  Check(scratchbird::core::hash::HexLower(expected)==
    "89f0cf42c925e82983c8ede5ec3c552244b1b0724649caaf8f97d0cb65474be0","independent Core hash");
  auto reordered=d;std::reverse(reordered.columns.begin(),reordered.columns.end());
  Hash digest{}; Check(ComputeBulkImportColumnDigestV2(reordered,&digest) && digest==expected,"ordinal normalization");
  Check(!ComputeBulkImportColumnDigestV2(d,nullptr),"null output");
  Check(kBulkImportTextConverterUuidV1==Id(0xb775),"registered converter identity");
  for(unsigned role=0;role<6;++role) {
    auto invalid=d;
    auto identity=[&](MgaRelationStorageDescriptor& v)->EngineUuid& {
      if(role==0) return v.relation_uuid;
      if(role==1) return v.descriptor_uuid;
      if(role==2) return v.columns[0].column_uuid;
      if(role==3) return v.columns[0].value_descriptor.descriptor_uuid;
      if(role==4) return v.columns[0].charset_uuid;
      return v.columns[0].collation_uuid;
    };
    for(unsigned byte=0;byte<16;++byte) for(unsigned value=0;value<256;++value) {
      auto changed=d; auto& id=identity(changed); id.bytes[byte]=value;
      Hash h=Sentinel();
      const bool ok=ComputeBulkImportColumnDigestV2(changed,&h);
      const bool admitted=(id.bytes[6]>>4)==7 && (id.bytes[8]&0xc0)==0x80 &&
          !(role==2 && id==changed.columns[1].column_uuid);
      Check(ok==admitted,"all-byte identity admission");
      if(ok) Check((h==expected)==(id==identity(invalid)),"all-byte identity hash sensitivity");
      else Check(h==Sentinel(),"all-byte failure atomicity");
    }
    identity(invalid)={};
    if(role<4) Refuse(invalid);
    else Check(ComputeBulkImportColumnDigestV2(invalid,&digest) && digest!=expected,"optional binary nil");
  }
  for(unsigned item=0;item<7;++item) {
    auto bad=d;
    if(item==0) bad.relation_generation=0;
    if(item==1) bad.descriptor_generation=0;
    if(item==2) bad.columns[0].column_generation=0;
    if(item==3) bad.columns.clear();
    if(item==4) bad.columns[1].ordinal=bad.columns[0].ordinal;
    if(item==5) bad.columns[1].column_uuid=bad.columns[0].column_uuid;
    if(item==6) bad.columns[0].canonical_name_key=std::string(65536,'x');
    Refuse(bad);
  }
  for(unsigned field=0;field<6;++field) {
    const auto select=[&](MgaRelationStorageDescriptor& v)->std::string& {
      auto& col=v.columns[0];
      if(field==0) return col.canonical_name_key;
      if(field==1) return col.value_descriptor.descriptor_kind;
      if(field==2) return col.value_descriptor.canonical_type_name;
      if(field==3) return col.value_descriptor.encoded_descriptor;
      if(field==4) return col.storage_class;
      return col.overflow_policy;
    };
    for(const std::string text:{std::string("a\0b",3),std::string("\xc0\x80",2),
        std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),
        std::string("\xe2\x82",2)}) {
      auto bad=d; select(bad)=text;Refuse(bad);
    }
    auto valid=d;select(valid)="\xce\xbb";
    Check(ComputeBulkImportColumnDigestV2(valid,&digest) && digest!=expected,"canonical UTF8");
  }
  for(unsigned field=0;field<11;++field) {
    auto changed=d;
    if(field==0) ++changed.relation_generation;
    if(field==1) ++changed.descriptor_generation;
    auto& col=changed.columns[0];
    if(field==2) ++col.column_generation;
    if(field==3) ++col.ordinal;
    if(field==4) col.nullable=!col.nullable;
    if(field==5) col.generated=!col.generated;
    if(field==6) col.identity_column=!col.identity_column;
    if(field==7) ++col.character_length;
    if(field==8) ++col.max_inline_bytes;
    if(field==9) col.value_descriptor.encoded_descriptor+=";x=1";
    if(field==10) col.canonical_name_key+="x";
    Check(ComputeBulkImportColumnDigestV2(changed,&digest) && digest!=expected,"bound field sensitivity");
  }
  unsigned faults=0;
  for(long n=0;n<4096;++n) {
    auto h=Sentinel(); bool ok=false; injected=false;allocation=n;
    try { ok=ComputeBulkImportColumnDigestV2(d,&h); } catch(const std::bad_alloc&) {}
    allocation=-1;
    if(!injected) { Check(ok && h==expected,"allocation sweep success");break; }
    ++faults;Check(!ok && h==Sentinel(),"allocation failure changed hash");
    Check(n<4095,"allocation sweep truncated");
  }
  Check(faults>0,"allocation injection reached production");
  std::cout<<"bulk column digest "<<checks<<" checks; "<<faults<<" allocation faults PASS\n";
 } catch(const std::exception& e) { allocation=-1;std::cerr<<checks<<": "<<e.what()<<'\n';return 1; }
}
