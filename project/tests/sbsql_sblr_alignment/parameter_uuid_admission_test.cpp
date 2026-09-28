// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_parameter_bind_runtime.hpp"
#include <cstdlib>
#include <iostream>
#include <initializer_list>

namespace s = scratchbird::engine::sblr;
using Id = std::array<std::uint8_t,16>;
using Bytes = std::vector<std::uint8_t>;
static unsigned checks = 0;
void Require(bool ok, const char* reason) {
  ++checks;
  if (!ok) { std::cerr << reason << " check=" << checks << '\n'; std::exit(1); }
}
Id Identity(unsigned suffix) {
  return {1,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,
          static_cast<std::uint8_t>(suffix)};
}
template<class T, class Encode>
void Required(const T& source, std::initializer_list<Id T::*> fields, Encode encode) {
  Require(!encode(source).empty(), "positive identity fixture refused");
  for (auto field : fields) {
    for (unsigned offset : {6u,8u}) for (unsigned byte=0;byte<256;++byte) {
      auto value=source; (value.*field)[offset]=byte;
      const bool valid=offset==6 ? byte>>4==7 : byte>>6==2;
      Require(encode(value).empty()!=valid, "required identity version/variant admission");
    }
    auto value=source; (value.*field).fill(0);
    Require(encode(value).empty(), "required nil identity admitted");
  }
}
template<class T, class Encode>
void Optional(T source, Id T::*field, std::uint64_t T::*generation, Encode encode) {
  source.*field={}; source.*generation=0;
  Require(!encode(source).empty(), "absent optional identity refused");
  source.*generation=1;
  Require(encode(source).empty(), "nil identity with generation admitted");
  source.*field=Identity(90);
  Required(source,{field},encode);
  source.*generation=0;
  Require(encode(source).empty(), "present identity without generation admitted");
  for (unsigned version=0;version<16;++version) {
    (source.*field)[6]=version<<4;
    Require(encode(source).empty(), "malformed optional identity mistaken for absence");
  }
  source.*field=Identity(90);
  for (unsigned variant=0;variant<4;++variant) {
    (source.*field)[8]=variant<<6;
    Require(encode(source).empty(), "malformed optional variant mistaken for absence");
  }
}
template<class Decode>
void Wire(const Bytes& canonical, std::initializer_list<unsigned> fields, Decode decode) {
  Require(!canonical.empty() && decode(canonical), "positive wire fixture refused");
  for(auto field:fields) {
    Require(field+16<=canonical.size(), "wire field outside fixture");
    for(unsigned version=0;version<16;++version) if(version!=7) {
      auto bytes=canonical; bytes[field+6]=version<<4;
      Require(!decode(bytes), "wire admitted non-v7 system identity");
    }
    for(unsigned variant=0;variant<4;++variant) if(variant!=2) {
      auto bytes=canonical; bytes[field+8]=variant<<6;
      Require(!decode(bytes), "wire admitted non-RFC system identity");
    }
    auto bytes=canonical;std::fill_n(bytes.begin()+field,16,0);
    Require(!decode(bytes), "wire admitted nil system identity");
  }
}
template<class T, class Encode, class Decode>
void Frame(T fixture, std::initializer_list<unsigned> fields, Encode encode, Decode decode) {
  const auto canonical=encode(fixture);
  Wire(canonical,fields,[&](const Bytes& bytes) {
    auto output=fixture;std::string detail;
    const bool ok=decode(bytes.data(),bytes.size(),&output,&detail);
    Require(encode(output)==(ok?bytes:canonical), "decoder modified refused output or canonical data");
    return ok;
  });
}
int main() {
  using N=s::SblrParameterNodeV1;
  N node;node.node_id=1;node.parent_operand_ordinal=1;
  node.parameter_set_descriptor_uuid=Identity(1);node.parameter_set_generation=1;
  node.datatype_descriptor_uuid=Identity(2);node.datatype_descriptor_generation=1;
  auto enc_node=[](N n){return s::EncodeSblrParameterNodeTableV1({{n}});};
  Required(node,{&N::parameter_set_descriptor_uuid,&N::datatype_descriptor_uuid},enc_node);
  const auto sbpn=enc_node(node);
  Wire(sbpn,{52,76},[](const Bytes& b){return s::DecodeSblrParameterNodeTableV1(b.data(),b.size()).ok;});
  const std::string domain="ScratchBird.SblrParameterNodeTable.V1";
  Bytes material(domain.begin(),domain.end());material.insert(material.end(),sbpn.begin(),sbpn.end());
  const auto table_hash=scratchbird::core::hash::ComputeSha256Digest(material).digest;
  using R=s::SblrParameterNodeReferenceV1;
  R ref;ref.occurrence_ordinal=1;ref.node_id=1;ref.table_sha256=table_hash;
  ref.parameter_set_descriptor_uuid=node.parameter_set_descriptor_uuid;ref.parameter_set_generation=1;
  Required(ref,{&R::parameter_set_descriptor_uuid},s::EncodeSblrParameterNodeReferenceV1);
  Frame(ref,{48},s::EncodeSblrParameterNodeReferenceV1,
    [](const auto*b,auto n,auto*out,auto*){return s::DecodeSblrParameterNodeReferenceV1(b,n,out);});
  using V=s::SblrParameterValueSetV1;
  using Record=s::SblrParameterValueRecordV1;
  V values;values.parameter_set_descriptor_uuid=Identity(1);values.descriptor_generation=1;
  values.execution_uuid=Identity(3);values.statement_receipt_uuid=Identity(4);
  Record record;record.slot_uuid=Identity(5);record.datatype_descriptor_uuid=Identity(2);
  record.datatype_descriptor_generation=1;record.direction=s::SblrParameterDirectionV1::in;
  record.state=s::SblrParameterValueStateV1::value;record.canonical_value_bytes={1,0,0,0,0,0,0,0};
  values.records.push_back(record);
  Required(values,{&V::parameter_set_descriptor_uuid,&V::execution_uuid,&V::statement_receipt_uuid},s::EncodeSblrParameterValueSetV1);
  Required(record,{&Record::slot_uuid,&Record::datatype_descriptor_uuid},[&](Record r){auto v=values;v.records={r};return s::EncodeSblrParameterValueSetV1(v);});
  Wire(s::EncodeSblrParameterValueSetV1(values),{16,40,56,120,136},[](const Bytes& b){return s::DecodeSblrParameterValueSetV1(b.data(),b.size()).ok;});
  for(unsigned version=0;version<16;++version) for(unsigned variant=0;variant<4;++variant) {
    auto v=values;auto data=Identity(30);data[6]=version<<4;data[8]=variant<<6;
    v.records[0].canonical_value_bytes.assign(data.begin(),data.end());
    auto b=s::EncodeSblrParameterValueSetV1(v);auto d=s::DecodeSblrParameterValueSetV1(b.data(),b.size());
    Require(d.ok&&d.value.records[0].canonical_value_bytes==v.records[0].canonical_value_bytes,"user UUID data restricted by system identity policy");
  }
  for(auto byte:{0u,255u}) {
    auto v=values;v.records[0].canonical_value_bytes.assign(16,byte);
    auto b=s::EncodeSblrParameterValueSetV1(v);auto d=s::DecodeSblrParameterValueSetV1(b.data(),b.size());
    Require(d.ok&&d.value.records[0].canonical_value_bytes==v.records[0].canonical_value_bytes,"opaque UUID user data changed");
  }
  using Q=s::SblrParameterNegotiateRequestV1;
  Q q;q.preliminary_receipt_uuid=Identity(6);q.mga_snapshot_uuid=Identity(7);q.catalog_generation=1;
  q.demands={{1,1,1,1,0}};q.demand_sha256=s::ComputeSblrParameterDemandSha256V1(q.demands);
  Required(q,{&Q::preliminary_receipt_uuid,&Q::mga_snapshot_uuid},s::EncodeSblrParameterNegotiateRequestV1);
  Frame(q,{16,56},s::EncodeSblrParameterNegotiateRequestV1,s::DecodeSblrParameterNegotiateRequestV1);
  auto invalid_demand=q;invalid_demand.demands[0].occurrence_id=0;
  invalid_demand.demand_sha256=s::ComputeSblrParameterDemandSha256V1(invalid_demand.demands);
  Require(s::EncodeSblrParameterNegotiateRequestV1(invalid_demand).empty(),"failed demand hash accepted as authentic digest");
  Wire(s::EncodeSblrParameterNegotiateRequestV1(q),{16,56},[](const Bytes& b){return s::PrevalidateSblrParameterNegotiateRequestV1(b.data(),b.size())==s::SblrParameterWirePrevalidationV1::ok;});
  using G=s::SblrParameterNegotiateResultV1;
  using M=s::SblrParameterMappingV1;
  G g;g.preliminary_receipt_uuid=q.preliminary_receipt_uuid;g.parameter_set_descriptor_uuid=Identity(1);
  g.descriptor_generation=1;g.execution_uuid=Identity(3);
  M mapping;mapping.occurrence_id=1;mapping.slot_uuid=Identity(5);
  mapping.datatype_descriptor_uuid=Identity(2);mapping.datatype_type_uuid=Identity(8);
  mapping.datatype_descriptor_generation=1;mapping.direction=1;
  g.mappings={mapping};g.mapping_sha256=s::ComputeSblrParameterMappingSha256V1(g.mappings);
  Required(g,{&G::preliminary_receipt_uuid,&G::parameter_set_descriptor_uuid,&G::execution_uuid},s::EncodeSblrParameterNegotiateResultV1);
  Required(mapping,{&M::slot_uuid,&M::datatype_descriptor_uuid,&M::datatype_type_uuid},[&](M m){auto v=g;v.mappings={m};v.mapping_sha256=s::ComputeSblrParameterMappingSha256V1(v.mappings);return s::EncodeSblrParameterNegotiateResultV1(v);});
  Frame(g,{16,40,64,124,140,156},s::EncodeSblrParameterNegotiateResultV1,s::DecodeSblrParameterNegotiateResultV1);
  auto invalid_mapping=g;invalid_mapping.mappings[0].slot_uuid={};
  invalid_mapping.mapping_sha256=s::ComputeSblrParameterMappingSha256V1(invalid_mapping.mappings);
  Require(s::EncodeSblrParameterNegotiateResultV1(invalid_mapping).empty(),"failed mapping hash accepted as authentic digest");
  invalid_mapping.mappings.clear();invalid_mapping.mapping_sha256={};
  Require(s::EncodeSblrParameterNegotiateResultV1(invalid_mapping).empty(),"empty descriptor mappings admitted");
  Wire(s::EncodeSblrParameterNegotiateResultV1(g),{16,40,64,124,140,156},[](const Bytes& b){return s::PrevalidateSblrParameterNegotiateResultV1(b.data(),b.size())==s::SblrParameterWirePrevalidationV1::ok;});
  using F=s::SblrParameterFinalizeRequestV1;
  F f;f.preliminary_receipt_uuid=q.preliminary_receipt_uuid;f.parameter_set_descriptor_uuid=Identity(1);
  f.descriptor_generation=1;f.execution_uuid=Identity(3);f.canonical_sbpn=sbpn;
  f.demand_sha256=q.demand_sha256;f.mapping_sha256=g.mapping_sha256;f.sbpn_sha256=table_hash;
  Required(f,{&F::preliminary_receipt_uuid,&F::parameter_set_descriptor_uuid,&F::execution_uuid},s::EncodeSblrParameterFinalizeRequestV1);
  Frame(f,{16,32,56,228,252},s::EncodeSblrParameterFinalizeRequestV1,s::DecodeSblrParameterFinalizeRequestV1);
  Wire(s::EncodeSblrParameterFinalizeRequestV1(f),{16,32,56,228,252},[](const Bytes& b){return s::PrevalidateSblrParameterFinalizeRequestV1(b.data(),b.size())==s::SblrParameterWirePrevalidationV1::ok;});
  using A=s::SblrParameterAdmissionV1;
  A a;a.final_receipt_uuid=Identity(4);a.admission_token_uuid=Identity(9);
  a.parameter_set_descriptor_uuid=Identity(1);a.descriptor_generation=1;a.execution_uuid=Identity(3);
  auto enc_a=[](A v){return s::EncodeSblrParameterAdmissionV1(&v);};
  Required(a,{&A::final_receipt_uuid,&A::admission_token_uuid,&A::parameter_set_descriptor_uuid,&A::execution_uuid},enc_a);
  Optional(a,&A::prepared_uuid,&A::prepared_generation,enc_a);
  Optional(a,&A::batch_uuid,&A::batch_generation,enc_a);
  Optional(a,&A::dynamic_uuid,&A::dynamic_generation,enc_a);
  Frame(a,{16,32,48,72},enc_a,s::DecodeSblrParameterAdmissionV1);
  auto prepared=a;prepared.prepared_uuid=Identity(10);prepared.prepared_generation=1;
  prepared.batch_uuid=Identity(11);prepared.batch_generation=1;
  Frame(prepared,{16,32,48,72,88,112},enc_a,s::DecodeSblrParameterAdmissionV1);
  auto dynamic=a;dynamic.dynamic_uuid=Identity(12);dynamic.dynamic_generation=1;
  Frame(dynamic,{16,32,48,72,136},enc_a,s::DecodeSblrParameterAdmissionV1);
  prepared.dynamic_uuid=dynamic.dynamic_uuid;prepared.dynamic_generation=1;
  Require(enc_a(prepared).empty(),"prepared and dynamic authority simultaneously admitted");
  using T=s::SblrPreparedParameterTemplateV1;
  T t;t.public_coordination_uuid=Identity(13);t.operation_uuid=Identity(14);
  t.provisional_prepared_uuid=Identity(10);t.provisional_prepared_generation=1;
  t.parameter_set_descriptor_uuid=Identity(1);t.descriptor_generation=1;
  t.mga_snapshot_uuid=Identity(7);t.catalog_generation=1;t.executor_availability_generation=1;
  t.canonical_schema4015={1,2,3,4};t.canonical_sbpa=enc_a(a);t.sbpn_sha256=table_hash;
  auto enc_t=[](T v){return s::EncodeSblrPreparedParameterTemplateV1(&v);};
  Required(t,{&T::public_coordination_uuid,&T::operation_uuid,&T::provisional_prepared_uuid,&T::parameter_set_descriptor_uuid,&T::mga_snapshot_uuid},enc_t);
  Wire(enc_t(t),{16,32,48,72,232},[](const Bytes& b){return s::DecodeSblrPreparedParameterTemplateV1(b.data(),b.size()).ok;});
  auto malformed_template=t;malformed_template.canonical_sbpa[16+6]=0x40;
  Require(enc_t(malformed_template).empty(),"prepared template accepted malformed nested admission identity");
  using BQ=s::SblrParameterBindRequestV1;
  BQ bq;bq.statement_receipt_uuid=Identity(4);bq.occurrence=1;
  bq.catalog_generation=1;bq.security_epoch=1;bq.resource_epoch=1;
  Required(bq,{&BQ::statement_receipt_uuid},s::EncodeSblrParameterBindRequestV1);
  Frame(bq,{16},s::EncodeSblrParameterBindRequestV1,s::DecodeSblrParameterBindRequestV1);
  using BD=s::SblrParameterBindDescriptorV1;
  BD bd;bd.execution_uuid=Identity(3);bd.statement_receipt_uuid=Identity(4);
  bd.prepared_statement_uuid=Identity(10);bd.prepared_generation=1;
  bd.parameter_set_uuid=Identity(1);bd.parameter_set_generation=1;
  bd.ordered_slot_table_sha256=table_hash;bd.catalog_snapshot_uuid=Identity(15);
  bd.catalog_generation=1;bd.security_epoch=1;bd.resource_epoch=1;bd.mga_snapshot_uuid=Identity(7);
  bd.executor_availability_generation=1;bd.canonical_value_vector=s::EncodeSblrParameterValueSetV1(values);
  auto enc_bd=[&](BD v){auto payload=values;payload.execution_uuid=v.execution_uuid;
    payload.statement_receipt_uuid=v.statement_receipt_uuid;payload.parameter_set_descriptor_uuid=v.parameter_set_uuid;
    v.canonical_value_vector=s::EncodeSblrParameterValueSetV1(payload);return s::EncodeSblrParameterBindDescriptorV1(v);};
  Required(bd,{&BD::execution_uuid,&BD::statement_receipt_uuid,&BD::prepared_statement_uuid,&BD::parameter_set_uuid,&BD::catalog_snapshot_uuid,&BD::mga_snapshot_uuid},enc_bd);
  Optional(bd,&BD::batch_uuid,&BD::batch_generation,enc_bd);
  Optional(bd,&BD::dynamic_package_uuid,&BD::dynamic_generation,enc_bd);
  Frame(bd,{16,32,48,72,176,216},s::EncodeSblrParameterBindDescriptorV1,s::DecodeSblrParameterBindDescriptorV1);
  using BR=s::SblrParameterBindResultV1;
  BR br;br.execution_uuid=Identity(3);br.prepared_statement_uuid=Identity(10);br.prepared_generation=1;
  br.parameter_set_uuid=Identity(1);br.parameter_set_generation=1;br.ordered_slot_table_sha256=table_hash;
  br.status=1;br.publication_barrier=1;br.bind_evidence_uuid=Identity(16);
  Required(br,{&BR::execution_uuid,&BR::prepared_statement_uuid,&BR::parameter_set_uuid,&BR::bind_evidence_uuid},s::EncodeSblrParameterBindResultV1);
  Optional(br,&BR::batch_uuid,&BR::batch_generation,s::EncodeSblrParameterBindResultV1);
  Frame(br,{16,32,56,140},s::EncodeSblrParameterBindResultV1,s::DecodeSblrParameterBindResultV1);
  std::cout<<"parameter UUID admission checks="<<checks<<'\n';
}
