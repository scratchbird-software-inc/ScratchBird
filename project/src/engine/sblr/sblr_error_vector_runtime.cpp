// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_error_vector_runtime.hpp"
#include "hash_digest.hpp"
#include "../../wire/message_vector_value_codec.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kEntryBytes = 128;
constexpr std::size_t kMaximumEntries = 4096;
constexpr std::uint32_t kMaximumSafeFields = 145;
// SHA256 of the exact Core domain and four zero count octets. This immutable
// value avoids provider/allocation work for each empty entry in a large vector.
constexpr SblrErrorSha256V1 kEmptySafeFieldsHash{
  0x41,0x62,0x18,0x4a,0xd8,0x13,0x0a,0x14,0xee,0x0d,0xcc,0x3c,0x0e,0xd7,0xdb,0xa1,
  0xca,0x8f,0x1a,0x23,0x9b,0x37,0xde,0x68,0x6c,0xd3,0x44,0x85,0x6d,0x95,0x0c,0x74};

void Put(Bytes& b, std::size_t at, std::uint64_t value, unsigned width) {
  for (unsigned i=0;i<width;++i) b[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
std::uint16_t U16(const std::uint8_t* p) {return p[0]|std::uint16_t(p[1])<<8;}
std::uint32_t U32(const std::uint8_t* p) {
  std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(p[i])<<(8*i);return n;
}
std::uint64_t U64(const std::uint8_t* p) {
  std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(p[i])<<(8*i);return n;
}
template<class A> void Copy(Bytes& b,std::size_t at,const A& a) {
  std::copy(a.begin(),a.end(),b.begin()+at);
}
template<class A> void Read(const std::uint8_t* p,A* a) {std::copy_n(p,a->size(),a->begin());}
template<class A> bool Nonzero(const A& a) {
  return std::any_of(a.begin(),a.end(),[](auto b){return b!=0;});
}
bool Uuid(const SblrErrorUuidV1& id) {
  return (id[6]&0xf0)==0x70 && (id[8]&0xc0)==0x80;
}
bool Hash(std::string_view domain,const std::uint8_t* bytes,std::size_t size,
          SblrErrorSha256V1* output) {
  Bytes material(domain.begin(),domain.end());
  material.insert(material.end(),bytes,bytes+size);
  const auto hashed=scratchbird::core::hash::ComputeSha256Digest(material);
  if(!hashed.ok() || hashed.digest_bytes!=output->size())return false;
  *output=hashed.digest;return true;
}
bool Failure(std::string* detail,const char* reason) noexcept {
  if(detail!=nullptr) {
    try {*detail=reason;}
    catch(const std::bad_alloc&) {}
    catch(const std::length_error&) {}
  }
  return false;
}
bool Shape(const SblrErrorVectorEntryV1& entry,std::size_t ordinal) {
  return entry.occurrence_ordinal==ordinal && entry.precedence_ordinal>=1 &&
    entry.precedence_ordinal<=kMaximumEntries &&
    Uuid(entry.diagnostic_uuid) && entry.diagnostic_generation!=0 &&
    entry.severity_code>=1 && entry.severity_code<=13 && entry.redaction_class<=4 &&
    entry.safe_field_count<=kMaximumSafeFields && Nonzero(entry.safe_fields_sha256) &&
    (entry.safe_field_count!=0 || entry.safe_fields_sha256==kEmptySafeFieldsHash);
}
bool OrderValid(const std::vector<SblrErrorVectorEntryV1>& entries) {
  for(std::size_t i=1;i<entries.size();++i) {
    const auto& previous=entries[i-1];const auto& current=entries[i];
    if(previous.precedence_ordinal>current.precedence_ordinal)return false;
    if(previous.precedence_ordinal==current.precedence_ordinal &&
       (previous.diagnostic_uuid!=current.diagnostic_uuid ||
        previous.diagnostic_generation!=current.diagnostic_generation ||
        previous.severity_code!=current.severity_code ||
        previous.redaction_class!=current.redaction_class))return false;
  }
  if(entries.size()<2)return true;
  // Sort bounded indices, not source records. Binary UUID/generation pairs
  // may repeat for distinct occurrences, but cannot acquire a second rank.
  // The fixed index workspace avoids allocation and quadratic UUID scans.
  std::array<std::uint16_t,kMaximumEntries> identities;
  for(std::size_t i=0;i<entries.size();++i)identities[i]=static_cast<std::uint16_t>(i);
  const auto end=identities.begin()+entries.size();
  std::sort(identities.begin(),end,[&](auto left,auto right) {
    const auto& a=entries[left];const auto& b=entries[right];
    if(a.diagnostic_uuid!=b.diagnostic_uuid)return a.diagnostic_uuid<b.diagnostic_uuid;
    return a.diagnostic_generation<b.diagnostic_generation;
  });
  for(std::size_t i=1;i<entries.size();++i) {
    const auto& a=entries[identities[i-1]];const auto& b=entries[identities[i]];
    if(a.diagnostic_uuid==b.diagnostic_uuid && a.diagnostic_generation==b.diagnostic_generation &&
       a.precedence_ordinal!=b.precedence_ordinal)return false;
  }
  return true;
}
bool EntriesValid(const std::vector<SblrErrorVectorEntryV1>& entries) {
  if(entries.empty() || entries.size()>kMaximumEntries)return false;
  for(std::size_t i=0;i<entries.size();++i)if(!Shape(entries[i],i+1))return false;
  return OrderValid(entries);
}
bool EncodeEntries(std::vector<SblrErrorVectorEntryV1>* entries,Bytes* bytes) {
  bytes->assign(entries->size()*kEntryBytes,0);
  for(std::size_t i=0;i<entries->size();++i) {
    auto& entry=(*entries)[i];const std::size_t at=i*kEntryBytes;
    Put(*bytes,at,entry.occurrence_ordinal,8);Copy(*bytes,at+8,entry.diagnostic_uuid);
    Put(*bytes,at+24,entry.diagnostic_generation,8);Put(*bytes,at+32,entry.precedence_ordinal,4);
    (*bytes)[at+36]=entry.severity_code;(*bytes)[at+37]=entry.redaction_class;
    Put(*bytes,at+40,entry.safe_field_count,4);Copy(*bytes,at+48,entry.safe_fields_sha256);
    if(!Hash("ScratchBird.SblrErrorVectorEntry.V1",bytes->data()+at,80,&entry.entry_sha256))
      return false;
    Copy(*bytes,at+80,entry.entry_sha256);
  }
  return true;
}
bool DecodeEntries(const std::uint8_t* bytes,std::size_t count,
                   std::vector<SblrErrorVectorEntryV1>* entries) {
  entries->reserve(count);
  for(std::size_t i=0;i<count;++i) {
    const auto* p=bytes+i*kEntryBytes;SblrErrorVectorEntryV1 entry;
    entry.occurrence_ordinal=U64(p);Read(p+8,&entry.diagnostic_uuid);
    entry.diagnostic_generation=U64(p+24);entry.precedence_ordinal=U32(p+32);
    entry.severity_code=p[36];entry.redaction_class=p[37];entry.safe_field_count=U32(p+40);
    Read(p+48,&entry.safe_fields_sha256);Read(p+80,&entry.entry_sha256);
    if(U16(p+38)!=0 || U32(p+44)!=0 ||
       std::any_of(p+112,p+128,[](auto b){return b!=0;}) || !Shape(entry,i+1))return false;
    SblrErrorSha256V1 expected{};
    if(!Hash("ScratchBird.SblrErrorVectorEntry.V1",p,80,&expected) ||
       expected!=entry.entry_sha256)return false;
    entries->push_back(entry);
  }
  return OrderValid(*entries);
}
bool Authority(const SblrErrorVectorDescriptorV1& v) {
  return Uuid(v.descriptor_uuid) && v.descriptor_generation!=0 &&
    Uuid(v.registry_snapshot_uuid) && v.registry_generation!=0 &&
    Uuid(v.statement_receipt_uuid) && Uuid(v.diagnostic_registry_snapshot_uuid) &&
    v.diagnostic_registry_generation!=0;
}
bool Authority(const SblrErrorVectorIssueRequestV1& v) {
  return Uuid(v.registry_snapshot_uuid) && v.registry_generation!=0 &&
    Uuid(v.statement_receipt_uuid) && Uuid(v.diagnostic_registry_snapshot_uuid) &&
    v.diagnostic_registry_generation!=0;
}
bool Binding(const SblrErrorVectorIssueResultV1& outer,const SblrErrorVectorDescriptorV1& inner) {
  return outer.descriptor_uuid==inner.descriptor_uuid &&
    outer.descriptor_generation==inner.descriptor_generation &&
    outer.registry_generation==inner.registry_generation;
}
}
const SblrErrorSha256V1& SblrErrorVectorEmptySafeFieldsHashV1() noexcept {
  return kEmptySafeFieldsHash;
}
bool ComputeSblrErrorVectorSafeFieldsHashV1(std::span<const std::uint8_t> bytes,
    std::uint32_t count,SblrErrorSha256V1* output) noexcept {
  try {
    if(output==nullptr || count>kMaximumSafeFields || bytes.size()>262144)return false;
    if(count==0) {
      if(!bytes.empty())return false;
      *output=kEmptySafeFieldsHash;return true;
    }
    namespace mv=scratchbird::wire::message_vector;
    std::size_t at=0;std::string previous;
    for(std::uint32_t i=0;i<count;++i) {
      if(bytes.size()-at<8)return false;
      const auto* p=bytes.data()+at;
      const auto key_bytes=U16(p),type=U16(p+2);const auto value_bytes=U32(p+4);
      if(type<1 || type>8 || key_bytes>bytes.size()-at-8 ||
         value_bytes>bytes.size()-at-8-key_bytes)return false;
      const auto raw=std::size_t(8)+key_bytes+value_bytes;
      const auto extent=(raw+3)&~std::size_t(3);
      if(extent>bytes.size()-at)return false;
      mv::ValueTlv field;
      if(mv::DecodeValueTlv(bytes.subspan(at,extent),&field)!=mv::ValueCodecError::none ||
         (i!=0 && !std::lexicographical_compare(previous.begin(),previous.end(),
           field.key.begin(),field.key.end(),[](char a,char b) {
             return static_cast<unsigned char>(a)<static_cast<unsigned char>(b);
           })))return false;
      previous=std::move(field.key);at+=extent;
    }
    if(at!=bytes.size())return false;
    Bytes material(4,0);Put(material,0,count,4);
    material.insert(material.end(),bytes.begin(),bytes.end());
    SblrErrorSha256V1 digest{};
    if(!Hash("ScratchBird.SblrErrorVectorSafeFields.V1",material.data(),material.size(),&digest) ||
       !Nonzero(digest))return false;
    *output=digest;return true;
  }catch(const std::bad_alloc&) {return false;}
   catch(const std::length_error&) {return false;}
}
Bytes EncodeSblrErrorVectorDescriptorV1(SblrErrorVectorDescriptorV1* input) {
  try {
    if(input==nullptr || !Authority(*input) || !EntriesValid(input->entries))return {};
    auto staged=*input;Bytes entries;
    if(!EncodeEntries(&staged.entries,&entries) ||
       !Hash("ScratchBird.SblrErrorVectorDescriptorVector.V1",entries.data(),entries.size(),
             &staged.vector_sha256))return {};
    Bytes bytes(152,0);std::copy_n("ERVD",4,bytes.begin());
    Put(bytes,4,1,2);Put(bytes,6,152,2);Put(bytes,8,152+entries.size(),4);
    Copy(bytes,16,staged.descriptor_uuid);Put(bytes,32,staged.descriptor_generation,8);
    Copy(bytes,40,staged.registry_snapshot_uuid);Put(bytes,56,staged.registry_generation,8);
    Copy(bytes,64,staged.statement_receipt_uuid);Copy(bytes,80,staged.diagnostic_registry_snapshot_uuid);
    Put(bytes,96,staged.diagnostic_registry_generation,8);Put(bytes,104,staged.entries.size(),4);
    Put(bytes,108,kEntryBytes,4);Copy(bytes,112,staged.vector_sha256);
    bytes.insert(bytes.end(),entries.begin(),entries.end());
    *input=std::move(staged);return bytes;
  }catch(const std::bad_alloc&) {return {};}
   catch(const std::length_error&) {return {};}
}
bool DecodeSblrErrorVectorDescriptorV1(const std::uint8_t* bytes,std::size_t size,
                                      SblrErrorVectorDescriptorV1* output,std::string* detail) {
  try {
    if(bytes==nullptr || output==nullptr || size<152 || size>524440 ||
       !std::equal(bytes,bytes+4,"ERVD") || U16(bytes+4)!=1 || U16(bytes+6)!=152 ||
       U32(bytes+8)!=size || U32(bytes+12)!=0 || U32(bytes+108)!=kEntryBytes ||
       U64(bytes+144)!=0)return Failure(detail,"ERVD header invalid");
    const auto count=U32(bytes+104);
    if(count==0 || count>kMaximumEntries || 152+std::size_t(count)*kEntryBytes!=size)
      return Failure(detail,"ERVD entry extent invalid");
    SblrErrorVectorDescriptorV1 staged;Read(bytes+16,&staged.descriptor_uuid);
    staged.descriptor_generation=U64(bytes+32);Read(bytes+40,&staged.registry_snapshot_uuid);
    staged.registry_generation=U64(bytes+56);Read(bytes+64,&staged.statement_receipt_uuid);
    Read(bytes+80,&staged.diagnostic_registry_snapshot_uuid);
    staged.diagnostic_registry_generation=U64(bytes+96);Read(bytes+112,&staged.vector_sha256);
    if(!Authority(staged))return Failure(detail,"ERVD authority identity invalid");
    SblrErrorSha256V1 digest{};
    if(!Hash("ScratchBird.SblrErrorVectorDescriptorVector.V1",bytes+152,size-152,&digest) ||
       digest!=staged.vector_sha256)return Failure(detail,"ERVD vector hash invalid");
    if(!DecodeEntries(bytes+152,count,&staged.entries))return Failure(detail,"ERVD entry invalid");
    *output=std::move(staged);return true;
  }catch(const std::bad_alloc&) {return Failure(detail,"ERVD allocation failed");}
   catch(const std::length_error&) {return Failure(detail,"ERVD allocation extent invalid");}
}
Bytes EncodeSblrErrorVectorIssueRequestV1(SblrErrorVectorIssueRequestV1* input) {
  try {
    if(input==nullptr || !Authority(*input) || !EntriesValid(input->entries))return {};
    auto staged=*input;Bytes entries;
    if(!EncodeEntries(&staged.entries,&entries) ||
       !Hash("ScratchBird.SblrErrorVectorIssueRequest.V1",entries.data(),entries.size(),
             &staged.entries_sha256))return {};
    Bytes bytes(120,0);std::copy_n("EVRQ",4,bytes.begin());Put(bytes,4,1,2);
    Put(bytes,6,120,2);Put(bytes,8,120+entries.size(),4);
    Copy(bytes,16,staged.statement_receipt_uuid);Copy(bytes,32,staged.registry_snapshot_uuid);
    Put(bytes,48,staged.registry_generation,8);Copy(bytes,56,staged.diagnostic_registry_snapshot_uuid);
    Put(bytes,72,staged.diagnostic_registry_generation,8);Put(bytes,80,staged.entries.size(),4);
    Put(bytes,84,kEntryBytes,4);Copy(bytes,88,staged.entries_sha256);
    bytes.insert(bytes.end(),entries.begin(),entries.end());
    *input=std::move(staged);return bytes;
  }catch(const std::bad_alloc&) {return {};}
   catch(const std::length_error&) {return {};}
}
bool DecodeSblrErrorVectorIssueRequestV1(const std::uint8_t* bytes,std::size_t size,
                                       SblrErrorVectorIssueRequestV1* output,std::string* detail) {
  try {
    if(bytes==nullptr || output==nullptr || size<120 || size>524408 ||
       !std::equal(bytes,bytes+4,"EVRQ") || U16(bytes+4)!=1 || U16(bytes+6)!=120 ||
       U32(bytes+8)!=size || U32(bytes+12)!=0 || U32(bytes+84)!=kEntryBytes)
      return Failure(detail,"EVRQ header invalid");
    const auto count=U32(bytes+80);
    if(count==0 || count>kMaximumEntries || 120+std::size_t(count)*kEntryBytes!=size)
      return Failure(detail,"EVRQ entry extent invalid");
    SblrErrorVectorIssueRequestV1 staged;Read(bytes+16,&staged.statement_receipt_uuid);
    Read(bytes+32,&staged.registry_snapshot_uuid);staged.registry_generation=U64(bytes+48);
    Read(bytes+56,&staged.diagnostic_registry_snapshot_uuid);
    staged.diagnostic_registry_generation=U64(bytes+72);Read(bytes+88,&staged.entries_sha256);
    if(!Authority(staged))return Failure(detail,"EVRQ authority identity invalid");
    SblrErrorSha256V1 digest{};
    if(!Hash("ScratchBird.SblrErrorVectorIssueRequest.V1",bytes+120,size-120,&digest) ||
       digest!=staged.entries_sha256)return Failure(detail,"EVRQ entries hash invalid");
    if(!DecodeEntries(bytes+120,count,&staged.entries))return Failure(detail,"EVRQ entry invalid");
    *output=std::move(staged);return true;
  }catch(const std::bad_alloc&) {return Failure(detail,"EVRQ allocation failed");}
   catch(const std::length_error&) {return Failure(detail,"EVRQ allocation extent invalid");}
}
Bytes EncodeSblrErrorVectorIssueResultV1(const SblrErrorVectorIssueResultV1& input) {
  try {
    if(input.canonical_ervd.size()>524440)return {};
    SblrErrorVectorDescriptorV1 inner;
    if(!DecodeSblrErrorVectorDescriptorV1(input.canonical_ervd.data(),input.canonical_ervd.size(),
                                        &inner,nullptr) || !Binding(input,inner))return {};
    Bytes bytes(64,0);std::copy_n("EVRS",4,bytes.begin());Put(bytes,4,1,2);Put(bytes,6,64,2);
    Put(bytes,8,64+input.canonical_ervd.size(),4);Copy(bytes,16,input.descriptor_uuid);
    Put(bytes,32,input.descriptor_generation,8);Put(bytes,40,input.registry_generation,8);
    Put(bytes,48,input.canonical_ervd.size(),4);
    bytes.insert(bytes.end(),input.canonical_ervd.begin(),input.canonical_ervd.end());return bytes;
  }catch(const std::bad_alloc&) {return {};}
   catch(const std::length_error&) {return {};}
}
bool DecodeSblrErrorVectorIssueResultV1(const std::uint8_t* bytes,std::size_t size,
                                      SblrErrorVectorIssueResultV1* output,std::string* detail) {
  try {
    if(bytes==nullptr || output==nullptr || size<64 || size>524504 ||
       !std::equal(bytes,bytes+4,"EVRS") || U16(bytes+4)!=1 || U16(bytes+6)!=64 ||
       U32(bytes+8)!=size || U32(bytes+12)!=0 || U32(bytes+52)!=0 || U64(bytes+56)!=0 ||
       std::size_t(U32(bytes+48))!=size-64)return Failure(detail,"EVRS header invalid");
    SblrErrorVectorIssueResultV1 staged;Read(bytes+16,&staged.descriptor_uuid);
    staged.descriptor_generation=U64(bytes+32);staged.registry_generation=U64(bytes+40);
    SblrErrorVectorDescriptorV1 inner;
    if(!DecodeSblrErrorVectorDescriptorV1(bytes+64,size-64,&inner,detail))return false;
    if(!Binding(staged,inner))return Failure(detail,"EVRS descriptor binding invalid");
    staged.canonical_ervd.assign(bytes+64,bytes+size);
    *output=std::move(staged);return true;
  }catch(const std::bad_alloc&) {return Failure(detail,"EVRS allocation failed");}
   catch(const std::length_error&) {return Failure(detail,"EVRS allocation extent invalid");}
}
} // namespace scratchbird::engine::sblr
