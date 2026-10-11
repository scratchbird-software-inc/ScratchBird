// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_context_namespace.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace { thread_local bool deny_heap = false; thread_local bool allocation_fault_fired = false; }
void* operator new(std::size_t size) {
  if (deny_heap) { allocation_fault_fired = true; throw std::bad_alloc(); }
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
p::Uuid Id(unsigned tag) { return {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,static_cast<p::byte>(tag)}}; }
void Put(std::string& s, std::size_t at, p::u64 value, unsigned width) {
  for (unsigned i=0;i<width;++i) s.at(at+i)=static_cast<char>(value>>(8*i));
}
// Independent oracle: six binary UUIDs, two LE uint64s, eight TLV headers.
std::string Golden(const c::CatalogContextNamespace& r) {
  std::string s(200,0); s.replace(0,4,"SBCV");
  Put(s,4,1,2);Put(s,6,24,2);Put(s,8,200,4);Put(s,12,8,4);Put(s,16,65560,4);Put(s,20,1,2);
  std::size_t at=24;
  const auto identity=[&](unsigned field, p::Uuid id) {
    Put(s,at,field,2);Put(s,at+2,5,1);Put(s,at+4,16,4);
    for (unsigned i=0;i<16;++i) s.at(at+8+i)=static_cast<char>(id.bytes[i]);
    at+=24;
  };
  const auto integer=[&](unsigned field,p::u64 value) {
    Put(s,at,field,2);Put(s,at+2,1,1);Put(s,at+4,8,4);Put(s,at+8,value,8);at+=16;
  };
  identity(1,r.namespace_uuid);integer(2,r.generation);identity(3,r.database_uuid);
  identity(4,r.schema_uuid);identity(5,r.owner_uuid);identity(6,r.security_label_uuid);
  identity(7,r.origin_transaction_uuid);integer(8,r.origin_local_transaction_id);
  Check(at==s.size(),"oracle width");return s;
}
c::CatalogContextNamespace Definition() { return {Id(1),1,Id(2),Id(3),Id(4),Id(5),Id(6),7}; }
c::CatalogMetadataVersion Metadata(const c::CatalogContextNamespace& r) {
  c::CatalogMetadataVersion m;
  m.record.header.kind=c::CatalogRecordKind::sql_object;
  m.record.header.row_uuid={p::UuidKind::row,Id(7)};
  m.record.header.object_uuid={p::UuidKind::object,r.namespace_uuid};
  m.record.header.parent_uuid={p::UuidKind::object,r.schema_uuid};
  m.record.payload=Golden(r);
  m.authority_scope=c::CatalogAuthorityScope::local;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.definition_version=r.generation;m.schema_epoch=1;m.security_epoch=1;
  m.resource_epoch=1;m.catalog_generation=1;m.dependency_generation=1;m.invalidation_generation=1;
  m.creator_local_transaction_id=r.origin_local_transaction_id;
  m.owner_uuid={p::UuidKind::principal,r.owner_uuid};
  m.creator_transaction_uuid={p::UuidKind::transaction,r.origin_transaction_uuid};
  m.default_name_uuid={p::UuidKind::object,Id(8)};m.name_vector_uuid={p::UuidKind::object,Id(9)};
  m.audit_uuid={p::UuidKind::object,Id(10)};m.owning_schema_uuid={p::UuidKind::schema,r.schema_uuid};
  m.trace_search_key="context_namespace.test";m.object_subtype="application_context_namespace";
  m.retention_class="catalog";return m;
}
template<class F> auto NoHeap(F f) {
  deny_heap=true;
  try { auto result=f();deny_heap=false;return result; }
  catch (...) { deny_heap=false;Check(false,"unexpected heap or exception in borrowed admission");throw; }
}
void Refused(const std::string& bytes) {
  const auto result=NoHeap([&]{return c::DecodeCatalogContextNamespace(bytes);});
  Check(!result.ok()&&!result.record,"malformed definition admitted");
}
}
int main() {
  const auto r=Definition(); const auto bytes=Golden(r);
  const auto encoded=c::EncodeCatalogContextNamespace(r);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==bytes,"binary golden encoding");
  const auto decoded=NoHeap([&]{return c::DecodeCatalogContextNamespace(bytes);});
  Check(decoded.ok()&&*decoded.record==r,"binary golden decoding");
  for (std::size_t length=0;length<bytes.size();++length) Refused(bytes.substr(0,length));
  Refused(bytes+"x");
  for (auto at: {0,4,6,8,12,16,20,22,24,26,27,28}) {
    auto bad=bytes;bad.at(at)^=0x40;Refused(bad);
  }
  for (auto at: {24,48,64,88,112,136,160,184}) {
    auto bad=bytes;Put(bad,at,1,2);if(at!=24)Refused(bad);
    bad=bytes;Put(bad,at+2,3,1);Refused(bad);
    bad=bytes;Put(bad,at+4,0,4);Refused(bad);
  }
  for (auto at: {32,72,96,120,144,168}) {
    auto bad=bytes;bad.replace(at,16,16,'\0');Refused(bad);
    bad=bytes;bad.at(at+6)=0x40;Refused(bad);
    bad=bytes;bad.at(at+8)=0x40;Refused(bad);
  }
  for (auto at: {56,192}) { auto bad=bytes;Put(bad,at,0,8);Refused(bad); }
  auto maximum=r;maximum.generation=maximum.origin_local_transaction_id=std::numeric_limits<p::u64>::max();
  const auto maximum_bytes=Golden(maximum);
  Check(NoHeap([&]{return c::DecodeCatalogContextNamespace(maximum_bytes);}).record==maximum,"uint64 bounds");
  auto m=Metadata(r);
  Check(!NoHeap([&]{return c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m));}),"valid metadata refused");
  const auto complete=c::EncodeCatalogMetadataVersion(m);
  Check(complete.ok(),"metadata encoding failed");
  Check(NoHeap([&]{return c::DecodeCatalogMetadataVersionView(complete.bytes);}).ok(),"metadata decode failed");
  auto RefuseMetadata=[&](const auto& changed) {
    Check(NoHeap([&]{return c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(changed));}).has_value(),
          "metadata relabel or mismatch admitted");
  };
  for (unsigned case_id=0;case_id<12;++case_id) {
    auto bad=m;
    switch(case_id) {
      case 0:bad.record.header.kind=c::CatalogRecordKind::policy;break;
      case 1:bad.record.header.object_uuid.value=Id(20);break;
      case 2:bad.record.header.parent_uuid.value=Id(20);break;
      case 3:bad.owning_schema_uuid.value=Id(20);break;
      case 4:bad.owner_uuid.value=Id(20);break;
      case 5:bad.owner_uuid.kind=p::UuidKind::object;break;
      case 6:bad.definition_version=2;break;
      case 7:bad.object_subtype="unrelated";break;
      case 8:bad.record.payload[16]^=1;break;
      case 9:bad.creator_transaction_uuid.value=Id(20);break;
      case 10:bad.creator_local_transaction_id=8;break;
      case 11:bad.authority_scope=c::CatalogAuthorityScope::cluster;break;
    }
    RefuseMetadata(bad);
  }
  auto successor=r;successor.generation=2;
  auto next=Metadata(successor);next.creator_local_transaction_id=9;
  next.creator_transaction_uuid.value=Id(21);
  Check(NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(c::BorrowCatalogMetadataVersion(m),c::BorrowCatalogMetadataVersion(next));}),
        "valid namespace successor refused");
  for (unsigned case_id=0;case_id<3;++case_id) {
    auto changed=successor;
    if(case_id==0)changed.database_uuid=Id(22);
    if(case_id==1)changed.origin_transaction_uuid=Id(22);
    if(case_id==2)changed.origin_local_transaction_id=8;
    next.record.payload=Golden(changed);
    Check(!NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(c::BorrowCatalogMetadataVersion(m),c::BorrowCatalogMetadataVersion(next));}),
          "namespace origin changed");
  }
  std::cout<<"context_namespace_definition checks="<<checks<<" PASS\n";
  return 0;
}
