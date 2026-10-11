// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "config.hpp"
#include "runtime_crypto_pool_owner.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"
#include <openssl/crypto.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <sys/random.h>
#include <unistd.h>

namespace s=scratchbird::server;
namespace m=scratchbird::core::memory;
namespace u=scratchbird::core::uuid;
namespace h=scratchbird::core::hash;
namespace r=scratchbird::core::runtime;
using scratchbird::core::platform::Uuid;
std::atomic<unsigned> rand_calls{0}, entropy_calls{0};
bool provider_allowed=false;
std::string_view test_mode;
int status_descriptor=-1;
unsigned write_calls=0;
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
  const bool override_budget=argc>1 && std::string_view(argv[1])=="override";
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
