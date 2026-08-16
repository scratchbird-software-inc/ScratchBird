// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_runtime.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <limits>
#include <string_view>

namespace scratchbird::engine::sblr { namespace {
void P16(std::vector<std::uint8_t>*o,std::uint16_t v){o->push_back(v);o->push_back(v>>8);}void P32(std::vector<std::uint8_t>*o,std::uint32_t v){for(unsigned i=0;i<4;++i)o->push_back(v>>(8*i));}void P64(std::vector<std::uint8_t>*o,std::uint64_t v){for(unsigned i=0;i<8;++i)o->push_back(v>>(8*i));}
std::uint16_t U16(const std::uint8_t*p){return p[0]|std::uint16_t(p[1])<<8;}std::uint32_t U32(const std::uint8_t*p){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(p[i])<<(8*i);return v;}std::uint64_t U64(const std::uint8_t*p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
template<class A>bool Nz(const A&a){return std::any_of(a.begin(),a.end(),[](auto b){return b!=0;});}
constexpr std::string_view kExecutor="engine.op.parameter",kResult="typed_value";
}
std::vector<std::uint8_t> EncodeSblrParameterNodeTableV1(const SblrParameterNodeTableV1& t){
 if(t.nodes.empty()||t.nodes.size()>4096||t.nodes.size()>(std::numeric_limits<std::size_t>::max()-32)/104)return{};std::vector<std::uint8_t>o;o.reserve(32+t.nodes.size()*104);o.insert(o.end(),{'S','B','P','N'});P16(&o,1);P16(&o,32);P32(&o,t.nodes.size());P32(&o,0);P64(&o,32+t.nodes.size()*104);P64(&o,32);std::uint32_t prior=0;std::uint64_t prior_node=0;
 for(const auto&n:t.nodes){if(!n.node_id||n.parent_operand_ordinal!=prior+1||!Nz(n.parameter_set_descriptor_uuid)||!n.parameter_set_generation||!Nz(n.datatype_descriptor_uuid)||!n.datatype_descriptor_generation||n.node_id<=prior_node)return{};P32(&o,104);P64(&o,n.node_id);P32(&o,n.parent_operand_ordinal);P32(&o,n.slot_ordinal);o.insert(o.end(),n.parameter_set_descriptor_uuid.begin(),n.parameter_set_descriptor_uuid.end());P64(&o,n.parameter_set_generation);o.insert(o.end(),n.datatype_descriptor_uuid.begin(),n.datatype_descriptor_uuid.end());P64(&o,n.datatype_descriptor_generation);P16(&o,kExecutor.size());o.insert(o.end(),kExecutor.begin(),kExecutor.end());P16(&o,kResult.size());o.insert(o.end(),kResult.begin(),kResult.end());P16(&o,1);prior=n.parent_operand_ordinal;prior_node=n.node_id;}return o;
}
#define DecodeSblrParameterNodeTableV1 DecodeSblrParameterNodeTableV1Unchecked
SblrParameterNodeTableCodecResultV1 DecodeSblrParameterNodeTableV1(const std::uint8_t*b,std::size_t z){SblrParameterNodeTableCodecResultV1 r;auto fail=[&](std::string d){r.detail=std::move(d);return r;};if(z>426016){r.diagnostic_id="RESOURCE.BUDGET_EXCEEDED";return fail("SBPN exceeds node budget");}if(!b||z<32||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPN"))||U16(b+4)!=1||U16(b+6)!=32||U32(b+12)||U64(b+16)!=z||U64(b+24)!=32)return fail("SBPN header invalid");auto count=U32(b+8);if(count==0)return fail("empty SBPN is forbidden");if(count>4096){r.diagnostic_id="RESOURCE.BUDGET_EXCEEDED";return fail("SBPN node count exceeds budget");}if(count>(z-32)/104||32+std::size_t(count)*104!=z)return fail("SBPN extent invalid");std::size_t o=32;for(std::uint32_t i=0;i<count;++i,o+=104){if(U32(b+o)!=104||U16(b+o+68)!=kExecutor.size()||!std::equal(b+o+70,b+o+89,kExecutor.begin())||U16(b+o+89)!=kResult.size()||!std::equal(b+o+91,b+o+102,kResult.begin())||U16(b+o+102)!=1)return fail("SBPN record identity invalid");SblrParameterNodeV1 n;n.node_id=U64(b+o+4);n.parent_operand_ordinal=U32(b+o+12);n.slot_ordinal=U32(b+o+16);std::copy_n(b+o+20,16,n.parameter_set_descriptor_uuid.begin());n.parameter_set_generation=U64(b+o+36);std::copy_n(b+o+44,16,n.datatype_descriptor_uuid.begin());n.datatype_descriptor_generation=U64(b+o+60);r.table.nodes.push_back(n);}r.canonical_bytes=EncodeSblrParameterNodeTableV1(r.table);if(r.canonical_bytes.size()!=z||!std::equal(r.canonical_bytes.begin(),r.canonical_bytes.end(),b))return fail("SBPN decode reencode differs");r.ok=true;return r;}
#undef DecodeSblrParameterNodeTableV1
SblrParameterNodeTableCodecResultV1 DecodeSblrParameterNodeTableV1(
    const std::uint8_t* bytes, std::size_t size) {
  SblrParameterNodeTableCodecResultV1 refused;
  if (bytes == nullptr || size < 32 ||
      !std::equal(bytes, bytes + 4,
                  reinterpret_cast<const std::uint8_t*>("SBPN")) ||
      U16(bytes + 4) != 1 || U16(bytes + 6) != 32 ||
      U32(bytes + 12) != 0 || U64(bytes + 16) != size ||
      U64(bytes + 24) != 32) {
    refused.detail = "SBPN header is structurally invalid";
    return refused;
  }
  const auto count = U32(bytes + 8);
  if (count > (std::numeric_limits<std::size_t>::max() - 32) / 104 ||
      32 + std::size_t(count) * 104 != size) {
    refused.detail = "SBPN extent is structurally invalid";
    return refused;
  }
  if (count == 0) {
    refused.detail = "empty SBPN is forbidden";
    return refused;
  }
  std::uint32_t prior_ordinal = 0;
  std::uint64_t prior_node = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto offset = 32 + std::size_t(index) * 104;
    std::array<std::uint8_t,16> parameter_set{}, datatype{};
    std::copy_n(bytes + offset + 20, 16, parameter_set.begin());
    std::copy_n(bytes + offset + 44, 16, datatype.begin());
    const auto node = U64(bytes + offset + 4);
    const auto ordinal = U32(bytes + offset + 12);
    if (U32(bytes + offset) != 104 || node <= prior_node ||
        ordinal != prior_ordinal + 1 || !Nz(parameter_set) ||
        U64(bytes + offset + 36) == 0 || !Nz(datatype) ||
        U64(bytes + offset + 60) == 0 ||
        U16(bytes + offset + 68) != kExecutor.size() ||
        !std::equal(bytes + offset + 70, bytes + offset + 89,
                    kExecutor.begin()) ||
        U16(bytes + offset + 89) != kResult.size() ||
        !std::equal(bytes + offset + 91, bytes + offset + 102,
                    kResult.begin()) || U16(bytes + offset + 102) != 1) {
      refused.detail = "SBPN record is structurally noncanonical";
      return refused;
    }
    prior_node = node;
    prior_ordinal = ordinal;
  }
  if (count > 4096 || size > 426016) {
    refused.diagnostic_id = "RESOURCE.BUDGET_EXCEEDED";
    refused.detail = "SBPN exceeds admitted node budget";
    return refused;
  }
  return DecodeSblrParameterNodeTableV1Unchecked(bytes, size);
}
std::vector<std::uint8_t> EncodeSblrParameterNodeReferenceV1(const SblrParameterNodeReferenceV1&r){if(!r.occurrence_ordinal||!r.node_id||!Nz(r.table_sha256)||!Nz(r.parameter_set_descriptor_uuid)||!r.parameter_set_generation)return{};std::vector<std::uint8_t>o;o.reserve(80);P16(&o,1);P16(&o,0);P32(&o,r.occurrence_ordinal);P64(&o,r.node_id);o.insert(o.end(),r.table_sha256.begin(),r.table_sha256.end());o.insert(o.end(),r.parameter_set_descriptor_uuid.begin(),r.parameter_set_descriptor_uuid.end());P64(&o,r.parameter_set_generation);P32(&o,r.slot_ordinal);P32(&o,0);return o;}
bool DecodeSblrParameterNodeReferenceV1(const std::uint8_t*b,std::size_t z,SblrParameterNodeReferenceV1*out){if(!b||!out||z!=80||U16(b)!=1||U16(b+2)||U32(b+76))return false;SblrParameterNodeReferenceV1 r;r.occurrence_ordinal=U32(b+4);r.node_id=U64(b+8);std::copy_n(b+16,32,r.table_sha256.begin());std::copy_n(b+48,16,r.parameter_set_descriptor_uuid.begin());r.parameter_set_generation=U64(b+64);r.slot_ordinal=U32(b+72);if(EncodeSblrParameterNodeReferenceV1(r).size()!=80)return false;*out=r;return true;}
bool ValidateSblrParameterReferenceBijectionV1(const SblrParameterNodeTableCodecResultV1&t,const std::vector<SblrParameterNodeReferenceV1>&rs){if(!t.ok||rs.size()!=t.table.nodes.size())return false;static constexpr std::string_view domain="ScratchBird.SblrParameterNodeTable.V1";std::vector<std::uint8_t> material(domain.begin(),domain.end());material.insert(material.end(),t.canonical_bytes.begin(),t.canonical_bytes.end());auto d=core::hash::ComputeSha256Digest(material);if(!d.ok())return false;std::vector<bool>seen(rs.size());for(const auto&r:rs){auto it=std::find_if(t.table.nodes.begin(),t.table.nodes.end(),[&](const auto&n){return n.node_id==r.node_id;});if(it==t.table.nodes.end())return false;auto i=std::distance(t.table.nodes.begin(),it);if(seen[i]||r.table_sha256!=d.digest||r.parameter_set_descriptor_uuid!=it->parameter_set_descriptor_uuid||r.parameter_set_generation!=it->parameter_set_generation||r.slot_ordinal!=it->slot_ordinal||r.occurrence_ordinal!=it->parent_operand_ordinal)return false;seen[i]=true;}return true;}
#define EncodeSblrParameterValueSetV1 EncodeSblrParameterValueSetV1Unchecked
#define DecodeSblrParameterValueSetV1 DecodeSblrParameterValueSetV1Unchecked
std::vector<std::uint8_t> EncodeSblrParameterValueSetV1(const SblrParameterValueSetV1&v){if(!Nz(v.parameter_set_descriptor_uuid)||!v.descriptor_generation||!Nz(v.execution_uuid)||!Nz(v.statement_receipt_uuid)||v.records.size()>std::numeric_limits<std::uint32_t>::max())return{};std::vector<std::uint8_t>records;std::uint32_t ordinal=0;for(const auto&r:v.records){if(r.slot_ordinal!=ordinal++||!Nz(r.slot_uuid)||!Nz(r.datatype_descriptor_uuid)||!r.datatype_descriptor_generation||static_cast<unsigned>(r.direction)<1||static_cast<unsigned>(r.direction)>3||static_cast<unsigned>(r.state)>2||(r.state!=SblrParameterValueStateV1::value&&!r.canonical_value_bytes.empty())||r.canonical_value_bytes.size()>std::numeric_limits<std::uint32_t>::max())return{};auto n=56+r.canonical_value_bytes.size();if(n>std::numeric_limits<std::uint32_t>::max())return{};P32(&records,n);P32(&records,r.slot_ordinal);records.insert(records.end(),r.slot_uuid.begin(),r.slot_uuid.end());records.insert(records.end(),r.datatype_descriptor_uuid.begin(),r.datatype_descriptor_uuid.end());P64(&records,r.datatype_descriptor_generation);records.push_back(static_cast<std::uint8_t>(r.direction));records.push_back(static_cast<std::uint8_t>(r.state));P16(&records,0);P32(&records,r.canonical_value_bytes.size());records.insert(records.end(),r.canonical_value_bytes.begin(),r.canonical_value_bytes.end());}static constexpr std::string_view domain="ScratchBird.SblrParameterValueRecords.V1";std::vector<std::uint8_t>hash_input(domain.begin(),domain.end());P32(&hash_input,v.records.size());hash_input.insert(hash_input.end(),records.begin(),records.end());auto digest=core::hash::ComputeSha256Digest(hash_input);if(!digest.ok()||records.size()>std::numeric_limits<std::uint32_t>::max()-112)return{};std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','P','V'});P16(&o,1);P16(&o,112);P32(&o,112+records.size());P32(&o,0);o.insert(o.end(),v.parameter_set_descriptor_uuid.begin(),v.parameter_set_descriptor_uuid.end());P64(&o,v.descriptor_generation);o.insert(o.end(),v.execution_uuid.begin(),v.execution_uuid.end());o.insert(o.end(),v.statement_receipt_uuid.begin(),v.statement_receipt_uuid.end());P32(&o,v.records.size());P32(&o,112);o.insert(o.end(),digest.digest.begin(),digest.digest.end());o.insert(o.end(),records.begin(),records.end());return o;}
SblrParameterValueSetCodecResultV1 DecodeSblrParameterValueSetV1(const std::uint8_t*b,std::size_t z){SblrParameterValueSetCodecResultV1 r;auto fail=[&](std::string d){r.detail=std::move(d);return r;};if(!b||z<112||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPV"))||U16(b+4)!=1||U16(b+6)!=112||U32(b+8)!=z||U32(b+12)||U32(b+76)!=112)return fail("SBPV header invalid");auto&v=r.value;std::copy_n(b+16,16,v.parameter_set_descriptor_uuid.begin());v.descriptor_generation=U64(b+32);std::copy_n(b+40,16,v.execution_uuid.begin());std::copy_n(b+56,16,v.statement_receipt_uuid.begin());auto count=U32(b+72);std::size_t o=112;for(std::uint32_t i=0;i<count;++i){if(o>z||z-o<56)return fail("SBPV record truncated");auto n=U32(b+o),bytes=U32(b+o+52);if(n<56||n>z-o||bytes!=n-56)return fail("SBPV record extent invalid");SblrParameterValueRecordV1 x;x.slot_ordinal=U32(b+o+4);std::copy_n(b+o+8,16,x.slot_uuid.begin());std::copy_n(b+o+24,16,x.datatype_descriptor_uuid.begin());x.datatype_descriptor_generation=U64(b+o+40);x.direction=static_cast<SblrParameterDirectionV1>(b[o+48]);x.state=static_cast<SblrParameterValueStateV1>(b[o+49]);if(U16(b+o+50))return fail("SBPV reserved fields invalid");x.canonical_value_bytes.assign(b+o+56,b+o+n);v.records.push_back(std::move(x));o+=n;}if(o!=z)return fail("SBPV trailing bytes");static constexpr std::string_view domain="ScratchBird.SblrParameterValueRecords.V1";std::vector<std::uint8_t>hash_input(domain.begin(),domain.end());P32(&hash_input,count);hash_input.insert(hash_input.end(),b+112,b+z);auto digest=core::hash::ComputeSha256Digest(hash_input);if(!digest.ok()||!std::equal(digest.digest.begin(),digest.digest.end(),b+80))return fail("SBPV records hash invalid");r.canonical_bytes=EncodeSblrParameterValueSetV1(v);if(r.canonical_bytes.size()!=z||!std::equal(r.canonical_bytes.begin(),r.canonical_bytes.end(),b))return fail("SBPV decode reencode differs");r.ok=true;return r;}
#undef EncodeSblrParameterValueSetV1
#undef DecodeSblrParameterValueSetV1

std::vector<std::uint8_t> EncodeSblrParameterValueSetV1(
    const SblrParameterValueSetV1& value) {
  if (value.records.empty() || value.records.size() > 4096) return {};
  std::size_t total = 112;
  for (const auto& record : value.records) {
    if (record.canonical_value_bytes.size() > 65536 ||
        total > 33554432 - 56 ||
        record.canonical_value_bytes.size() > 33554432 - total - 56) {
      return {};
    }
    total += 56 + record.canonical_value_bytes.size();
  }
  return EncodeSblrParameterValueSetV1Unchecked(value);
}

SblrParameterValueSetCodecResultV1 DecodeSblrParameterValueSetV1(
    const std::uint8_t* bytes, std::size_t size) {
  SblrParameterValueSetCodecResultV1 refused;
  if (bytes == nullptr || size < 112 ||
      !std::equal(bytes, bytes + 4,
                  reinterpret_cast<const std::uint8_t*>("SBPV")) ||
      U16(bytes + 4) != 1 || U16(bytes + 6) != 112 ||
      U32(bytes + 8) != size || U32(bytes + 12) != 0 ||
      U32(bytes + 76) != 112) {
    refused.detail = "SBPV header is structurally invalid";
    return refused;
  }
  const auto count = U32(bytes + 72);
  if (count == 0) {
    refused.detail = "empty SBPV is forbidden";
    return refused;
  }
  bool value_over_limit = false;
  std::size_t offset = 112;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (offset > size || size - offset < 56) {
      refused.detail = "SBPV record is structurally truncated";
      return refused;
    }
    const auto record_bytes = U32(bytes + offset);
    const auto value_bytes = U32(bytes + offset + 52);
    if (record_bytes < 56 || record_bytes > size - offset ||
        value_bytes != record_bytes - 56) {
      refused.detail = "SBPV record extent is structurally invalid";
      return refused;
    }
    std::array<std::uint8_t,16> slot{}, datatype{};
    std::copy_n(bytes + offset + 8, 16, slot.begin());
    std::copy_n(bytes + offset + 24, 16, datatype.begin());
    if (U32(bytes + offset + 4) != index || !Nz(slot) || !Nz(datatype) ||
        U64(bytes + offset + 40) == 0 || bytes[offset + 48] < 1 ||
        bytes[offset + 48] > 3 || bytes[offset + 49] > 2 ||
        U16(bytes + offset + 50) != 0 ||
        (bytes[offset + 49] != 1 && value_bytes != 0)) {
      refused.detail = "SBPV record is structurally noncanonical";
      return refused;
    }
    value_over_limit = value_over_limit || value_bytes > 65536;
    offset += record_bytes;
  }
  if (offset != size) {
    refused.detail = "SBPV trailing bytes are forbidden";
    return refused;
  }
  if (size > 33554432 || count > 4096 || value_over_limit) {
    refused.diagnostic_id = "RESOURCE.BUDGET_EXCEEDED";
    refused.detail = "SBPV exceeds admitted resource budget";
    return refused;
  }
  return DecodeSblrParameterValueSetV1Unchecked(bytes, size);
}

std::vector<std::uint8_t> EncodeSblrPreparedParameterTemplateV1(
    SblrPreparedParameterTemplateV1* value) {
  if (value == nullptr || !Nz(value->public_coordination_uuid) ||
      !Nz(value->operation_uuid) || !Nz(value->provisional_prepared_uuid) ||
      !Nz(value->parameter_set_descriptor_uuid) || !Nz(value->mga_snapshot_uuid) ||
      value->provisional_prepared_generation == 0 ||
      value->descriptor_generation == 0 ||
      value->executor_availability_generation == 0 ||
      value->catalog_generation == 0 || value->canonical_schema4015.empty() ||
      value->canonical_sbpa.size() != 192 ||
      value->canonical_schema4015.size() >
          std::numeric_limits<std::uint32_t>::max() - 472) return {};
  const auto schema_hash=core::hash::ComputeSha256Digest(value->canonical_schema4015);
  const auto sbpa_hash=core::hash::ComputeSha256Digest(value->canonical_sbpa);
  if(!schema_hash.ok()||!sbpa_hash.ok())return{};
  value->schema4015_sha256=schema_hash.digest;value->sbpa_sha256=sbpa_hash.digest;
  std::vector<std::uint8_t> o;o.insert(o.end(),{'S','B','P','T'});P16(&o,1);P16(&o,280);
  P32(&o,280+value->canonical_schema4015.size()+192);P32(&o,0);
  const auto uuid=[&](const auto&x){o.insert(o.end(),x.begin(),x.end());};
  uuid(value->public_coordination_uuid);uuid(value->operation_uuid);
  uuid(value->provisional_prepared_uuid);P64(&o,value->provisional_prepared_generation);
  uuid(value->parameter_set_descriptor_uuid);P64(&o,value->descriptor_generation);
  P32(&o,value->canonical_schema4015.size());P32(&o,192);
  o.insert(o.end(),value->schema4015_sha256.begin(),value->schema4015_sha256.end());
  o.insert(o.end(),value->sbpn_sha256.begin(),value->sbpn_sha256.end());
  o.insert(o.end(),value->sbpa_sha256.begin(),value->sbpa_sha256.end());
  P64(&o,value->executor_availability_generation);P64(&o,value->catalog_generation);
  P64(&o,value->security_epoch);P64(&o,value->resource_epoch);uuid(value->mga_snapshot_uuid);
  o.resize(280);
  static constexpr std::string_view domain="ScratchBird.SblrPreparedParameterTemplate.V1";
  std::vector<std::uint8_t> material(domain.begin(),domain.end());
  material.insert(material.end(),o.begin()+16,o.begin()+248);
  material.insert(material.end(),value->canonical_schema4015.begin(),value->canonical_schema4015.end());
  material.insert(material.end(),value->canonical_sbpa.begin(),value->canonical_sbpa.end());
  auto binding=core::hash::ComputeSha256Digest(material);if(!binding.ok())return{};
  value->prepared_template_binding_sha256=binding.digest;
  std::copy(binding.digest.begin(),binding.digest.end(),o.begin()+248);
  o.insert(o.end(),value->canonical_schema4015.begin(),value->canonical_schema4015.end());
  o.insert(o.end(),value->canonical_sbpa.begin(),value->canonical_sbpa.end());return o;
}

SblrPreparedParameterTemplateCodecResultV1 DecodeSblrPreparedParameterTemplateV1(
    const std::uint8_t* b,std::size_t z){SblrPreparedParameterTemplateCodecResultV1 r;
  auto fail=[&](std::string d){r.detail=std::move(d);return r;};
  if(!b||z<472||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPT"))||
     U16(b+4)!=1||U16(b+6)!=280||U32(b+8)!=z||U32(b+12)!=0||U32(b+100)!=192||
     static_cast<std::uint64_t>(280)+U32(b+96)+192!=z)return fail("SBPT header or extent invalid");
  auto&v=r.value;std::copy_n(b+16,16,v.public_coordination_uuid.begin());
  std::copy_n(b+32,16,v.operation_uuid.begin());std::copy_n(b+48,16,v.provisional_prepared_uuid.begin());
  v.provisional_prepared_generation=U64(b+64);std::copy_n(b+72,16,v.parameter_set_descriptor_uuid.begin());
  v.descriptor_generation=U64(b+88);std::copy_n(b+104,32,v.schema4015_sha256.begin());
  std::copy_n(b+136,32,v.sbpn_sha256.begin());std::copy_n(b+168,32,v.sbpa_sha256.begin());
  v.executor_availability_generation=U64(b+200);v.catalog_generation=U64(b+208);
  v.security_epoch=U64(b+216);v.resource_epoch=U64(b+224);std::copy_n(b+232,16,v.mga_snapshot_uuid.begin());
  std::copy_n(b+248,32,v.prepared_template_binding_sha256.begin());
  if(!Nz(v.public_coordination_uuid)||!Nz(v.operation_uuid)||!Nz(v.provisional_prepared_uuid)||
     !Nz(v.parameter_set_descriptor_uuid)||!Nz(v.mga_snapshot_uuid)||!v.provisional_prepared_generation||
     !v.descriptor_generation||!v.executor_availability_generation||!v.catalog_generation)
    return fail("SBPT required identity or generation invalid");
  const auto n=U32(b+96);v.canonical_schema4015.assign(b+280,b+280+n);
  v.canonical_sbpa.assign(b+280+n,b+z);auto copy=v;auto encoded=EncodeSblrPreparedParameterTemplateV1(&copy);
  if(encoded.size()!=z||!std::equal(encoded.begin(),encoded.end(),b)||
     copy.prepared_template_binding_sha256!=v.prepared_template_binding_sha256)
    return fail("SBPT hash or canonical re-encode invalid");
  r.canonical_bytes=std::move(encoded);r.ok=true;return r;}
}
