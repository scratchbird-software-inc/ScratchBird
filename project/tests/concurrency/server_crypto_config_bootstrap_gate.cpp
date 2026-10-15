// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "config.hpp"
#include "runtime_crypto_pool_owner.hpp"
#include "process_crypto_owner.hpp"
#include "startup.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"
#include <openssl/crypto.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace s=scratchbird::server;
namespace m=scratchbird::core::memory;
namespace u=scratchbird::core::uuid;
namespace h=scratchbird::core::hash;
namespace r=scratchbird::core::runtime;
using scratchbird::core::platform::Uuid;
std::atomic<unsigned> rand_calls{0}, entropy_calls{0};
std::atomic<unsigned> host_probe_calls{0};
bool provider_allowed=false;
std::string_view test_mode;
int status_descriptor=-1;
unsigned write_calls=0;
extern "C" int __real_uname(struct utsname*);
extern "C" int __wrap_uname(struct utsname* out) {
  ++host_probe_calls;
  const auto result=__real_uname(out);
  if(result==0&&test_mode=="host_platform_refusal")std::memcpy(out->release,"6.5.0",6);
  return result;
}
extern "C" ssize_t __real_getrandom(void*,std::size_t,unsigned);
extern "C" ssize_t __wrap_getrandom(void* out,std::size_t size,unsigned flags) {
  ++entropy_calls;
  if(test_mode=="diag_source_failure") {errno=EIO; return -1;}
  return __real_getrandom(out,size,flags);
}
extern "C" ssize_t __real_write(int,const void*,std::size_t);
extern "C" ssize_t __wrap_write(int fd,const void* data,std::size_t size) {
  if(fd==status_descriptor && fd>=0) {
    ++write_calls;
    if(test_mode=="diag_write_eintr") {errno=EINTR; return -1;}
    if(test_mode=="diag_write_partial" && size>3) size=3;
  }
  return __real_write(fd,data,size);
}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int bytes) {
  ++rand_calls;
  return provider_allowed ? __real_RAND_bytes(out,bytes) : 0;
}
void Check(bool value,const char* reason) {
  if(!value) {std::cerr<<reason<<'\n'; throw std::runtime_error(reason);}
}
void DiagnosticBoundary() {
  using E=u::CryptoBootstrapDiagnosticError;
  bool failed=false;
  try {
    s::BootstrapDiagnosticScope outer;
    const s::ServerDiagnostic first{"CONFIG.MALFORMED","config.malformed",
        s::ServerDiagnosticSeverity::kError,"Invalid configuration",{{"reason","fixture"}}};
    const auto copied=first;
    Check(first.occurrence_uuid==copied.occurrence_uuid &&
          u::IsEngineIdentityUuid(Uuid{first.occurrence_uuid}),"source-owned startup occurrence survives copy");
    {
      s::BootstrapDiagnosticScope inner;
      const s::ServerDiagnostic next{"CONFIG.MALFORMED","config.malformed"};
      Check(next.occurrence_uuid!=first.occurrence_uuid,"nested source emits fresh occurrence");
    }
    const s::ServerDiagnostic after{"CONFIG.MALFORMED","config.malformed"};
    Check(after.occurrence_uuid!=first.occurrence_uuid,"inner scope restores outer source");
    Check(rand_calls==0,"explicit startup scope never calls ordinary provider");
    bool ordinary_refused=false;
    std::thread worker([&] {
      try {const s::ServerDiagnostic other{"CONFIG.MALFORMED","config.malformed"};}
      catch(const s::BootstrapDiagnosticIdentityFailure&) {return;}
      catch(const std::runtime_error&) {ordinary_refused=true;}
    });
    worker.join();
    Check(ordinary_refused && rand_calls>0,"startup source selection is thread-local, not a global fallback");
    Check(u::SealCryptoBootstrapDiagnosticIdentities()==E::none,"host seals startup source");
    const s::ServerDiagnostic forbidden{"CONFIG.MALFORMED","config.malformed"};
  } catch(const s::BootstrapDiagnosticIdentityFailure& error) {
    failed=true;
    Check(error.error()==(test_mode=="diag_source_failure"?E::source_failure:E::sealed),
          "terminal startup source failure retains exact typed outcome");
    int descriptors[2]; Check(::pipe(descriptors)==0,"status pipe");
    status_descriptor=descriptors[1];
    Check(!s::WriteBootstrapDiagnosticFailureStatus(status_descriptor,error.error()) && write_calls==0,
          "blocking pipe refused before any potentially blocking write");
    Check(::fcntl(status_descriptor,F_SETFL,O_NONBLOCK)==0,"fixture owns nonblocking sink flags");
    if(test_mode=="diag_write_full") {
      std::array<char,4096> padding{};
      while(__real_write(status_descriptor,padding.data(),padding.size())>0) {}
      Check(errno==EAGAIN || errno==EWOULDBLOCK,"fill actual nonblocking pipe to backpressure");
    }
    const bool written=s::WriteBootstrapDiagnosticFailureStatus(status_descriptor,error.error());
    Check(!s::WriteBootstrapDiagnosticFailureStatus(-1,error.error()),"bad sink cannot report delivery");
    Check(!s::WriteBootstrapDiagnosticFailureStatus(status_descriptor,E::none),"success is not a terminal error");
    ::close(descriptors[1]); status_descriptor=-1;
    std::array<char,128> data{};
    const auto size=::read(descriptors[0],data.data(),data.size()); ::close(descriptors[0]);
    if(test_mode=="diag_write_full")
      Check(!written && write_calls==1,"backpressure returns without retry or blocking");
    else if(test_mode=="diag_write_eintr")
      Check(!written && write_calls==32 && size==0,"status-only writes have bounded interruption retry");
    else Check(written && size>0 && std::string_view(data.data(),size).starts_with("startup failed:") &&
               std::string_view(data.data(),size).find("message_vector")==std::string_view::npos,
               "terminal output is bounded status, not an invented canonical diagnostic");
    if(test_mode=="diag_write_partial") Check(write_calls>1,"partial terminal writes actually exercised");
  }
  Check(failed,"terminal source failure must prevent diagnostic publication");
  const auto before=entropy_calls.load();
  bool ordinary_refused=false;
  try {const s::ServerDiagnostic after{"CONFIG.MALFORMED","config.malformed"};}
  catch(const s::BootstrapDiagnosticIdentityFailure&) {}
  catch(const std::runtime_error&) {ordinary_refused=true;}
  Check(ordinary_refused && entropy_calls==before,"unwinding restores ordinary source without OS fallback");
}
int main(int argc,char** argv) try {
  test_mode=argc>1?argv[1]:"default";
  if(test_mode.starts_with("diag")) {DiagnosticBoundary(); return 0;}
  const bool override_budget=test_mode=="override"||test_mode=="host_override";
  scratchbird::tests::OwnedTempDirectory artifacts;
  const auto path=artifacts.path()/"bootstrap.conf";
  {
    std::ofstream out(path);
    out<<"[config]\nformat = SBCD1\n[server.memory]\nenable_platform_memory_probe = false\n";
    if(override_budget) out<<"openssl_budget_bytes = 8388608\n";
    out<<"[server.database]\npolicy_seed_pack_root = "<<SB_DEFAULT_POLICY_PACK_ROOT<<"\n";
    Check(bool(out),"write actual config fixture");
  }
  s::ServerConfigResolutionContext context;
  context.current_directory=artifacts.path(); context.include_system_paths=false;
  s::ServerCliOptions cli; cli.config_path=path.string(); cli.validate_config=true;
  const auto compatible=s::ClassifyServerConfigFormat("SBCD1");
  Check(compatible.accepted && compatible.diagnostic.code.empty() &&
        compatible.diagnostic.occurrence_uuid==std::array<std::uint8_t,16>{},
        "success slot is not an emitted diagnostic occurrence");
  const auto loaded=s::ResolveServerBootstrapConfig(cli,context);
  Check(loaded.ok() && loaded.diagnostics.empty() && rand_calls==0,
        "real successful config must not call RAND before provider admission");
  const auto policy=s::ResolveServerMemoryAllocationPolicy(loaded.config);
  Check(policy.ok() && rand_calls==0,"actual config policy resolution remains provider-free");
  const auto bytes=loaded.config.memory_openssl_budget_bytes;
  Check(bytes==(override_budget?8388608u:4194304u),"use actual selected budget");
  Check(m::ConfigureDefaultMemoryManager(policy.policy,loaded.config.memory_policy_provenance).ok(),
        "install actual shared process manager policy");
  auto& manager=m::DefaultMemoryManager();
  if(test_mode=="host_product") {
    const auto output_path=(artifacts.path()/"product-output.json").string();
    const auto run=[&](const std::string& config_path,bool valid,bool invalid_cli=false) {
      const int output=::open(output_path.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
      Check(output>=0,"owned product evidence file");
      const auto child=::fork();
      if(child<0) {::close(output);Check(false,"fork real server executable");}
      if(child==0) {
        if(::dup2(output,STDOUT_FILENO)<0||::dup2(output,STDERR_FILENO)<0)::_exit(126);
        ::close(output);
        if(invalid_cli)::execl(SB_SERVER_EXECUTABLE,SB_SERVER_EXECUTABLE,"--no-such-option",nullptr);
        else ::execl(SB_SERVER_EXECUTABLE,SB_SERVER_EXECUTABLE,"--validate-config","--config",config_path.c_str(),nullptr);
        ::_exit(127);
      }
      ::close(output);
      int status=0;pid_t waited;
      do {waited=::waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
      Check(waited==child&&WIFEXITED(status)&&WEXITSTATUS(status)==(valid?0:2),
            "real product startup exits with exact success/refusal outcome, not signal or exec failure");
      std::ifstream input(output_path);
      const std::string text{std::istreambuf_iterator<char>(input),{}};
      Check(valid?text.find("\"server_config_validation\":{\"accepted\":true")!=std::string::npos:
                  text.find("message_vector")!=std::string::npos,
            "real product emits validation evidence or the source-owned diagnostic vector");
    };
    run(path.string(),true);
    const auto bad=artifacts.path()/"bad-product.conf";
    {std::ofstream out(bad);out<<"[config]\nformat = SBCD2\n";Check(bool(out),"bad product config fixture");}
    run(bad.string(),false);
    run(path.string(),false,true);
    Check(rand_calls==0&&manager.Snapshot().current_bytes==0,
          "external product lifetime cannot consume the fixture parent's provider or backing");
    artifacts.Cleanup();return 0;
  }
  if(test_mode.starts_with("host_")) {
    s::ServerProcessCryptoOwner host;
    if(test_mode=="host_bad_config") {
      const auto bad_path=artifacts.path()/"bad.conf";
      {std::ofstream out(bad_path);out<<"[config]\nformat = SBCD2\n";Check(bool(out),"bad config fixture");}
      auto bad_cli=cli;bad_cli.config_path=bad_path.string();
      const auto startup=s::RunServerStartup(bad_cli,host);
      Check(startup.exit_code!=0&&!startup.diagnostics.empty()&&rand_calls==0&&!host.installed(),
          "actual host config rejection has source-owned diagnostics before provider admission");
      Check(u::IsEngineIdentityUuid(Uuid{startup.diagnostics.front().occurrence_uuid}),
          "bad host config retains a genuine binary diagnostic occurrence");
    } else if(test_mode=="host_startup"||test_mode=="host_platform_startup"||test_mode=="host_platform_refusal") {
      provider_allowed=true;
      auto resolved=s::ResolveServerBootstrapConfig(cli);
      if(test_mode!="host_startup") {
        resolved.config.mode=s::ServerMode::kForeground;
        resolved.config.data_dir=artifacts.path()/"data";
        resolved.config.control_dir=artifacts.path()/"control";
        resolved.config.pid_file=resolved.config.control_dir/"server.pid";
        resolved.config.lifecycle_state_file=resolved.config.control_dir/"server.state";
        resolved.config.lifecycle_journal_file=resolved.config.control_dir/"server.journal";
        resolved.config.database_default_path=artifacts.path()/"node.sbdb";
      }
      auto startup=s::RunServerStartup(std::move(resolved),host);
      if(test_mode=="host_platform_refusal") {
        Check(startup.exit_code==2&&host.installed()&&!startup.emergency_reserve&&host_probe_calls==1&&
            host.Snapshot().backing_bytes==bytes&&manager.Snapshot().current_bytes==bytes&&
            !std::filesystem::exists(startup.effective_config.control_dir),
            "unsupported host retains actual provider custody but refuses before emergency/lifecycle effects");
        bool refused=false,kernel=false,capability=false;
        for(const auto& diagnostic:startup.diagnostics)if(diagnostic.code=="PROFILE.BUILTIN_PROFILE_UNAVAILABLE") {
          refused=true;for(const auto& field:diagnostic.fields){kernel|=field.key=="kernel_release"&&field.value=="6.5.0";
            capability|=field.key=="failed_capability"&&field.value=="linux_kernel_6_6_minimum";}
        }
        Check(refused&&kernel&&capability,"host refusal preserves exact kernel/capability diagnostic after provider admission");
      }else {
        Check(startup.exit_code==0&&host.installed()&&startup.emergency_reserve&&rand_calls>0,
            "actual startup installs process provider before emergency owner UUID issuance");
        if(test_mode=="host_platform_startup")
          Check(host_probe_calls==1&&std::filesystem::exists(startup.effective_config.lifecycle_state_file)&&
              startup.effective_config.mode==s::ServerMode::kForeground,
              "serving startup executes real host IPC probe after provider admission and publishes actual lifecycle artifacts");
        Check(host.Snapshot().backing_bytes==bytes&&manager.Snapshot().current_bytes>bytes,
            "actual startup retains separately accounted provider and emergency backing");
      }
      startup.emergency_reserve.reset();
    } else {
      auto selected=loaded.config;
      if(test_mode=="host_low_budget")selected.memory_openssl_budget_bytes=1;
      if(test_mode=="host_reservation_refusal")selected.memory_openssl_budget_bytes=policy.effective_hard_limit_bytes+1;
      if(test_mode=="host_late_provider") {
        provider_allowed=true;std::array<unsigned char,16> random{};
        Check(__real_RAND_bytes(random.data(),random.size())==1,"actual unowned provider initialization");
      }
      if(test_mode=="host_foreign_thread") {
        s::ProcessCryptoStartResult foreign;
        std::thread other([&]{foreign=host.Start(selected,policy);});other.join();
        Check(foreign.error==s::ProcessCryptoStartError::wrong_owner&&rand_calls==0,
              "wrong native owner cannot claim bootstrap or touch provider");
      }
      const auto admitted=host.Start(selected,policy);
      if(test_mode=="host_low_budget"||test_mode=="host_reservation_refusal"||test_mode=="host_late_provider") {
        Check(!admitted.ok()&&!host.installed()&&rand_calls==0,
              "actual resource or late-install refusal cannot yield provider readiness");
        if(test_mode=="host_late_provider")Check(admitted.pool.adapter_error==h::CryptoMemoryError::late_installation,
            "late provider refusal retains exact adapter cause");
        if(test_mode=="host_reservation_refusal")Check(admitted.error==s::ProcessCryptoStartError::reservation_failure,
            "budget refusal precedes backing allocation");
      } else {
        Check(admitted.ok()&&host.installed()&&rand_calls==0,"production owner installs before actual RAND");
        provider_allowed=true;
        const auto identity=u::IssueRuntimeIdentityV7();
        Check(identity&&rand_calls>0&&u::IsEngineIdentityUuid(*identity),"actual admitted host UUID provider");
        const auto before=host.Snapshot();
        Check(before.backing_bytes==bytes&&manager.Snapshot().current_bytes==bytes,
              "selected host budget is charged once to the shared actual manager");
        Check(host.Start(selected,policy).error==s::ProcessCryptoStartError::already_started,
              "repeat host startup cannot replace process identities or grants");
        if(test_mode=="host_foreign_thread") {
          r::RuntimeCryptoPoolResult foreign;
          h::CryptoMemorySnapshot observed;
          std::thread other([&]{foreign=host.CloseAfterUserDrain();observed=host.Snapshot();});other.join();
          Check(!foreign.ok()&&host.installed()&&host.Snapshot().binding==before.binding,
                "wrong owner cannot run global provider cleanup or release custody");
          Check(observed.observation_error==h::CryptoMemoryError::invalid_binding&&observed.binding==h::CryptoMemoryBinding{},
                "foreign-thread observation rejects before reading shared pool state");
          const auto child=::fork();Check(child>=0,"fork native process-custody refusal fixture");
          if(child==0) {
            const auto inherited=host.Snapshot();
            const bool rejected=inherited.observation_error==h::CryptoMemoryError::invalid_binding&&
                inherited.binding==h::CryptoMemoryBinding{}&&!host.installed()&&
                host.Start(selected,policy).error==s::ProcessCryptoStartError::wrong_owner&&
                !host.CloseAfterUserDrain().ok();
            ::_exit(rejected?0:1);
          }
          int status=0;pid_t waited;
          do {waited=::waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
          Check(waited==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
                "inherited process cannot enter pool observation or cleanup synchronization");
        }
        Check(host.WithProviderScope([&] {
          Check(host.CloseAfterUserDrain().adapter_error==h::CryptoMemoryError::busy&&host.installed(),
                "live admitted work refuses terminal cleanup without losing provider custody");
        }).ok(),"actual process provider work scope");
        bool work_threw=false;
        try {host.WithProviderScope([] {throw std::runtime_error("actual scoped work failure");});}
        catch(const std::runtime_error&) {work_threw=true;}
        Check(work_threw&&host.Snapshot().scopes==0&&host.installed(),
              "callback exception releases its actual scope without losing pool ownership");
        std::array<bool,2> worker_ok{};
        std::array<std::thread,2> workers;
        for(std::size_t i=0;i<workers.size();++i)workers[i]=std::thread([&,i] {
          const auto issued=u::IssueRuntimeIdentityV7();
          worker_ok[i]=issued&&u::IsEngineIdentityUuid(*issued);
          OPENSSL_thread_stop();
        });
        for(auto& worker:workers)worker.join();
        Check(worker_ok[0]&&worker_ok[1]&&host.Snapshot().allocations>before.allocations,
              "real provider workers use retained shared backing and finish before cleanup");
        const s::ServerDiagnostic ordinary{"CONFIG.MALFORMED","config.malformed"};
        Check(u::IsEngineIdentityUuid(Uuid{ordinary.occurrence_uuid}),
              "production readiness restores ordinary source instead of sealed bootstrap selection");
        Check(u::IssueCryptoBootstrapDiagnosticIdentityV7().error==u::CryptoBootstrapDiagnosticError::sealed,
              "production host permanently seals pre-provider source");
      }
    }
    Check(host.CloseAfterUserDrain().ok()&&!host.installed()&&manager.Snapshot().current_bytes==0&&
          manager.Snapshot().reserved_capacity_bytes==0,"actual production teardown releases all backing and reservations");
    Check(host.CloseAfterUserDrain().ok(),"host terminal close is idempotent");
    artifacts.Cleanup();return 0;
  }
  const auto batch=u::IssueCryptoBootstrapIdentitiesV7();
  Check(bool(batch) && rand_calls==0,"OS bootstrap before provider");
  h::CryptoMemoryBinding binding{{},batch->operation,batch->owner,batch->context,batch->process};
  m::HierarchicalMemoryBudgetLedger ledger;
  m::HierarchicalMemoryBudget root;
  root.scope={m::HierarchicalMemoryScopeKind::process,{},batch->process.bytes};
  root.hard_limit_bytes=policy.effective_hard_limit_bytes;
  root.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  root.provenance.source_label=loaded.config.memory_policy_provenance;
  Check(ledger.SetBudget(root).ok(),"real selected process root budget");
  m::ReservationBackedMemoryResourceRequest request;
  request.memory_manager=&manager; request.reservation_ledger=&ledger;
  request.scope_chain={root.scope}; request.provenance=root.provenance;
  request.requested_bytes=bytes; request.category=m::MemoryCategory::core_runtime;
  request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
  request.route_label="server_config_crypto_bootstrap"; request.purpose="actual process provider backing";
  request.memory_class="crypto_provider"; request.binary_operation_uuid=batch->operation.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::process]=batch->process.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::owner]=batch->owner.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::context]=batch->context.bytes;
  auto acquired=m::AcquireReservationBackedMemoryResource(request);
  Check(acquired.ok() && rand_calls==0,"actual reservation is provider-free");
  r::RuntimeCryptoPoolOwner owner;
  bool installed=false;
  bool injected_failure=false;
  const auto inject=[&](std::string_view stage) {
    if(test_mode==stage) {injected_failure=true; throw std::runtime_error("injected qualification failure");}
  };
  h::PreparedSha256 hash;
  try {
  Check(owner.Adopt(binding,acquired.resource,bytes).ok(),"actual physical provider backing");
  inject("fail_adopt");
  installed=owner.InstallProcessAdapter().ok();
  Check(installed,"config must not have initialized provider behind explicit bootstrap");
  provider_allowed=true;
  inject("fail_install");
  Check(u::SealCryptoBootstrapDiagnosticIdentities()==u::CryptoBootstrapDiagnosticError::none,
        "explicitly close pre-provider diagnostic issuance");
  const auto ordinary=u::IssueRuntimeIdentityV7();
  Check(ordinary && rand_calls>0 && u::IsEngineIdentityUuid(*ordinary),"real admitted RAND issues native identity");
  const auto refused=s::ClassifyServerConfigFormat("SBCD2");
  Check(!refused.accepted && refused.diagnostic.code=="CONFIG.VERSION_NEWER_THAN_SUPPORTED" &&
        refused.diagnostic.message_key=="config.version_newer_than_supported" &&
        u::IsEngineIdentityUuid(Uuid{refused.diagnostic.occurrence_uuid}),
        "real format refusal retains registered code/key and issued occurrence");
  Check(owner.Prepare(hash,binding).ok(),"actual prepared SHA provider");
  inject("fail_prepare");
  const std::array<unsigned char,3> text{'a','b','c'};
  const h::HashDigestSegment segment{text.data(),text.size()};
  const auto digest=hash.Compute(&segment,1);
  const h::Digest256 expected{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
      0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,
      0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  Check(digest.ok() && digest.digest==expected,"provider computes exact SHA256 result");
  Check(hash.Close()==h::CryptoMemoryError::none,"drain prepared context");
  Check(manager.Snapshot().current_bytes==bytes && owner.Snapshot().live_blocks>0,
        "shared manager retains all process backing while provider caches remain");
  Check(owner.Close().adapter_error==h::CryptoMemoryError::busy,"live provider cannot release backing");
  OPENSSL_thread_stop(); OPENSSL_cleanup();
  Check(h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none,"terminal hook stop after provider cleanup");
  installed=false;
  Check(owner.Close().ok() && manager.Snapshot().current_bytes==0 &&
        manager.Snapshot().reserved_capacity_bytes==0,"release real process backing exactly once");
  } catch(...) {
    // Keep all custody providers alive until failure cleanup has completed.
    // A failed assertion must not skip private artifact cleanup, but a busy
    // provider must never be force-freed just to allow stack unwinding.
    bool drained=hash.Close()==h::CryptoMemoryError::none;
    if(installed && drained) {
      OPENSSL_thread_stop(); OPENSSL_cleanup();
      drained=h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none;
    }
    if(!drained || !owner.Close().ok()) {
      std::cerr<<"qualification failure retains crypto custody; unsafe to unwind\n";
      std::terminate();
    }
    if(injected_failure) {
      Check(!owner.has_custody() && manager.Snapshot().current_bytes==0 &&
            manager.Snapshot().reserved_capacity_bytes==0,"failure cleanup releases actual backing and grants");
      artifacts.Cleanup();
      return 0;
    }
    throw;
  }
  Check(!test_mode.starts_with("fail_"),"failure injection must have been reached");
  artifacts.Cleanup();
  return 0;
} catch(const std::exception& error) {
  std::cerr<<error.what()<<'\n'; return 1;
}
