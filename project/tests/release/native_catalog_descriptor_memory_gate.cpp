// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse the independent Core byte oracle, not any production packing helper.
#define main descriptor_existing_test_main
#include "../sbsql_sblr_alignment/catalog_metric_descriptor_test.cpp"
#undef main

void* operator new(std::size_t n,std::align_val_t alignment) {
  ++descriptor_allocation_fault::allocations;
  if(descriptor_allocation_fault::remaining==0){descriptor_allocation_fault::fired=true;throw std::bad_alloc();}
  if(descriptor_allocation_fault::remaining>0)--descriptor_allocation_fault::remaining;
  void* p=nullptr;
  if(posix_memalign(&p,static_cast<std::size_t>(alignment),n?n:1)==0)return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}

namespace {
auto Borrow(std::string_view bytes){return WithoutDescriptorHeap([&]{return c::DecodeCatalogMetricDescriptorView(bytes);});}
bool Inside(std::string_view part,std::string_view whole){
  return part.data()>=whole.data()&&part.data()+part.size()<=whole.data()+whole.size();
}
std::pair<m::MetricScalar,m::MetricScalar> NumericPair(unsigned type){
  switch(type){
    case 1:return {p::u64(0),p::u64(-1)};
    case 2:return {std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()};
    case 3:return {-0.,std::numeric_limits<double>::max()};
    case 4:{m::MetricFloat128 a{},b{};b.bytes[14]=0xff;b.bytes[15]=0x3f;return {a,b};}
    default:{m::MetricDecimal128 a{},b{};b.bytes[0]=1;return {a,b};}
  }
}
void Matrix(){
  const std::array classes{m::MetricType::counter,m::MetricType::gauge,m::MetricType::histogram,
      m::MetricType::rate,m::MetricType::state,m::MetricType::sample};
  for(unsigned klass=0;klass<6;++klass)for(unsigned type=1;type<=9;++type)
    for(unsigned unit=0;unit<16;++unit)for(unsigned visibility=0;visibility<5;++visibility)
      for(bool cluster:{false,true}){
    auto r=Descriptor();auto& d=r.definition;
    d.type=classes[klass];d.value_type=static_cast<m::MetricScalarType>(type);
    d.unit=static_cast<m::MetricUnit>(unit);d.visibility=static_cast<m::MetricVisibilityScope>(visibility);
    d.cluster_only=cluster;d.namespace_path=cluster?"cluster.sys.metrics.x":"sys.metrics.x";
    d.min_value.reset();d.max_value.reset();
    if(type==9)d.enum_values={0,p::u64(-1)};
    if(klass==2&&type<=5){const auto [a,b]=NumericPair(type);d.histogram_buckets={a,b};}
    if(klass==3){r.binding.rate_source_counter_uuid=Id(p::UuidKind::object,12).value;
      r.binding.rate_source_counter_generation=1;d.rate_window_nanoseconds=1;}
    const bool valid=(type<=5||unit!=9)&&
        ((klass==0||klass==2||klass==3)?type<=5:klass==4?type==9:true);
    const auto bytes=Golden(r);const auto got=Borrow(bytes);
    Check(got.ok()==valid,"independent class/type/unit/visibility/scope admission matrix");
    if(!valid){Check(!got.record,"invalid matrix exposed partial view");continue;}
    const auto& v=*got.record;
    Check(v.binding==r.binding&&v.origin_transaction_uuid.value==r.origin_transaction_uuid.value&&
        v.origin_local_transaction_id==r.origin_local_transaction_id,"native bindings preserved");
    Check(v.definition.type==d.type&&v.definition.value_type==d.value_type&&v.definition.unit==d.unit&&
        v.definition.visibility==d.visibility&&v.definition.cluster_only==cluster,"descriptor enums preserved");
    WithoutDescriptorHeap([&]{
      Check(Inside(v.definition.family,bytes)&&Inside(v.definition.namespace_path,bytes)&&
          Inside(v.definition.producer_owner,bytes)&&Inside(v.definition.help,bytes)&&
          Inside(v.definition.security_family,bytes),"annotation bytes borrow source");
      std::size_t i=0;
      for(const auto label:v.definition.labels){const auto& expected=d.labels[i++];
        Check(Inside(label.key,bytes)&&label.key==expected.key&&label.required==expected.required&&
          label.sensitive==expected.sensitive&&label.value_type==expected.value_type,"complete borrowed label");}
      i=0;for(const auto alias:v.definition.aliases)Check(Inside(alias,bytes)&&alias==d.aliases[i++],"borrowed alias");
      for(i=0;i<d.histogram_buckets.size();++i)
        Check(v.definition.histogram_buckets[i]==d.histogram_buckets[i],"native numeric bucket bits");
      for(i=0;i<d.enum_values.size();++i)Check(v.definition.enum_values[i]==d.enum_values[i],"native enumeration bits");
      const auto copy=v;auto a=copy.definition.aliases.begin(),b=a;
      Check(a++==b&&*b==d.aliases[0]&&*a==d.aliases[1],"copied multipass input-backed aliases");
      return true;
    });
    const auto meta=Metadata(r);
    Check(WithoutDescriptorHeap([&]{return c::CatalogMetricDescriptorMatchesMetadata(meta)&&
        c::CatalogMetadataPreservesFamilyOrigin(meta,meta);}),"metadata and shared origin without heap");
  }
}
void SequenceBounds(){
  for(unsigned count:{0u,1u,2u,17u,1024u,1025u}){
    auto r=Descriptor();r.definition.labels.clear();r.definition.aliases.clear();
    for(unsigned i=0;i<count;++i){const auto key=std::to_string(i);
      r.definition.labels.push_back({key,bool(i&1),bool(i&2),static_cast<m::MetricLabelType>(i%3)});
      r.definition.aliases.push_back(key);}
    const auto raw=Golden(r),before=raw;const auto result=Borrow(raw);
    Check(result.ok()==(count<=1024)&&raw==before,"label alias sequence count bounds and immutable input");
    if(result.ok())Check(result.record->definition.labels.size()==count&&
        result.record->definition.aliases.size()==count,"exact sequence counts");
  }
  for(unsigned type=1;type<=5;++type)for(unsigned count:{0u,1u,2u}){
    auto r=Descriptor();r.definition.type=m::MetricType::histogram;
    r.definition.value_type=static_cast<m::MetricScalarType>(type);
    const auto [a,b]=NumericPair(type);r.definition.min_value=a;r.definition.max_value=b;
    if(count)r.definition.histogram_buckets.push_back(a);
    if(count==2)r.definition.histogram_buckets.push_back(b);
    for(bool cumulative:{false,true}){r.definition.histogram_cumulative=cumulative;
      Check(Borrow(Golden(r)).ok()==bool(count),"numeric widths exact bounds cumulative modes");}
  }
  auto r=Descriptor();r.definition.type=m::MetricType::histogram;
  for(p::u64 i=0;i<4096;++i)r.definition.histogram_buckets.emplace_back(i);
  auto raw=Golden(r);auto got=Borrow(raw);
  Check(got.ok()&&got.record->definition.histogram_buckets.size()==4096,"maximum borrowed histogram");
  r.definition.histogram_buckets.emplace_back(p::u64(4096));Refused(Golden(r));
  r=Descriptor();r.definition.type=m::MetricType::state;r.definition.value_type=m::MetricScalarType::enumeration;
  r.definition.min_value.reset();r.definition.max_value.reset();
  for(p::u64 i=0;i<4096;++i)r.definition.enum_values.push_back(i);
  raw=Golden(r);got=Borrow(raw);Check(got.ok()&&got.record->definition.enum_values[4095]==4095,"maximum borrowed enum");
  r.definition.enum_values.push_back(4096);Refused(Golden(r));
  raw=Golden(Descriptor());const auto owned=c::DecodeCatalogMetricDescriptor(raw);
  std::fill(raw.begin(),raw.end(),0);Check(owned.ok()&&Golden(*owned.record)==Golden(Descriptor()),"owning output independent of input lifetime");
  c::CatalogMetricAliasSequenceView empty;
  Check(empty.empty()&&empty.begin()==empty.end(),"default alias range safe");
}
}
int main(){
  // Before any owning schema initialization: detect hidden first-use allocation.
  const auto first=Golden(Descriptor());Check(Borrow(first).ok(),"first-use descriptor without heap");
  Matrix();SequenceBounds();Shapes();Malformed();Binding();Limits();
  std::cout<<"descriptor borrowed checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
