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
// NOTHING in the verification writes. S3d: after it the credential read is
// released (its custody operation), then the transaction-store Restart runs ONCE
// on this thread under the custody write operation (RunRestart below).

#include "native_database_loop.hpp"

#include "engine/internal_api/security/agent_password_derivation.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "core/catalog/catalog_database_record_codec.hpp"
#include "core/catalog/catalog_isolation_profile.hpp"
#include "native_hosted_careful_write_sink.hpp"
#include "node_uuid_issuer.hpp"
#include "startup_credential_source_adapter.hpp"
#include "transaction_inventory_writer.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <exception>
#include <map>
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

#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
// SB_TEST_SEAM (owner Q295): compiled only into test targets; see the header.
namespace {
std::mutex test_wrapper_mutex;
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)> test_wrapper;
}  // namespace
void SetNativeRestartSinkWrapperForTest(
    std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)> wrapper) {
  std::lock_guard<std::mutex> guard(test_wrapper_mutex);
  test_wrapper = std::move(wrapper);
}
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)>
NativeRestartSinkWrapperForTest() {
  std::lock_guard<std::mutex> guard(test_wrapper_mutex);
  return test_wrapper;
}
namespace {
std::mutex test_transaction_wrapper_mutex;
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)>
    test_transaction_wrapper;
}  // namespace
void SetNativeTransactionSinkWrapperForTest(
    std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)> wrapper) {
  std::lock_guard<std::mutex> guard(test_transaction_wrapper_mutex);
  test_transaction_wrapper = std::move(wrapper);
}
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)>
NativeTransactionSinkWrapperForTest() {
  std::lock_guard<std::mutex> guard(test_transaction_wrapper_mutex);
  return test_transaction_wrapper;
}
#endif

const char* NativeLoopTransactionErrorName(NativeLoopTransactionError error) noexcept {
  using E = NativeLoopTransactionError;
  switch (error) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::database_not_serving: return "database_not_serving";
    case E::database_failed: return "database_failed";
    case E::database_restricted: return "database_restricted";
    case E::busy: return "busy";
    case E::unknown_transaction: return "unknown_transaction";
    case E::not_owner: return "not_owner";
    case E::identity_failure: return "identity_failure";
    case E::writer_refused: return "writer_refused";
    case E::writer_failed: return "writer_failed";
  }
  return "invalid";
}

// One distinct code per cause (owner Q193). database_restricted is the Q299 code the native handle already uses.
const char* NativeLoopTransactionErrorCode(NativeLoopTransactionError error) noexcept {
  using E = NativeLoopTransactionError;
  switch (error) {
    case E::none: return "";
    case E::invalid_request: return "ENGINE.NATIVE.TRANSACTION_REQUEST_INVALID";
    case E::database_not_serving: return "ENGINE.NATIVE.TRANSACTION_DATABASE_NOT_SERVING";
    case E::database_failed: return "ENGINE.NATIVE.TRANSACTION_DATABASE_FAILED";
    case E::database_restricted: return "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY";
    case E::busy: return "ENGINE.NATIVE.TRANSACTION_QUEUE_BUSY";
    case E::unknown_transaction: return "ENGINE.NATIVE.TRANSACTION_UNKNOWN";
    case E::not_owner: return "ENGINE.NATIVE.TRANSACTION_NOT_OWNER";
    case E::identity_failure: return "ENGINE.NATIVE.TRANSACTION_IDENTITY_FAILED";
    case E::writer_refused: return "ENGINE.NATIVE.TRANSACTION_WRITER_REFUSED";
    case E::writer_failed: return "ENGINE.NATIVE.TRANSACTION_WRITER_FAILED";
  }
  return "";
}

const char* NativeServingStateName(NativeServingState state) noexcept {
  switch (state) {
    case NativeServingState::serving: return "serving";
    case NativeServingState::restricted: return "restricted";
    case NativeServingState::failed: return "failed";
  }
  return "failed";
}

namespace {
namespace db = storage::database;
namespace pg = storage::page;
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
  // S3d: the single writer of this open, constructed once by RunRestart and owned here (thread-affine
  // by its callers, not by itself; only this thread ever calls it). The issuer is the chain-growth
  // identity source its binding names; both are released before the runtime closes. PLAN36_T1:
  // CompleteAndBegin is a new method of THIS writer, same restarted precondition (not implemented).
  std::optional<core::uuid::StandaloneUuidV7Issuer> issuer;
  std::optional<pg::TransactionInventoryWriter> writer;
  // S4d (SB_NATIVE_LOOP_TRANSACTIONS): what a transaction request needs to write through the runtime's write door
  // (the filespace and page profile the careful-write sink addresses), kept from the open, and the ownership
  // table: transaction order -> the opaque owner key and the transaction's UUID. Loop thread only; memory only.
  core::platform::Uuid filespace_uuid, page_profile_uuid;
  pg::TransactionWriterBinding txn_binding;  // for the snapshot-point capture (reads the chains through the sink)
  struct OwnedTransaction {
    core::platform::Uuid owner_key;
    core::platform::Uuid transaction_uuid;
  };
  std::map<std::uint64_t, OwnedTransaction> transactions;
  // S4a (SB_NATIVE_LOGIN_MATERIAL): what a login-material read needs beyond the runtime, kept from the open: the
  // runtime authority binding's generation (the security reader is addressed through that binding) and the
  // fixed-root read limits. Values only.
  std::uint64_t runtime_binding_generation = 0;
  db::NativeFixedRootMemoryLimits login_limits;
  // S4b (SB_NATIVE_NAME_RESOLVE): the SCAN-tier relation limits the names relation needs (the open's own).
  db::NativeCatalogRelationMemoryLimits name_scan_limits;

  // Reverse of acquisition, explicit because the close result must be seen.
  // The read first (it pins the host and holds the source lease), then the
  // runtime, then the credential source.
  bool Teardown(NativeLoopDiagnostic* failure) noexcept {
    bool ok = true;
    transactions.clear();  // abandoned transactions stay active in the file: dead at the next open's Restart
    writer.reset();
    issuer.reset();
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
  loop->names_binding_->loop = loop.get();
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

// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL (S4a). Zeroes the protected password bytes of a request.
static void ConsumeLoginPassword(NativeLoopLoginRequest& request) noexcept {
  if (request.password.valid()) OPENSSL_cleanse(request.password.data(), request.password.size());
}

std::future<NativeLoginMaterialResult> NativeDatabaseLoop::SubmitReadLoginMaterial(NativeLoopLoginRequest request) {
  std::promise<NativeLoginMaterialResult> refused;
  const auto refuse = [&](NativeLoginMaterialError error) {
    ConsumeLoginPassword(request);
    NativeLoginMaterialResult result;
    result.error = error;
    refused.set_value(std::move(result));
    return refused.get_future();
  };
  if (on_loop_thread()) return refuse(NativeLoginMaterialError::invalid_request);  // the loop never waits on itself
  LoginWork work{std::move(request), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_ || state() != NativeLoopState::serving)
      return refuse(NativeLoginMaterialError::database_not_serving);
    if (serving_state() == NativeServingState::failed) return refuse(NativeLoginMaterialError::database_failed);
    if (queue_.size() >= kQueueCapacity) return refuse(NativeLoginMaterialError::busy);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

// SB_NATIVE_LOOP_TRANSACTIONS (S4d): the S4a read as a function of the loop thread, so a login's loop work can call it
// inside RunInEngineReadTransaction. The submit's HandleLogin calls the same code (one read path, no copy).
NativeLoginMaterialResult NativeDatabaseLoop::ReadLoginMaterialNow(NativeLoopLoginRequest& request) noexcept {
  NativeLoginMaterialResult result;
  try {
    if (!serving_ || state() != NativeLoopState::serving) {
      result.error = NativeLoginMaterialError::database_not_serving;
    } else if (serving_state() == NativeServingState::failed) {
      result.error = NativeLoginMaterialError::database_failed;
    } else {
      // serving or restricted (Q309): the credential read has no write and no transaction of its own.
      auto& serving = *serving_;
      NativeLoginMaterialRequest read;
      read.principal_uuid = request.principal_uuid;
      read.runtime_binding_uuid = serving.facts.runtime_binding_uuid;
      read.runtime_binding_generation = serving.runtime_binding_generation;
      read.limits = serving.login_limits;
      result = Host::ReadNativeLoginMaterial(serving.runtime, read, request.password, request.password_length,
                                             serving.derived);
    }
  } catch (...) {
    result = {};
    result.error = NativeLoginMaterialError::security_source_failure;
  }
  return result;
}

NativeLoginMaterialResult NativeDatabaseLoop::ReadLoginMaterialOnLoopThread(NativeLoopLoginRequest request) noexcept {
  if (!on_loop_thread()) {
    ConsumeLoginPassword(request);
    NativeLoginMaterialResult refused;
    refused.error = NativeLoginMaterialError::invalid_request;
    request.password = {};
    return refused;
  }
  auto result = ReadLoginMaterialNow(request);
  ConsumeLoginPassword(request);  // consumed whatever happened (the callee also zeroes it)
  request.password = {};          // released on this thread
  return result;
}

void NativeDatabaseLoop::HandleLogin(LoginWork& work) noexcept {
  auto result = ReadLoginMaterialNow(work.request);
  ConsumeLoginPassword(work.request);  // consumed whatever happened (the callee also zeroes it)
  work.request.password = {};          // released on this thread
  work.done.set_value(std::move(result));
}

// SEARCH_KEY: SB_NATIVE_NAMES_SOURCE (S4b rework "Part A"). Same admission as the login-material read: serving and
// restricted are allowed (Q309), failed and not-serving are refused before anything is queued.
std::future<engine::internal_api::NativeNamesSnapshot> NativeDatabaseLoop::SubmitReadNamesSnapshot() {
  using Snapshot = engine::internal_api::NativeNamesSnapshot;
  using Error = engine::internal_api::NativeNamesSnapshotError;
  std::promise<Snapshot> refused;
  const auto refuse = [&](Error error) {
    Snapshot result;
    result.error = error;
    refused.set_value(std::move(result));
    return refused.get_future();
  };
  if (on_loop_thread()) return refuse(Error::invalid_request);  // the loop never waits on itself
  NamesWork work{{}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_ || state() != NativeLoopState::serving) return refuse(Error::database_not_serving);
    if (serving_state() == NativeServingState::failed) return refuse(Error::database_failed);
    if (queue_.size() >= kQueueCapacity) return refuse(Error::busy);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

engine::internal_api::NativeNamesSnapshot NativeDatabaseLoop::ReadNamesSnapshotOnLoopThread() noexcept {
  if (!on_loop_thread()) {
    engine::internal_api::NativeNamesSnapshot refused;
    refused.error = engine::internal_api::NativeNamesSnapshotError::invalid_request;
    return refused;
  }
  return ReadNamesSnapshotNow();
}

engine::internal_api::NativeNamesSnapshot NativeDatabaseLoop::ReadNamesSnapshotNow() noexcept {
  using Error = engine::internal_api::NativeNamesSnapshotError;
  engine::internal_api::NativeNamesSnapshot result;
  try {
    if (!serving_ || state() != NativeLoopState::serving) {
      result.error = Error::database_not_serving;
    } else if (serving_state() == NativeServingState::failed) {
      result.error = Error::database_failed;
    } else {
      // serving or restricted (Q309): a names read has no write and no transaction.
      auto& serving = *serving_;
      NativeNamesSnapshotRequest request;
      request.limits = serving.login_limits;
      request.catalog = serving.name_scan_limits;
      result = Host::ReadNativeNamesSnapshot(serving.runtime, request);
    }
  } catch (...) {
    result = {};
    result.error = Error::names_source_failure;
  }
  return result;
}

bool NativeDatabaseLoop::RunOnLoopThreadForTest(std::function<void()> work) {
  if (on_loop_thread() || !work) return false;
  TestWork item{std::move(work), {}};
  auto future = item.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_ || queue_.size() >= kQueueCapacity) return false;
    queue_.emplace_back(std::move(item));
  }
  wake_.notify_one();
  future.get();
  return true;
}

void NativeDatabaseLoop::HandleNames(NamesWork& work) noexcept {
  work.done.set_value(ReadNamesSnapshotNow());
}

namespace {
// SB_NATIVE_NAMES_SOURCE: the source the served facts carry. From the loop thread it reads directly; from any other
// thread it submits to the loop and waits. A source that outlives its loop answers `closed`.
class NativeLoopNamesSource final : public engine::internal_api::NativeNamesSource {
 public:
  explicit NativeLoopNamesSource(std::shared_ptr<NativeNamesBinding> binding)
      : binding_(std::move(binding)) {}
  engine::internal_api::NativeNamesSnapshot Read() const noexcept override {
    using Error = engine::internal_api::NativeNamesSnapshotError;
    engine::internal_api::NativeNamesSnapshot closed;
    closed.error = Error::closed;
    try {
      NativeDatabaseLoop* loop = nullptr;
      std::future<engine::internal_api::NativeNamesSnapshot> future;
      {
        std::lock_guard<std::mutex> guard(binding_->mutex);
        loop = binding_->loop;
        if (loop == nullptr) return closed;
        if (!loop->on_loop_thread()) future = loop->SubmitReadNamesSnapshot();
      }
      // On the loop thread the loop is alive by construction: read directly, outside the binding lock.
      if (!future.valid()) return loop->ReadNamesSnapshotOnLoopThread();
      return future.get();
    } catch (...) {
      engine::internal_api::NativeNamesSnapshot failed;
      failed.error = Error::names_source_failure;
      return failed;
    }
  }

 private:
  std::shared_ptr<NativeNamesBinding> binding_;
};
}  // namespace

// ===================================================================================================================
// SEARCH_KEY: SB_NATIVE_LOOP_TRANSACTIONS (thin-slice S4d, plan 40; owner Q305, Q309, Q315). See the header.
// Everything below runs on the loop thread: the writer is thread-affine by its callers, and every write goes through
// the runtime's write door (TryBeginWriterOperation), one custody operation per request, released before the request
// returns. The writer's own refusals (restart not run, unknown order, header codec, horizon regression, chain full,
// read-only database) all come BEFORE any write: the request is `writer_refused`, nothing changed, and the loop
// goes on. A write or sync failure leaves the file in a crash state the next open's Restart resolves: the request is
// `writer_failed` and the database becomes `failed` (reopen), exactly as after a failed Restart.
// ===================================================================================================================
namespace {
std::uint64_t NowUtcNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Opens the write door, builds the careful-write sink (decorated by the test seam when one is set) and runs `body`
// with it; the operation is released when this returns. False, with `out` filled, when the door could not be opened.
template <class S, class Body>
bool WithTransactionSink(S& serving, NativeLoopTransactionResult& out, Body&& body) {
  const auto operation_identity = core::uuid::IssueRuntimeIdentityV7();
  if (!operation_identity) {
    out.error = NativeLoopTransactionError::identity_failure;
    return false;
  }
  // Declared before the sink: released after it.
  auto write_operation = Host::TryBeginWriterOperation(serving.runtime, *operation_identity);
  if (!write_operation.ok()) {
    out.error = NativeLoopTransactionError::writer_refused;
    out.writer_error = "writer_operation_refused";
    return false;
  }
  NativeHostedCarefulWriteSink sink(write_operation.operation, serving.filespace_uuid, serving.page_profile_uuid);
  std::unique_ptr<storage::disk::CarefulWriteSink> decorated;
  storage::disk::CarefulWriteSink* writing = &sink;
#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
  if (const auto wrapper = NativeTransactionSinkWrapperForTest()) {
    decorated = wrapper(sink);
    if (decorated) writing = decorated.get();
  }
#endif
  body(*writing);
  return true;
}

template <class S>
bool IssueTransactionUuid(S& serving, core::platform::Uuid& out) noexcept {
  if (!serving.issuer) return false;
  const auto issued = serving.issuer->Issue(core::platform::UuidKind::transaction);
  if (!issued.ok()) return false;
  out = issued.value->value;
  return true;
}

// The header of a transaction as S4d states it: local authority, intent 0, the caller's isolation profile and
// identities, the loop's own engine instance (+48). Policy (+112), security (+128) and start snapshot (+144) stay
// zero (no policy snapshot exists before plan 32; Q316: the writer takes +144 as given). The writer sets the
// database id and the order.
template <class S>
pg::TransactionHeaderRecord MakeTransactionHeader(const S& serving, const core::platform::Uuid& transaction_uuid,
                                                  bool read_only, std::uint8_t isolation_profile,
                                                  const NativeLoopTransactionIdentity& identity) noexcept {
  pg::TransactionHeaderRecord header;
  header.authority = pg::TipAuthorityClass::local;
  header.intent = 0;
  header.isolation_profile = isolation_profile;
  header.read_only = read_only;
  header.transaction_uuid = transaction_uuid;
  header.engine_instance_uuid = serving.facts.engine_instance_uuid;
  header.principal_or_agent_uuid = identity.principal_uuid;
  header.attachment_uuid = identity.attachment_uuid;
  header.session_uuid = identity.session_uuid;
  return header;
}
}  // namespace

NativeLoopTransactionError NativeDatabaseLoop::AdmitTransactionWork() noexcept {
  if (!serving_ || state() != NativeLoopState::serving) return NativeLoopTransactionError::database_not_serving;
  switch (serving_state()) {
    case NativeServingState::failed: return NativeLoopTransactionError::database_failed;
    case NativeServingState::restricted: return NativeLoopTransactionError::database_restricted;
    case NativeServingState::serving: break;
  }
  if (!serving_->writer || !serving_->writer->restarted()) return NativeLoopTransactionError::database_not_serving;
  if (serving_->writer->failed()) return NativeLoopTransactionError::database_failed;
  return NativeLoopTransactionError::none;
}

NativeLoopTransactionError NativeDatabaseLoop::AdmitQueuedTransaction() const noexcept {
  if (!accepting_ || state() != NativeLoopState::serving) return NativeLoopTransactionError::database_not_serving;
  if (serving_state() == NativeServingState::failed) return NativeLoopTransactionError::database_failed;
  if (serving_state() == NativeServingState::restricted) return NativeLoopTransactionError::database_restricted;
  if (queue_.size() >= kQueueCapacity) return NativeLoopTransactionError::busy;
  return NativeLoopTransactionError::none;
}

void NativeDatabaseLoop::NoteTransactionWriterFailure() noexcept {
  if (!serving_) return;
  auto& facts = serving_->facts;
  facts.serving_state = NativeServingState::failed;
  facts.serving_diagnostic = Refusal("DATABASE.TRANSACTION_WRITE_FAILED", "transaction", {{"requires", "reopen"}});
  serving_state_.store(NativeServingState::failed, std::memory_order_release);
}

NativeLoopTransactionResult NativeDatabaseLoop::BeginNow(const NativeLoopBeginRequest& request) noexcept {
  NativeLoopTransactionResult out;
  try {
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    if (request.owner_key.is_nil()) {
      out.error = NativeLoopTransactionError::invalid_request;
      return out;
    }
    auto& serving = *serving_;
    core::platform::Uuid transaction_uuid;
    if (!IssueTransactionUuid(serving, transaction_uuid)) {
      out.error = NativeLoopTransactionError::identity_failure;
      return out;
    }
    const auto header = MakeTransactionHeader(serving, transaction_uuid, request.read_only, request.isolation_profile,
                                              request.identity);
    pg::TransactionBeginResult begun;
    if (!WithTransactionSink(serving, out, [&](storage::disk::CarefulWriteSink& sink) {
          begun = serving.writer->Begin(sink, header);
        }))
      return out;
    out.writes = begun.writes;
    out.syncs = begun.syncs;
    out.growth_writes = begun.growth_writes;
    out.growth_syncs = begun.growth_syncs;
    if (!begun.ok()) {
      out.writer_error = pg::TransactionWriterErrorName(begun.error);
      if (serving.writer->failed()) {
        out.error = NativeLoopTransactionError::writer_failed;
        NoteTransactionWriterFailure();
      } else {
        out.error = NativeLoopTransactionError::writer_refused;
      }
      return out;
    }
    serving.transactions[*begun.transaction_order] = {request.owner_key, transaction_uuid};
    out.begun_order = *begun.transaction_order;
    out.begun_uuid = transaction_uuid;
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.writer_error = "exception";
    return out;
  }
}

NativeLoopTransactionResult NativeDatabaseLoop::CompleteNow(const NativeLoopCompleteRequest& request) noexcept {
  NativeLoopTransactionResult out;
  try {
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    if (request.owner_key.is_nil() || request.transaction_order == 0 ||
        (request.completion != NativeLoopCompletion::commit && request.completion != NativeLoopCompletion::rollback)) {
      out.error = NativeLoopTransactionError::invalid_request;
      return out;
    }
    auto& serving = *serving_;
    const auto owned = serving.transactions.find(request.transaction_order);
    if (owned == serving.transactions.end()) {
      out.error = NativeLoopTransactionError::unknown_transaction;
      return out;
    }
    if (!(owned->second.owner_key == request.owner_key)) {  // Q315: before anything is written
      out.error = NativeLoopTransactionError::not_owner;
      return out;
    }
    pg::TransactionCompleteResult done;
    if (!WithTransactionSink(serving, out, [&](storage::disk::CarefulWriteSink& sink) {
          done = request.completion == NativeLoopCompletion::commit
                     ? serving.writer->Commit(sink, request.transaction_order, NowUtcNs())
                     : serving.writer->Rollback(sink, request.transaction_order, NowUtcNs());
        }))
      return out;
    out.writes = done.writes;
    out.syncs = done.syncs;
    out.outcome_uncertain = done.outcome_uncertain;
    if (!done.ok()) {
      out.writer_error = pg::TransactionWriterErrorName(done.error);
      if (serving.writer->failed()) {
        out.error = NativeLoopTransactionError::writer_failed;
        NoteTransactionWriterFailure();
      } else {
        out.error = NativeLoopTransactionError::writer_refused;
      }
      return out;
    }
    serving.transactions.erase(owned);
    out.completed_order = request.transaction_order;
    out.committed = done.final_state == pg::TipStateCode::committed;
    out.completed_at_utc_ns = done.completed_at_utc_ns;
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.writer_error = "exception";
    return out;
  }
}

NativeLoopTransactionResult NativeDatabaseLoop::CompleteAndBeginNow(
    const NativeLoopCompleteAndBeginRequest& request) noexcept {
  NativeLoopTransactionResult out;
  try {
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    if (request.owner_key.is_nil() || request.transaction_order == 0 ||
        (request.completion != NativeLoopCompletion::commit && request.completion != NativeLoopCompletion::rollback)) {
      out.error = NativeLoopTransactionError::invalid_request;
      return out;
    }
    auto& serving = *serving_;
    const auto owned = serving.transactions.find(request.transaction_order);
    if (owned == serving.transactions.end()) {
      out.error = NativeLoopTransactionError::unknown_transaction;
      return out;
    }
    if (!(owned->second.owner_key == request.owner_key)) {  // Q315: before CompleteAndBegin writes anything
      out.error = NativeLoopTransactionError::not_owner;
      return out;
    }
    core::platform::Uuid replacement_uuid;
    if (!IssueTransactionUuid(serving, replacement_uuid)) {
      out.error = NativeLoopTransactionError::identity_failure;
      return out;
    }
    const auto header = MakeTransactionHeader(serving, replacement_uuid, request.read_only, request.isolation_profile,
                                              request.identity);
    pg::TransactionCompleteAndBeginResult done;
    if (!WithTransactionSink(serving, out, [&](storage::disk::CarefulWriteSink& sink) {
          done = serving.writer->CompleteAndBegin(sink, request.transaction_order,
                                                  request.completion == NativeLoopCompletion::commit
                                                      ? pg::TipStateCode::committed
                                                      : pg::TipStateCode::rolled_back,
                                                  header, NowUtcNs());
        }))
      return out;
    out.writes = done.writes;
    out.syncs = done.syncs;
    out.growth_writes = done.growth_writes;
    out.growth_syncs = done.growth_syncs;
    out.outcome_uncertain = done.outcome_uncertain;
    if (!done.ok()) {
      out.writer_error = pg::TransactionWriterErrorName(done.error);
      if (serving.writer->failed()) {
        out.error = NativeLoopTransactionError::writer_failed;
        NoteTransactionWriterFailure();
      } else {
        out.error = NativeLoopTransactionError::writer_refused;
      }
      return out;
    }
    const auto owner = owned->second.owner_key;
    serving.transactions.erase(owned);
    serving.transactions[done.replacement_order] = {owner, replacement_uuid};  // the replacement has the same owner
    out.completed_order = request.transaction_order;
    out.committed = done.final_state == pg::TipStateCode::committed;
    out.completed_at_utc_ns = done.completed_at_utc_ns;
    out.begun_order = done.replacement_order;
    out.begun_uuid = replacement_uuid;
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.writer_error = "exception";
    return out;
  }
}

template <class WorkT, class Request>
std::future<NativeLoopTransactionResult> NativeDatabaseLoop::SubmitTransaction(Request request) {
  const auto refused = [](NativeLoopTransactionError error) {
    std::promise<NativeLoopTransactionResult> promise;
    NativeLoopTransactionResult result;
    result.error = error;
    promise.set_value(std::move(result));
    return promise.get_future();
  };
  if (on_loop_thread()) return refused(NativeLoopTransactionError::invalid_request);  // the loop never waits on itself
  WorkT work{std::move(request), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (const auto error = AdmitQueuedTransaction(); error != NativeLoopTransactionError::none) return refused(error);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

std::future<NativeLoopTransactionResult> NativeDatabaseLoop::SubmitBeginWork(NativeLoopBeginRequest request) {
  return SubmitTransaction<BeginWork>(std::move(request));
}
std::future<NativeLoopTransactionResult> NativeDatabaseLoop::SubmitCompleteWork(NativeLoopCompleteRequest request) {
  return SubmitTransaction<CompleteWork>(std::move(request));
}
std::future<NativeLoopTransactionResult> NativeDatabaseLoop::SubmitCompleteAndBeginWork(
    NativeLoopCompleteAndBeginRequest request) {
  return SubmitTransaction<CompleteAndBeginWork>(std::move(request));
}

NativeLoopTransactionResult NativeDatabaseLoop::BeginWorkOnLoopThread(const NativeLoopBeginRequest& request) noexcept {
  if (!on_loop_thread()) {
    NativeLoopTransactionResult refused;
    refused.error = NativeLoopTransactionError::invalid_request;
    return refused;
  }
  return BeginNow(request);
}
NativeLoopTransactionResult NativeDatabaseLoop::CompleteWorkOnLoopThread(
    const NativeLoopCompleteRequest& request) noexcept {
  if (!on_loop_thread()) {
    NativeLoopTransactionResult refused;
    refused.error = NativeLoopTransactionError::invalid_request;
    return refused;
  }
  return CompleteNow(request);
}
NativeLoopTransactionResult NativeDatabaseLoop::CompleteAndBeginWorkOnLoopThread(
    const NativeLoopCompleteAndBeginRequest& request) noexcept {
  if (!on_loop_thread()) {
    NativeLoopTransactionResult refused;
    refused.error = NativeLoopTransactionError::invalid_request;
    return refused;
  }
  return CompleteAndBeginNow(request);
}

std::vector<std::uint64_t> NativeDatabaseLoop::OpenTransactionOrdersOnLoopThread() const {
  std::vector<std::uint64_t> orders;
  if (!on_loop_thread() || !serving_) return orders;
  for (const auto& entry : serving_->transactions) orders.push_back(entry.first);
  return orders;
}

void NativeDatabaseLoop::HandleTransaction(BeginWork& work) noexcept { work.done.set_value(BeginNow(work.request)); }
void NativeDatabaseLoop::HandleTransaction(CompleteWork& work) noexcept { work.done.set_value(CompleteNow(work.request)); }
void NativeDatabaseLoop::HandleTransaction(CompleteAndBeginWork& work) noexcept {
  work.done.set_value(CompleteAndBeginNow(work.request));
}

// SEARCH_KEY: SB_NATIVE_ENGINE_READ_TRANSACTION (owner 2026-11-21). begin (read-only, isolation read_committed_statement,
// the engine instance as +48, principal and session identities ZERO) -> work -> complete (commit) -> report.
//
// HEADER PRINCIPAL (+64) of an engine-owned read transaction is UNSETTLED (owner question). The codec admits zero
// ("not yet bound"); the only provisioned internal principal in a created file is L5's service principal (the
// facts' service_principal_uuid), which this scope must not borrow (L5 is an agent with its own session and
// transactions), and a principal may not be minted here. So the least-invented value is zero.
//
// SB_READ_TXN_SNAPSHOT_SEAM: the reads the work makes (ReadLoginMaterialOnLoopThread, ReadNamesSnapshotOnLoopThread)
// are NOT bound to this transaction's snapshot. They call the catalog visibility reader's committed entries
// (ReadCommittedSelected / ReadCommitted), which compute visibility from the inventory read at that moment:
// "committed through next_transaction - 1 / next_commit_sequence - 1, own uncommitted rows hidden". The reader's
// only snapshot-bound entry, ReadPinned, needs a mga::TransactionIdentity and a published snapshot pin
// (mga::PublishedSnapshotPin from the MGA snapshot publisher: descriptor with visible_committed_high_watermark and
// the active / in-doubt exclusion lists); the native TIP writer publishes no snapshot, a native transaction has no
// TransactionIdentity of that kind, and neither security read passes a pin. What IS true today: the loop is the only
// writer and runs the reads right after the begin with nothing interleaved, so "committed now" equals "committed as
// of this transaction's start" for this scope. That is a property of the loop's serialisation, not of the
// transaction. A scope that spans loop turns, or a session transaction, needs the snapshot (built from the file's
// TIP at BEGIN: visible-through order, commit sequence and the active list) passed to the reader.
// SB_NATIVE_ENGINE_READ_TRANSACTION mode snapshot_point: NO header, NO write, NO transaction number. The capture is
// the TIP counters (Next and the stored horizons) read through the write door's sink (reads only) once, held in
// memory for the work, like Firebird's in-memory system transaction 0. Needs only a served, not failed database
// (restricted included). What a snapshot-bound read still needs from the visibility reader: see SB_READ_TXN_SNAPSHOT_SEAM.
NativeLoopReadScopeResult NativeDatabaseLoop::RunSnapshotScopeNow(
    const std::function<void(const NativeLoopReadTransaction&)>& work) noexcept {
  NativeLoopReadScopeResult out;
  out.mode = NativeLoopReadScopeMode::snapshot_point;
  try {
    if (!serving_ || state() != NativeLoopState::serving) {
      out.begin.error = NativeLoopTransactionError::database_not_serving;
      return out;
    }
    if (serving_state() == NativeServingState::failed) {
      out.begin.error = NativeLoopTransactionError::database_failed;
      return out;
    }
    auto& serving = *serving_;
    NativeLoopReadTransaction info;
    info.mode = NativeLoopReadScopeMode::snapshot_point;
    if (!WithTransactionSink(serving, out.begin, [&](storage::disk::CarefulWriteSink& sink) {
          const auto chains = pg::ReadTransactionChainsThroughSink(sink, serving.txn_binding);
          if (!chains.ok()) {
            out.begin.error = NativeLoopTransactionError::writer_refused;
            out.begin.writer_error = pg::TransactionWriterErrorName(chains.error);
            return;
          }
          info.snapshot_next_transaction = chains.counters.next_transaction;
          info.snapshot_oit = chains.counters.horizons.oldest_interesting;
          info.snapshot_oat = chains.counters.horizons.oldest_active;
          info.snapshot_ost = chains.counters.horizons.oldest_snapshot;
          out.begin.error = NativeLoopTransactionError::none;
        }))
      return out;
    if (!out.begin.ok()) return out;
    out.snapshot_next_transaction = info.snapshot_next_transaction;
    if (work) {
      try {
        work(info);
        out.work_ran = true;
      } catch (...) {
        out.work_ran = true;
        out.work_threw = true;
      }
    }
    out.complete.error = NativeLoopTransactionError::none;  // nothing to complete: no transaction exists
    return out;
  } catch (...) {
    out.work_threw = true;
    return out;
  }
}

NativeLoopReadScopeResult NativeDatabaseLoop::RunReadScopeNow(
    const std::function<void(const NativeLoopReadTransaction&)>& work, NativeLoopReadScopeMode mode) noexcept {
  if (mode == NativeLoopReadScopeMode::snapshot_point) return RunSnapshotScopeNow(work);
  NativeLoopReadScopeResult out;
  try {
    const auto owner = core::uuid::IssueRuntimeIdentityV7();
    if (!owner) {
      out.begin.error = NativeLoopTransactionError::identity_failure;
      return out;
    }
    NativeLoopBeginRequest begin;
    begin.owner_key = *owner;
    begin.read_only = true;
    begin.isolation_profile = static_cast<std::uint8_t>(core::catalog::IsolationProfileKind::read_committed_statement);
    out.begin = BeginNow(begin);
    if (!out.begin.ok()) return out;  // the work does not run
    NativeLoopReadTransaction info;
    info.order = out.begin.begun_order;
    info.transaction_uuid = out.begin.begun_uuid;
    info.owner_key = *owner;
    if (work) {
      try {
        work(info);
        out.work_ran = true;
      } catch (...) {
        out.work_ran = true;
        out.work_threw = true;
      }
    }
    // ALWAYS completed: a read-only transaction commits (as Firebird's), refusal or exception of the work included.
    NativeLoopCompleteRequest complete;
    complete.owner_key = *owner;
    complete.transaction_order = info.order;
    complete.completion = NativeLoopCompletion::commit;
    out.complete = CompleteNow(complete);
    return out;
  } catch (...) {
    out.work_threw = true;
    return out;
  }
}

NativeLoopReadScopeResult NativeDatabaseLoop::RunInEngineReadTransaction(
    std::function<void(const NativeLoopReadTransaction&)> work, NativeLoopReadScopeMode mode) {
  if (on_loop_thread()) return RunReadScopeNow(work, mode);
  const auto refused = [](NativeLoopTransactionError error) {
    NativeLoopReadScopeResult result;
    result.begin.error = error;
    return result;
  };
  ReadScopeWork item{std::move(work), mode, {}};
  auto future = item.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto error = AdmitQueuedTransaction();
    // snapshot_point is a read: a restricted database serves it (Q309), only the queue bound and failed refuse.
    if (mode == NativeLoopReadScopeMode::snapshot_point && error == NativeLoopTransactionError::database_restricted)
      error = queue_.size() >= kQueueCapacity ? NativeLoopTransactionError::busy : NativeLoopTransactionError::none;
    if (error != NativeLoopTransactionError::none) return refused(error);
    queue_.emplace_back(std::move(item));
  }
  wake_.notify_one();
  return future.get();
}

void NativeDatabaseLoop::HandleReadScope(ReadScopeWork& work) noexcept { work.done.set_value(RunReadScopeNow(work.work, work.mode)); }

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
    if (auto* login = std::get_if<LoginWork>(&work)) {
      HandleLogin(*login);
      continue;
    }
    if (auto* names = std::get_if<NamesWork>(&work)) {
      HandleNames(*names);
      continue;
    }
    if (auto* begin = std::get_if<BeginWork>(&work)) {
      HandleTransaction(*begin);
      continue;
    }
    if (auto* complete = std::get_if<CompleteWork>(&work)) {
      HandleTransaction(*complete);
      continue;
    }
    if (auto* replace = std::get_if<CompleteAndBeginWork>(&work)) {
      HandleTransaction(*replace);
      continue;
    }
    if (auto* scope = std::get_if<ReadScopeWork>(&work)) {
      HandleReadScope(*scope);
      continue;
    }
    if (auto* test = std::get_if<TestWork>(&work)) {
      test->work();
      test->done.set_value();
      continue;
    }
    auto& close = std::get<CloseWork>(work);
    close.done.set_value(HandleClose());
    return;
  }
}

NativeLoopCloseResult NativeDatabaseLoop::HandleClose() noexcept {
  NativeLoopCloseResult result;
  {
    // SB_NATIVE_NAMES_SOURCE: a source handed out in the served facts stops reaching this loop now.
    std::lock_guard<std::mutex> guard(names_binding_->mutex);
    names_binding_->loop = nullptr;
  }
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

// SEARCH_KEY: SB_NATIVE_RESTART (thin-slice S3d, plan 38 2d; owner Q241 = 1, Q284).
// The transaction-store Restart (FirebirdSQL TRA_cleanup) of this open, run ONCE on the loop thread
// through the runtime's write door (TryBeginWriterOperation, S3c). Binding: state_root = page-zero
// root kind 4, allocation_root = kind 3 + an identity issuer (chain growth; Restart itself never
// grows), read_only_database from the verified system state (SBSYS002), default barrier policy.
// Restart marks every in-progress slot, every unused slot below Next (a header over an unused slot is
// one) dead; a CLEAN file performs 0 writes and 0 syncs (checked here: a clean Restart that wrote is
// refused). Counters are not rewritten. committing / recovering slots are "unresolved": Restart
// refuses BEFORE any write and the database opens restricted (Q241 = 1). A write or sync failure
// leaves the writer failed (refuses everything) and the database in the failed serving state.
// Nothing begins a transaction here (Q284: L5's first transaction is S5).
bool NativeDatabaseLoop::RunRestart(const NativeLoopOpenRequest& request,
                                    const NativeOpenVerificationObservation& observed,
                                    NativeLoopDiagnostic& refusal) noexcept {
  try {
    auto& serving = *serving_;
    auto& facts = serving.facts;
    auto& report = facts.restart;
    serving.filespace_uuid = request.filespace_uuid;           // S4d: the write door's addressing, kept
    serving.page_profile_uuid = observed.identity.primary_profile_uuid;
    report.uncertain_transactions = observed.inventory.in_doubt_transactions;
    const auto reject = [&](const char* step, std::vector<std::pair<std::string, std::string>> fields) {
      refusal = Refusal("DATABASE.OPEN_NATIVE_RESTART_REFUSED", step, std::move(fields));
      return false;
    };

    pg::TransactionWriterBinding binding;
    binding.database_uuid = request.database_uuid;
    const storage::disk::FilespaceRootReference* state_root = nullptr;
    const storage::disk::FilespaceRootReference* map_root = nullptr;
    for (std::size_t i = 0; i < observed.identity.primary_root_count && i < observed.identity.primary_roots.size(); ++i) {
      const auto& root = observed.identity.primary_roots[i];
      if (root.kind == 4 && !state_root) state_root = &root;
      if (root.kind == 3 && !map_root) map_root = &root;
    }
    if (!state_root) return reject("restart_binding", {{"error", "state_root_absent"}});
    binding.state_root = *state_root;
    binding.read_only_database = (observed.identity.system_state.flags & db::NativeDatabaseStateFlag::read_only) != 0;
    binding.refuse_restart_with_unresolved = true;
    // Chain growth needs the allocation map and an issuer. The issuer's policy-snapshot input is the
    // engine instance for now: no policy snapshot exists before plan 32 (owner question). Restart
    // never issues, so this is only the binding the later BEGIN will use.
    if (map_root) {
      serving.issuer.emplace(core::uuid::StandaloneUuidV7Binding{request.database_uuid, facts.engine_instance_uuid},
                             core::uuid::kStandardLocalClockUuidV7Policy);
      binding.allocation_root = *map_root;
      binding.issuer = &*serving.issuer;
    }
    serving.txn_binding = binding;
    serving.writer.emplace(binding);
    if (serving.writer->page_size() == 0) return reject("restart_binding", {{"error", "writer_binding_invalid"}});

    const auto operation_identity = core::uuid::IssueRuntimeIdentityV7();
    if (!operation_identity) return reject("writer_operation", {{"error", "identity_issue_failed"}});
    // Declared before the sink: released after it.
    auto write_operation = Host::TryBeginWriterOperation(serving.runtime, *operation_identity);
    if (!write_operation.ok())
      return reject("writer_operation", {{"custody_error", Num(static_cast<unsigned>(write_operation.error))},
                                         {"writer_error", Num(static_cast<unsigned>(write_operation.writer_error))},
                                         {"route_error", Num(static_cast<unsigned>(write_operation.route_error))}});
    NativeHostedCarefulWriteSink sink(write_operation.operation, request.filespace_uuid,
                                      observed.identity.primary_profile_uuid);
    std::unique_ptr<storage::disk::CarefulWriteSink> decorated;
    storage::disk::CarefulWriteSink* writing = &sink;
#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
    if (const auto wrapper = NativeRestartSinkWrapperForTest()) {
      decorated = wrapper(sink);
      if (decorated) writing = decorated.get();
    }
#endif

    const auto restart = serving.writer->Restart(*writing);
    // The stored horizons, read back (Restart never rewrites them), unless a write failed (the
    // writer is failed and the file is in a crash state: report nothing it might misstate).
    const auto read_horizons = [&] {
      const auto chains = pg::ReadTransactionChainsThroughSink(sink, binding);
      if (!chains.ok()) return;
      report.next_transaction = chains.counters.next_transaction;
      report.horizon_oit = chains.counters.horizons.oldest_interesting;
      report.horizon_oat = chains.counters.horizons.oldest_active;
      report.horizon_ost = chains.counters.horizons.oldest_snapshot;
      report.horizon_oct = chains.counters.horizons.oldest_cleanup;
      report.horizon_oar = chains.counters.horizons.oldest_archive_required;
      report.horizon_olr = chains.counters.horizons.oldest_limbo_resolution;
    };
    if (!serving.writer->failed()) read_horizons();
    report.writes = restart.writes;
    report.syncs = restart.syncs;
    report.next_transaction = restart.next_transaction;
    report.scanned_from = restart.scanned_from;
    report.marked_dead = restart.marked_dead;
    report.unresolved = restart.unresolved;
    report.writer_error = pg::TransactionWriterErrorName(restart.error);
    report.read_only_skipped = restart.read_only_skipped;
    if (restart.write_error != storage::disk::CarefulWriteError::none)
      report.write_error = storage::disk::CarefulWriteErrorName(restart.write_error);
    report.native_io_error = static_cast<std::int32_t>(restart.io.native_error);

    if (restart.ok()) {
      // A clean file performs no write and no sync.
      if (restart.marked_dead == 0 && (restart.writes != 0 || restart.syncs != 0))
        return reject("restart", {{"error", "clean_restart_wrote"}, {"writes", Num(restart.writes)}});
      report.ran = true;
      facts.restart_pending = false;
      facts.serving_state = NativeServingState::serving;
    } else if (restart.error == pg::TransactionWriterError::unresolved_transactions) {
      facts.restart_pending = true;
      facts.serving_state = NativeServingState::restricted;
      facts.serving_diagnostic = Refusal(
          "DATABASE.OPEN_RESTRICTED_UNRESOLVED_TRANSACTIONS", "restart",
          {{"unresolved_count", Num(restart.unresolved.size())},
           {"first_unresolved_order", Num(restart.unresolved.empty() ? 0 : restart.unresolved.front())},
           {"uncertain_count_including_unresolved", Num(report.uncertain_transactions)},
           {"written", "0"}});
    } else if (serving.writer->failed()) {
      report.writer_failed = true;
      facts.restart_pending = true;
      facts.serving_state = NativeServingState::failed;
      facts.serving_diagnostic = Refusal(
          "DATABASE.RESTART_WRITE_FAILED", "restart",
          {{"error", report.writer_error}, {"write_error", report.write_error},
           {"native_io_error", Num(static_cast<std::uint32_t>(restart.io.native_error))},
           {"writes_done", Num(restart.writes)}, {"syncs_issued", Num(restart.syncs)},
           {"requires", "reopen"}});
    } else {
      // Refused before any write (a read failed, the inventory no longer checks, resources): the
      // file is unchanged; the open is refused.
      return reject("restart", {{"error", report.writer_error},
                                {"page_error", Num(static_cast<unsigned>(restart.page_error))}});
    }
    serving_state_.store(facts.serving_state, std::memory_order_release);
    return true;
  } catch (...) {
    refusal = Refusal("DATABASE.OPEN_NATIVE_LOOP_EXCEPTION", "restart");
    return false;
  }
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
    serving.runtime_binding_generation = service.runtime_binding_generation;
    serving.login_limits = limits.fixed_root;
    serving.name_scan_limits = limits.scan;

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

    // ---- verified; S3d: the credential read ends, then Restart ----
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
    // SB_NATIVE_SERVED_DATABASE_FACTS (S3e): the datatype cohort of the captured database record (the one live,
    // unprovisional row of kind database whose key is the page-zero database UUID). Read from the read's own
    // capture BEFORE the read is released; absent when the record carries none (the statement path then
    // refuses; a frozen record needs an explicit migration, never a relabel).
    std::optional<core::catalog::CatalogDatabaseDatatypeCohort> captured_cohort;
    for (const auto& captured : read.catalog_capture().rows()) {
      const auto& metadata = captured.row.metadata;
      if (captured.row.provisional || metadata.record.header.deleted ||
          metadata.record.header.kind != core::catalog::CatalogRecordKind::database ||
          metadata.record.header.object_uuid.value != request.database_uuid)
        continue;
      const auto decoded = core::catalog::DecodeCatalogDatabaseRecord(metadata.record.payload);
      if (decoded.ok()) captured_cohort = decoded.record->datatype_cohort;
      break;
    }
    // SB_NATIVE_RESTART ordering (plan 38 risk "custody operation exclusivity"): the credential read
    // retains the custody's ONE operation until it is reset, and an owned-writer operation is
    // exclusive (a second TryBeginOperation is busy), so the read is released here, BEFORE the writer
    // takes the write operation. Everything the verification published is copied above; the read's
    // retained session / catalog capture end with it (S3e re-acquires what it needs).
    serving.read.reset();
    NativeLoopDiagnostic restart_refusal;
    if (!RunRestart(request, observed, restart_refusal)) return fail(std::move(restart_refusal));
    {
      // SB_NATIVE_SERVED_DATABASE_FACTS (S3e): the immutable marker, complete only after Restart (the stored
      // horizons are the ones RunRestart read back).
      auto marker = std::make_shared<engine::internal_api::NativeServedDatabaseFacts>();
      marker->database_uuid = request.database_uuid;
      marker->filespace_uuid = request.filespace_uuid;
      marker->page_profile_uuid = observed.identity.primary_profile_uuid;
      marker->engine_instance_uuid = serving.facts.engine_instance_uuid;
      marker->page_size_bytes = request.page_size_bytes;
      marker->total_pages = request.total_pages;
      for (std::size_t i = 0; i < observed.identity.primary_root_count && i < observed.identity.primary_roots.size(); ++i)
        marker->fixed_roots.push_back(observed.identity.primary_roots[i]);
      if (captured_cohort) {
        marker->datatype_cohort_present = true;
        marker->datatype_catalog_snapshot_uuid = captured_cohort->catalog_snapshot_uuid.value;
        marker->datatype_catalog_generation = captured_cohort->catalog_generation;
        marker->datatype_registry_generation = captured_cohort->registry_generation;
      }
      const auto& restarted = serving.facts.restart;
      marker->tip_next_transaction = restarted.next_transaction;
      marker->tip_horizon_oit = restarted.horizon_oit;
      marker->tip_horizon_oat = restarted.horizon_oat;
      marker->tip_horizon_ost = restarted.horizon_ost;
      marker->tip_horizon_oct = restarted.horizon_oct;
      marker->tip_horizon_oar = restarted.horizon_oar;
      marker->tip_horizon_olr = restarted.horizon_olr;
      marker->availability_table_uuid = api::NativeExecutorAvailabilityTableUuid();
      marker->serving_state = static_cast<engine::internal_api::NativeServedServingState>(
          static_cast<std::uint8_t>(serving.facts.serving_state));
      marker->serving_diagnostic_code = serving.facts.serving_diagnostic.code;
      marker->names_source = std::make_shared<NativeLoopNamesSource>(names_binding_);
      serving.facts.served_facts = std::move(marker);
    }
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
