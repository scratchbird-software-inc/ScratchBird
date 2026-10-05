// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_table_datatype_binding.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <unistd.h>

namespace {
bool no_heap=false;
long fail_after=-1;
std::size_t allocations=0,checks=0,faults=0;
}
void* operator new(std::size_t n) {
  ++allocations;
  if(no_heap||(fail_after>=0&&fail_after--==0))throw std::bad_alloc{};
  if(auto* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace c=scratchbird::core::catalog;
namespace d=scratchbird::core::datatypes;
namespace p=scratchbird::core::platform;
namespace {
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct NoHeap {NoHeap(){no_heap=true;}~NoHeap(){no_heap=false;}};
p::TypedUuid Id(unsigned n,p::UuidKind kind=p::UuidKind::object) {
  p::TypedUuid out{kind,{}};out.value.bytes[6]=0x70;out.value.bytes[8]=0x80;
  out.value.bytes[14]=n>>8;out.value.bytes[15]=n;return out;
}
void Put(std::string& s,std::size_t at,p::u64 n,unsigned width) {
  for(unsigned i=0;i<width;++i){s.at(at+i)=static_cast<char>(n%256);n/=256;}
}
// Independent SBCV framing: no production schema or encoder used.
std::string Golden(const c::CatalogIntervalOccurrence& v) {
  std::string out(720,0);out.replace(0,4,"SBCV");
  Put(out,4,1,2);Put(out,6,24,2);Put(out,8,720,4);Put(out,12,4,4);Put(out,16,65702,4);Put(out,20,1,2);
  Put(out,24,1,2);Put(out,26,5,1);Put(out,28,16,4);
  std::copy(v.occurrence.uuid.value.bytes.begin(),v.occurrence.uuid.value.bytes.end(),out.begin()+32);
  Put(out,48,2,2);Put(out,50,1,1);Put(out,52,8,4);Put(out,56,v.occurrence.generation,8);
  Put(out,64,3,2);Put(out,66,4,1);Put(out,68,608,4);
  std::copy(v.profile_material.begin(),v.profile_material.end(),out.begin()+72);
  Put(out,680,4,2);Put(out,682,4,1);Put(out,684,32,4);
  std::copy(v.profile_fingerprint.begin(),v.profile_fingerprint.end(),out.begin()+688);return out;
}
struct Fixture {
  d::IntervalValidatedProfileHandleV3 profile;
  c::CatalogIntervalOccurrence occurrence;
  c::CatalogColumnDefinition column;
  c::CatalogMetadataVersion column_metadata,occurrence_metadata;
};
c::CatalogMetadataVersion Common() {
  c::CatalogMetadataVersion m;
  m.record.header.row_uuid=Id(30,p::UuidKind::row);m.record.header.object_uuid=Id(10);
  m.record.header.parent_uuid=Id(1);
  m.owner_uuid=Id(40,p::UuidKind::principal);m.audit_uuid=Id(41);
  m.owning_schema_uuid=Id(50,p::UuidKind::schema);
  m.creator_transaction_uuid=Id(60,p::UuidKind::transaction);m.creator_local_transaction_id=9;
  m.definition_version=m.schema_epoch=m.security_epoch=m.catalog_generation=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.trace_search_key="INTERVAL-OCCURRENCE-COMPONENT";m.retention_class="catalog_history";return m;
}
Fixture MakeFixture() {
  Fixture f;auto result=d::BuildCurrentIntervalValidatedProfileHandleV3(d::kDatatypeCohortV10);
  Check(result.ok(),"qualified d710 interval profile unavailable");f.profile=std::move(result.profile);
  f.occurrence={{Id(20),8},f.profile.profile_material,f.profile.profile_fingerprint};
  const auto& id=f.profile.identity.legacy_fields;
  f.column.column_uuid=Id(10);f.column.table_uuid=Id(1);
  f.column.origin_transaction_uuid=Id(60,p::UuidKind::transaction);f.column.origin_local_transaction_id=9;
  f.column.ordinal=7;f.column.value_descriptor=f.occurrence.occurrence;
  f.column.datatype_descriptor={{p::UuidKind::object,id.descriptor_uuid},id.descriptor_generation};
  f.column.type_uuid={p::UuidKind::object,id.type_uuid};
  f.column_metadata=Common();f.column_metadata.record.header.kind=c::CatalogRecordKind::column_descriptor;
  f.column_metadata.default_name_uuid=Id(70);f.column_metadata.name_vector_uuid=Id(71);
  f.column_metadata.object_subtype="persistent_table_column";
  const auto encoded=c::EncodeCatalogColumnDefinition(f.column);Check(encoded.ok(),"column fixture invalid");
  f.column_metadata.record.payload={encoded.bytes.begin(),encoded.bytes.end()};
  f.occurrence_metadata=Common();f.occurrence_metadata.record.header.kind=c::CatalogRecordKind::datatype_descriptor;
  f.occurrence_metadata.record.header.row_uuid=Id(31,p::UuidKind::row);
  f.occurrence_metadata.record.header.object_uuid=Id(20);f.occurrence_metadata.record.header.parent_uuid=Id(50);
  f.occurrence_metadata.object_subtype="persistent_interval_value_occurrence";
  f.occurrence_metadata.definition_version=8;f.occurrence_metadata.record.payload=Golden(f.occurrence);
  return f;
}
auto Bind(const Fixture& f) {
  NoHeap guard;return c::BindCatalogIntervalColumn(c::BorrowCatalogMetadataVersion(f.column_metadata),
      c::BorrowCatalogMetadataVersion(f.occurrence_metadata),f.profile);
}
auto Decode(std::string_view bytes,const Fixture& f) {
  NoHeap guard;return c::DecodeCatalogIntervalOccurrence(bytes,f.profile);
}
void Payloads(const Fixture& f) {
  const auto golden=Golden(f.occurrence);
  auto encoded=c::EncodeCatalogIntervalOccurrence(f.occurrence,f.profile);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==golden,"720-byte independent oracle mismatch");
  auto decoded=Decode(golden,f);Check(decoded.ok()&&Golden(*decoded.definition)==golden,"fixed decode mismatch");
  for(std::size_t n=0;n<golden.size();++n) {
    auto r=Decode(std::string_view(golden).substr(0,n),f);
    Check(!r.ok()&&!r.definition,"truncated occurrence accepted");
  }
  for(unsigned offset=72;offset<720;++offset) {
    if(offset>=680&&offset<688)continue; // Headers tested separately below.
    auto bad=golden;bad[offset]^=1;auto r=Decode(bad,f);
    Check(!r.ok()&&!r.definition&&r.error==c::CatalogIntervalBindingError::profile_mismatch,
          "profile or fingerprint mutation accepted");
  }
  for(unsigned offset:{0,4,6,8,12,16,20,22,24,26,27,28,48,50,51,52,64,66,67,68,680,682,683,684}) {
    auto bad=golden;bad[offset]^=0x40;auto r=Decode(bad,f);
    Check(!r.ok()&&!r.definition,"bad SBCV framing/field admitted");
  }
  auto tail=golden;tail.push_back(0);Check(!Decode(tail,f).ok(),"trailing bytes admitted");
  for(p::u64 generation:{p::u64{0},p::u64{1},~p::u64{0}}) {
    auto v=f.occurrence;v.occurrence.generation=generation;auto bytes=Golden(v);
    const auto r=Decode(bytes,f);Check(r.ok()==(generation!=0),"occurrence generation boundary");
    auto e=c::EncodeCatalogIntervalOccurrence(v,f.profile);Check(e.ok()==(generation!=0),"encode generation boundary");
  }
  for(auto identity:{p::Uuid{},f.profile.identity.legacy_fields.descriptor_uuid,
      f.profile.identity.legacy_fields.type_uuid,f.profile.identity.legacy_fields.codec_uuid}) {
    auto v=f.occurrence;v.occurrence.uuid.value=identity;
    Check(!Decode(Golden(v),f).ok(),"occurrence identity collision/nil accepted");
    const auto r=c::EncodeCatalogIntervalOccurrence(v,f.profile);
    Check(!r.ok()&&r.bytes.empty(),"invalid occurrence encoded");
  }
  auto wrong_kind=f.occurrence;wrong_kind.occurrence.uuid.kind=p::UuidKind::schema;
  Check(!c::EncodeCatalogIntervalOccurrence(wrong_kind,f.profile).ok(),"wrong identity kind encoded");
  auto wrong_version=golden;wrong_version[38]=0x40;Check(!Decode(wrong_version,f).ok(),"non-v7 occurrence accepted");
  for(unsigned field=0;field<4;++field) {
    const std::array<unsigned,4> offsets{24,48,64,680},lengths{24,16,616,40};
    auto missing=golden;missing.erase(offsets[field],lengths[field]);
    Put(missing,8,missing.size(),4);Put(missing,12,3,4);
    const auto absent=Decode(missing,f);
    Check(!absent.ok()&&!absent.definition&&absent.value_error==c::CatalogValueError::missing_field,
          "required occurrence field omitted");
    auto duplicate=golden;duplicate.insert(offsets[field],golden.substr(offsets[field],lengths[field]));
    Put(duplicate,8,duplicate.size(),4);Put(duplicate,12,5,4);
    Check(!Decode(duplicate,f).ok(),"duplicate occurrence field admitted");
  }
  for(unsigned n:{0u,1u,31u,32u,607u}) {
    auto short_profile=golden;short_profile.erase(72+n,608-n);
    Put(short_profile,8,short_profile.size(),4);Put(short_profile,68,n,4);
    const auto r=Decode(short_profile,f);
    Check(!r.ok()&&!r.definition,"short well-framed profile material accepted");
  }
  for(unsigned n:{0u,1u,31u}) {
    auto short_digest=golden;short_digest.resize(688+n);
    Put(short_digest,8,short_digest.size(),4);Put(short_digest,684,n,4);
    Check(!Decode(short_digest,f).ok(),"short well-framed fingerprint accepted");
  }
}
void Bindings(const Fixture& f) {
  auto accepted=Bind(f);Check(accepted.ok(),"valid interval column binding refused");
  Check(accepted.binding->column.ordinal==7&&accepted.binding->column.column_uuid.value==Id(10).value,
        "binding changed column identity or ordinal");
  for(unsigned mutation=0;mutation<17;++mutation) {
    auto v=f;auto& m=v.occurrence_metadata;
    if(mutation==0)m.record.header.object_uuid=Id(21);
    if(mutation==1)m.definition_version=9;
    if(mutation==2)m.record.header.parent_uuid=Id(51);
    if(mutation==3)m.owning_schema_uuid=Id(51,p::UuidKind::schema),m.record.header.parent_uuid=Id(51);
    if(mutation==4)m.object_subtype="datatype_descriptor";
    if(mutation==5)m.authority_scope=c::CatalogAuthorityScope::temporary;
    if(mutation==6)m.record.header.kind=c::CatalogRecordKind::sql_object;
    if(mutation==7)m.owning_schema_uuid={};
    if(mutation==8)v.column.value_descriptor.generation=9;
    if(mutation==9)v.column.value_descriptor.uuid=Id(22);
    if(mutation==10)v.column.datatype_descriptor.generation=2;
    if(mutation==11)v.column.datatype_descriptor.uuid=Id(23);
    if(mutation==12)v.column.type_uuid=Id(24);
    if(mutation==13)v.column.charset={Id(25),1};
    if(mutation==14)v.column.charset={Id(25),1},v.column.collation={Id(26),2};
    if(mutation==15)v.column_metadata.record.header.object_uuid=Id(12);
    if(mutation==16)v.column_metadata.object_subtype="other_column";
    auto bytes=c::EncodeCatalogColumnDefinition(v.column);Check(bytes.ok(),"negative column fixture malformed");
    v.column_metadata.record.payload={bytes.bytes.begin(),bytes.bytes.end()};
    auto r=Bind(v);Check(!r.ok()&&!r.binding,"mismatched metadata/column reference admitted");
  }
  auto shared=f;shared.column.column_uuid=Id(11);shared.column.ordinal=100;
  shared.column_metadata.record.header.object_uuid=shared.column.column_uuid;
  auto bytes=c::EncodeCatalogColumnDefinition(shared.column);
  shared.column_metadata.record.payload={bytes.bytes.begin(),bytes.bytes.end()};
  Check(Bind(shared).ok(),"schema-local shared occurrence rejected");
  auto retired=f;
  for(auto* m:{&retired.column_metadata,&retired.occurrence_metadata}) {
    m->record.header.deleted=true;m->lifecycle=c::CatalogObjectLifecycle::dropped;
    m->status=c::CatalogObjectStatus::retired;m->retired_transaction_uuid=m->creator_transaction_uuid;
  }
  Check(Bind(retired).ok(),"retained structural history rejected; visibility belongs to caller");
  auto renamed=f;renamed.column_metadata.default_name_uuid=Id(75);renamed.column_metadata.name_vector_uuid=Id(76);
  Check(Bind(renamed).ok(),"name reference change altered occurrence binding");
}
bool Cancel(void*) noexcept{return true;}
void Diagnostics(const Fixture& f) {
  auto invalid=f;invalid.profile.receipt.catalog_generation=9;invalid.column_metadata.record.payload="bad";
  const auto expected=d::ValidateIntervalProfileHandleV3(invalid.profile);
  auto r=Bind(invalid);
  Check(!r.ok()&&r.error==c::CatalogIntervalBindingError::invalid_profile&&r.datatype_diagnostic&&
        r.datatype_diagnostic->diagnostic_code==expected.diagnostic.diagnostic_code&&
        r.datatype_diagnostic->detail==expected.diagnostic.detail,"profile diagnostic/precedence lost");
  for(unsigned mode=0;mode<2;++mode) {
    d::IntervalExecutionControlV3 control;
    if(mode==0)control.maximum_allocation_bytes=0;else control.cancelled=Cancel;
    const auto expected_control=d::ValidateIntervalProfileHandleV3(f.profile,control);
    c::CatalogIntervalOccurrenceResult decoded;
    {NoHeap guard;decoded=c::DecodeCatalogIntervalOccurrence("bad",f.profile,control);}
    Check(!decoded.ok()&&decoded.datatype_diagnostic&&
          decoded.datatype_diagnostic->diagnostic_code==expected_control.diagnostic.diagnostic_code&&
          decoded.datatype_diagnostic->detail==expected_control.diagnostic.detail,"datatype resource/cancellation diagnostic lost");
  }
  auto bad=f;bad.column_metadata.definition_version=0;
  auto refused=Bind(bad);Check(!refused.ok()&&refused.metadata_diagnostic&&
      refused.metadata_diagnostic->detail=="counter_zero","common diagnostic lost");
  // Corrupt both the stored material and supplied profile: a matching forged
  // pair must still fail the datatype owner's independent profile validation.
  for(unsigned i=0;i<608;++i) {
    auto forged=f;forged.profile.profile_material[i]^=1;forged.occurrence.profile_material[i]^=1;
    forged.occurrence_metadata.record.payload=Golden(forged.occurrence);
    const auto r=Bind(forged);
    Check(!r.ok()&&!r.binding&&r.datatype_diagnostic&&
          r.datatype_diagnostic->diagnostic_code=="CTI.INTERVAL.DESCRIPTOR_INVALID",
          "matching forged stored/supplied profile admitted");
  }
}
void AllocationFaults(const Fixture& f) {
  const auto before=Golden(f.occurrence);const auto start=allocations;
  const auto encoded=c::EncodeCatalogIntervalOccurrence(f.occurrence,f.profile);
  const auto count=allocations-start;Check(encoded.ok()&&count>0,"allocation measurement failed");
  for(std::size_t i=0;i<count;++i) {
    bool thrown=false;fail_after=i;
    try{(void)c::EncodeCatalogIntervalOccurrence(f.occurrence,f.profile);}catch(const std::bad_alloc&){thrown=true;++faults;}
    fail_after=-1;Check(thrown&&Golden(f.occurrence)==before,"encoding fault lost input or partial success");
  }
}
class File {
 public:
  File() {
    const auto* base=std::getenv("TMPDIR");
    auto pattern=(std::filesystem::path(base?base:"/tmp")/"sb_01c_interval_XXXXXX").string();
    const int fd=::mkstemp(pattern.data());Check(fd>=0,"fixture create failed");
    path=pattern;Check(::close(fd)==0,"fixture close failed");
  }
  ~File(){std::error_code ec;std::filesystem::remove(path,ec);}
  void Write(const std::vector<p::byte>& bytes) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());stream.close();
    Check(!stream.fail(),"fixture write/close failed");
  }
  std::string Read()const {
    std::ifstream stream(path,std::ios::binary);Check(stream.good(),"fixture reopen failed");
    return {std::istreambuf_iterator<char>(stream),{}};
  }
 private:std::filesystem::path path;
};
void PersistedDefinitions(const Fixture& f) {
  // Real files containing complete common metadata, not a private production
  // store or native table/root publication. Each stream is closed then reopened.
  File column_file,occurrence_file;
  auto col=c::EncodeCatalogMetadataVersion(f.column_metadata);
  auto occ=c::EncodeCatalogMetadataVersion(f.occurrence_metadata);
  Check(col.ok()&&occ.ok(),"metadata fixture encoding failed");
  column_file.Write(col.bytes);occurrence_file.Write(occ.bytes);
  auto col_bytes=column_file.Read(),occ_bytes=occurrence_file.Read();
  c::CatalogIntervalColumnBindingResult binding;
  {
    NoHeap guard;
    auto column=c::DecodeCatalogMetadataVersionView({reinterpret_cast<const p::byte*>(col_bytes.data()),col_bytes.size()});
    auto occurrence=c::DecodeCatalogMetadataVersionView({reinterpret_cast<const p::byte*>(occ_bytes.data()),occ_bytes.size()});
    if(column.ok()&&occurrence.ok())binding=c::BindCatalogIntervalColumn(*column.record,*occurrence.record,f.profile);
  }
  Check(binding.ok()&&Golden(binding.binding->value_definition)==Golden(f.occurrence),"reopened exact binding lost");
  Check(column_file.Read()==col_bytes&&occurrence_file.Read()==occ_bytes,"read altered durable definitions");
  std::fill(col_bytes.begin(),col_bytes.end(),'x');std::fill(occ_bytes.begin(),occ_bytes.end(),'x');
  Check(Golden(binding.binding->value_definition)==Golden(f.occurrence)&&binding.binding->column.ordinal==7,
        "decoded definition retained borrowed input");
}
}
int main() {
  try {const auto f=MakeFixture();Payloads(f);Bindings(f);Diagnostics(f);AllocationFaults(f);PersistedDefinitions(f);
    std::cout<<"PASS interval occurrence checks="<<checks<<" faults="<<faults<<"; component only, no native CRUD claim\n";return 0;
  }catch(const std::exception& e){no_heap=false;fail_after=-1;std::cerr<<e.what()<<'\n';return 1;}
}
