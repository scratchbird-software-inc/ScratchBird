// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_prepared_coordination_registry.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace scratchbird::engine::internal_api;

namespace {

unsigned checks=0;
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
void RequireStaleMetadata(const EngineApiDiagnostic& diagnostic) {
  Require(diagnostic.error && diagnostic.code=="SBLR.PARAMETER.STALE" &&
          diagnostic.canonical_metadata.has_value(),"stale parameter failure is not registered");
  const auto& m=*diagnostic.canonical_metadata;
  Require(m.code==diagnostic.code && m.is_failure &&
          m.severity==scratchbird::core::diagnostics::CanonicalSeverity::error &&
          m.sqlstate=="not_applicable" && m.numeric_binding=="not_applicable" &&
          m.retry_class=="only_after_verified_authority_revalidation_and_fresh_receipt" &&
          m.required_outcome=="refuse_without_changing_authoritative_transaction_outcome" &&
          m.diagnostic_class=="SBLR","stale parameter metadata differs from owning contract");
  Require(scratchbird::core::uuid::IsEngineIdentityUuid(EngineUuid{diagnostic.occurrence_uuid}),
          "stale occurrence is not binary UUIDv7");
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary);
  Require(bool(input),"journal not found");
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path,std::string_view bytes) {
  std::ofstream output(path,std::ios::binary|std::ios::trunc);
  output.write(bytes.data(),bytes.size());output.close();
  Require(bool(output),"fixture journal write failed");
}
std::uint64_t Get(std::string_view bytes,std::size_t at,std::size_t width) {
  std::uint64_t n=0;
  for(std::size_t i=0;i<width;++i)n|=std::uint64_t(static_cast<unsigned char>(bytes.at(at+i)))<<(8*i);
  return n;
}
void Set(std::string& bytes,std::size_t at,std::uint64_t value,std::size_t width) {
  for(std::size_t i=0;i<width;++i)bytes.at(at+i)=static_cast<char>(value>>(8*i));
}
void Reseal(std::string& bytes,std::size_t offset) {
  const auto length=Get(bytes,offset+8,4);
  std::string material="ScratchBird.SblrPreparedCoordinationRegistry.V2";
  auto record=bytes.substr(offset,length);record[6]=0;
  std::fill(record.begin()+168,record.begin()+200,0);material+=record;
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const std::uint8_t*>(material.data()),material.size());
  Require(digest.ok(),"independent decision hash failed");
  std::copy(digest.digest.begin(),digest.digest.end(),bytes.begin()+offset+168);
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    path=std::filesystem::temp_directory_path()/
      ("sb-prepared-coordination-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(path),"private fixture directory collision");
  }
  ~Directory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};

EngineUuid Id(scratchbird::core::platform::UuidKind kind) {
  static auto next = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  scratchbird::core::platform::TypedUuid typed;
  if (scratchbird::core::uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto value =
        scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++next);
    Require(value.ok(), "durable coordination UUID generation failed");
    typed = value.value;
  } else {
    const auto value =
        scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(++next);
    Require(value.ok(), "compatibility coordination UUID generation failed");
    const auto classified =
        scratchbird::core::uuid::MakeTypedUuid(kind, value.value);
    Require(classified.ok(), "coordination UUID classification failed");
    typed = classified.value;
  }
  return typed.value;
}

EngineRequestContext Context(const std::filesystem::path& path) {
  EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid =
      Id(scratchbird::core::platform::UuidKind::database);
  context.session_uuid =
      Id(scratchbird::core::platform::UuidKind::session);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_prepared_coordination");
  return context;
}
void SaveContext(const std::filesystem::path& path,const EngineRequestContext& c) {
  std::string bytes;
  bytes.append(reinterpret_cast<const char*>(c.database_uuid.bytes.data()),16);
  bytes.append(reinterpret_cast<const char*>(c.session_uuid.bytes.data()),16);
  Write(path,bytes);
}
EngineRequestContext LoadContext(const std::filesystem::path& path) {
  auto c=Context(path);const auto bytes=Read(path.string()+".test_context");
  Require(bytes.size()==32,"fixture context is not binary32");
  std::copy_n(bytes.begin(),16,c.database_uuid.bytes.begin());
  std::copy_n(bytes.begin()+16,16,c.session_uuid.bytes.begin());
  return c;
}

}  // namespace

int main(int argc,char** argv) {
  try {
    if(argc==3&&std::string_view(argv[1])=="--recover") {
      auto c=LoadContext(argv[2]);const auto operation=Id(scratchbird::core::platform::UuidKind::object);
      Require(!BeginSblrPreparedCoordination(c,operation).ok,
              "fresh process admitted unresolved provisional identity");
      auto admin=c;admin.trace_tags={"right:SBLR_PREPARED_COORDINATION_ADMIN"};
      Require(!RecoverSblrPreparedCoordinationRegistry(admin).error,"fresh process recovery failed");
      const auto begin=BeginSblrPreparedCoordination(c,operation);
      Require(begin.ok&&begin.snapshot.private_handle!=0,"fresh process cannot admit after revocation");
      Require(!RecoverSblrPreparedCoordinationRegistry(admin).error,"child cleanup recovery failed");
      std::cout<<"prepared_coordination_fresh_process checks="<<checks<<" failures=0\n";
      return 0;
    }
    Directory directory;
    const auto base=directory.path/"node";
    const auto journal=base.string()+".sb.sblr_prepared_coordination.v1";
    auto context = Context(base);
    const auto operation = Id(scratchbird::core::platform::UuidKind::object);
    const auto begin = BeginSblrPreparedCoordination(context, operation);
    Require(begin.ok &&
                begin.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation,
            "preparation coordination begin failed");
    Require(begin.snapshot.provisional_prepared_generation ==
                begin.snapshot.coordinator_generation,
            "provisional generation did not match begin generation");
    Require(begin.snapshot.private_handle != 0,
            "preparation coordination handle was not issued");

    auto admin=context;admin.trace_tags={"right:SBLR_PREPARED_COORDINATION_ADMIN"};
    const auto begin_bytes=Read(journal);
    const auto first_size=Get(begin_bytes,8,4);
    Require(begin_bytes.substr(0,4)=="SBPC"&&Get(begin_bytes,4,2)==2&&
            Get(begin_bytes,6,1)==1&&Get(begin_bytes,first_size+6,1)==2&&
            begin_bytes.size()==2*first_size,"binary evidence/state frame shape differs");
    auto resealed=begin_bytes;Reseal(resealed,0);Reseal(resealed,first_size);
    Require(resealed==begin_bytes,"independent decision hash oracle differs");
    const std::array<const EngineUuid*,5> identities{&begin.snapshot.coordination_uuid,
      &operation,&context.database_uuid,&context.session_uuid,&begin.snapshot.provisional_prepared_uuid};
    for(std::size_t i=0;i<identities.size();++i)
      for(std::size_t n=0;n<16;++n)
        Require(static_cast<unsigned char>(begin_bytes.at(16+16*i+n))==identities[i]->bytes[n],
                "journal UUID is not exact raw16");
    const auto reject=[&](const std::string& bytes) {
      Write(journal,bytes);
      const auto failure=RecoverSblrPreparedCoordinationRegistry(admin);
      Require(failure.error,"malformed journal recovered successfully");
      RequireStaleMetadata(failure);
      Require(!BeginSblrPreparedCoordination(context,operation).ok,
              "warm process admitted begin over corrupt journal");
      Require(Read(journal)==bytes,"malformed bytes were repaired or overwritten");
    };
    for(std::size_t size=0;size<begin_bytes.size();++size)reject(begin_bytes.substr(0,size));
    reject("SBPCR1\tEVIDENCE\told-text-identities\n");
    for(std::size_t role=0;role<5;++role) {
      for(unsigned version=0;version<16;++version) {
        if(version==7)continue;
        auto bad=begin_bytes;
        for(const auto offset:{std::size_t(0),static_cast<std::size_t>(first_size)}) {
          bad[offset+16+16*role+6]=static_cast<char>(version<<4);Reseal(bad,offset);
        }
        reject(bad);
      }
    }
    for(const auto field:{4u,6u,7u,12u,13u,120u,121u,160u,162u}) {
      auto bad=begin_bytes;bad[field]=static_cast<char>(255);Reseal(bad,0);reject(bad);
    }
    Write(journal,begin_bytes);

    const auto acquire = AcquireSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        begin.snapshot.coordinator_generation);
    Require(acquire.ok &&
                acquire.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                acquire.snapshot.coordinator_generation >
                    begin.snapshot.coordinator_generation,
            "preparation coordination acquire failed");
    const auto acquired_bytes=Read(journal);
    const auto stale = SealSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        begin.snapshot.coordinator_generation,
        begin.snapshot.provisional_prepared_uuid,
        begin.snapshot.provisional_prepared_generation,
        "sha256:" + std::string(64, 'a'));
    Require(!stale.ok && stale.diagnostic.code == "SBLR.PARAMETER.STALE",
            "stale preparation coordination generation was admitted");
    RequireStaleMetadata(stale.diagnostic);
    const auto seal = SealSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        acquire.snapshot.coordinator_generation,
        begin.snapshot.provisional_prepared_uuid,
        begin.snapshot.provisional_prepared_generation,
        "sha256:" + std::string(64, 'b'));
    Require(seal.ok &&
                seal.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation,
            "preparation coordination seal failed");
    const auto sealed_bytes=Read(journal);
    // A validly rehashed begun->sealed edge must not bypass acquisition.
    auto skipped=begin_bytes+sealed_bytes.substr(acquired_bytes.size());
    auto at=begin_bytes.size();
    for(unsigned phase=0;phase<2;++phase) {
      Set(skipped,at+112,begin.snapshot.coordinator_generation,8);Reseal(skipped,at);
      at+=Get(skipped,at+8,4);
    }
    reject(skipped);
    auto changed_kind=acquired_bytes;at=begin_bytes.size();
    for(unsigned phase=0;phase<2;++phase) {
      changed_kind[at+12]=2;Reseal(changed_kind,at);at+=Get(changed_kind,at+8,4);
    }
    reject(changed_kind);
    Write(journal,sealed_bytes);

    const auto second = BeginSblrPreparedCoordination(
        context, Id(scratchbird::core::platform::UuidKind::object));
    Require(second.ok &&
                second.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                second.snapshot.coordinator_generation >
                    seal.snapshot.coordinator_generation,
            "second preparation coordination begin failed");
    Require(RecoverSblrPreparedCoordinationRegistry(admin).code == "OK",
            "coordination registry recovery failed");
    const auto hidden = AcquireSblrPreparedCoordination(
        context, second.snapshot.coordination_uuid,
        second.snapshot.operation_uuid,
        second.snapshot.coordinator_generation);
    Require(!hidden.ok && hidden.diagnostic.code == "SECURITY.ACCESS_DENIED",
            "recovery did not revoke unfinished coordination");
    const auto third = BeginSblrPreparedCoordination(
        context, Id(scratchbird::core::platform::UuidKind::object));
    Require(third.ok &&
                third.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                third.snapshot.coordinator_generation >
                    second.snapshot.coordinator_generation,
            "post-recovery coordination generation did not advance");

#if defined(__linux__)
    SaveContext(base.string()+".test_context",context);
    const auto child=::fork();Require(child>=0,"fresh-process fork failed");
    if(child==0) {
      ::execl(argv[0],argv[0],"--recover",base.c_str(),static_cast<char*>(nullptr));
      ::_exit(127);
    }
    int status=0;
    Require(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
            "fresh process failed coordination recovery");
    Require(!AcquireSblrPreparedCoordination(context,third.snapshot.coordination_uuid,
              third.snapshot.operation_uuid,third.snapshot.coordinator_generation).ok,
            "stale parent handle survived durable child revocation");
#endif

    Require(RecoverSblrPreparedCoordinationRegistry(admin).code=="OK",
            "final fixture recovery failed");
    std::cout<<"prepared_coordination_binary checks="<<checks<<" failures=0\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
