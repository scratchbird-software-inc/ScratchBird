// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <cstdio>
#include <new>
#include <utility>
namespace name_memory_fault {
thread_local long remaining=-1;
thread_local std::size_t allocations=0;
thread_local bool fired=false;
void* Allocate(std::size_t n){
  ++allocations;if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return name_memory_fault::Allocate(n);}
void* operator new[](std::size_t n){return name_memory_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  ++name_memory_fault::allocations;if(name_memory_fault::remaining==0){name_memory_fault::fired=true;throw std::bad_alloc();}
  if(name_memory_fault::remaining>0)--name_memory_fault::remaining;
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
template<class F> auto WithoutNameHeap(F action){
  name_memory_fault::remaining=0;name_memory_fault::fired=false;
  try{auto result=action();name_memory_fault::remaining=-1;
    if(name_memory_fault::fired){std::fputs("FAIL borrowed record attempted allocation\n",stderr);std::abort();}
    return result;
  }catch(...){name_memory_fault::remaining=-1;throw;}
}


#define SB_NATIVE_NAME_MEMORY_TESTS 1
#define main NameRecordRegressionMain
#include "../sbsql_sblr_alignment/catalog_name_record_codec_test.cpp"
#undef main

template<class Record> void ViewLifetime(const Record& r) {
  auto bytes=Oracle(SchemaId(r),Fields(r));
  const auto borrowed=DecodeView(r,bytes);
  Check(borrowed.ok(),"lifetime fixture");
  if(!borrowed.ok())return;
  const auto copy=WithoutNameHeap([&]{return *borrowed.record;});
  const auto fields=Fields(r);
  std::size_t cursor=24;
  for(const auto& f:fields){
    cursor+=8;
    if(f.tag==3||f.tag==4){
      // All non-native carriers must point into input, not a temporary field array.
      const auto check=[&](auto value){
        Check(reinterpret_cast<const byte*>(value.data())==bytes.data()+cursor &&
              value.size()==f.bytes.size(),"exact input-backed view extent");
      };
      if constexpr(std::is_same_v<Record,c::CatalogNameVector>){
        if(f.id==3)check(copy.object_class);
        if(f.id==5)check(copy.default_language_tag);
      }else{
        switch(f.id){
          case 4:check(copy.object_class);break;case 8:check(copy.language_tag);break;
          case 10:check(copy.donor_id);break;case 15:check(copy.raw_name_text);break;
          case 16:check(copy.display_name);break;case 20:check(copy.normalized_lookup_key);break;
          case 21:check(copy.exact_lookup_key);break;case 22:check(copy.full_path_lookup_key);break;
        }
      }
    }
    cursor+=f.bytes.size();
  }
  const auto owned=Materialize(copy);
  std::fill(bytes.begin(),bytes.end(),0); // Borrowed values are not used after source mutation.
  Check(Fields(owned)==fields,"final materialization independent of source lifetime");
}
template<class Record> void FieldBoundaries(const Record& r) {
  const auto original=Fields(r);
  for(std::size_t i=0;i<original.size();++i){
    const auto tag=original[i].tag;
    if(tag!=3&&tag!=4)continue;
    auto fields=original;
    // Maximum complete block, not merely per-field ceiling.
    const auto overhead=Oracle(SchemaId(r),fields).size()-fields[i].bytes.size();
    const auto maximum=131072-overhead;
    for(std::size_t length:{std::size_t(0),std::size_t(1),maximum,maximum+1,std::size_t(131041)}){
      fields[i].bytes.assign(length,tag==3?'x':0xff);
      const auto bytes=Oracle(SchemaId(r),fields);
      const auto result=DecodeView(r,bytes);
      const bool nonempty=(std::is_same_v<Record,c::CatalogNameVector>?(fields[i].id==3||fields[i].id==5):
           (fields[i].id==4||fields[i].id==8||fields[i].id==22));
      const bool valid=length<=maximum&&(!nonempty||length!=0);
      Check(result.ok()==valid,"independent complete-block/text/key bound");
      if(!valid)Check(!result.record,"bounds publish no partial name record");
      if(length>maximum)Check(result.error==c::CatalogValueError::size_limit,"exact size diagnostic");
      if(result.ok())Check(Fields(Materialize(*result.record))==fields,"maximum full field preservation");
    }
    if(tag==3){
      for(const std::vector<byte>& invalid:std::vector<std::vector<byte>>{
          {0x80},{0xc0,0x80},{0xc2},{0xe0,0x80,0x80},{0xed,0xa0,0x80},
          {0xf0,0x80,0x80,0x80},{0xf4,0x90,0x80,0x80},{0xff}}){
        fields=original;fields[i].bytes=invalid;
        const auto invalid_bytes=Oracle(SchemaId(r),fields);
        const auto result=DecodeView(r,invalid_bytes);
        Check(!result.record&&result.error==c::CatalogValueError::invalid_value,"strict UTF8 exact refusal");
      }
      fields=original;fields[i].bytes={0,0x0a,0x0d,0xc3,0xa9,0xf4,0x8f,0xbf,0xbf};
      const auto unicode_bytes=Oracle(SchemaId(r),fields);
      const auto result=DecodeView(r,unicode_bytes);
      Check(result.ok()&&Fields(Materialize(*result.record))==fields,"NUL and Unicode maximum preserved");
    }
  }
}
template<class Record> void ExactWireErrors(const Record& r) {
  const auto fields=Fields(r);
  auto bytes=Oracle(SchemaId(r),fields);
  auto reject=[&](const auto& b,c::CatalogValueError expected){
    const auto result=DecodeView(r,b);
    Check(!result.record&&result.error==expected,"independent exact framing/type/semantic diagnostic");
  };
  auto bad=bytes;bad[0]^=1;reject(bad,c::CatalogValueError::invalid_framing);
  bad=bytes;bad[4]=2;reject(bad,c::CatalogValueError::unsupported_version);
  bad=bytes;bad[22]=1;reject(bad,c::CatalogValueError::invalid_framing);
  auto missing=fields;missing.erase(missing.begin());reject(Oracle(SchemaId(r),missing),c::CatalogValueError::missing_field);
  auto wrong=fields;wrong.front().tag=4;reject(Oracle(SchemaId(r),wrong),c::CatalogValueError::type_mismatch);
  auto extra=fields;extra.push_back(Bytes(65535,{}));
  reject(Oracle(SchemaId(r),extra),c::CatalogValueError::invalid_framing); // count precedes field admission
  const u16 optional=std::is_same_v<Record,c::CatalogNameVector>?4:6;
  extra.erase(std::find_if(extra.begin(),extra.end(),[&](const auto& f){return f.id==optional;}));
  reject(Oracle(SchemaId(r),extra),c::CatalogValueError::unknown_field);
  auto nil=fields;nil.front().bytes.assign(16,0);reject(Oracle(SchemaId(r),nil),c::CatalogValueError::invalid_value);
  // Exhaust every octet mutation; the independent original tests specify invalid
  // cases separately. Here borrowed and owning readers must agree for all inputs.
  for(std::size_t offset=0;offset<bytes.size();++offset)for(unsigned value=0;value<256;++value){
    if(bytes[offset]==value)continue;
    bad=bytes;bad[offset]=value;
    (void)Decode(r,bad);
  }
}
template<class Record> void OwningFaults(Record r) {
  r.object_class=std::string(64,'c');
  if constexpr(std::is_same_v<Record,c::CatalogNameVector>)r.default_language_tag=std::string(64,'l');
  else {
    r.language_tag=std::string(64,'l');r.donor_id=std::string(64,'d');
    r.raw_name_text=std::string(64,'r');r.display_name=std::string(64,'s');
    r.normalized_lookup_key.assign(128,0xfe);r.exact_lookup_key.assign(128,0xff);
    r.full_path_lookup_key.assign(128,0x80);r.path_component_count=1;
  }
  const auto bytes=Oracle(SchemaId(r),Fields(r));
  const auto action=[&]{
    if constexpr(std::is_same_v<Record,c::CatalogNameVector>)return c::DecodeCatalogNameVector(bytes);
    else return c::DecodeCatalogNameEntry(bytes);
  };
  const auto before=name_memory_fault::allocations;
  Check(action().ok(),"owning baseline");
  const auto count=name_memory_fault::allocations-before;
  Check(count==(std::is_same_v<Record,c::CatalogNameVector>?2:8),"only final fields allocate");
  for(std::size_t point=0;point<count;++point){
    bool published=false;name_memory_fault::remaining=point;name_memory_fault::fired=false;
    try{published=action().ok();}catch(const std::bad_alloc&){}
    name_memory_fault::remaining=-1;
    Check(name_memory_fault::fired&&!published,"every final allocation fault has no partial record");
  }
  name_memory_fault::remaining=count;
  const auto result=action();name_memory_fault::remaining=-1;
  Check(result.ok()&&Fields(*result.record)==Fields(r),"exact allocation budget recovery");
  std::cout<<"name schema="<<SchemaId(r)<<" owning allocation_sites="<<count<<'\n';
}
int main(){
  // First use is before any owning schema/decoder initialization.
  const auto v=Vector();const auto e=Entry();
  Check(DecodeView(v,Oracle(327681,Fields(v))).ok(),"first-use vector no heap");
  Check(DecodeView(e,Oracle(327682,Fields(e))).ok(),"first-use entry no heap");
  NameRecordRegressionMain(); // Existing independent oracles and all assertions retained.
  for(unsigned mask=0;mask<2;++mask){auto r=v;if(mask)r.owning_schema_uuid.reset();Roundtrip(r);ViewLifetime(r);}
  for(unsigned mask=0;mask<32;++mask){
    auto r=e;unsigned bit=0;
    for(auto member:{&c::CatalogNameEntry::parent_object_uuid,&c::CatalogNameEntry::parent_schema_uuid,
        &c::CatalogNameEntry::case_fold_profile_uuid,&c::CatalogNameEntry::quoted_identifier_profile_uuid,
        &c::CatalogNameEntry::dropped_transaction_uuid}){
      if(mask&(1u<<bit))(r.*member).reset();++bit;
    }
    Roundtrip(r);ViewLifetime(r);
  }
  FieldBoundaries(v);FieldBoundaries(e);ExactWireErrors(v);ExactWireErrors(e);
  OwningFaults(v);OwningFaults(e);
  std::cout<<"name memory checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
