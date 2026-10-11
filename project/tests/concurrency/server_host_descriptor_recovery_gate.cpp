// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine_host.hpp"
#include "sblr_startup_descriptor_recovery.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace a = scratchbird::engine::internal_api;
namespace s = scratchbird::server;
namespace u = scratchbird::core::uuid;
namespace db = scratchbird::storage::database;
using Uuid = scratchbird::core::platform::Uuid;
unsigned native_opens = 0, native_closes = 0;
extern "C" sb_engine_status_t __real_sb_engine_open(const sb_engine_open_params_v1_t*, sb_engine_handle_t*, sb_engine_result_t*);
extern "C" sb_engine_status_t __wrap_sb_engine_open(const sb_engine_open_params_v1_t* p, sb_engine_handle_t* h, sb_engine_result_t* r) {
  ++native_opens; return __real_sb_engine_open(p,h,r);
}
extern "C" sb_engine_status_t __real_sb_engine_close(sb_engine_handle_t, sb_engine_result_t*);
extern "C" sb_engine_status_t __wrap_sb_engine_close(sb_engine_handle_t h, sb_engine_result_t* r) {
  ++native_closes; return __real_sb_engine_close(h,r);
}
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
Uuid Id() { const auto id = u::IssueRuntimeIdentityV7(); Check(bool(id),"identity"); return *id; }
using Bytes = std::vector<std::uint8_t>;
Bytes Read(const std::string& path) {
  std::ifstream file(path,std::ios::binary); Check(bool(file),"journal read");
  return Bytes(std::istreambuf_iterator<char>(file),{});
}
void Write(const std::string& path,const Bytes& bytes) {
  std::ofstream file(path,std::ios::binary|std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); file.close();
  Check(bool(file),"journal fixture write");
}
int main(int argc,char** argv) try {
  Check(argc==2,"mode"); const std::string mode=argv[1];
  const bool abi=mode.starts_with("abi-"), mismatch=mode=="mismatch";
  const bool corrupt_error=mode.ends_with("corrupt-error"), corrupt_source=mode.ends_with("corrupt-source");
  const bool read_only=mode.ends_with("read-only"), maintenance=mode.ends_with("maintenance"), restricted=mode.ends_with("restricted");
  const bool recover=!read_only&&!maintenance&&!restricted;
  namespace m=scratchbird::core::memory;
  Check(m::ConfigureDefaultMemoryManagerForFixture(m::DefaultLocalEngineMemoryPolicy(),"host_recovery").ok(),"memory policy");
  scratchbird::tests::OwnedTempDirectory files;
  {
  db::DatabaseCreateConfig create;
  create.path=(files.path()/"host.sbdb").string();
  create.database_uuid=u::MakeTypedUuid(scratchbird::core::platform::UuidKind::database,Id()).value;
  create.filespace_uuid=u::MakeTypedUuid(scratchbird::core::platform::UuidKind::filespace,Id()).value;
  create.page_size=16384;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  Check(db::CreateDatabaseFile(create).ok(),"credentialed database");
  a::EngineRequestContext context;
  context.database_path=create.path; context.database_uuid=create.database_uuid.value;
  context.session_uuid=Id(); context.principal_uuid=Id(); context.transaction_uuid=Id();
  context.statement_receipt_uuid=Id(); context.security_context_present=true;
  context.statement_metadata_snapshot_engine_owned=true;
  scratchbird::engine::sblr::SblrErrorVectorEntryV1 error;
  error.occurrence_ordinal=1; error.diagnostic_uuid=Id().bytes; error.diagnostic_generation=1;
  error.precedence_ordinal=1; error.severity_code=1; error.redaction_class=1;
  error.safe_fields_sha256=scratchbird::engine::sblr::SblrErrorVectorEmptySafeFieldsHashV1();
  const auto ev=a::IssueSblrErrorVectorDescriptorV1(context,context.statement_receipt_uuid.bytes,Id().bytes,1,Id().bytes,1,{error});
  Check(ev.ok,"real error-vector issuance");
  scratchbird::engine::sblr::SblrSourceMapEntryV1 entry;
  entry.node_id=1; entry.source_artifact_uuid=Id().bytes;
  entry.source_artifact_generation=1; entry.byte_length=4;
  a::SblrSourceMapHashV2 bound{}; bound.fill(0xa5);
  const auto sm=a::IssueSblrSourceMapDescriptorV1(context,context.statement_receipt_uuid,bound,Id(),1,{entry});
  Check(sm.ok,"real source-map issuance");
  const auto ep=create.path+".sb.sblr_error_vector_registry.v1";
  const auto sp=create.path+".sb.sblr_source_map_registry.v1";
  const auto original_e=Read(ep), original_s=Read(sp);
  const auto error_live=[&] {return a::LookupSblrErrorVectorDescriptorV1(context,context.statement_receipt_uuid.bytes,ev.snapshot.descriptor_uuid,1).ok;};
  const auto source_live=[&] {return a::LookupSblrSourceMapDescriptorV1(context,context.statement_receipt_uuid,sm.snapshot.descriptor_uuid,1,bound,sm.snapshot.registry_snapshot_uuid,1).ok;};
  Check(error_live()&&source_live(),"both descriptors active before startup");
  if(mismatch) {
    auto foreign=context; foreign.database_uuid=Id();
    const auto refused=a::RecoverSblrStartupDescriptorsV1(foreign);
    Check(!refused.ok()&&refused.diagnostic.code=="SECURITY.ACCESS_DENIED", "exact database mismatch refusal");
    Check(Read(ep)==original_e&&Read(sp)==original_s,"foreign recovery has no effects");
  } else {
    if(corrupt_error) Write(ep,{0x01,0x02});
    if(corrupt_source) Write(sp,{0x03,0x04});
    const auto open=[&](bool expect_failure) {
      const auto expected=corrupt_error?"SBLR.ERROR_VECTOR.STALE":"SBLR.SOURCE_MAP.STALE";
      if(abi) {
        sb_engine_open_params_v1_t params{};
        params.struct_size=sizeof(params); params.abi_version=SB_ENGINE_ABI_VERSION_PACKED;
        params.database_path_utf8=create.path.data(); params.database_path_size=create.path.size();
        params.mode=read_only?SB_ENGINE_OPEN_READ_ONLY:maintenance?SB_ENGINE_OPEN_MAINTENANCE:SB_ENGINE_OPEN_NORMAL;
        sb_engine_handle_t engine=nullptr; sb_engine_result_t result=nullptr;
        const auto status=sb_engine_open(&params,&engine,&result);
        if(expect_failure) {
          scratchbird::server_engine_bridge::EngineDiagnosticSnapshot source;
          Check(status==SB_ENGINE_STATUS_CONFLICT&&!engine&&result&&
                scratchbird::server_engine_bridge::CopyEngineDiagnosticSnapshot(result,0,&source)&&source.code==expected,
                "ABI exact recovery failure");
        } else Check(status==SB_ENGINE_STATUS_OK&&engine,"ABI successful open");
        if(result) Check(sb_engine_result_release(result)==SB_ENGINE_STATUS_OK,"native result release");
        if(engine) Check(sb_engine_close(engine,nullptr)==SB_ENGINE_STATUS_OK,"real ABI close");
      } else {
        s::ServerBootstrapConfig config;
        config.database_default_path=create.path; config.embedded_direct_mode=true;
        config.database_open_mode=read_only?"read_only":maintenance?"maintenance":restricted?"restricted":"normal";
        auto result=s::StartHostedEngine(config);
        if(expect_failure) {
          Check(!result.ok()&&result.descriptor_recovery_failure&&result.descriptor_recovery_failure->code==expected,
                "host retains exact source failure");
          Check(!result.state.databases.empty()&&!result.state.databases.front().database_open&&
                result.state.databases.front().write_admission_fenced,"failed startup is fenced");
          Check(result.diagnostics.front().occurrence_uuid==result.descriptor_recovery_failure->occurrence_uuid,
                "host preserves source occurrence");
        } else Check(result.ok()&&result.state.databases.front().database_open,"host ready after recovery");
        Check(native_opens==0&&native_closes==0,"host never allocates transient native facade");
      }
    };
    open(corrupt_error||corrupt_source);
    if(corrupt_error||corrupt_source) {
      if(corrupt_error) {
        Check(Read(sp)==original_s,"first-stage failure leaves source map untouched");
        Write(ep,original_e);
      } else {
        Check(!error_live()&&Read(ep)!=original_e,"first-stage revocations survive second-stage failure");
        Write(sp,original_s);
      }
      open(false);
    }
    Check(error_live()==!recover&&source_live()==!recover,"mode-specific actual descriptor revocation");
    if(!recover) Check(Read(ep)==original_e&&Read(sp)==original_s,"non-normal modes preserve journal bytes");
    if(recover) {
      const auto after_e=Read(ep),after_s=Read(sp);
      Check(a::RecoverSblrStartupDescriptorsV1(context).ok()&&Read(ep)==after_e&&Read(sp)==after_s,
            "successful recovery is idempotent on durable state");
    }
  }
  }
  files.Cleanup(); std::cout<<"host_descriptor_recovery=PASS "<<mode<<'\n'; return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
