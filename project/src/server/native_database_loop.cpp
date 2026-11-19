// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_DATABASE_LOOP (see the header). The read-only open
// verification here is plan 38 section 2c, in the order the production
// producer test proves (native_local_service_enrollment_test, producer-api):
//   ownership (held by the caller) -> custody Begin -> ProbeBootstrap (native
//   only) -> crypto / memory setup -> AdoptWriter                  [Host::Open]
//   -> the L5 identity read from the file -> the L5 password derived from the
//   database UUID and the stored seed -> the startup credential read and the
//   L5 credential verified -> memory envelope -> allocation behavior -> L5
//   authenticated -> catalog scan and capture -> catalog metadata verified ->
//   management definition -> verification policies -> the startup service
//   session attached -> transaction policies -> the open verification
//   (identity, allocation, inventory, catalog routes).
// NOTHING here writes: Restart (the TIP) is S3d and runs after this.

#include "native_database_loop.hpp"

#include "engine/internal_api/security/agent_password_derivation.hpp"
#include "startup_credential_source_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <exception>
#include <optional>
#include <span>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#endif

namespace scratchbird::server {

const char* NativeLoopStateName(NativeLoopState state) noexcept {
  switch (state) {
    case NativeLoopState::opening: return "opening";
    case NativeLoopState::verifying: return "verifying";
    case NativeLoopState::serving: return "serving";
    case NativeLoopState::failed: return "failed";
    case NativeLoopState::closing: return "closing";
    case NativeLoopState::closed: return "closed";
  }
  return "failed";
}

namespace {
namespace db = storage::database;
namespace api = engine::internal_api;
using Host = NativeHostedDatabaseRuntime;

const char* RuntimeErrorName(NativeHostedRuntimeError e) noexcept {
  using E = NativeHostedRuntimeError;
  switch (e) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::wrong_owner: return "wrong_owner";
    case E::process_unavailable: return "process_unavailable";
    case E::ownership_failure: return "ownership_failure";
    case E::custody_failure: return "custody_failure";
    case E::bootstrap_failure: return "bootstrap_failure";
    case E::identity_failure: return "identity_failure";
    case E::memory_failure: return "memory_failure";
    case E::crypto_failure: return "crypto_failure";
    case E::adoption_failure: return "adoption_failure";
    case E::preparation_failure: return "preparation_failure";
    case E::busy: return "busy";
    case E::closed: return "closed";
    case E::credential_failure: return "credential_failure";
    case E::resource_exhausted: return "resource_exhausted";
    case E::process_admission_failure: return "process_admission_failure";
    case E::synchronization_failure: return "synchronization_failure";
    case E::authorization_preparation_failure: return "authorization_preparation_failure";
    case E::authorization_failure: return "authorization_failure";
    case E::configuration_failure: return "configuration_failure";
    case E::agent_configuration_failure: return "agent_configuration_failure";
    case E::memory_envelope_failure: return "memory_envelope_failure";
    case E::agent_policy_failure: return "agent_policy_failure";
    case E::cancelled: return "cancelled";
    case E::catalog_failure: return "catalog_failure";
    case E::catalog_verification_failure: return "catalog_verification_failure";
    case E::verification_policy_failure: return "verification_policy_failure";
    case E::management_definition_failure: return "management_definition_failure";
    case E::transaction_policy_failure: return "transaction_policy_failure";
  }
  return "unknown";
}

const char* IdentityErrorName(NativeServiceIdentityError e) noexcept {
  using E = NativeServiceIdentityError;
  switch (e) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::wrong_owner: return "wrong_owner";
    case E::busy: return "busy";
    case E::closed: return "closed";
    case E::preparation_failure: return "preparation_failure";
    case E::custody_failure: return "custody_failure";
    case E::crypto_failure: return "crypto_failure";
    case E::memory_failure: return "memory_failure";
    case E::configuration_source_failure: return "configuration_source_failure";
    case E::binding_absent: return "binding_absent";
    case E::binding_ambiguous: return "binding_ambiguous";
    case E::binding_invalid: return "binding_invalid";
    case E::binding_not_active: return "binding_not_active";
    case E::external_authority_required: return "external_authority_required";
    case E::credential_reference_absent: return "credential_reference_absent";
    case E::security_source_failure: return "security_source_failure";
    case E::principal_absent: return "principal_absent";
    case E::principal_wrong_family: return "principal_wrong_family";
    case E::principal_not_active: return "principal_not_active";
    case E::principal_not_service: return "principal_not_service";
    case E::principal_credential_absent: return "principal_credential_absent";
    case E::seed_absent: return "seed_absent";
    case E::password_inadmissible: return "password_inadmissible";
  }
  return "unknown";
}

const char* VerificationErrorName(NativeOpenVerificationError e) noexcept {
  using E = NativeOpenVerificationError;
  switch (e) {
    case E::none: return "none";
    case E::not_ready: return "not_ready";
    case E::busy: return "busy";
    case E::cancelled: return "cancelled";
    case E::invalid_request: return "invalid_request";
    case E::policy_refused: return "policy_refused";
    case E::memory_failure: return "memory_failure";
    case E::crypto_failure: return "crypto_failure";
    case E::io_failure: return "io_failure";
    case E::source_failure: return "source_failure";
    case E::source_mismatch: return "source_mismatch";
    case E::finding: return "finding";
  }
  return "unknown";
}

const char* VerificationStageName(NativeOpenVerificationStage s) noexcept {
  using S = NativeOpenVerificationStage;
  switch (s) {
    case S::none: return "none";
    case S::identity: return "identity";
    case S::allocation: return "allocation";
    case S::inventory: return "inventory";
    case S::catalog_routes: return "catalog_routes";
    case S::complete: return "complete";
  }
  return "unknown";
}

const char* CredentialsErrorName(NativeLocalServiceStartupCredentialsError e) noexcept {
  using E = NativeLocalServiceStartupCredentialsError;
  switch (e) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::cancelled: return "cancelled";
    case E::credential_source: return "credential_source";
    case E::checkpoint_source: return "fixed_root_source";
    case E::credential_verification: return "credential_verification";
    case E::declaration_selection: return "declaration_selection";
  }
  return "unknown";
}

const char* CredentialVerificationErrorName(NativeRuntimePrincipalCredentialError e) noexcept {
  using E = NativeRuntimePrincipalCredentialError;
  switch (e) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::cancelled: return "cancelled";
    case E::backing_unavailable: return "backing_unavailable";
    case E::security_source_failure: return "security_source_failure";
    case E::provider_source_failure: return "provider_source_failure";
    case E::provider_absent: return "provider_absent";
    case E::provider_ambiguous: return "provider_ambiguous";
    case E::provider_invalid: return "provider_invalid";
    case E::provider_inactive: return "provider_inactive";
    case E::provider_generation_mismatch: return "provider_generation_mismatch";
    case E::local_provider_required: return "local_provider_required";
    case E::credential_source_unavailable: return "credential_source_unavailable";
    case E::credential_binding_mismatch: return "credential_binding_mismatch";
    case E::protected_workspace_required: return "protected_workspace_required";
    case E::crypto_scope_unavailable: return "crypto_scope_unavailable";
    case E::credential_rejected: return "credential_rejected";
    case E::provider_failure: return "provider_failure";
    case E::resource_exhausted: return "resource_exhausted";
  }
  return "unknown";
}

std::string Num(std::uint64_t v) { return std::to_string(v); }

NativeLoopDiagnostic Refusal(std::string code, std::string step,
                             std::vector<std::pair<std::string, std::string>> fields = {}) {
  return NativeLoopDiagnostic{std::move(code), std::move(step), std::move(fields)};
}

// DATABASE.OPEN_NATIVE_<STEP>_REFUSED for a step whose own error enum is the
// detail. The step is part of the code so a cause is findable without parsing.
std::string StepCode(std::string_view step) {
  std::string code = "DATABASE.OPEN_NATIVE_";
  for (const char ch : step) code.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
  code += "_REFUSED";
  return code;
}

// The limits the open uses: the producer test's, as functions of the file's
// page size and page count (which discovery read from page zero). The catalog
// capture bounds are sized for a freshly created database and the transaction
// inventory bound (1024) for its first history; they grow with S5+ and are
// provisional, not a tuned policy.
struct OpenLimits {
  std::uint64_t budget = 0;
  NativeHostedRuntimeLimits host;
  db::NativeFixedRootMemoryLimits fixed_root;
  db::NativeFixedRootMemoryLimits catalog;
  db::NativeCatalogRelationMemoryLimits scan;
  db::NativeCatalogCaptureLimits capture;
  NativeOpenVerificationLimits verification;
};
bool BuildLimits(std::uint32_t page_size, std::uint64_t total_pages, OpenLimits& out) {
  const auto* profile = storage::disk::FindCanonicalFilespacePageProfileForSize(page_size);
  if (profile == nullptr || total_pages == 0 || total_pages > UINT64_MAX / 8 / page_size) return false;
  const std::size_t ps = page_size;
  out.budget = db::NativeHostedSourceBackingBytes(ps);
  constexpr auto rows = db::kNativeHostedCatalogRelationMaximumRows;
  const db::NativeCatalogRelationMemoryLimits credential_catalog{
      profile->uuid, 1, db::NativeHostedCatalogBasePages(ps), rows, rows,
      db::NativeHostedCatalogNavigationLimits(ps)};
  out.host = NativeHostedRuntimeLimits{{64 * 1024, {out.budget, 32 * 1024 * 1024}}, credential_catalog,
                                       2 * 1024 * 1024, {rows, 128u, 1000000u}};
  out.fixed_root = {out.budget, 32 * 1024 * 1024};
  out.catalog = {out.budget, 12 * 1024 * 1024};
  out.scan = db::NativeCatalogRelationMemoryLimits{
      profile->uuid, 1, db::NativeCatalogScanBasePages(ps), db::kNativeCatalogScanRelationMaximumRows,
      db::kNativeCatalogScanRelationMaximumRows, db::NativeCatalogScanNavigationLimits(ps),
      db::NativeNameTreeNavigationLimits(ps, db::kNativeCatalogScanRelationMaximumRows)};
  out.capture = {4096, 8 * 1024 * 1024, 1, 1024, 1024, 128, 8192};
  // Eight times the file: the verification image bound.
  const std::uint64_t image_budget = 8 * total_pages * page_size;
  out.verification = NativeOpenVerificationLimits{{image_budget, 64 * 1024 * 1024}, 8 * 1024 * 1024};
  return true;
}
}  // namespace

// Everything the loop thread holds for one open database. Created, used and
// destroyed on the loop thread only.
struct NativeDatabaseLoop::Serving {
  // Declared first, so destroyed last: the credential manager outlives the
  // protected buffers and the source allocated from it.
  std::shared_ptr<core::memory::MemoryManager> credential_memory;
  ServerProcessCryptoOwner* crypto = nullptr;
  std::shared_ptr<Host> runtime;
  std::shared_ptr<StartupCredentialSource> source;
  core::memory::MemoryTag tag;
  core::memory::ScopedProtectedBuffer derived;
  std::optional<Host::CredentialRead> read;
  NativeLoopServingFacts facts;

  // Reverse of acquisition, explicit because the close result must be seen.
  // The read first (it pins the host and holds the source lease), then the
  // runtime, then the credential source.
  bool Teardown(NativeLoopDiagnostic* failure) noexcept {
    bool ok = true;
    read.reset();
    if (runtime) {
      const auto closed = Host::Close(runtime);
      if (!closed.ok()) {
        ok = false;
        if (failure)
          *failure = Refusal("DATABASE.CLOSE_NATIVE_RUNTIME_REFUSED", "close_runtime",
                             {{"error", RuntimeErrorName(closed.error)},
                              {"original_route_released", closed.original_route_released ? "true" : "false"}});
      }
    }
    if (ok) runtime.reset();
    if (source) source->Close();
    source.reset();
    derived = {};
    return ok;
  }
};

std::shared_ptr<NativeDatabaseLoop> NativeDatabaseLoop::Create() {
  std::shared_ptr<NativeDatabaseLoop> loop(new NativeDatabaseLoop());
  std::promise<void> started;
  auto running = started.get_future();
  loop->thread_ = std::thread([raw = loop.get(), &started] {
    raw->thread_id_ = std::this_thread::get_id();
#if defined(__linux__)
    (void)::pthread_setname_np(::pthread_self(), "sb-db-loop");
#endif
    started.set_value();
    raw->Run();
  });
  running.wait();
  return loop;
}

NativeDatabaseLoop::~NativeDatabaseLoop() {
  if (on_loop_thread()) std::terminate();
  (void)Close();
}

std::future<NativeLoopOpenResult> NativeDatabaseLoop::SubmitOpen(NativeLoopOpenRequest request) {
  std::promise<NativeLoopOpenResult> refused;
  const auto refuse = [&](NativeLoopDiagnostic diagnostic) {
    NativeLoopOpenResult result;
    result.state = NativeLoopState::failed;
    result.diagnostic = std::move(diagnostic);
    refused.set_value(std::move(result));
    return refused.get_future();
  };
  if (request.crypto == nullptr || !request.ownership)
    return refuse(Refusal("DATABASE.OPEN_NATIVE_LOOP_REQUEST_INVALID", "submit_open"));
  // The owner thread (this one) names the loop thread as the crypto owner's
  // native-database delegate before the loop can touch it.
  if (!request.crypto->DelegateNativeDatabaseThread(thread_id_))
    return refuse(Refusal("DATABASE.OPEN_NATIVE_CRYPTO_DELEGATION_REFUSED", "submit_open"));
  OpenWork work{std::move(request), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_ || queue_.size() >= kQueueCapacity || state() != NativeLoopState::opening) {
      work.request.crypto->ClearNativeDatabaseDelegate();
      return refuse(Refusal("DATABASE.OPEN_NATIVE_LOOP_NOT_ACCEPTING", "submit_open",
                            {{"state", NativeLoopStateName(state())},
                             {"queue_full", queue_.size() >= kQueueCapacity ? "true" : "false"}}));
    }
    delegated_crypto_ = work.request.crypto;
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

std::future<NativeLoopCloseResult> NativeDatabaseLoop::SubmitClose() {
  CloseWork work;
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_) {
      // Already closing: report the state; the thread is (being) joined.
      NativeLoopCloseResult result;
      result.ok = true;
      result.state = state();
      work.done.set_value(std::move(result));
      return future;
    }
    // A close is always admitted, even when the queue is at capacity.
    accepting_ = false;
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

NativeLoopCloseResult NativeDatabaseLoop::Close() noexcept {
  try {
    if (on_loop_thread()) {
      NativeLoopCloseResult refused;
      refused.diagnostic = Refusal("DATABASE.CLOSE_NATIVE_LOOP_SELF_JOIN", "close");
      return refused;
    }
    auto done = SubmitClose();
    auto result = done.get();
    ServerProcessCryptoOwner* crypto = nullptr;
    // The thread leaves Run() after the close request; join outside the lock
    // (the loop takes the lock itself to read its queue).
    if (thread_.joinable()) thread_.join();
    {
      std::lock_guard<std::mutex> guard(mutex_);
      joined_ = true;
      crypto = std::exchange(delegated_crypto_, nullptr);
    }
    // The delegate thread is joined: nothing can observe the delegation.
    if (crypto != nullptr) crypto->ClearNativeDatabaseDelegate();
    return result;
  } catch (...) {
    NativeLoopCloseResult failed;
    failed.diagnostic = Refusal("DATABASE.CLOSE_NATIVE_LOOP_EXCEPTION", "close");
    return failed;
  }
}

void NativeDatabaseLoop::Run() noexcept {
  for (;;) {
    Work work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return !queue_.empty(); });
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    if (auto* open = std::get_if<OpenWork>(&work)) {
      HandleOpen(*open);
      continue;
    }
    auto& close = std::get<CloseWork>(work);
    close.done.set_value(HandleClose());
    return;
  }
}

NativeLoopCloseResult NativeDatabaseLoop::HandleClose() noexcept {
  NativeLoopCloseResult result;
  state_.store(NativeLoopState::closing, std::memory_order_release);
  result.ok = true;
  if (serving_) {
    NativeLoopDiagnostic failure;
    if (!serving_->Teardown(&failure)) {
      result.ok = false;
      result.diagnostic = std::move(failure);
    }
    // A runtime that refused to close must not be destroyed here (its
    // destructor terminates on an unreconciled close); it is leaked on
    // purpose only in that already-fatal case.
    if (result.ok) serving_.reset();
    else (void)serving_.release();
  }
  state_.store(NativeLoopState::closed, std::memory_order_release);
  result.state = NativeLoopState::closed;
  return result;
}

void NativeDatabaseLoop::HandleOpen(OpenWork& work) noexcept {
  NativeLoopOpenResult result;
  const auto fail = [&](NativeLoopDiagnostic diagnostic) {
    // Close everything this open acquired on this thread, then report.
    if (serving_) {
      NativeLoopDiagnostic ignored;
      if (serving_->Teardown(&ignored)) serving_.reset();
      else (void)serving_.release();
    }
    // This request's reference to the ownership is dropped before the caller
    // can observe the failure, so a failed open leaves the file ownable by
    // the time the future is ready (the caller drops its own reference too).
    work.request.ownership.reset();
    state_.store(NativeLoopState::failed, std::memory_order_release);
    result.ok = false;
    result.state = NativeLoopState::failed;
    result.diagnostic = std::move(diagnostic);
    work.done.set_value(std::move(result));
  };
  try {
    auto& request = work.request;
    state_.store(NativeLoopState::opening, std::memory_order_release);
    OpenLimits limits;
    if (!BuildLimits(request.page_size_bytes, request.total_pages, limits))
      return fail(Refusal("DATABASE.OPEN_NATIVE_PAGE_PROFILE_UNSUPPORTED", "limits",
                          {{"page_size_bytes", Num(request.page_size_bytes)}}));
    serving_ = std::make_unique<Serving>();
    auto& serving = *serving_;
    serving.crypto = request.crypto;
    if (!request.crypto->installed())
      return fail(Refusal("DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED", "crypto_owner"));

    // ---- ownership (held) -> custody Begin -> ProbeBootstrap(native only) ->
    //      crypto / memory setup -> AdoptWriter ----
    DatabaseOwnershipRequest ownership;
    ownership.owner_kind = "server";
    ownership.ownership = storage::disk::DatabaseFileOwnership::native_data_file;
    ownership.storage_access = storage::disk::RouteStorageAccess::read_write;
    std::vector<std::unique_ptr<storage::disk::FileDevice>> secondary;
    auto opened = Host::OpenAdmissionHeldByCaller(*request.crypto, ownership, limits.host, secondary,
                                                  request.ownership);
    if (!opened.ok()) {
      // The runtime is closed here, on its constructing thread, before the
      // failure leaves; an error from Close would be fatal to destroy.
      NativeLoopDiagnostic ignored;
      if (opened.runtime) {
        const auto closed = Host::Close(opened.runtime);
        if (!closed.ok()) (void)new std::shared_ptr<Host>(std::move(opened.runtime));  // never destroyed unreconciled
      }
      return fail(Refusal(StepCode("runtime_open"), "runtime_open",
                          {{"error", RuntimeErrorName(opened.error)},
                           {"custody_error", Num(static_cast<unsigned>(opened.custody_error))},
                           {"route_error", Num(static_cast<unsigned>(opened.route_error))}}));
    }
    serving.runtime = std::move(opened.runtime);
    if (serving.runtime->database_uuid() != request.database_uuid ||
        serving.runtime->primary_filespace_uuid() != request.filespace_uuid)
      return fail(Refusal("DATABASE.OPEN_NATIVE_IDENTITY_MISMATCH", "runtime_open",
                          {{"detail", "page_zero_identity_changed_between_discovery_and_open"}}));
    state_.store(NativeLoopState::verifying, std::memory_order_release);

    // ---- the L5 identity, from the file (no receipt exists) ----
    const auto identity = Host::ReadStartupServiceIdentity(serving.runtime, limits.fixed_root);
    if (!identity.ok())
      return fail(Refusal(StepCode("l5_identity"), "l5_identity",
                          {{"error", IdentityErrorName(identity.error)},
                           {"security_error", Num(static_cast<unsigned>(identity.security_error))},
                           {"source_error", Num(static_cast<unsigned>(identity.source_error))}}));
    const auto& service = *identity.identity;

    // ---- SB_AGENT_PASSWORD_DERIVED (owner Q274 + Q281): the password is the
    //      database UUID XOR the stored seed, derived here from the file; no
    //      outside party hands the server L5's password. The sealed-descriptor
    //      source below is the in-process custody the existing verification
    //      path reads (frame, memfd, seals), now fed by derivation instead of
    //      by an operator file or a service-manager credential. ----
    auto password = api::DeriveAgentPassword(
        service.database_uuid, std::span<const core::platform::byte, 16>(service.password_seed));
    struct Cleanse {
      std::array<unsigned char, 16>& bytes;
      ~Cleanse() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    } cleanse{password};
    if (!api::AgentPasswordAdmissible(password))
      return fail(Refusal(StepCode("l5_identity"), "l5_identity", {{"error", IdentityErrorName(NativeServiceIdentityError::password_inadmissible)}}));
    const auto incarnation = core::uuid::IssueRuntimeIdentityV7();
    const auto publish_task = core::uuid::IssueRuntimeIdentityV7();
    const auto read_task = core::uuid::IssueRuntimeIdentityV7();
    if (!incarnation || !publish_task || !read_task)
      return fail(Refusal("DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED", "l5_credential",
                          {{"error", "identity_issue_failed"}}));
#if defined(__linux__)
    const StartupCredentialTransportBinding binding{service.database_uuid, service.service_principal_uuid,
                                                    service.provider_uuid, service.credential_reference_uuid, 1,
                                                    static_cast<std::uint32_t>(::geteuid())};
    serving.credential_memory = std::make_shared<core::memory::MemoryManager>(
        core::memory::DefaultLocalEngineMemoryPolicy());
    serving.source = std::make_shared<StartupCredentialSource>(*incarnation, 4);
    serving.tag.category = core::memory::MemoryCategory::core_runtime;
    serving.tag.purpose = "native L5 startup credential custody";
    serving.tag.callsite = "native database loop startup credential derivation";
    serving.tag.binary_ownership[core::memory::MemoryBinaryScopeKind::database] = service.database_uuid.bytes;
    serving.tag.binary_ownership[core::memory::MemoryBinaryScopeKind::owner] = service.service_principal_uuid.bytes;
    serving.tag.binary_ownership[core::memory::MemoryBinaryScopeKind::context] = service.database_uuid.bytes;
    {
      auto sealed = CreateSealedStartupCredentialSource(
          binding, std::string_view(reinterpret_cast<const char*>(password.data()), password.size()));
      if (!sealed.ok())
        return fail(Refusal("DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED", "l5_credential",
                            {{"error", "seal_failed"},
                             {"adapter_error", Num(static_cast<unsigned>(sealed.error))},
                             {"native_error", Num(static_cast<unsigned>(sealed.native_error))}}));
      struct Descriptor {
        int fd;
        ~Descriptor() { if (fd >= 0) ::close(fd); }
      } descriptor{sealed.fd};
      const auto published = serving.source->Publish({*incarnation, *publish_task, 0, {}, {}}, descriptor.fd,
                                                     binding, *serving.credential_memory, serving.tag);
      if (!published.ok())
        return fail(Refusal("DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED", "l5_credential",
                            {{"error", "publish_failed"}, {"source_error", Num(static_cast<unsigned>(published.error))}}));
    }
    core::memory::ProtectedMemoryRequest scratch;
    scratch.bytes = 32;
    scratch.tag = serving.tag;
    scratch.material_class = "native service PBKDF2 scratch";
    scratch.platform_policy = core::memory::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto derived = serving.credential_memory->AllocateProtected(scratch);
    if (!derived.ok())
      return fail(Refusal("DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED", "l5_credential",
                          {{"error", "protected_scratch_unavailable"}}));
    serving.derived = std::move(derived.buffer);
#else
    return fail(Refusal("DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED", "l5_credential",
                        {{"error", "unsupported_platform"}}));
#endif

    // ---- the startup credential read: the derived L5 credential is verified
    //      against the principal's stored PBKDF2 verifier by the engine ----
    NativeLocalServiceStartupCredentialsRequest read_request;
    read_request.credential.security.database_uuid = service.database_uuid;
    read_request.credential.security.runtime_binding_uuid = service.runtime_binding_uuid;
    read_request.credential.security.runtime_binding_generation = service.runtime_binding_generation;
    read_request.credential.security.maximum_retained_image_bytes = limits.budget;
    read_request.credential.security.catalog_selection =
        api::NativeRuntimeCatalogSelection::discover_selected;
    read_request.source = {*incarnation, *read_task, 1,
                           StartupCredentialSource::Mutex::Clock::now() + std::chrono::seconds(10), {}};
    read_request.fixed_root_limits = limits.fixed_root;
    serving.read.emplace(Host::ReadStartupCredentials(serving.runtime, read_request, serving.source,
                                                      serving.tag, serving.derived));
    auto& read = *serving.read;
    if (!read.ok())
      return fail(Refusal(StepCode("l5_credential"), "l5_credential",
                          {{"error", RuntimeErrorName(read.error)},
                           {"credential_error", CredentialsErrorName(read.credentials().error)},
                           {"verification_error", read.credentials().comparison
                                                      ? CredentialVerificationErrorName(read.credentials().comparison->error)
                                                      : "none"},
                           {"configuration_error", Num(static_cast<unsigned>(read.configuration().error))},
                           {"agents_error", Num(static_cast<unsigned>(read.agents().error))},
                           {"authorization_error", Num(static_cast<unsigned>(read.authorization().error))}}));
    const auto step = [&](const char* name, NativeHostedRuntimeError error) -> bool {
      if (error == NativeHostedRuntimeError::none) return true;
      fail(Refusal(StepCode(name), name, {{"error", RuntimeErrorName(error)}}));
      return false;
    };
    if (!step("retain_observation", Host::RetainStartupObservation(read))) return;
    {
      const auto envelope = Host::ConstrainStartupMemory(read);
      if (!envelope.ok())
        return fail(Refusal(StepCode("memory_envelope"), "memory_envelope",
                            {{"error", Num(static_cast<unsigned>(envelope.error))}}));
      const auto behavior = Host::InstallStartupAllocationBehavior(read);
      if (!behavior.ok())
        return fail(Refusal(StepCode("allocation_behavior"), "allocation_behavior",
                            {{"error", Num(static_cast<unsigned>(behavior.error))}}));
    }
    // L5's credential authenticates through the engine's verifier path.
    const auto authenticated = Host::AuthenticateStartupService(read, serving.derived);
    if (!authenticated.ok())
      return fail(Refusal(StepCode("l5_authentication"), "l5_authentication",
                          {{"error", Num(static_cast<unsigned>(authenticated.error))}}));
    if (authenticated.observation.principal_uuid != service.service_principal_uuid)
      return fail(Refusal(StepCode("l5_authentication"), "l5_authentication",
                          {{"error", "authenticated_principal_differs_from_binding"}}));

    // ---- the catalog, verified ----
    if (!step("catalog_scan", Host::ReadStartupCatalogs(read, limits.catalog, limits.scan))) return;
    if (!step("catalog_capture", Host::ReadStartupCatalogs(read, limits.catalog, limits.scan, limits.capture))) return;
    {
      const auto metadata = Host::VerifyStartupCatalogMetadata(read);
      if (metadata != NativeHostedRuntimeError::none) {
        const auto& m = read.catalog_metadata();
        return fail(Refusal(StepCode("catalog_metadata"), "catalog_metadata",
                            {{"error", RuntimeErrorName(metadata)},
                             {"names", Num(static_cast<unsigned>(m.names.error))},
                             {"indexes", Num(static_cast<unsigned>(m.indexes.error))},
                             {"namespaces", Num(static_cast<unsigned>(m.namespaces.error))},
                             {"filespaces", Num(static_cast<unsigned>(m.filespaces.error))},
                             {"audit", Num(static_cast<unsigned>(m.audit.error))},
                             {"security", Num(static_cast<unsigned>(m.security.error))},
                             {"naming_profiles", Num(static_cast<unsigned>(m.naming_profiles.error))},
                             {"collisions", Num(static_cast<unsigned>(m.collisions.error))},
                             {"name_trees", Num(static_cast<unsigned>(m.name_trees.error))}}));
      }
    }
    if (!step("management_definition", Host::ResolveStartupManagementDefinition(read))) return;
    if (!step("verification_policies", Host::ResolveConfiguredStartupVerificationPolicies(read))) return;
    // The startup service session attaches (the L5 identity, no client
    // context); transaction policies resolve; then the open verification.
    {
      const auto attached = Host::AttachStartupServiceSession(read);
      if (!attached.ok() || !read.session() || read.session()->principal_uuid != service.service_principal_uuid)
        return fail(Refusal(StepCode("startup_session"), "startup_session",
                            {{"error", Num(static_cast<unsigned>(attached.error))}}));
    }
    if (!step("transaction_policies", Host::ResolveStartupTransactionPolicies(read))) return;
    const auto verified = Host::VerifyStartupDatabaseFile(read, limits.verification);
    if (!verified.ok())
      return fail(Refusal(verified.error == NativeOpenVerificationError::finding ? "DATABASE.OPEN_VERIFICATION_FINDING"
                                                                                 : "DATABASE.OPEN_VERIFICATION_FAILED",
                          "open_verification",
                          {{"stage", VerificationStageName(verified.stage)},
                           {"error", VerificationErrorName(verified.error)},
                           {"source_error", Num(static_cast<unsigned>(verified.source_error))}}));

    // ---- verified, serving-ready; Restart (S3d) has not run ----
    const auto& observed = *verified.observation;
    serving.facts.engine_instance_uuid = serving.runtime->engine_instance_uuid();
    serving.facts.service_principal_uuid = service.service_principal_uuid;
    serving.facts.runtime_binding_uuid = service.runtime_binding_uuid;
    serving.facts.transactions = observed.inventory.transactions;
    serving.facts.active_transactions = observed.inventory.active_transactions;
    serving.facts.in_doubt_transactions = observed.inventory.in_doubt_transactions;
    serving.facts.filespaces = observed.identity.filespaces;
    serving.facts.catalog_pages = observed.allocation.catalog_pages;
    serving.facts.wasted_pages = observed.allocation.wasted_pages;
    serving.facts.verification_workspace_bytes = observed.workspace_bytes;
    result.facts = serving.facts;
    work.request.ownership.reset();  // custody holds the runtime's reference
    result.ok = true;
    result.state = NativeLoopState::serving;
    state_.store(NativeLoopState::serving, std::memory_order_release);
    work.done.set_value(std::move(result));
  } catch (...) {
    fail(Refusal("DATABASE.OPEN_NATIVE_LOOP_EXCEPTION", "open"));
  }
}

}  // namespace scratchbird::server
