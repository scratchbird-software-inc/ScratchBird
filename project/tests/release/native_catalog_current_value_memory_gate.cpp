// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main descriptor_regression_main
#include "../sbsql_sblr_alignment/catalog_metric_descriptor_test.cpp"
#undef main

void* operator new(std::size_t n,std::align_val_t a){
  ++descriptor_allocation_fault::allocations;
  if(descriptor_allocation_fault::remaining==0){descriptor_allocation_fault::fired=true;throw std::bad_alloc();}
  if(descriptor_allocation_fault::remaining>0)--descriptor_allocation_fault::remaining;
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}

namespace {
std::string ScalarBytes(const m::MetricScalar& v){
  if(const auto* s=std::get_if<std::string>(&v))return Number(s->size(),4)+*s;
  if(const auto* id=std::get_if<m::MetricUuid>(&v))return Uuid(*id);
  if(const auto* flag=std::get_if<bool>(&v))return Number(*flag,1);
  if(const auto* code=std::get_if<m::MetricEnumValue>(&v))return Number(code->code);
  return Scalar(v);
}
// Independent registry bytes, not the production value encoder.
std::string ValueBytes(const m::MetricDescriptorDefinition& d,const m::MetricValue& v){
  std::string s(24,0);s.replace(0,4,"SBMV");Put(s,4,1,2);Put(s,6,24,2);
  const std::array classes{m::MetricType::counter,m::MetricType::gauge,m::MetricType::histogram,
      m::MetricType::rate,m::MetricType::state,m::MetricType::sample};
  Put(s,12,std::find(classes.begin(),classes.end(),d.type)-classes.begin()+1,1);
  Put(s,13,static_cast<unsigned>(d.value_type),1);Put(s,14,v.buckets_cumulative+2*v.arithmetic_inexact,1);
  Put(s,16,v.labels.size(),4);Put(s,20,v.bucket_bounds.size(),4);
  s+=ScalarBytes(v.value);s+=Number(v.count);
  if(d.type==m::MetricType::histogram){s+=ScalarBytes(v.sum);
    for(std::size_t i=0;i<v.bucket_bounds.size();++i){s+=ScalarBytes(v.bucket_bounds[i]);s+=Number(v.buckets[i]);}
    s+=Number(v.buckets.back());}
  s+=Number(v.state_text.size(),4)+v.state_text;
  auto labels=v.labels;std::sort(labels.begin(),labels.end(),[](const auto& a,const auto& b){
    return std::lexicographical_compare(a.key.begin(),a.key.end(),b.key.begin(),b.key.end(),
        [](unsigned char x,unsigned char y){return x<y;});});
  for(const auto& l:labels){const auto schema=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& x){return x.key==l.key;});
    s+=Number(l.key.size(),4)+l.key+Number(static_cast<unsigned>(schema->value_type)+1,1);
    if(const auto* text=std::get_if<std::string>(&l.value))s+=Number(text->size(),4)+*text;
    else s+=Uuid(std::get<m::MetricUuid>(l.value));}
  Put(s,8,s.size(),4);return s;
}
std::string CurrentBytes(const c::CatalogMetricCurrentValue& r){
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);Put(s,12,4,4);Put(s,16,65547,4);Put(s,20,1,2);
  Field(s,1,5,Uuid(r.object_uuid.value));Field(s,2,5,Uuid(r.database_uuid.value));
  Field(s,3,4,Golden(r.descriptor));Field(s,4,4,ValueBytes(r.descriptor.definition,r.value));Put(s,8,s.size(),4);return s;
}
c::CatalogMetricCurrentValue Fixture(){
  c::CatalogMetricCurrentValue r;r.object_uuid=Id(p::UuidKind::object,20);r.database_uuid=Id(p::UuidKind::database,21);
  r.descriptor=Descriptor();r.descriptor.definition.min_value.reset();r.descriptor.definition.max_value.reset();
  r.value.family=r.descriptor.definition.family;r.value.type=m::MetricType::gauge;r.value.value=p::u64(-1);
  r.value.labels={{"database",r.database_uuid.value},{"tag",std::string(80,'t')},{"data-id",Id(p::UuidKind::object,22).value}};
  return r;
}
auto Borrow(std::string_view bytes){return WithoutDescriptorHeap([&]{return c::DecodeCatalogMetricCurrentValueView(bytes);});}
bool Inside(std::string_view part,std::string_view bytes){
  return part.data()>=bytes.data()&&part.data()+part.size()<=bytes.data()+bytes.size();
}
void Compare(std::string_view raw){
  const auto view=Borrow(raw);const auto owned=c::DecodeCatalogMetricCurrentValue(raw);
  Check(view.error==owned.error&&view.ok()==owned.ok(),"current-value exact owning error parity");
  if(!view.ok()){Check(!view.record,"no partially validated catalog output");return;}
  WithoutDescriptorHeap([&]{const auto copy=*view.record;
    Check(copy.object_uuid.value==owned.record->object_uuid.value&&copy.database_uuid.value==owned.record->database_uuid.value,
        "native catalog identities preserved");
    Check(Inside(copy.value.family,raw)&&Inside(copy.value.state_text,raw),"value borrows outer input not local descriptor");
    if(copy.value.value.text)Check(Inside(*copy.value.value.text,raw),"scalar text aliases input");
    for(const auto label:copy.value.labels){Check(Inside(label.key,raw),"label key aliases input");
      if(const auto* text=std::get_if<std::string_view>(&label.value))Check(Inside(*text,raw),"label text aliases input");}
    if(copy.value.labels.size()>1){auto a=copy.value.labels.begin(),b=a;
      Check(a++==b&&(*b).key!=(*a).key,"independent copied label iterators");}return true;});
  Check(CurrentBytes(*owned.record)==raw,"owning exact canonical recovery");
  c::CatalogTypedRecord record;record.header.kind=c::CatalogRecordKind::metric_current_value;
  record.header.object_uuid=owned.record->object_uuid;record.payload=raw;
  Check(WithoutDescriptorHeap([&]{return c::CatalogMetricCurrentValueMatchesHeader(record);}),"header validation without heap");
}
void Matrix(){
  const std::array classes{m::MetricType::counter,m::MetricType::gauge,m::MetricType::histogram,
      m::MetricType::rate,m::MetricType::state,m::MetricType::sample};
  for(auto klass:classes)for(unsigned type=1;type<=9;++type)for(bool cumulative:{false,true}){
    const bool valid=klass==m::MetricType::state?type==9:
        (klass==m::MetricType::counter||klass==m::MetricType::histogram||klass==m::MetricType::rate)?type<=5:true;
    auto r=Fixture();auto& d=r.descriptor.definition;auto& v=r.value;d.type=v.type=klass;d.value_type=static_cast<m::MetricScalarType>(type);
    switch(type){
      case 1:v.value=p::u64(-1);break;case 2:v.value=std::int64_t(1);break;case 3:v.value=-0.;break;
      case 4:{m::MetricFloat128 n;n.bytes[14]=0xff;n.bytes[15]=0x3f;v.value=n;break;}
      case 5:{m::MetricDecimal128 n;n.bytes[0]=1;v.value=n;break;}
      case 6:v.value=true;break;case 7:v.value=std::string(80,'v');break;
      case 8:{auto id=Id(p::UuidKind::object,23).value;id.bytes[6]=0x41;v.value=id;break;}
      case 9:v.value=m::MetricEnumValue{p::u64(-1)};d.enum_values={p::u64(-1)};break;
    }
    if(klass==m::MetricType::histogram){d.histogram_buckets={v.value};d.histogram_cumulative=v.buckets_cumulative=cumulative;
      v.bucket_bounds=d.histogram_buckets;v.count=1;v.sum=v.value;v.buckets=cumulative?std::vector<p::u64>{1,1}:std::vector<p::u64>{1,0};
      if(type>5)continue; // Descriptor oracle only packs normative numeric bounds.
    }
    if(klass==m::MetricType::rate){d.rate_window_nanoseconds=1;r.descriptor.binding.rate_source_counter_uuid=Id(p::UuidKind::object,24).value;
      r.descriptor.binding.rate_source_counter_generation=1;}
    if(klass==m::MetricType::state)v.state_text=std::string(90,'s');
    auto raw=CurrentBytes(r);Check(Borrow(raw).ok()==valid,"independent class/type matrix");Compare(raw);
  }
}
void MalformedAndFaults(){
  auto r=Fixture();const auto raw=CurrentBytes(r);
  for(std::size_t n=0;n<raw.size();++n)Compare(std::string_view(raw).substr(0,n));
  const auto value=ValueBytes(r.descriptor.definition,r.value);
  for(std::size_t n=0;n<value.size();++n){auto short_value=value.substr(0,n);
    if(n>=24)Put(short_value,8,n,4);const auto bad=Replace(raw,4,short_value);Check(!Borrow(bad).ok(),"embedded truncation rejected");Compare(bad);}
  for(std::size_t at=0;at<value.size();++at){auto changed=value;changed[at]^=0xff;Compare(Replace(raw,4,changed));}
  for(unsigned version=1;version<=8;++version){auto user=Id(p::UuidKind::object,22).value;user.bytes[6]=version<<4;
    r.value.labels[2].value=user;Check(Borrow(CurrentBytes(r)).ok()==(version<=7),"user UUID versions remain data");}
  r=Fixture();r.descriptor.definition.value_type=m::MetricScalarType::text;r.value.value=std::string(1024,'v');
  const auto text=CurrentBytes(r);const auto before=descriptor_allocation_fault::allocations;
  Check(c::DecodeCatalogMetricCurrentValue(text).ok(),"measure owning decoder allocations");
  const auto allocations=descriptor_allocation_fault::allocations-before;
  Check(allocations>0,"owning fault sweep nonvacuous");
  for(std::size_t point=0;point<allocations;++point){descriptor_allocation_fault::remaining=point;descriptor_allocation_fault::fired=false;
    bool success=false;try{success=c::DecodeCatalogMetricCurrentValue(text).ok();}catch(const std::bad_alloc&){}
    const bool fired=descriptor_allocation_fault::fired;descriptor_allocation_fault::remaining=-1;
    Check(fired&&!success,"every actual owning allocation fails atomically");}
  descriptor_allocation_fault::remaining=allocations;descriptor_allocation_fault::fired=false;
  auto owned=c::DecodeCatalogMetricCurrentValue(text);descriptor_allocation_fault::remaining=-1;
  Check(owned.ok()&&!descriptor_allocation_fault::fired&&CurrentBytes(*owned.record)==text,"exact allocation boundary recovery");
  auto disposable=text;auto independent=c::DecodeCatalogMetricCurrentValue(disposable);std::fill(disposable.begin(),disposable.end(),0);
  Check(independent.ok()&&CurrentBytes(*independent.record)==text,"owning lifetime independent of input");
  std::cout<<"current-value owning allocation sites="<<allocations<<'\n';
}
void SemanticBoundaries(){
  auto r=Fixture();auto& d=r.descriptor.definition;auto& v=r.value;
  auto expect=[&](bool valid,const char* why){const auto raw=CurrentBytes(r);Check(Borrow(raw).ok()==valid,why);Compare(raw);};
  d.min_value=p::u64(10);d.max_value=p::u64(20);
  for(p::u64 n:{9u,10u,20u,21u}){v.value=n;expect(n>=10&&n<=20,"exact scalar bounds");}
  d.min_value.reset();d.max_value.reset();d.unit=m::MetricUnit::percent;
  for(p::u64 n:{0u,100u,101u}){v.value=n;expect(n<=100,"percent bounds");}
  r=Fixture();d.type=v.type=m::MetricType::counter;d.value_type=m::MetricScalarType::int64;
  for(std::int64_t n:{-1,0,1}){v.value=n;expect(n>=0,"counter negative refusal");}
  r=Fixture();d.type=v.type=m::MetricType::state;d.value_type=m::MetricScalarType::enumeration;d.enum_values={0,2};
  for(p::u64 n:{0u,1u,2u}){v.value=m::MetricEnumValue{n};expect(n!=1,"state enumeration membership");}
  v.state_text=std::string("\xc0\x80",2);expect(false,"invalid UTF8 state text");
  r=Fixture();d.type=v.type=m::MetricType::histogram;d.histogram_buckets={p::u64(1),p::u64(2)};
  v.value=p::u64(2);v.sum=p::u64(2);v.count=3;v.bucket_bounds=d.histogram_buckets;
  for(bool cumulative:{false,true}){d.histogram_cumulative=v.buckets_cumulative=cumulative;
    v.buckets=cumulative?std::vector<p::u64>{1,2,3}:std::vector<p::u64>{1,1,1};expect(true,"complete histogram modes");
    const auto saved=v.buckets;v.buckets[0]=4;expect(false,"bucket count exceeds count");v.buckets=saved;
    v.buckets.back()=0;expect(false,"terminal histogram count or sum");v.buckets=saved;
    v.bucket_bounds[0]=p::u64(0);expect(false,"exact descriptor bucket binding");v.bucket_bounds=d.histogram_buckets;
    v.buckets_cumulative=!cumulative;expect(false,"cumulative mode binding");v.buckets_cumulative=cumulative;
  }
  d.histogram_cumulative=v.buckets_cumulative=false;v.count=p::u64(-1);v.buckets={p::u64(-1),1,0};
  expect(false,"histogram count summation overflow");
  r=Fixture();v.labels.erase(v.labels.begin());expect(false,"required label missing");
  r=Fixture();v.labels[1].value=std::string();expect(false,"empty label text");
  r=Fixture();v.labels[0].value=Id(p::UuidKind::object,21).value;
  std::get<m::MetricUuid>(v.labels[0].value).bytes[6]=0x40;expect(false,"system label rejects older UUID");
  r=Fixture();d.labels[0].key=v.labels[0].key="database_uuid";d.labels[0].value_type=m::MetricLabelType::uuid_value;
  expect(false,"reserved owner label requires system UUID type");
  r=Fixture();d.value_type=m::MetricScalarType::text;v.value=std::string(1,'v');
  const auto overhead=ValueBytes(d,v).size()-1;v.value=std::string(65536-overhead,'v');
  expect(true,"embedded value exact 65536 byte cap");std::get<std::string>(v.value).push_back('v');
  expect(false,"embedded value over cap");
  r=Fixture();d.labels.clear();v.labels.clear();r.descriptor.binding.label_schema_uuid={};r.descriptor.binding.label_schema_generation=0;
  expect(true,"empty label range");
  m::MetricValueLabelsView empty;Check(empty.empty()&&empty.begin()==empty.end(),"default labels safe empty range");
  const auto raw=CurrentBytes(Fixture());
  auto check_error=[&](std::string input,c::CatalogValueError error){const auto result=Borrow(input);
    Check(result.error==error&&!result.record,"independent structural error vector");};
  auto bad=raw;Put(bad,4,2,2);check_error(bad,c::CatalogValueError::unsupported_version);
  bad=raw;Put(bad,6,25,2);check_error(bad,c::CatalogValueError::invalid_framing);
  bad=raw;Put(bad,Offset(bad,1)+8+6,0x40,1);check_error(bad,c::CatalogValueError::invalid_value);
  bad=raw;Put(bad,Offset(bad,3)+8+4,2,2);check_error(bad,c::CatalogValueError::unsupported_version);
  bad=raw;Put(bad,Offset(bad,4)+8+4,2,2);check_error(bad,c::CatalogValueError::invalid_value);
}
}
int main(){
  const auto initial=CurrentBytes(Fixture());Check(Borrow(initial).ok(),"first-use decode without heap");
  Matrix();MalformedAndFaults();SemanticBoundaries();descriptor_regression_main();
  std::cout<<"current-value memory checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
