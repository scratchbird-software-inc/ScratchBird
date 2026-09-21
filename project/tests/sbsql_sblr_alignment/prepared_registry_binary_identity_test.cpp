// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual private registry publication/reload, not SQL or descriptor admission.
#include "sblr_prepared_statement_registry.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace { long fail_after=-1; unsigned allocation_faults=0; }
void* operator new(std::size_t n) {
  if(fail_after==0)throw std::bad_alloc();
  if(fail_after>0)--fail_after;
  if(auto* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept {
  try{return ::operator new(n);}catch(...){return nullptr;}
}
void* operator new[](std::size_t n,const std::nothrow_t& tag) noexcept{return ::operator new(n,tag);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}

namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0;
void Require(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}
void RequireDiagnostic(const api::EngineApiDiagnostic& diagnostic,
                       std::string_view code,std::string_view retry,
                       std::string_view outcome,std::string_view sqlstate,
                       std::string_view family) {
  Require(diagnostic.error && diagnostic.code==code && diagnostic.canonical_metadata.has_value(),
          "failure has no exact registered source metadata");
  const auto& m=*diagnostic.canonical_metadata;
  Require(m.code==code && m.severity==scratchbird::core::diagnostics::CanonicalSeverity::error &&
          m.is_failure && m.retry_class==retry && m.required_outcome==outcome &&
          m.sqlstate==sqlstate && m.numeric_binding=="not_applicable" && m.diagnostic_class==family,
          "registered failure metadata differs from owning contract");
  Require(scratchbird::core::uuid::IsEngineIdentityUuid(api::EngineUuid{diagnostic.occurrence_uuid}),
          "failure occurrence is not source-owned binary UUIDv7");
}
api::EngineUuid Id(unsigned n) {
  api::EngineUuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = n;
  return id;
}
api::EngineRequestContext Context(const std::filesystem::path& node) {
  api::EngineRequestContext c;
  c.database_path=node.string();
  c.database_uuid=Id(1); c.session_uuid=Id(2); c.principal_uuid=Id(3);
  c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_prepared_statement_registry"};
  return c;
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    path = std::filesystem::temp_directory_path() /
        ("sb-prepared-binary-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(path), "private test directory collision");
  }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::vector<std::uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(bool(input), "registry file missing");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  output.close(); Require(bool(output), "test corruption write failed");
}
std::uint32_t U32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return std::uint32_t(bytes.at(offset)) | (std::uint32_t(bytes.at(offset+1)) << 8) |
         (std::uint32_t(bytes.at(offset+2)) << 16) | (std::uint32_t(bytes.at(offset+3)) << 24);
}
void HashAt(std::vector<std::uint8_t>& bytes, std::size_t offset,
            std::string_view domain, std::size_t start) {
  std::fill_n(bytes.begin()+offset, 32, 0);
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin()+start, bytes.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
  Require(hash.ok(), "oracle hash failed");
  std::copy(hash.digest.begin(), hash.digest.end(), bytes.begin()+offset);
}
void Reseal(std::vector<std::uint8_t>& bytes) {
  HashAt(bytes, 160+276, "ScratchBird.SblrPreparedStatementRegistryRecord.V2", 160);
  const std::vector<std::uint8_t> payload(bytes.begin()+160, bytes.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(payload);
  Require(hash.ok(), "oracle payload hash failed");
  std::copy(hash.digest.begin(), hash.digest.end(), bytes.begin()+72);
  HashAt(bytes, 104, "ScratchBird.SblrPreparedStatementRegistry.V2", 0);
}
api::SblrPreparedStatementRegistryRecordV1 Record() {
  api::SblrPreparedStatementRegistryRecordV1 r;
  r.canonical_name="fixture"; r.body_operation_id="fixture.persistence";
  r.body_operation_family="fixture.family"; r.body_result_shape="fixture.shape";
  r.source_free_parameterized_query_template=true;
  r.parameter_set_uuid=Id(11); r.parameter_prepared_statement_uuid=Id(12);
  r.parameter_set_snapshot_uuid=Id(13); r.parameter_set_generation=21;
  r.parameter_set_snapshot_generation=22; r.ordered_slot_table_sha256="sha256:"+std::string(64,'a');
  r.statement_uuid=Id(14).bytes; r.statement_name_uuid=Id(15).bytes;
  r.preparing_receipt_uuid=Id(16).bytes; r.prepared_generation=23;
  r.descriptor_sha256.fill(0x12);
  // Opaque already-admitted material at this private persistence boundary.
  // This fixture does not prove admission or execute any of these byte arrays.
  r.canonical_descriptor_bytes={1,2,3}; r.canonical_container_bytes={4,5,6};
  r.canonical_execution_envelope_bytes={7,8,9}; r.canonical_prepare_result_bytes.resize(160,0x23);
  return r;
}
}
int main(int argc, char** argv) {
  try {
    if(argc==3 && std::string_view(argv[1])=="--reload") {
      const auto result=api::LoadSblrPreparedStatementRegistryV1(Context(argv[2]));
      Require(result.ok && result.found && result.snapshot.records.size()==1,
              "fresh process did not load durable registry");
      const auto& record=result.snapshot.records[0];
      Require(record.parameter_set_uuid==Id(11) &&
              record.parameter_prepared_statement_uuid==Id(12) &&
              record.parameter_set_snapshot_uuid==Id(13),
              "fresh process lost binary parameter identity");
      std::cout<<"prepared_registry_fresh_process checks="<<checks<<" failures=0\n";
      return 0;
    }
    Directory temporary;
    auto c=Context(temporary.path/"node");
    auto path=std::filesystem::path(c.database_path+".sb.sblr_prepared_statement_registry.v1.");
    path+=*scratchbird::core::uuid::EngineIdentityPathComponent(c.session_uuid);
    const auto record=Record();
    const auto published=api::PublishSblrPreparedStatementV1(c,record);
    Require(published.ok && published.found, "binary record publication failed");
    const auto original=Read(path);
    Require(std::equal(original.begin(),original.begin()+8,"SBPSRG2\0") &&
            original[8]==2 && original[9]==0 && original[164]==2 && original[165]==0,
            "session or record format was not versioned");
    auto resealed=original; Reseal(resealed);
    Require(resealed==original, "independent domain/offset hash oracle disagrees");
    std::size_t start=160+320;
    for(unsigned field=0;field<4;++field) start+=U32(original,160+224+4*field);
    for(unsigned field=4;field<=6;++field) {
      Require(U32(original,160+224+4*field)==16,"parameter identity is not raw16");
      const auto id=Id(field+7);
      Require(std::equal(id.bytes.begin(),id.bytes.end(),original.begin()+start+16*(field-4)),
              "parameter identity bytes changed");
    }
    const auto check_reload=[&]() {
      const auto loaded=api::LoadSblrPreparedStatementRegistryV1(c);
      Require(loaded.ok && loaded.found && loaded.snapshot.records.size()==1,"durable binary reload failed");
      const auto& actual=loaded.snapshot.records[0];
      Require(actual.parameter_set_uuid==record.parameter_set_uuid &&
              actual.parameter_prepared_statement_uuid==record.parameter_prepared_statement_uuid &&
              actual.parameter_set_snapshot_uuid==record.parameter_set_snapshot_uuid,
              "durable parameter identity not preserved");
    };
    check_reload();
    Require(api::PublishSblrPreparedStatementV1(c,record).exact_replay,"exact publication replay lost");
    Require(api::ResolveActiveSblrPreparedStatementCapabilityV1(c,Id(12),23).ok,
            "binary parameter capability lookup failed");
    Require(api::ResolveActiveSblrPreparedStatementCapabilityV1(c,Id(14),23).ok,
            "binary statement capability lookup failed");
    const auto stale=api::ResolveActiveSblrPreparedStatementCapabilityV1(c,Id(12),24);
    Require(!stale.ok,"stale prepared generation admitted");
    RequireDiagnostic(stale.diagnostic,"PREPARED.REGISTRY.STALE",
        "only_after_verified_prepared_authority_revalidation_and_fresh_request",
        "refuse_without_registry_mutation_or_transaction_outcome_change","55000","PREPARED");
    auto wrong_owner=c; wrong_owner.principal_uuid=Id(4);
    Require(!api::LoadSblrPreparedStatementRegistryV1(wrong_owner).ok,"wrong principal sees registry");
    Require(Read(path)==original,"read/replay changed durable bytes");
#if defined(__linux__)
    const auto saved=std::filesystem::path(path.string()+".fixture_saved");
    std::filesystem::rename(path,saved);
    std::filesystem::create_symlink(saved.filename(),path);
    const auto unreadable=api::LoadSblrPreparedStatementRegistryV1(c);
    Require(!unreadable.ok,"no-follow registry read admitted a symlink");
    RequireDiagnostic(unreadable.diagnostic,"SBLR.EXECUTION_FAILED","false",
        "fail_current_operation_preserve_authoritative_transaction_outcome","not_applicable","SBLR");
    Require(Read(saved)==original,"read failure modified existing registry");
    std::filesystem::remove(path);std::filesystem::rename(saved,path);
#endif
    const auto reject=[&](const std::vector<std::uint8_t>& bytes) {
      Write(path,bytes);
      const auto loaded=api::LoadSblrPreparedStatementRegistryV1(c);
      Require(!loaded.ok && !loaded.found,"malformed registry admitted or treated as absent");
      RequireDiagnostic(loaded.diagnostic,"PREPARED.REGISTRY.INVALID",
          "only_after_verified_registry_recovery_and_fresh_request",
          "refuse_without_registry_mutation_or_transaction_outcome_change","55000","PREPARED");
      Require(Read(path)==bytes,"refusal modified registry");
    };
    for(std::size_t n=0;n<original.size();++n)
      reject({original.begin(),original.begin()+n});
    for(const auto offset:{std::size_t(8),std::size_t(164)}) {
      auto bad=original; bad[offset]=1; Reseal(bad); reject(bad);
    }
    auto bad=original; bad[6]='1'; Reseal(bad); reject(bad);
    for(const auto offset:{std::size_t(32),std::size_t(48),std::size_t(136),
                          std::size_t(216),std::size_t(232),std::size_t(248),
                          start,start+16,start+32}) {
      for(unsigned version=0;version<16;++version) {
        if(version==7) continue;
        bad=original; bad[offset+6]=static_cast<std::uint8_t>(version<<4);
        Reseal(bad); reject(bad);
      }
      bad=original; bad[offset+8]=0; Reseal(bad); reject(bad);
      bad=original; std::fill_n(bad.begin()+offset,16,0); Reseal(bad); reject(bad);
    }
    // Preserve total length and reseal: moving a byte between UUID fields must
    // still be rejected by exact-width validation, not merely the outer hash.
    bad=original; bad[160+224+4*4]=15; bad[160+224+4*5]=17; Reseal(bad); reject(bad);
    Write(path,original); check_reload();
#if defined(__linux__)
    const auto open_descriptors=[]() {
      std::size_t count=0;
      for(const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;++count;
      }
      return count;
    };
    const auto descriptor_count=open_descriptors();
    bool completed=false;
    for(long point=0;point<1024;++point) {
      try {
        fail_after=point;
        const auto loaded=api::LoadSblrPreparedStatementRegistryV1(c);
        fail_after=-1;
        Require(loaded.ok&&loaded.found,"allocation retry did not load original registry");
        completed=true;break;
      } catch(const std::bad_alloc&) {
        fail_after=-1;++allocation_faults;
        Require(open_descriptors()==descriptor_count,"registry read leaked a descriptor on allocation failure");
        Require(Read(path)==original,"allocation failure changed registry bytes");
      }
    }
    Require(completed&&allocation_faults>0,"registry read allocation sweep did not complete");
    Require(open_descriptors()==descriptor_count,"successful registry reload leaked a descriptor");
#endif
#if defined(__linux__)
    const auto child=::fork();
    Require(child>=0,"fresh-process fork failed");
    if(child==0) {
      ::execl(argv[0],argv[0],"--reload",c.database_path.c_str(),static_cast<char*>(nullptr));
      ::_exit(127);
    }
    int status=0;
    Require(::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "fresh executable failed to recover binary registry");
#endif
    for(unsigned slot=0;slot<3;++slot) {
      auto invalid=record;
      auto* id=slot==0?&invalid.parameter_set_uuid:slot==1?&invalid.parameter_prepared_statement_uuid:&invalid.parameter_set_snapshot_uuid;
      id->bytes[6]=0x40;
      const auto result=api::PublishSblrPreparedStatementV1(c,invalid);
      Require(!result.ok,"non-v7 source parameter identity published");
      RequireDiagnostic(result.diagnostic,"SBLR.OPERAND_INVALID","false","refuse",
                        "not_applicable","SBLR");
      Require(Read(path)==original,"invalid source mutated registry");
    }
    auto parameterless=record;
    parameterless.source_free_parameterless_query_template=true;
    parameterless.source_free_parameterized_query_template=false;
    parameterless.parameter_set_uuid={}; parameterless.parameter_prepared_statement_uuid={};
    parameterless.parameter_set_snapshot_uuid={}; parameterless.parameter_set_generation=0;
    parameterless.parameter_set_snapshot_generation=0; parameterless.ordered_slot_table_sha256.clear();
    auto second=c; second.session_uuid=Id(90);
    Require(api::PublishSblrPreparedStatementV1(second,parameterless).ok,"nil parameterless identity rejected");
    std::cout<<"prepared_registry_binary checks="<<checks<<" allocation_faults="<<allocation_faults<<" failures=0\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<"prepared_registry_binary checks="<<checks<<" FAIL "<<error.what()<<'\n';
    return 1;
  }
}
