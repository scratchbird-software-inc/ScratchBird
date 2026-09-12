// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "diagnostic_identity_projection_codec.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::wire {
namespace {
bool Shape(const DiagnosticIdentityProjectionV1& row,bool allow_internal) {
  return (row.diagnostic_uuid[6]&0xf0)==0x70 && (row.diagnostic_uuid[8]&0xc0)==0x80 &&
    row.diagnostic_generation!=0 && row.precedence_ordinal>=1 && row.precedence_ordinal<=4096 &&
    row.severity_code>=1 && row.severity_code<=(allow_internal?13:12) &&
    row.redaction_class>=1 && row.redaction_class<=4 && row.maximum_safe_field_count<=145;
}
void Put(DiagnosticIdentityProjectionBytesV1& bytes,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(n>>(8*i));
}
std::uint64_t Get(const std::uint8_t* bytes,std::size_t at,unsigned width) {
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(bytes[at+i])<<(8*i);return n;
}
bool Hash(const std::uint8_t* bytes,DiagnosticRegistrySha256V1* hash) {
  constexpr std::string_view domain="ScratchBird.DiagnosticIdentityRegistryRow.V1";
  std::array<std::uint8_t,domain.size()+40> material{};
  std::copy(domain.begin(),domain.end(),material.begin());
  std::copy_n(bytes,40,material.begin()+domain.size());
  const auto result=core::hash::ComputeSha256Digest(material.data(),material.size());
  if(!result.ok() || result.digest_bytes!=hash->size())return false;
  *hash=result.digest;return true;
}
}
bool EncodeDiagnosticIdentityProjectionV1(DiagnosticIdentityProjectionV1* row,
    DiagnosticIdentityProjectionBytesV1* output,bool allow_internal) noexcept {
  try {
    if(!row || !output || !Shape(*row,allow_internal))return false;
    DiagnosticIdentityProjectionBytesV1 bytes{};DiagnosticRegistrySha256V1 digest{};
    std::copy(row->diagnostic_uuid.begin(),row->diagnostic_uuid.end(),bytes.begin());
    Put(bytes,16,row->diagnostic_generation,8);Put(bytes,24,row->precedence_ordinal,4);
    bytes[28]=row->severity_code;bytes[29]=row->redaction_class;
    Put(bytes,32,row->maximum_safe_field_count,4);
    if(!Hash(bytes.data(),&digest))return false;
    std::copy(digest.begin(),digest.end(),bytes.begin()+40);
    row->row_identity_sha256=digest;*output=bytes;return true;
  }catch(const std::bad_alloc&) {return false;}
   catch(const std::length_error&) {return false;}
}
bool DecodeDiagnosticIdentityProjectionV1(const std::uint8_t* bytes,std::size_t size,
    DiagnosticIdentityProjectionV1* row,bool allow_internal) noexcept {
  try {
    if(!bytes || !row || size!=72 || Get(bytes,30,2)!=0 || Get(bytes,36,4)!=0)return false;
    DiagnosticIdentityProjectionV1 staged;DiagnosticRegistrySha256V1 digest{};
    std::copy_n(bytes,16,staged.diagnostic_uuid.begin());
    staged.diagnostic_generation=Get(bytes,16,8);
    staged.precedence_ordinal=static_cast<std::uint32_t>(Get(bytes,24,4));
    staged.severity_code=bytes[28];staged.redaction_class=bytes[29];
    staged.maximum_safe_field_count=static_cast<std::uint32_t>(Get(bytes,32,4));
    std::copy_n(bytes+40,32,staged.row_identity_sha256.begin());
    if(!Shape(staged,allow_internal) || !Hash(bytes,&digest) || digest!=staged.row_identity_sha256)
      return false;
    *row=staged;return true;
  }catch(const std::bad_alloc&) {return false;}
   catch(const std::length_error&) {return false;}
}
bool ValidateDiagnosticIdentityCohortV1(const std::vector<DiagnosticIdentityProjectionV1>& rows,
    bool allow_internal) noexcept {
  if(rows.empty() || rows.size()>4096)return false;
  std::array<std::uint16_t,4096> identities;
  std::uint32_t previous=0;
  for(std::size_t i=0;i<rows.size();++i) {
    const auto& row=rows[i];auto staged=row;DiagnosticIdentityProjectionBytesV1 bytes{};
    if(row.precedence_ordinal<=previous || !EncodeDiagnosticIdentityProjectionV1(&staged,&bytes,allow_internal) ||
       staged.row_identity_sha256!=row.row_identity_sha256)return false;
    previous=row.precedence_ordinal;identities[i]=static_cast<std::uint16_t>(i);
  }
  const auto end=identities.begin()+rows.size();
  std::sort(identities.begin(),end,[&](auto left,auto right) {
    const auto& a=rows[left];const auto& b=rows[right];
    if(a.diagnostic_uuid!=b.diagnostic_uuid)return a.diagnostic_uuid<b.diagnostic_uuid;
    return a.diagnostic_generation<b.diagnostic_generation;
  });
  for(std::size_t i=1;i<rows.size();++i) {
    const auto& a=rows[identities[i-1]];const auto& b=rows[identities[i]];
    if(a.diagnostic_uuid==b.diagnostic_uuid && a.diagnostic_generation==b.diagnostic_generation)
      return false;
  }
  return true;
}
} // namespace scratchbird::wire
