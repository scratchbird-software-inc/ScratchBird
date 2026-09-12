#include "engine/sblr/sblr_source_map_runtime.hpp"
#include <cstdlib>
#include <iostream>
namespace s=scratchbird::engine::sblr;void R(bool v,const char*m){if(!v){std::cerr<<m<<'\n';std::exit(1);}}
namespace { int IdentityChecks(); }
int main(){s::SblrSourceMapDescriptorVectorV1 v;v.descriptor_uuid[0]=1;v.descriptor_uuid[6]=0x70;v.descriptor_uuid[8]=0x80;v.descriptor_generation=1;v.registry_snapshot_uuid[0]=2;v.registry_snapshot_uuid[6]=0x70;v.registry_snapshot_uuid[8]=0x80;v.registry_generation=3;v.statement_receipt_uuid[0]=4;v.statement_receipt_uuid[6]=0x70;v.statement_receipt_uuid[8]=0x80;v.bound_ast_sha256[0]=5;s::SblrSourceMapEntryV1 e;e.node_id=7;e.source_artifact_uuid[0]=6;e.source_artifact_uuid[6]=0x70;e.source_artifact_uuid[8]=0x80;e.source_artifact_generation=1;e.byte_length=2;e.line=1;e.column=1;v.entries.push_back(e);auto b=s::EncodeSblrSourceMapDescriptorVectorV1(&v);R(b.size()==256,"SMVD extent");auto d=s::DecodeSblrSourceMapDescriptorVectorV1(b.data(),b.size());R(d.status==s::SblrSourceMapDecodeStatusV1::ok&&d.canonical_bytes==b,"SMVD roundtrip");for(auto o:{0u,4u,6u,8u,12u,116u,120u,218u,224u}){auto m=b;m[o]^=1;R(s::DecodeSblrSourceMapDescriptorVectorV1(m.data(),m.size()).status==s::SblrSourceMapDecodeStatusV1::operand_invalid,"SMVD mutation admitted");}
  s::SblrSourceMapBoundAstV1 ast;ast.statement_receipt_uuid=v.statement_receipt_uuid;ast.nodes.push_back({7,0,1});auto ab=s::EncodeSblrSourceMapBoundAstV1(&ast);R(ab.size()==96,"SMBA extent");s::SblrSourceMapBoundAstV1 ad;std::string detail;R(s::DecodeSblrSourceMapBoundAstV1(ab.data(),ab.size(),&ad,&detail),"SMBA roundtrip");
  s::SblrSourceMapIssueRequestV1 q;q.statement_receipt_uuid=v.statement_receipt_uuid;q.registry_snapshot_uuid=v.registry_snapshot_uuid;q.registry_generation=3;q.canonical_bound_ast=ab;q.entries={e};auto qb=s::EncodeSblrSourceMapIssueRequestV1(&q);R(qb.size()==336,"SMRQ extent");s::SblrSourceMapIssueRequestV1 qd;R(s::DecodeSblrSourceMapIssueRequestV1(qb.data(),qb.size(),&qd,&detail),"SMRQ roundtrip");auto qm=qb;qm[92]^=1;R(!s::DecodeSblrSourceMapIssueRequestV1(qm.data(),qm.size(),&qd,&detail),"SMRQ mutation admitted");auto coverage=q;coverage.entries[0].node_id=8;R(s::EncodeSblrSourceMapIssueRequestV1(&coverage).empty(),"SMRQ node coverage mismatch admitted");
  s::SblrSourceMapIssueResultV1 rs;rs.descriptor_uuid=v.descriptor_uuid;rs.descriptor_generation=1;rs.registry_generation=3;rs.canonical_smvd=b;auto rb=s::EncodeSblrSourceMapIssueResultV1(rs);R(rb.size()==320,"SMRS extent");s::SblrSourceMapIssueResultV1 rd;R(s::DecodeSblrSourceMapIssueResultV1(rb.data(),rb.size(),&rd,&detail),"SMRS roundtrip");auto rm=rb;rm[52]=1;R(!s::DecodeSblrSourceMapIssueResultV1(rm.data(),rm.size(),&rd,&detail),"SMRS mutation admitted");
  auto absent=v;absent.entries[0].redaction_class=4;absent.entries[0].source_artifact_uuid={};absent.entries[0].source_artifact_generation=0;absent.entries[0].byte_length=0;absent.entries[0].line=0;absent.entries[0].column=0;R(!s::EncodeSblrSourceMapDescriptorVectorV1(&absent).empty(),"SMVD absent redaction refused");return IdentityChecks();}
#include "hash_digest.hpp"
#include <algorithm>
#include <string_view>
namespace {
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::cerr << reason << '\n'; }
}
s::SblrSourceMapUuidV1 Identity(unsigned seed) {
  s::SblrSourceMapUuidV1 id{};
  id[0]=seed; id[6]=0x70; id[8]=0x80; id[15]=seed;
  return id;
}
void Put(Bytes& b, std::size_t offset, const s::SblrSourceMapUuidV1& id) {
  std::copy(id.begin(), id.end(), b.begin()+offset);
}
// Independent resealing of manifest-defined byte offsets/domains: a mutated
// identity must be rejected even when every digest and duplicated UUID agrees.
void Seal(Bytes& b, std::size_t output, std::string_view domain,
          std::size_t offset, std::size_t size) {
  Bytes material(domain.begin(),domain.end());
  material.insert(material.end(),b.begin()+offset,b.begin()+offset+size);
  auto hash=scratchbird::core::hash::ComputeSha256Digest(material);
  R(hash.ok(),"oracle digest failed");
  std::copy(hash.digest.begin(),hash.digest.end(),b.begin()+output);
}
void SealVector(Bytes& b) {
  Seal(b,224,"ScratchBird.SblrSourceMapEntry.V1",152,72);
  Seal(b,120,"ScratchBird.SblrSourceMapDescriptorVector.V1",152,104);
}
void SealRequest(Bytes& b) {
  Seal(b,176,"ScratchBird.SblrSourceMapBoundAst.V1",208,24);
  Seal(b,56,"ScratchBird.SblrSourceMapBoundAst.V1",136,96);
  Seal(b,304,"ScratchBird.SblrSourceMapEntry.V1",232,72);
  Seal(b,96,"ScratchBird.SblrSourceMapIssueRequest.V1",232,104);
}
int IdentityChecks() {
  s::SblrSourceMapDescriptorVectorV1 v;
  v.descriptor_uuid=Identity(1);v.descriptor_generation=1;
  v.registry_snapshot_uuid=Identity(2);v.registry_generation=3;
  v.statement_receipt_uuid=Identity(4);v.bound_ast_sha256[0]=5;
  s::SblrSourceMapEntryV1 e;e.node_id=7;e.source_artifact_uuid=Identity(6);
  e.source_artifact_generation=1;e.byte_length=2;e.line=1;e.column=1;v.entries={e};
  const auto vb=s::EncodeSblrSourceMapDescriptorVectorV1(&v);
  s::SblrSourceMapBoundAstV1 a;a.statement_receipt_uuid=v.statement_receipt_uuid;
  a.nodes={{7,0,1}};const auto ab=s::EncodeSblrSourceMapBoundAstV1(&a);
  s::SblrSourceMapIssueRequestV1 q;q.statement_receipt_uuid=v.statement_receipt_uuid;
  q.registry_snapshot_uuid=v.registry_snapshot_uuid;q.registry_generation=3;
  q.canonical_bound_ast=ab;q.entries={e};
  const auto qb=s::EncodeSblrSourceMapIssueRequestV1(&q);
  s::SblrSourceMapIssueResultV1 r;r.descriptor_uuid=v.descriptor_uuid;
  r.descriptor_generation=1;r.registry_generation=3;r.canonical_smvd=vb;
  const auto rb=s::EncodeSblrSourceMapIssueResultV1(r);
  R(vb.size()==256&&ab.size()==96&&qb.size()==336&&rb.size()==320,"oracle extents");
  s::SblrSourceMapBoundAstV1 ad;s::SblrSourceMapIssueRequestV1 qd;
  s::SblrSourceMapIssueResultV1 rd;std::string detail;
  // Validate the independent resealing oracle against unmodified controls.
  auto control=vb;SealVector(control);Check(control==vb,"SMVD oracle");
  control=qb;SealRequest(control);Check(control==qb,"SMRQ oracle");
  std::vector<s::SblrSourceMapUuidV1> invalid;
  for(unsigned version=0;version!=16;++version)if(version!=7){
    auto id=Identity(9);id[6]=version<<4;invalid.push_back(id);}
  for(unsigned variant:{0u,1u,3u}){
    auto id=Identity(9);id[8]=variant<<6;invalid.push_back(id);}
  invalid.push_back({});
  s::SblrSourceMapUuidV1 ascii{};ascii.fill('a');invalid.push_back(ascii);
  for(const auto& id:invalid){
    for(unsigned field=0;field!=4;++field){
      auto value=v;
      auto* slot=field==0?&value.descriptor_uuid:field==1?&value.registry_snapshot_uuid:
          field==2?&value.statement_receipt_uuid:&value.entries[0].source_artifact_uuid;
      *slot=id;
      Check(s::EncodeSblrSourceMapDescriptorVectorV1(&value).empty(),"SMVD invalid identity encode");
      auto wire=vb;Put(wire,std::array<std::size_t,4>{16,40,64,168}[field],id);SealVector(wire);
      Check(s::DecodeSblrSourceMapDescriptorVectorV1(wire.data(),wire.size()).status==
          s::SblrSourceMapDecodeStatusV1::operand_invalid,"SMVD resealed invalid identity decode");
      auto response=r;response.canonical_smvd=wire;if(field==0)response.descriptor_uuid=id;
      Check(s::EncodeSblrSourceMapIssueResultV1(response).empty(),"SMRS invalid nested identity encode");
      auto result_wire=rb;std::copy(wire.begin(),wire.end(),result_wire.begin()+64);
      if(field==0)Put(result_wire,16,id);
      Check(!s::DecodeSblrSourceMapIssueResultV1(result_wire.data(),result_wire.size(),&rd,&detail),
            "SMRS resealed invalid identity decode");
    }
    auto ast=a;ast.statement_receipt_uuid=id;
    Check(s::EncodeSblrSourceMapBoundAstV1(&ast).empty(),"SMBA invalid identity encode");
    auto wire=ab;Put(wire,16,id);
    Check(!s::DecodeSblrSourceMapBoundAstV1(wire.data(),wire.size(),&ad,&detail),"SMBA invalid identity decode");
    for(unsigned field=0;field!=3;++field){
      auto request=q;
      if(field==0){request.statement_receipt_uuid=id;Put(request.canonical_bound_ast,16,id);}
      else if(field==1)request.registry_snapshot_uuid=id;
      else request.entries[0].source_artifact_uuid=id;
      Check(s::EncodeSblrSourceMapIssueRequestV1(&request).empty(),"SMRQ invalid identity encode");
      wire=qb;Put(wire,std::array<std::size_t,3>{16,32,248}[field],id);
      if(field==0)Put(wire,152,id);
      SealRequest(wire);
      Check(!s::DecodeSblrSourceMapIssueRequestV1(wire.data(),wire.size(),&qd,&detail),
            "SMRQ resealed invalid identity decode");
    }
  }
  for(unsigned tail:{0u,15u}){
    auto value=v;value.descriptor_uuid[6]=0x70|tail;value.descriptor_uuid[8]=0x80|(tail*4+3);
    auto wire=s::EncodeSblrSourceMapDescriptorVectorV1(&value);
    Check(!wire.empty()&&s::DecodeSblrSourceMapDescriptorVectorV1(wire.data(),wire.size()).status==
          s::SblrSourceMapDecodeStatusV1::ok,"UUIDv7 random bits incorrectly constrained");
  }
  std::cout<<"source_map_identity checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
}
