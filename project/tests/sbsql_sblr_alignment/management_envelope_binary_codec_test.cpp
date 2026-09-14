// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_management_envelope.hpp"
#include "hash_digest.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace sb = scratchbird::engine::sblr;
namespace hash = scratchbird::core::hash;
namespace {
std::atomic<long> fail_after{-1};
std::atomic<unsigned> allocations{0};
bool fail_hash=false;
unsigned checks=0;
}
void* operator new(std::size_t n) {
  ++allocations;
  auto remaining=fail_after.load();
  if(remaining>=0 && fail_after.fetch_sub(1)==0) throw std::bad_alloc();
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t n,unsigned char* out,unsigned int* size,const EVP_MD* type,ENGINE* impl) {
  return fail_hash?0:__real_EVP_Digest(data,n,out,size,type,impl);
}
namespace {
scratchbird::core::platform::Uuid Id(std::uint8_t seed) {
  scratchbird::core::platform::Uuid id; id.bytes.fill(seed); id.bytes[6]=0x70; id.bytes[8]=0x80; return id;
}
const auto kUuidA=Id(1),kUuidB=Id(2),kUuidC=Id(3);

[[noreturn]] void Fail(const std::string& what) { std::cerr << what << '\n'; std::exit(1); }
void Require(bool condition, const std::string& what) { ++checks; if (!condition) Fail(what); }

std::vector<std::string> Names(sb::SblrManagementEnvelopeKind kind) {
  using K = sb::SblrManagementEnvelopeKind;
  if (kind == K::operation) return {"operation_uuid", "opcode", "opcode_version_major", "opcode_version_minor", "target_database_uuid", "target_filespace_uuid", "target_cluster_uuid", "target_node_uuid", "target_object_uuid", "security_context_uuid", "policy_snapshot_uuid", "policy_epoch", "request_source", "idempotency_key", "causal_transaction_id", "requested_snapshot", "wait_mode", "timeout_ms", "dry_run", "audit_reason", "diagnostic_locale", "disclosure_class", "operation_generation"};
  if (kind == K::payload) return {"operation_uuid", "payload_schema_uuid", "payload_version_major", "payload_version_minor", "canonical_serialization_hash", "payload_body"};
  if (kind == K::result) return {"operation_uuid", "terminal_state", "summary_code", "affected_object_counts", "durable_state_refs", "diagnostic_vector_uuid", "metric_snapshot_refs", "evidence_bundle_uuid"};
  if (kind == K::progress) return {"operation_uuid", "phase", "phase_generation", "total_work_estimate", "completed_work", "current_object_uuid", "retry_count", "last_safe_retry_point"};
  if (kind == K::diagnostic) return {"operation_uuid", "diagnostic_code", "severity", "object_uuid", "search_key", "safe_human_text", "disclosure_class", "retry_class"};
  return {"operation_uuid", "metric_scope", "metric_path", "sample_window", "metric_row_refs", "classification"};
}

sb::SblrManagementEnvelopeRecord Record(sb::SblrManagementEnvelopeKind kind,
                                        const scratchbird::core::platform::Uuid& operation_uuid) {
  sb::SblrManagementEnvelopeRecord record; record.kind = kind;
  for (const auto& name : Names(kind)) {
    std::string value = "1";
    if (name.find("uuid") != std::string::npos) {
      const auto& id = name == "operation_uuid" ? operation_uuid : kUuidB;
      record.fields.push_back({name, {id.bytes.begin(), id.bytes.end()}}); continue;
    }
    if (name == "opcode") value = "mga.checkpoint";
    else if (name == "request_source") value = "test_harness";
    else if (name == "wait_mode") value = "try";
    else if (name == "dry_run") value = "false";
    else if (name == "audit_reason") value = "focused_test";
    else if (name == "diagnostic_locale") value = "en";
    else if (name == "disclosure_class") value = "operational";
    else if (name == "terminal_state") value = "completed";
    else if (name == "metric_scope") value = "local";
    else if (name == "metric_path") value = "sys.metrics.mga.cmo.operations_accepted_total";
    else if (name == "payload_body") value = "payload";
    record.fields.push_back({name, {value.begin(), value.end()}});
  }
  if (kind == sb::SblrManagementEnvelopeKind::payload) {
    const auto digest = hash::ComputeSha256Digest(
        record.fields.back().value);
    for (auto& field : record.fields) if (field.name == "canonical_serialization_hash") field.value.assign(digest.digest.begin(), digest.digest.end());
  }
  return record;
}


using Bytes=std::vector<std::uint8_t>;
void Put(Bytes& b,std::size_t at,std::uint64_t n,unsigned width){for(unsigned i=0;i<width;++i)b[at+i]=n>>(8*i);}
std::uint32_t Crc(const Bytes& b,std::size_t n){
  std::uint32_t crc=~0u;
  for(std::size_t i=0;i<n;++i){crc^=b[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^((crc&1)?0x82f63b78u:0);}
  return ~crc;
}
void Seal(Bytes& b){
  SHA256(b.data(),b.size()-36,b.data()+b.size()-36);
  Put(b,b.size()-4,Crc(b,b.size()-4),4);
}
Bytes Oracle(const sb::SblrManagementEnvelopeRecord& r){
  Bytes b(16);b[0]='S';b[1]='B';b[2]='M';b[3]='G';Put(b,4,2,2);Put(b,8,static_cast<unsigned>(r.kind),2);Put(b,10,r.fields.size(),2);
  for(std::size_t i=0;i<r.fields.size();++i){const auto p=b.size();b.resize(p+8);Put(b,p,i+1,2);Put(b,p+4,r.fields[i].value.size(),4);b.insert(b.end(),r.fields[i].value.begin(),r.fields[i].value.end());}
  Put(b,12,b.size()-16,4);b.resize(b.size()+36);Seal(b);return b;
}
void Failed(const sb::SblrManagementEnvelopeCodecResult& r){
  Require(!r.ok&&r.canonical_bytes.empty()&&r.record.fields.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](auto b){return b==0;}),"failure has no result prefix");
}
void Bad(const sb::SblrManagementEnvelopeRecord& r){
  Failed(sb::EncodeSblrManagementEnvelopeRecord(r));const auto b=Oracle(r);Failed(sb::DecodeSblrManagementEnvelopeRecord(b.data(),b.size()));
}
sb::SblrOperationEnvelope Carrier(const sb::SblrManagementEnvelopeRecord& r){
  sb::SblrOperationEnvelope e;e.operation_id=sb::ManagementEnvelopeOperationId(r.kind);e.opcode=sb::ManagementEnvelopeOpcode(r.kind);e.opcode_code=sb::ManagementEnvelopeOpcodeCode(r.kind);
  e.operands={sb::MakeSblrManagementEnvelopeOperand(sb::EncodeSblrManagementEnvelopeRecord(r))};return e;
}
template<class F> void AllocationFailures(F&& fn){
  allocations=0;const auto baseline=fn();const auto sites=allocations.load();Require(baseline.ok,"allocation baseline");
  for(unsigned n=0;n<sites;++n){bool rejected=false;fail_after=n;
    try {const auto r=fn();fail_after=-1;rejected=!r.ok;if(rejected)Failed(r);}
    catch(const std::bad_alloc&){fail_after=-1;rejected=true;}
    Require(rejected,"allocation failure cannot succeed");
  }
}
}
int main(){
  using K=sb::SblrManagementEnvelopeKind;
  for(auto kind:{K::operation,K::payload,K::result,K::progress,K::diagnostic,K::metric_snapshot_ref}){
    const auto r=Record(kind,kUuidA);const auto encoded=sb::EncodeSblrManagementEnvelopeRecord(r);
    const auto oracle=Oracle(r);Require(encoded.ok&&encoded.canonical_bytes==oracle,"independent full binary frame");
    const auto decoded=sb::DecodeSblrManagementEnvelopeRecord(oracle.data(),oracle.size());Require(decoded.ok&&decoded.sha256==encoded.sha256,"binary frame digest");
    for(std::size_t f=0;f<r.fields.size();++f){
      Require(decoded.record.fields[f].value==r.fields[f].value,"exact field bytes");
      if(r.fields[f].name.find("uuid")==std::string::npos)continue;
      const bool nullable=r.fields[f].name=="target_filespace_uuid"||r.fields[f].name=="target_cluster_uuid"||r.fields[f].name=="target_node_uuid"||r.fields[f].name=="target_object_uuid"||r.fields[f].name=="current_object_uuid"||r.fields[f].name=="object_uuid";
      auto bad=r;bad.fields[f].value.clear();
      if(nullable){const auto b=Oracle(bad);Require(sb::EncodeSblrManagementEnvelopeRecord(bad).ok&&sb::DecodeSblrManagementEnvelopeRecord(b.data(),b.size()).ok,"nullable UUID is empty");}else Bad(bad);
      for(unsigned width=1;width<=40;++width)if(width!=16){bad=r;bad.fields[f].value.resize(width,0x71);Bad(bad);}
      bad=r;bad.fields[f].value.assign(16,0);Bad(bad);
      for(unsigned version=0;version<16;++version)if(version!=7){bad=r;bad.fields[f].value[6]=version<<4;Bad(bad);}
      for(unsigned variant:{0u,0x40u,0xc0u}){bad=r;bad.fields[f].value[8]=variant;Bad(bad);}
      bad=r;const std::string text="019a0000-0000-7000-8000-000000000001";bad.fields[f].value.assign(text.begin(),text.end());Bad(bad);
    }
    auto old=oracle;Put(old,4,1,2);Seal(old);Failed(sb::DecodeSblrManagementEnvelopeRecord(old.data(),old.size()));
    for(std::size_t n=0;n<oracle.size();++n)Failed(sb::DecodeSblrManagementEnvelopeRecord(oracle.data(),n));
    auto e=Carrier(r);Require(sb::DecodeSblrManagementEnvelopeOperand(e).ok,"exact SBOP binary carrier");
    for(unsigned byte=0;byte<16;++byte){auto bad=e;bad.operands[0].value_body[byte]^=0x40;Failed(sb::DecodeSblrManagementEnvelopeOperand(bad));}
    auto bad=e;bad.operands[0].value_body[16]^=1;Failed(sb::DecodeSblrManagementEnvelopeOperand(bad));
    fail_hash=true;Failed(sb::EncodeSblrManagementEnvelopeRecord(r));Failed(sb::DecodeSblrManagementEnvelopeRecord(oracle.data(),oracle.size()));fail_hash=false;
    AllocationFailures([&]{return sb::EncodeSblrManagementEnvelopeRecord(r);});
    AllocationFailures([&]{return sb::DecodeSblrManagementEnvelopeRecord(oracle.data(),oracle.size());});
  }
  auto payload=Record(K::payload,kUuidA);
  auto maximum=Record(K::operation,kUuidA);std::size_t remaining=65512-Oracle(maximum).size();
  std::size_t last=0;
  for(std::size_t i=0;i<maximum.fields.size()&&remaining;++i){auto& f=maximum.fields[i];if(f.name.find("uuid")!=std::string::npos)continue;
    const auto add=std::min(remaining,16384-f.value.size());f.value.insert(f.value.end(),add,'x');remaining-=add;last=i;
  }
  const auto limit_wire=Oracle(maximum);Require(!remaining&&limit_wire.size()==65512&&sb::EncodeSblrManagementEnvelopeRecord(maximum).canonical_bytes==limit_wire&&sb::DecodeSblrManagementEnvelopeRecord(limit_wire.data(),limit_wire.size()).ok,"exact maximum frame");
  maximum.fields[last].value.push_back('x');Bad(maximum);
  auto field_limit=Record(K::diagnostic,kUuidA);field_limit.fields[5].value.assign(16384,'x');Require(sb::EncodeSblrManagementEnvelopeRecord(field_limit).ok,"maximum text field");field_limit.fields[5].value.push_back('x');Bad(field_limit);
  for(const Bytes value:{Bytes{0},Bytes{0x1f},Bytes{0x7f},Bytes{0xc2,0x80},Bytes{0x80},Bytes{0xc0,0xaf},Bytes{0xe0,0x80,0xaf},Bytes{0xed,0xa0,0x80},Bytes{0xf0,0x80,0x80,0xaf},Bytes{0xf4,0x90,0x80,0x80},Bytes{0xf5,0x80,0x80,0x80},Bytes{0xe2,0x82}}){
    auto bad=Record(K::diagnostic,kUuidA);bad.fields[5].value=value;Bad(bad);
  }
  for(const Bytes value:{Bytes{0xc2,0xa0},Bytes{0xe4,0xb8,0xad},Bytes{0xf0,0x9f,0x98,0x80}}){
    auto good=Record(K::diagnostic,kUuidA);good.fields[5].value=value;const auto wire=Oracle(good);
    Require(sb::EncodeSblrManagementEnvelopeRecord(good).ok&&sb::DecodeSblrManagementEnvelopeRecord(wire.data(),wire.size()).ok,"valid non-ASCII text retained");
  }
  for(unsigned width=0;width<=65;++width)if(width!=32){auto bad=payload;bad.fields[4].value.resize(width);Bad(bad);}
  for(unsigned byte=0;byte<32;++byte){auto bad=payload;bad.fields[4].value[byte]^=1;Bad(bad);}
  std::cout<<"PASS management binary codec checks="<<checks<<" codec_only=true\n";
}
