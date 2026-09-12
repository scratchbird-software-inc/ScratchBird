// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/diagnostic_identity_projection_codec.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>
namespace w=scratchbird::wire;
using Row=w::DiagnosticIdentityProjectionV1;
using Bytes=w::DiagnosticIdentityProjectionBytesV1;
std::size_t checks=0;
void Check(bool b,const char* why){++checks;if(!b){std::cerr<<why<<'\n';std::exit(1);}}
void Put(Bytes& b,std::size_t at,std::uint64_t n,unsigned size){
  for(unsigned i=0;i<size;++i)b[at+i]=static_cast<std::uint8_t>(n>>(8*i));
}
Bytes Oracle(const Row& r){
  Bytes b{};std::copy(r.diagnostic_uuid.begin(),r.diagnostic_uuid.end(),b.begin());
  Put(b,16,r.diagnostic_generation,8);Put(b,24,r.precedence_ordinal,4);
  b[28]=r.severity_code;b[29]=r.redaction_class;Put(b,32,r.maximum_safe_field_count,4);
  constexpr std::string_view domain="ScratchBird.DiagnosticIdentityRegistryRow.V1";
  std::vector<std::uint8_t> preimage(domain.begin(),domain.end());
  preimage.insert(preimage.end(),b.begin(),b.begin()+40);
  auto h=scratchbird::core::hash::ComputeSha256Digest(preimage);
  Check(h.ok()&&h.digest_bytes==32,"oracle hash failed");
  std::copy(h.digest.begin(),h.digest.end(),b.begin()+40);return b;
}
Row Fixture(unsigned rank=4){
  Row r;r.diagnostic_uuid={1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,16};
  r.diagnostic_uuid[12]=rank>>8;r.diagnostic_uuid[13]=rank;
  r.diagnostic_generation=0x0807060504030201ULL;r.precedence_ordinal=rank;
  r.severity_code=3;r.redaction_class=1;r.maximum_safe_field_count=145;return r;
}
bool Same(const Row&a,const Row&b){
  return a.diagnostic_uuid==b.diagnostic_uuid && a.diagnostic_generation==b.diagnostic_generation &&
    a.precedence_ordinal==b.precedence_ordinal && a.severity_code==b.severity_code &&
    a.redaction_class==b.redaction_class && a.maximum_safe_field_count==b.maximum_safe_field_count &&
    a.row_identity_sha256==b.row_identity_sha256;
}
int main(){
  for(unsigned rank=1;rank<=4096;++rank){
    auto row=Fixture(rank);Bytes out{};auto expected=Oracle(row);
    Check(w::EncodeDiagnosticIdentityProjectionV1(&row,&out),"rank encode refused");
    Check(out==expected,"independent bytes mismatch");Row decoded;
    Check(w::DecodeDiagnosticIdentityProjectionV1(expected.data(),expected.size(),&decoded),"oracle decode refused");
    Check(Same(row,decoded),"roundtrip mismatch");
  }
  auto row=Fixture();Bytes good{};Check(w::EncodeDiagnosticIdentityProjectionV1(&row,&good),"fixture");
  for(unsigned at=0;at<72;++at)for(unsigned bit=0;bit<8;++bit){
    auto bad=good;bad[at]^=1<<bit;auto output=Fixture(9),before=output;
    Check(!w::DecodeDiagnosticIdentityProjectionV1(bad.data(),bad.size(),&output),"single-bit corruption accepted");
    Check(Same(output,before),"failed decode changed output");
  }
  for(std::size_t size=0;size<72;++size){Row output;Check(!w::DecodeDiagnosticIdentityProjectionV1(good.data(),size,&output),"truncation");}
  std::vector<std::uint8_t> oversized(good.begin(),good.end());oversized.push_back(0);Row decoded;
  Check(!w::DecodeDiagnosticIdentityProjectionV1(oversized.data(),oversized.size(),&decoded),"trailing byte");
  Check(!w::DecodeDiagnosticIdentityProjectionV1(nullptr,72,&decoded),"null input");
  Check(!w::DecodeDiagnosticIdentityProjectionV1(good.data(),72,nullptr),"null output");
  for(unsigned severity=0;severity<=255;++severity){
    auto r=Fixture();r.severity_code=severity;auto b=Oracle(r);
    Check(w::DecodeDiagnosticIdentityProjectionV1(b.data(),72,&decoded)==(severity>=1&&severity<=12),"public severity");
    Check(w::DecodeDiagnosticIdentityProjectionV1(b.data(),72,&decoded,true)==(severity>=1&&severity<=13),"private severity");
  }
  for(unsigned category=0;category<=255;++category){
    auto r=Fixture();r.redaction_class=category;auto b=Oracle(r);
    Check(w::DecodeDiagnosticIdentityProjectionV1(b.data(),72,&decoded)==(category>=1&&category<=4),"category bounds");
  }
  for(auto count:{0u,1u,145u,146u,UINT32_MAX}){
    auto r=Fixture();r.maximum_safe_field_count=count;auto b=Oracle(r);
    Check(w::DecodeDiagnosticIdentityProjectionV1(b.data(),72,&decoded)==(count<=145),"count bounds");
  }
  for(unsigned mutation=0;mutation<5;++mutation){
    auto r=Fixture();
    if(mutation==0)r.diagnostic_uuid[6]=0x40;
    if(mutation==1)r.diagnostic_uuid[8]=0xc0;
    if(mutation==2)r.diagnostic_generation=0;
    if(mutation==3)r.precedence_ordinal=0;
    if(mutation==4)r.precedence_ordinal=4097;
    const auto original=r;Bytes b{};b.fill(0xaa);const auto sentinel=b;
    Check(!w::EncodeDiagnosticIdentityProjectionV1(&r,&b),"invalid shape encoded");
    Check(Same(r,original)&&b==sentinel,"failed encode mutated output");
    auto oracle=Oracle(r);Check(!w::DecodeDiagnosticIdentityProjectionV1(oracle.data(),72,&decoded),"valid hash invalid shape");
  }
  std::vector<Row> cohort;
  for(unsigned rank=1;rank<=4096;++rank){auto r=Fixture(rank);Bytes b;Check(w::EncodeDiagnosticIdentityProjectionV1(&r,&b),"cohort fixture");cohort.push_back(r);}
  Check(w::ValidateDiagnosticIdentityCohortV1(cohort),"maximum cohort rejected");
  std::vector<Row> sparse{cohort[3],cohort[31],cohort.back()};
  Check(w::ValidateDiagnosticIdentityCohortV1(sparse),"filtered ranks renumbered");
  auto bad=sparse;std::swap(bad[0],bad[1]);Check(!w::ValidateDiagnosticIdentityCohortV1(bad),"descending ranks");
  bad=sparse;bad[1]=bad[0];Check(!w::ValidateDiagnosticIdentityCohortV1(bad),"duplicate rank");
  bad=sparse;bad[1].diagnostic_uuid=bad[0].diagnostic_uuid;Bytes b;
  Check(w::EncodeDiagnosticIdentityProjectionV1(&bad[1],&b),"duplicate identity fixture");
  Check(!w::ValidateDiagnosticIdentityCohortV1(bad),"duplicate identity generation");
  bad=sparse;bad[1].row_identity_sha256[0]^=1;Check(!w::ValidateDiagnosticIdentityCohortV1(bad),"cohort hash");
  cohort.push_back(Fixture());Check(!w::ValidateDiagnosticIdentityCohortV1(cohort),"oversized cohort");
  Check(!w::ValidateDiagnosticIdentityCohortV1({}),"empty cohort");
  std::cout<<"PASS diagnostic_identity_projection checks="<<checks<<'\n';
}
