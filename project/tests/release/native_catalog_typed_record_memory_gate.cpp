// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <cstdio>
#include <new>
#include <utility>
namespace record_memory_fault {
thread_local long remaining=-1;
thread_local std::size_t allocations=0;
thread_local bool fired=false;
void* Allocate(std::size_t n){
  ++allocations;if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return record_memory_fault::Allocate(n);}
void* operator new[](std::size_t n){return record_memory_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  ++record_memory_fault::allocations;if(record_memory_fault::remaining==0){record_memory_fault::fired=true;throw std::bad_alloc();}
  if(record_memory_fault::remaining>0)--record_memory_fault::remaining;
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
template<class F> auto WithoutRecordHeap(F action){
  record_memory_fault::remaining=0;record_memory_fault::fired=false;
  try{auto result=action();record_memory_fault::remaining=-1;
    if(record_memory_fault::fired){std::fputs("FAIL borrowed record attempted allocation\n",stderr);std::abort();}
    return result;
  }catch(...){record_memory_fault::remaining=-1;throw;}
}
#define SB_CATALOG_TYPED_BORROWED_MEMORY_GATE
#define main existing_uuid_admission_main
#include "../sbsql_sblr_alignment/catalog_uuid_admission_test.cpp"
#undef main

namespace {
cat::CatalogTypedRecord Fixture(){
  cat::CatalogTypedRecord r;r.header.kind=cat::CatalogRecordKind::sql_object;
  for(unsigned i=0;i<3;++i){auto& id=r.header.*fields[i];id.kind=i?UuidKind::object:UuidKind::row;
    id.value.bytes[6]=0x70;id.value.bytes[8]=0x80;id.value.bytes[15]=i+1;}
  r.payload=std::string(80,'x');return r;
}
void Registry(){
  // Existing builtin registry explicitly imported by Core SBCTREC2. Expected
  // numeric identities, not production enumeration or lookup as the oracle.
  constexpr std::array<unsigned,50> ids{1,2,3,4,5,6,7,84,85,86,87,88,89,10,11,20,21,22,23,30,31,32,
      40,41,50,51,60,61,62,63,64,65,70,71,72,80,81,82,83,90,91,92,93,99,98,94,95,96,97,100};
  const auto registry=WithoutRecordHeap([]{return cat::BuiltinCatalogRecordDescriptorViews();});
  Check(registry.size()==ids.size(),"fixed builtin count");
  for(unsigned raw=0;raw<=65535;++raw){const auto* found=WithoutRecordHeap([&]{
      return cat::FindBuiltinCatalogRecordDescriptor(static_cast<cat::CatalogRecordKind>(raw));});
    const bool expected=std::find(ids.begin(),ids.end(),raw)!=ids.end();Check(bool(found)==expected,"exhaustive kind lookup");
    if(found)Check(!found->stable_name.empty()&&found->engine_authority&&found->requires_row_uuid,"complete immutable builtin descriptor");}
  const auto& owned=cat::BuiltinCatalogRecordDescriptors();Check(owned.size()==registry.size(),"owning registry size preserved");
  for(std::size_t i=0;i<registry.size();++i){const auto& a=registry[i];const auto& b=owned[i];
    Check(static_cast<unsigned>(a.kind)==ids[i]&&a.kind==b.kind&&a.scope==b.scope&&a.stable_name==b.stable_name&&
        a.requires_row_uuid==b.requires_row_uuid&&a.requires_object_uuid==b.requires_object_uuid&&
        a.requires_parent_uuid==b.requires_parent_uuid&&a.may_reference_toast==b.may_reference_toast&&
        a.mutable_after_create==b.mutable_after_create&&a.parser_visible==b.parser_visible&&a.engine_authority==b.engine_authority,
        "every owning descriptor field preserved");}
}
void Boundaries(){
  const auto fixture=Fixture();const auto raw=Golden(fixture);using K=scratchbird::storage::page::CatalogPageRowKind;
  for(std::size_t n=0;n<raw.size();++n){const auto input=std::string_view(raw).substr(0,n);
    const auto result=WithoutRecordHeap([&]{return cat::DecodeCatalogTypedRecordView(K::typed_catalog_record,input);});
    Check(!result.ok()&&!result.record,"every truncation fails without partial view");}
  for(unsigned raw_kind=0;raw_kind<=65535;++raw_kind){const auto kind=static_cast<K>(raw_kind);
    const auto result=WithoutRecordHeap([&]{return cat::DecodeCatalogTypedRecordView(kind,raw);});
    Check(result.ok()==(kind==K::typed_catalog_record),"exact enclosing row kind");}
  for(auto at:{18u,19u,35u,36u,37u,38u,39u,88u,89u,90u,91u,92u,93u,94u,95u})for(unsigned value=1;value<256;++value){
    auto bad=raw;bad[at]=value;
    auto result=WithoutRecordHeap([&]{return cat::DecodeCatalogTypedRecordView(K::typed_catalog_record,bad);});
    Check(!result.ok()&&result.diagnostic.detail=="binary_lengths_flags_or_reserved_invalid","reserved octets exact error");}
  auto bad=fixture;bad.header.row_uuid={};bad.header.object_uuid={};
  auto result=WithoutRecordHeap([&]{return cat::ValidateCatalogTypedRecordView(cat::BorrowCatalogTypedRecord(bad));});
  Check(result.diagnostic.diagnostic_code=="SB-CATALOG-RECORD-CODEC-ROW-UUID-MUST-BE-V7","row before object precedence");
  bad.header.record_version=2;result=WithoutRecordHeap([&]{return cat::ValidateCatalogTypedRecordView(cat::BorrowCatalogTypedRecord(bad));});
  Check(result.diagnostic.diagnostic_code=="SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED","version before identity precedence");
  bad.header.kind=cat::CatalogRecordKind::unknown;result=WithoutRecordHeap([&]{return cat::ValidateCatalogTypedRecordView(cat::BorrowCatalogTypedRecord(bad));});
  Check(result.diagnostic.diagnostic_code=="SB-CATALOG-RECORD-UNKNOWN-KIND"&&result.diagnostic.origin=="core.catalog.records","kind before version precedence");
  // Optional parent selects the complete durable UUID diagnostic vector.
  for(unsigned kind=0;kind<256;++kind)for(unsigned shape=0;shape<4;++shape){
    bad=fixture;bad.header.kind=cat::CatalogRecordKind::database;auto& id=bad.header.parent_uuid;
    id.kind=static_cast<UuidKind>(kind);if(shape==1)id.value={};if(shape==2)id.value.bytes[8]=0;if(shape==3)id.value.bytes[6]=0x40;
    if(id.kind==UuidKind::unknown&&id.value.is_nil())continue;
    const auto expected=uuid::MakeDurableEngineIdentityUuid(id.kind,id.value);
    result=WithoutRecordHeap([&]{return cat::ValidateCatalogTypedRecordView(cat::BorrowCatalogTypedRecord(bad));});
    Check(result.ok()==expected.ok(),"durable UUID helper admission parity");
    if(!expected.ok())DiagnosticParity(result.diagnostic,expected.diagnostic);
  }
  auto disposable=raw;const auto view=WithoutRecordHeap([&]{return cat::DecodeCatalogTypedRecordView(K::typed_catalog_record,disposable);});
  auto copy=*view.record;Check(copy.payload.data()==disposable.data()+96,"copied record remains input-backed");
  const auto owned=cat::DecodeCatalogTypedRecord({K::typed_catalog_record,73,disposable});std::fill(disposable.begin(),disposable.end(),0);
  Check(owned.ok()&&owned.row.ordinal==73&&owned.row.payload==raw&&owned.record.payload==fixture.payload,"owning lifetime and ordinal independent");
}
void Faults(){
  const auto r=Fixture();using K=scratchbird::storage::page::CatalogPageRowKind;
  const cat::CatalogPageRow row{K::typed_catalog_record,73,Golden(r)};
  for(unsigned mode=0;mode<2;++mode){auto action=[&]{return mode?cat::DecodeCatalogTypedRecord(row):cat::EncodeCatalogTypedRecord(r,73);};
    const auto before=record_memory_fault::allocations;Check(action().ok(),"owning measurement succeeds");
    const auto count=record_memory_fault::allocations-before;Check(count>0,"nonvacuous owning allocation sweep");
    for(std::size_t point=0;point<count;++point){record_memory_fault::remaining=point;record_memory_fault::fired=false;
      bool success=false;try{success=action().ok();}catch(const std::bad_alloc&){}
      const bool fired=record_memory_fault::fired;record_memory_fault::remaining=-1;Check(fired&&!success,"owning every allocation fault");}
    record_memory_fault::remaining=count;record_memory_fault::fired=false;const auto result=action();record_memory_fault::remaining=-1;
    Check(result.ok()&&!record_memory_fault::fired&&result.row.payload==row.payload,"exact allocation boundary recovery");
    std::printf("typed record owning mode=%u allocation_sites=%zu\n",mode,count);
  }
}
}
int main(){
  const auto first=Golden(Fixture());
  Check(WithoutRecordHeap([&]{return cat::DecodeCatalogTypedRecordView(
      scratchbird::storage::page::CatalogPageRowKind::typed_catalog_record,first);}).ok(),"first-use common decoder no heap");
  Registry();Boundaries();Faults();existing_uuid_admission_main();
  std::printf("typed record memory checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
