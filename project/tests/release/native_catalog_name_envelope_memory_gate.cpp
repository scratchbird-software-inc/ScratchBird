// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <cstdio>
#include <new>
#include <utility>
namespace envelope_memory_fault {
thread_local long remaining=-1;
thread_local std::size_t allocations=0;
thread_local bool fired=false;
void* Allocate(std::size_t n){
  ++allocations;if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return envelope_memory_fault::Allocate(n);}
void* operator new[](std::size_t n){return envelope_memory_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  ++envelope_memory_fault::allocations;if(envelope_memory_fault::remaining==0){envelope_memory_fault::fired=true;throw std::bad_alloc();}
  if(envelope_memory_fault::remaining>0)--envelope_memory_fault::remaining;
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
template<class F> auto WithoutEnvelopeHeap(F action){
  envelope_memory_fault::remaining=0;envelope_memory_fault::fired=false;
  try{auto result=action();envelope_memory_fault::remaining=-1;
    if(envelope_memory_fault::fired){std::fputs("FAIL borrowed record attempted allocation\n",stderr);std::abort();}
    return result;
  }catch(...){envelope_memory_fault::remaining=-1;throw;}
}


#define SB_NATIVE_NAME_ENVELOPE_MEMORY_TESTS 1
#define main NameEnvelopeRegressionMain
#include "../sbsql_sblr_alignment/catalog_name_envelope_test.cpp"
#undef main

void OwningEnvelopeFaults(bool entry){
  auto fixture=Fixture(entry);
  std::visit([](auto& r){
    r.object_class=std::string(64,'c');
    if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameVector>)
      r.default_language_tag=std::string(64,'l');
    else {
      r.language_tag=std::string(64,'l');r.donor_id=std::string(64,'d');
      r.raw_name_text=std::string(64,'r');r.display_name=std::string(64,'s');
      r.normalized_lookup_key.assign(128,0xfe);r.exact_lookup_key.assign(128,0xff);
      r.full_path_lookup_key.assign(128,0x80);r.path_component_count=1;
    }
  },fixture.payload);
  auto bytes=c::EncodeCatalogNameEnvelope(fixture).bytes;
  const auto view=WithoutEnvelopeHeap([&]{return c::DecodeCatalogNameEnvelopeView(bytes,fixture.binding);});
  Check(view.ok(),"borrowed envelope lifetime fixture");
  if(!view.ok())return;
  const auto copy=WithoutEnvelopeHeap([&]{return *view.record;});
  std::visit([&](const auto& payload){
    const auto* p=reinterpret_cast<const byte*>(payload.object_class.data());
    Check(p>=bytes.data()+176&&p+payload.object_class.size()<=bytes.data()+bytes.size(),
        "envelope payload aliases retained original bytes");
  },copy.payload);
  const auto action=[&]{return c::DecodeCatalogNameEnvelope(bytes,fixture.binding);};
  const auto before=envelope_memory_fault::allocations;
  Check(action().ok(),"owning envelope allocation baseline");
  const auto count=envelope_memory_fault::allocations-before;
  Check(count==(entry?8:2),"envelope only final fields allocate");
  for(std::size_t point=0;point<count;++point){
    bool success=false;envelope_memory_fault::remaining=point;envelope_memory_fault::fired=false;
    try{success=action().ok();}catch(const std::bad_alloc&){}
    envelope_memory_fault::remaining=-1;
    Check(envelope_memory_fault::fired&&!success,"all final envelope allocations fault without partial result");
  }
  envelope_memory_fault::remaining=count;
  const auto owned=action();envelope_memory_fault::remaining=-1;
  Check(owned.ok(),"exact final envelope allocation budget");
  const auto independently_owned=c::MaterializeCatalogNameEnvelope(copy);
  std::fill(bytes.begin(),bytes.end(),0); // No borrowed access after mutation.
  const auto expected=c::EncodeCatalogNameEnvelope(fixture).bytes;
  Check(c::EncodeCatalogNameEnvelope(*owned.record).bytes==expected&&
      c::EncodeCatalogNameEnvelope(independently_owned).bytes==expected,
      "owning envelope independent of source destruction");
  std::cout<<"name envelope entry="<<entry<<" owning allocation_sites="<<count<<'\n';
}
void ExactEnvelopeBounds(bool entry){
  auto fixture=Fixture(entry);
  const auto initial=c::EncodeCatalogNameEnvelope(fixture);
  Check(initial.ok(),"envelope maximum fixture");
  const auto fixed=std::visit([&](const auto& p){return initial.bytes.size()-p.object_class.size();},fixture.payload);
  for(const auto size:{std::size_t(131071),std::size_t(131072),std::size_t(131073)}){
    std::visit([&](auto& p){p.object_class.assign(size-fixed,'x');},fixture.payload);
    // Independently assemble outer lengths around the admitted payload; the
    // oversized case bypasses the owning envelope encoder's expected refusal.
    const auto payload=std::visit([](const auto& p){
      if constexpr(std::is_same_v<std::decay_t<decltype(p)>,c::CatalogNameVector>)return c::EncodeCatalogNameVector(p);
      else return c::EncodeCatalogNameEntry(p);
    },fixture.payload);
    Check(payload.ok(),"maximum nested payload independently admitted");
    auto bytes=initial.bytes;bytes.resize(176);bytes.insert(bytes.end(),payload.bytes.begin(),payload.bytes.end());
    Set32(bytes,8,bytes.size());Set32(bytes,12,payload.bytes.size());
    const auto decoded=WithoutEnvelopeHeap([&]{return c::DecodeCatalogNameEnvelopeView(bytes,fixture.binding);});
    Check(bytes.size()==size&&decoded.ok()==(size<=131072),"exact envelope maximum boundary");
    if(size>131072)Check(decoded.error==c::CatalogNameEnvelopeError::size_limit&&!decoded.record,
        "maximum plus one exact refusal without partial envelope");
    else Check(c::EncodeCatalogNameEnvelope(c::MaterializeCatalogNameEnvelope(*decoded.record)).bytes==bytes,
        "maximum envelope preserves exact admitted bytes");
  }
}
c::CatalogMetadataVersion NameMetadata(const c::CatalogNamePayload& payload){
  c::CatalogMetadataVersion m;
  m.record.header.kind=c::CatalogRecordKind::localized_name;
  m.definition_version=1;
  std::visit([&](const auto& r){
    m.name_vector_uuid=r.name_vector_uuid;
    m.security_policy_uuid=r.security_policy_uuid;
    m.catalog_generation=r.catalog_generation_id;
    if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameVector>){
      m.object_subtype="name_vector";m.record.header.object_uuid=r.name_vector_uuid;
      m.record.header.parent_uuid=r.object_uuid;m.default_name_uuid=r.default_name_entry_uuid;
      m.owning_schema_uuid=r.owning_schema_uuid.value_or(TypedUuid{});
    }else{
      m.object_subtype="name_entry";m.record.header.object_uuid=r.name_entry_uuid;
      m.record.header.parent_uuid=r.name_vector_uuid;m.default_name_uuid=r.name_entry_uuid;
      m.owning_schema_uuid=r.parent_schema_uuid.value_or(TypedUuid{});
      m.retired_transaction_uuid=r.dropped_transaction_uuid.value_or(TypedUuid{});
      m.creator_transaction_uuid=r.created_transaction_uuid;m.resource_epoch=r.resource_epoch;
    }
  },payload);
  return m;
}
void MetadataAdmission(bool entry){
  auto payload=Fixture(entry).payload;
  if(entry){
    auto& r=std::get<c::CatalogNameEntry>(payload);
    r.parent_schema_uuid=Id(UuidKind::schema,7);
    r.dropped_transaction_uuid=Id(UuidKind::transaction,29);
  }
  const auto metadata=NameMetadata(payload);
  const auto matches=[&](const auto& p,const auto& m,bool expected){
    Check(WithoutEnvelopeHeap([&]{return c::CatalogNamePayloadMatchesMetadata(p,m);})==expected,
        "owning name metadata admission without encoding or heap");
    Check(WithoutEnvelopeHeap([&]{return c::CatalogNamePayloadMatchesMetadata(
        c::BorrowCatalogNamePayload(p),c::BorrowCatalogMetadataVersion(m));})==expected,
        "borrowed name metadata admission exact parity");
  };
  matches(payload,metadata,true);
  auto m=metadata;m.record.header.kind=c::CatalogRecordKind::sql_object;matches(payload,m,false);
  m=metadata;m.object_subtype="unknown";matches(payload,m,false);
  for(auto member:{&c::CatalogMetadataVersion::name_vector_uuid,&c::CatalogMetadataVersion::default_name_uuid,
      &c::CatalogMetadataVersion::owning_schema_uuid,&c::CatalogMetadataVersion::security_policy_uuid}){
    m=metadata;(m.*member).value.bytes[15]^=0x80;matches(payload,m,false);
    m=metadata;(m.*member).kind=UuidKind::unknown;matches(payload,m,false);
  }
  for(bool parent:{false,true}){
    m=metadata;(parent?m.record.header.parent_uuid:m.record.header.object_uuid).value.bytes[15]^=0x80;
    matches(payload,m,false);
  }
  m=metadata;++m.catalog_generation;matches(payload,m,false);
  m=metadata;m.record.header.deleted=true;matches(payload,m,false);
  auto dropped=payload;
  std::visit([](auto& r){r.lifecycle_state=c::CatalogNameLifecycle::dropped;},dropped);
  matches(dropped,m,true);
  auto invalid=payload;std::visit([](auto& r){r.object_class=std::string(1,char(0x80));},invalid);
  matches(invalid,metadata,false);
  invalid=payload;std::visit([](auto& r){r.object_class.assign(131041,'x');},invalid);
  matches(invalid,metadata,false);
  invalid=payload;
  std::visit([](auto& r){
    r.object_class.assign(70000,'x');
    if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameVector>)
      r.default_language_tag.assign(70000,'y');
    else r.display_name.assign(70000,'y');
  },invalid);
  matches(invalid,metadata,false); // Individually valid fields exceed total block.
  auto absent=payload;
  std::visit([](auto& r){
    if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameVector>)r.owning_schema_uuid.reset();
    else {r.parent_schema_uuid.reset();r.dropped_transaction_uuid.reset();}
  },absent);
  matches(absent,NameMetadata(absent),true);
  auto dirty_absent=NameMetadata(absent);dirty_absent.owning_schema_uuid.kind=UuidKind::schema;
  matches(absent,dirty_absent,false);
  if(entry){
    m=metadata;++m.resource_epoch;matches(payload,m,false);
    m=metadata;m.retired_transaction_uuid.value.bytes[15]^=0x80;matches(payload,m,false);
    m=metadata;m.creator_transaction_uuid.value.bytes[15]^=0x80;matches(payload,m,false);
    m.definition_version=2;matches(payload,m,true); // Successor writer differs from origin.
  }
  const auto preserves=[&](const auto& before,const auto& after,bool expected){
    Check(WithoutEnvelopeHeap([&]{return c::CatalogNamePayloadPreservesIdentity(before,after);})==expected,
        "owning native name origin continuity without heap");
    Check(WithoutEnvelopeHeap([&]{return c::CatalogNamePayloadPreservesIdentity(
        c::BorrowCatalogNamePayload(before),c::BorrowCatalogNamePayload(after));})==expected,
        "borrowed native name origin continuity exact parity");
  };
  preserves(payload,payload,true);preserves(payload,Fixture(!entry).payload,false);
  for(unsigned change=0;change<5;++change){
    auto next=payload;
    std::visit([&](auto& r){
      switch(change){
        case 0:r.name_vector_uuid.value.bytes[15]^=0x80;break;
        case 1:r.object_uuid.value.bytes[15]^=0x80;break;
        case 2:r.object_class="other";break;
        case 3:
          if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameEntry>)r.created_transaction_uuid.value.bytes[15]^=0x80;
          else r.object_uuid.kind=UuidKind::unknown;
          break;
        case 4:
          if constexpr(std::is_same_v<std::decay_t<decltype(r)>,c::CatalogNameEntry>)--r.name_resolution_epoch;
          else r.name_vector_uuid.kind=UuidKind::unknown;
          break;
      }
    },next);
    preserves(payload,next,false);
  }
}
int main(){
  const auto literal=Golden();const auto binding=Fixture().binding;
  Check(WithoutEnvelopeHeap([&]{return c::DecodeCatalogNameEnvelopeView(literal,binding);}).ok(),
      "first-use literal envelope has no heap allocation");
  NameEnvelopeRegressionMain();
  for(bool entry:{false,true}){OwningEnvelopeFaults(entry);ExactEnvelopeBounds(entry);MetadataAdmission(entry);}
  std::cout<<"name envelope memory checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
