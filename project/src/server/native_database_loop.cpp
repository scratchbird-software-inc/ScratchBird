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
#include "session_ledger_chain.hpp"
#include "transaction_inventory_reader.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_snapshot.hpp"

#include <algorithm>
#include <cstring>
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
namespace {
std::atomic<std::uint64_t> test_catalog_sizing_budget{0};
}  // namespace
void SetNativeCatalogSizingBudgetForTest(std::uint64_t bytes) { test_catalog_sizing_budget.store(bytes); }
std::uint64_t NativeCatalogSizingBudgetForTest() { return test_catalog_sizing_budget.load(); }
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
    case E::ledger_missing: return "ledger_missing";
    case E::ledger_invalid: return "ledger_invalid";
  }
  return "unknown";
}

const char* LedgerFindingName(NativeLedgerFinding f) noexcept {
  using F = NativeLedgerFinding;
  switch (f) {
    case F::none: return "none";
    case F::root_type: return "root_type";
    case F::root_filespace: return "root_filespace";
    case F::root_unallocated: return "root_unallocated";
    case F::root_creator: return "root_creator";
    case F::root_owner: return "root_owner";
    case F::chain_unreadable: return "chain_unreadable";
    case F::chain_page_unallocated: return "chain_page_unallocated";
    case F::chain_page_type: return "chain_page_type";
    case F::chain_page_owner: return "chain_page_owner";
    case F::record_reference: return "record_reference";
    case F::header_citation: return "header_citation";
    case F::inventory_unreadable: return "inventory_unreadable";
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
    case S::session_ledger: return "session_ledger";
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
// SB_OPEN_SIZED_FROM_FILE (owner Q287, 2026-11-19: size the capture and verification limits from the file; done 2026-11-26):
// the scan tier and the capture bounds below are the FIRST sizing, for a freshly created database; `scale` multiplies every
// bound that grows with the catalog (relation rows, leaf bytes, navigation and name-tree pages, captured rows, text, name-tree
// cells, physical pages, transactions). The open retries at the next power of two while the cohort scan or the capture reports a
// size bound as the cause (CohortFilled below), up to the governed memory budget, so there is no fixed tier: the first sizing is
// all a normal file ever needs and costs nothing extra.
bool BuildLimits(std::uint32_t page_size, std::uint64_t total_pages, OpenLimits& out, std::uint32_t scale = 1) {
  const auto* profile = storage::disk::FindCanonicalFilespacePageProfileForSize(page_size);
  if (profile == nullptr || total_pages == 0 || total_pages > UINT64_MAX / 8 / page_size) return false;
  const std::size_t ps = page_size;
  out.budget = db::NativeHostedSourceBackingBytes(ps);
  constexpr auto rows = db::kNativeHostedCatalogRelationMaximumRows;
  // SB_OPEN_SIZED_FROM_FILE: the verification image bound of the fixed-root reads (the bytes of page images one read may verify
  // in all) is the file's own size times eight, the rule the open verification already used below, no longer the 8 MiB
  // backing floor (a names relation past about 5000 rows verifies more than that). It is an accounting bound, not memory:
  // the workspaces are sized by the scan limits and the capture bounds.
  const std::uint64_t image_bound = std::max<std::uint64_t>(out.budget, 8 * total_pages * page_size);
  const db::NativeCatalogRelationMemoryLimits credential_catalog{
      profile->uuid, 1, db::NativeHostedCatalogBasePages(ps), rows, rows,
      db::NativeHostedCatalogNavigationLimits(ps)};
  out.host = NativeHostedRuntimeLimits{{64 * 1024, {image_bound, 32 * 1024 * 1024}}, credential_catalog,
                                       2 * 1024 * 1024, {rows, 128u, 1000000u}};
  out.fixed_root = {image_bound, 32 * 1024 * 1024};
  out.catalog = {image_bound, 12 * 1024 * 1024};
  if (scale == 0) return false;
  const std::size_t sc = scale;
  const std::size_t scan_rows = db::kNativeCatalogScanRelationMaximumRows * sc;
  out.scan = db::NativeCatalogRelationMemoryLimits{
      profile->uuid, 1, db::NativeCatalogBasePagesFor(ps, db::kNativeCatalogScanRelationMaximumBytes * sc), scan_rows,
      scan_rows, db::NativeCatalogNavigationLimitsFor(ps, scan_rows), db::NativeNameTreeNavigationLimits(ps, scan_rows)};
  // The initial-control-page bound follows the file: the allocation map's pages grow with the page count (measured 2026-11-26:
  // 142 control pages at 8192 pages of 8 KiB, 37 at 4096 of 16 KiB, 4 at 512 of 128 KiB), so a large file is not made to retry.
  const std::size_t control_pages = 128 * sc + static_cast<std::size_t>(total_pages * 192 / page_size);
  out.capture = {4096 * sc, 8 * 1024 * 1024 * sc, 1, 1024 * sc, 1024 * sc, control_pages, 8192 * sc};
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
  // SB_SESSION_LEDGER (plan 36 T3/T4): the durable session ledger, constructed before the writer (the writer's binding
  // names it), loaded by RunRestart, destroyed after the writer. Loop thread only.
  std::optional<pg::SessionLedger> ledger;
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
  // S4e (SB_NATIVE_LOOP_SESSIONS): the sessions this loop approved and has not ended, with the record that opened them (the
  // ended record is that record again with the end time and reason). Loop thread only; memory only (a restart empties it;
  // the ledger's un-ended session records are then "orphaned").
  std::map<core::platform::Uuid, pg::LedgerSessionRecord> open_sessions;
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
    transactions.clear();  // (S4e: the graceful close rolled them back first; after a crash they stay active: dead at Restart)
    open_sessions.clear();
    writer.reset();
    ledger.reset();  // the writer's binding named it
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
                                             serving.derived, scope_view_ ? &*scope_view_ : nullptr);
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

// ===================================================================================================================
// SEARCH_KEY: SB_NATIVE_LOGIN_SOURCE (thin-slice S4c, plan 40; owner 2026-11-21: login reads at an in-memory snapshot
// point, the user's transaction only on approval). See the header. Runs on the loop thread, inside one snapshot-point
// scope: the engine's `select` (names read, resolver, hidden policy; these are READS ending before the next starts),
// then the S4a credential read (PBKDF2 included). Nothing is written, no transaction number is taken.
// ===================================================================================================================
void NativeDatabaseLoop::LoginInScope(const LoginSelect& select, std::string& password,
                                      engine::internal_api::NativeLoginAttemptResult* out) noexcept {
  using engine::internal_api::NativeLoginSelectionError;
  try {
    out->selection = select ? select() : engine::internal_api::NativeLoginSelection{};
  } catch (...) {
    out->selection = {};
    out->selection.error = NativeLoginSelectionError::resolver_failed;
    out->selection.resolver_code = "SELECT_THREW";
  }
  const auto sel = out->selection.error;
  // A failed selection that is a CREDENTIAL cause still burns one credential read (see the header); an
  // infrastructure selection failure does not read credentials at all.
  const bool credential_cause = sel == NativeLoginSelectionError::none || sel == NativeLoginSelectionError::name_not_found ||
                                sel == NativeLoginSelectionError::name_hidden || sel == NativeLoginSelectionError::name_ambiguous;
  if (!credential_cause) {
    out->material_error = NativeLoginMaterialError::invalid_request;  // not read
    return;
  }
  NativeLoopLoginRequest request;
  if (out->selection.ok()) {
    request.principal_uuid = out->selection.principal_uuid;
  } else {
    const auto burn = core::uuid::IssueRuntimeIdentityV7();
    if (!burn) {
      out->material_error = NativeLoginMaterialError::crypto_failure;
      return;
    }
    request.principal_uuid = *burn;
  }
  // The password into protected memory ON THIS THREAD (the loop's own credential memory); longer than the S4a limit
  // is handed over as limit + 1 bytes (the read refuses it as inadmissible without a file read).
  constexpr std::size_t kLimit = 1024;
  const std::size_t length = password.size() > kLimit ? kLimit + 1 : password.size();
  core::memory::ProtectedMemoryRequest protect;
  protect.bytes = std::max<std::size_t>(length, 1);
  if (!serving_ || !serving_->credential_memory) {
    out->material_error = NativeLoginMaterialError::database_not_serving;
    return;
  }
  protect.tag = serving_->tag;
  protect.material_class = "native login password";
  protect.platform_policy = core::memory::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
  auto allocated = serving_->credential_memory->AllocateProtected(protect);
  if (!allocated.ok()) {
    out->material_error = NativeLoginMaterialError::memory_failure;
    return;
  }
  if (length > 0) std::memcpy(allocated.buffer.data(), password.data(), length);
  request.password = std::move(allocated.buffer);
  request.password_length = length;
  auto result = ReadLoginMaterialNow(request);
  ConsumeLoginPassword(request);
  request.password = {};
  out->material_error = result.error;
  out->security_error = static_cast<std::uint32_t>(result.security_error);
  out->source_error = static_cast<std::uint32_t>(result.source_error);
  out->custody_error = static_cast<std::uint32_t>(result.custody_error);
  // Only a verdict for a SELECTED principal may carry material; a burn read's material (it cannot succeed) is dropped.
  if (out->selection.ok() && result.ok()) out->material = std::move(result.material);
  else if (result.error == NativeLoginMaterialError::none) out->material_error = NativeLoginMaterialError::principal_absent;
}

namespace {
void ScrubLoginPassword(std::string& password) noexcept {
  if (!password.empty()) OPENSSL_cleanse(password.data(), password.size());
  password.clear();
}
}  // namespace

std::future<NativeLoopReadScopeResult> NativeDatabaseLoop::SubmitLoginAttempt(
    const LoginSelect& select, std::string& password, engine::internal_api::NativeLoginAttemptResult* out) {
  return SubmitReadScope(
      [this, &select, &password, out](const NativeLoopReadTransaction&) { LoginInScope(select, password, out); },
      NativeLoopReadScopeMode::snapshot_point);
}

void NativeDatabaseLoop::FinishLoginAttempt(const NativeLoopReadScopeResult& scope, std::string& password,
                                            engine::internal_api::NativeLoginAttemptResult* out) noexcept {
  using engine::internal_api::NativeLoginScopeError;
  ScrubLoginPassword(password);  // whatever happened; the work copied what it needed into protected memory
  if (!scope.begin.ok()) {
    switch (scope.begin.error) {
      case NativeLoopTransactionError::database_not_serving: out->scope_error = NativeLoginScopeError::database_not_serving; break;
      case NativeLoopTransactionError::database_failed: out->scope_error = NativeLoginScopeError::database_failed; break;
      case NativeLoopTransactionError::busy: out->scope_error = NativeLoginScopeError::busy; break;
      default: out->scope_error = NativeLoginScopeError::scope_refused; break;
    }
    return;
  }
  out->scope_error = (scope.work_ran && !scope.work_threw) ? NativeLoginScopeError::none : NativeLoginScopeError::work_failed;
}

engine::internal_api::NativeLoginAttemptResult NativeDatabaseLoop::AttemptLoginOnLoopThread(
    const LoginSelect& select, std::string& password) noexcept {
  engine::internal_api::NativeLoginAttemptResult out;
  if (!on_loop_thread()) {
    ScrubLoginPassword(password);
    out.scope_error = engine::internal_api::NativeLoginScopeError::scope_refused;
    return out;
  }
  const auto scope = RunReadScopeNow(
      [this, &select, &password, &out](const NativeLoopReadTransaction&) { LoginInScope(select, password, &out); },
      NativeLoopReadScopeMode::snapshot_point);
  FinishLoginAttempt(scope, password, &out);
  return out;
}

namespace {
// SB_NATIVE_LOGIN_SOURCE: the source the served facts carry (as the names source). The attempt is queued under the
// binding's lock (the loop clears the binding when it starts to close) and awaited outside it.
class NativeLoopLoginSource final : public engine::internal_api::NativeLoginSource {
 public:
  explicit NativeLoopLoginSource(std::shared_ptr<NativeNamesBinding> binding) : binding_(std::move(binding)) {}
  engine::internal_api::NativeLoginAttemptResult Login(
      const std::function<engine::internal_api::NativeLoginSelection()>& select, std::string& password) const noexcept override {
    engine::internal_api::NativeLoginAttemptResult out;
    try {
      NativeDatabaseLoop* loop = nullptr;
      std::future<NativeLoopReadScopeResult> future;
      {
        std::lock_guard<std::mutex> guard(binding_->mutex);
        loop = binding_->loop;
        if (loop == nullptr) {
          ScrubLoginPassword(password);
          out.scope_error = engine::internal_api::NativeLoginScopeError::database_not_serving;  // the loop closed
          return out;
        }
        if (!loop->on_loop_thread()) future = loop->SubmitLoginAttempt(select, password, &out);
      }
      if (!future.valid()) return loop->AttemptLoginOnLoopThread(select, password);  // the loop is alive: it is this thread
      const auto scope = future.get();
      NativeDatabaseLoop::FinishLoginAttempt(scope, password, &out);
      return out;
    } catch (...) {
      ScrubLoginPassword(password);
      out.scope_error = engine::internal_api::NativeLoginScopeError::work_failed;
      return out;
    }
  }

 private:
  std::shared_ptr<NativeNamesBinding> binding_;
};
}  // namespace

// SEARCH_KEY: SB_NATIVE_NAMES_SOURCE (S4b rework "Part A"). Same admission as the login-material read: serving and
// restricted are allowed (Q309), failed and not-serving are refused before anything is queued.
std::future<engine::internal_api::NativeNamesSnapshot> NativeDatabaseLoop::SubmitReadNamesSnapshot(
    const engine::internal_api::NativeNamesReadFilter& filter) {
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
  NamesWork work{{}, filter};
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

engine::internal_api::NativeNamesSnapshot NativeDatabaseLoop::ReadNamesSnapshotOnLoopThread(
    const engine::internal_api::NativeNamesReadFilter& filter) noexcept {
  if (!on_loop_thread()) {
    engine::internal_api::NativeNamesSnapshot refused;
    refused.error = engine::internal_api::NativeNamesSnapshotError::invalid_request;
    return refused;
  }
  return ReadNamesSnapshotNow(filter);
}

namespace {
// SB_NAMES_SCOPE_ARGUMENT: the rows a filter retains out of an all-scopes snapshot (the totals are kept as they are).
engine::internal_api::NativeNamesSnapshot RetainedBy(const engine::internal_api::NativeNamesSnapshot& all,
                                                      const engine::internal_api::NativeNamesReadFilter& f) {
  if (f.all_scopes) return all;
  engine::internal_api::NativeNamesSnapshot out;
  out.error = all.error;
  out.source_error = all.source_error;
  out.relation_error = all.relation_error;
  out.custody_error = all.custody_error;
  out.database_uuid = all.database_uuid;
  out.principal_schemas = all.principal_schemas;
  out.names_digest = all.names_digest;
  out.visible_entry_count = all.visible_entry_count;
  out.max_catalog_generation_id = all.max_catalog_generation_id;
  out.max_name_resolution_epoch = all.max_name_resolution_epoch;
  for (const auto& row : all.rows)
    if (std::find(f.scopes.begin(), f.scopes.end(), row.scope_uuid) != f.scopes.end() ||
        std::find(f.objects.begin(), f.objects.end(), row.object_uuid) != f.objects.end())
      out.rows.push_back(row);
  return out;
}
}  // namespace

engine::internal_api::NativeNamesSnapshot NativeDatabaseLoop::ReadNamesSnapshotNow(
    const engine::internal_api::NativeNamesReadFilter& filter) noexcept {
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
      // Inside a snapshot-point scope the cache holds the ALL-scopes rows of that view (one scan per scope); a filtered read
      // is cut from it.
      if (scope_view_ && scope_names_) return RetainedBy(*scope_names_, filter);  // the same view, the same rows: no second scan (see scope_names_)
      if (scope_view_) {
        request.filter = {};
        result = Host::ReadNativeNamesSnapshot(serving.runtime, request, &*scope_view_);
        if (result.ok()) {
          scope_names_ = result;
          return RetainedBy(result, filter);
        }
      } else {
        request.filter = filter;
        result = Host::ReadNativeNamesSnapshot(serving.runtime, request, nullptr);
      }
    }
  } catch (...) {
    result = {};
    result.error = Error::names_source_failure;
  }
  return result;
}


#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
bool NativeDatabaseLoop::ShrinkCredentialTierOnLoopThreadForTest(std::size_t rows) {
  if (!on_loop_thread() || !serving_ || !serving_->runtime) return false;
  Host::ShrinkCredentialTierForTest(serving_->runtime, rows);
  return true;
}
#endif

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
  work.done.set_value(ReadNamesSnapshotNow(work.filter));
}

namespace {
// SB_NATIVE_NAMES_SOURCE: the source the served facts carry. From the loop thread it reads directly; from any other
// thread it submits to the loop and waits. A source that outlives its loop answers `closed`.
class NativeLoopNamesSource final : public engine::internal_api::NativeNamesSource {
 public:
  explicit NativeLoopNamesSource(std::shared_ptr<NativeNamesBinding> binding)
      : binding_(std::move(binding)) {}
  engine::internal_api::NativeNamesSnapshot Read(const engine::internal_api::NativeNamesReadFilter& filter) const noexcept override {
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
        if (!loop->on_loop_thread()) future = loop->SubmitReadNamesSnapshot(filter);
      }
      // On the loop thread the loop is alive by construction: read directly, outside the binding lock.
      if (!future.valid()) return loop->ReadNamesSnapshotOnLoopThread(filter);
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
  // S4e Part 4: the published (immutable) facts answer CurrentServingState() from this cell, so a later failure is seen.
  if (served_state_cell_)
    served_state_cell_->state.store(engine::internal_api::NativeServedServingState::failed, std::memory_order_release);
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

#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
bool NativeDatabaseLoop::RunWithTransactionSinkOnLoopThreadForTest(
    const std::function<void(storage::disk::CarefulWriteSink&)>& body) {
  if (!on_loop_thread() || !body || !serving_ || state() != NativeLoopState::serving ||
      serving_state() == NativeServingState::failed)
    return false;
  NativeLoopTransactionResult door;
  return WithTransactionSink(*serving_, door, [&](storage::disk::CarefulWriteSink& sink) { body(sink); });
}
#endif

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

// SEARCH_KEY: SB_NATIVE_LOOP_LEDGER (plan 36 T4). See the header.
NativeLoopLedgerResult NativeDatabaseLoop::AppendLedgerNow(const storage::page::SessionLedgerRecord& record) noexcept {
  NativeLoopLedgerResult out;
  try {
    out.record_uuid = record.record_uuid;
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    auto& serving = *serving_;
    if (!serving.ledger || !serving.ledger->loaded()) {
      out.error = NativeLoopTransactionError::writer_refused;
      out.ledger_error = pg::TransactionWriterErrorName(pg::TransactionWriterError::ledger_not_loaded);
      return out;
    }
    pg::SessionLedgerAppendResult appended;
    NativeLoopTransactionResult door;
    if (!WithTransactionSink(serving, door, [&](storage::disk::CarefulWriteSink& sink) {
          appended = serving.ledger->Append(sink, record);
        })) {
      out.error = door.error;
      out.ledger_error = door.writer_error;
      return out;
    }
    out.ordinal = appended.ordinal;
    out.writes = appended.writes;
    out.syncs = appended.syncs;
    out.growth_writes = appended.growth_writes;
    out.growth_syncs = appended.growth_syncs;
    if (!appended.ok()) {
      out.ledger_error = pg::TransactionWriterErrorName(appended.error);
      if (appended.record_error != pg::SessionLedgerRecordError::none)
        out.record_error = pg::SessionLedgerRecordErrorName(appended.record_error);
      if (serving.ledger->failed()) {
        out.error = NativeLoopTransactionError::writer_failed;
        out.outcome_uncertain = true;
        NoteTransactionWriterFailure();
      } else {
        out.error = NativeLoopTransactionError::writer_refused;
      }
      return out;
    }
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.ledger_error = "exception";
    return out;
  }
}

void NativeDatabaseLoop::HandleLedger(LedgerWork& work) noexcept { work.done.set_value(AppendLedgerNow(work.record)); }

NativeLoopLedgerResult NativeDatabaseLoop::AppendLedgerRecordOnLoopThread(
    const storage::page::SessionLedgerRecord& record) noexcept {
  if (!on_loop_thread()) {
    NativeLoopLedgerResult refused;
    refused.error = NativeLoopTransactionError::invalid_request;
    return refused;
  }
  return AppendLedgerNow(record);
}

std::future<NativeLoopLedgerResult> NativeDatabaseLoop::SubmitAppendLedgerRecord(storage::page::SessionLedgerRecord record) {
  const auto refused = [](NativeLoopTransactionError error) {
    std::promise<NativeLoopLedgerResult> promise;
    NativeLoopLedgerResult result;
    result.error = error;
    promise.set_value(std::move(result));
    return promise.get_future();
  };
  if (on_loop_thread()) return refused(NativeLoopTransactionError::invalid_request);  // the loop never waits on itself
  LedgerWork work{std::move(record), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (const auto error = AdmitQueuedTransaction(); error != NativeLoopTransactionError::none) return refused(error);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

std::optional<storage::page::SessionLedgerEntry> NativeDatabaseLoop::FindLedgerEntryOnLoopThread(
    const core::platform::Uuid& uuid) const {
  if (!on_loop_thread() || !serving_ || !serving_->ledger) return std::nullopt;
  if (const auto* entry = serving_->ledger->Find(uuid)) return *entry;
  return std::nullopt;
}
std::optional<NativeLoopServingFacts> NativeDatabaseLoop::FactsOnLoopThread() const {
  if (!on_loop_thread() || !serving_) return std::nullopt;
  return serving_->facts;
}
std::uint64_t NativeDatabaseLoop::LedgerRecordCountOnLoopThread() const noexcept {
  if (!on_loop_thread() || !serving_ || !serving_->ledger) return 0;
  return serving_->ledger->record_count();
}

std::vector<std::uint64_t> NativeDatabaseLoop::OpenTransactionOrdersOnLoopThread() const {
  std::vector<std::uint64_t> orders;
  if (!on_loop_thread() || !serving_) return orders;
  for (const auto& entry : serving_->transactions) orders.push_back(entry.first);
  return orders;
}

// ---- SEARCH_KEY: SB_NATIVE_LOOP_SESSIONS (S4e) -----------------------------------------------------------------------
const char* NativeLoopSessionStepName(NativeLoopSessionStep step) noexcept {
  switch (step) {
    case NativeLoopSessionStep::none: return "none";
    case NativeLoopSessionStep::connection: return "connection";
    case NativeLoopSessionStep::session: return "session";
    case NativeLoopSessionStep::attachment: return "attachment";
    case NativeLoopSessionStep::begin: return "begin";
    case NativeLoopSessionStep::ended: return "ended";
  }
  return "unknown";
}

std::vector<core::platform::Uuid> NativeDatabaseLoop::OpenSessionsOnLoopThread() const {
  std::vector<core::platform::Uuid> sessions;
  if (!on_loop_thread() || !serving_) return sessions;
  for (const auto& entry : serving_->open_sessions) sessions.push_back(entry.first);
  return sessions;
}

NativeLoopSessionOpenResult NativeDatabaseLoop::OpenSessionNow(const NativeLoopSessionOpenRequest& request) noexcept {
  NativeLoopSessionOpenResult out;
  try {
    // Admission first: a restricted, failed or not-serving database refuses BEFORE anything is written.
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    auto& serving = *serving_;
    if (!serving.facts.served_facts || request.session_uuid.is_nil() || request.connection_uuid.is_nil() ||
        request.attachment_uuid.is_nil()) {
      out.error = NativeLoopTransactionError::invalid_request;
      return out;
    }
    const auto database_uuid = serving.facts.served_facts->database_uuid;
    const auto engine_instance = serving.facts.engine_instance_uuid;
    const auto account = [&](const NativeLoopLedgerResult& r) {
      out.writes += r.writes;
      out.syncs += r.syncs;
    };
    const auto step_failed = [&](NativeLoopSessionStep step, NativeLoopTransactionError error, std::string detail) {
      out.error = error;
      out.failed_step = step;
      out.detail = std::move(detail);
    };
    const auto make = [&](const core::platform::Uuid& record_uuid, pg::LedgerBody body) {
      pg::SessionLedgerRecord record;
      record.database_uuid = database_uuid;
      record.record_uuid = record_uuid;
      record.body = std::move(body);
      return record;
    };
    // 1. the CONNECTION record (durable before the attachment names it). A channel's record UUID is written once (ledger
    // UUIDs are write-once): a second approval on the same channel (a retry after a failed setup, a reconnect on the same
    // channel) REUSES the durable connection record; it is never appended twice.
    {
      const auto* existing = serving.ledger ? serving.ledger->Find(request.connection_uuid) : nullptr;
      if (existing && existing->kind == pg::SessionLedgerKind::connection) {
        out.connection_ordinal = existing->ordinal;
      } else {
        const auto r = AppendLedgerNow(make(request.connection_uuid, request.connection));
        account(r);
        if (!r.ok()) {
          step_failed(NativeLoopSessionStep::connection, r.error,
                      r.ledger_error + (r.record_error.empty() ? "" : "/" + r.record_error));
          return out;
        }
        out.connection_ordinal = r.ordinal;
      }
    }
    // 2. the SESSION record: the authenticated principal, the loop's own engine instance.
    auto session_record = request.session;
    session_record.engine_instance_uuid = engine_instance;
    session_record.ended_at_utc_ns = 0;
    session_record.end_reason = pg::LedgerEndReason::none;
    {
      const auto r = AppendLedgerNow(make(request.session_uuid, session_record));
      account(r);
      if (!r.ok()) {
        step_failed(NativeLoopSessionStep::session, r.error, r.ledger_error + (r.record_error.empty() ? "" : "/" + r.record_error));
        return out;
      }
      out.session_ordinal = r.ordinal;
    }
    serving.open_sessions[request.session_uuid] = session_record;
    // From here a failure leaves a durable session that must be ended (setup_failed) before this request answers.
    const auto fail_and_end = [&](NativeLoopSessionStep step, NativeLoopTransactionError error, std::string detail) {
      step_failed(step, error, std::move(detail));
      if (error == NativeLoopTransactionError::writer_failed) return;  // the database is failed: nothing more can be written
      const auto ended = EndSessionNow(request.session_uuid, pg::LedgerEndReason::setup_failed);
      out.writes += ended.writes;
      out.syncs += ended.syncs;
      out.setup_failed_recorded = ended.ended_recorded;
    };
    // 3. the ATTACHMENT record.
    {
      pg::LedgerAttachmentRecord attachment;
      attachment.session_uuid = request.session_uuid;
      attachment.connection_uuid = request.connection_uuid;
      attachment.engine_instance_uuid = engine_instance;
      attachment.at_utc_ns = NowUtcNs();
      const auto r = AppendLedgerNow(make(request.attachment_uuid, attachment));
      account(r);
      if (!r.ok()) {
        fail_and_end(NativeLoopSessionStep::attachment, r.error, r.ledger_error + (r.record_error.empty() ? "" : "/" + r.record_error));
        return out;
      }
      out.attachment_ordinal = r.ordinal;
    }
    // 4. BEGIN: +64 is the session record's principal (never a caller's separate value), +80 the attachment, +96 the session.
    {
      NativeLoopBeginRequest begin;
      begin.owner_key = request.session_uuid;
      begin.read_only = session_record.settings.read_only;
      begin.isolation_profile = session_record.settings.isolation_profile;
      begin.identity.principal_uuid = session_record.principal_uuid;
      begin.identity.attachment_uuid = request.attachment_uuid;
      begin.identity.session_uuid = request.session_uuid;
      const auto r = BeginNow(begin);
      out.writes += r.writes;
      out.syncs += r.syncs;
      if (!r.ok()) {
        fail_and_end(NativeLoopSessionStep::begin, r.error, r.writer_error);
        return out;
      }
      out.begun_order = r.begun_order;
      out.begun_uuid = r.begun_uuid;
    }
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.detail = "exception";
    return out;
  }
}

NativeLoopSessionEndResult NativeDatabaseLoop::EndSessionNow(const core::platform::Uuid& session_uuid,
                                                             pg::LedgerEndReason reason) noexcept {
  NativeLoopSessionEndResult out;
  try {
    out.error = AdmitTransactionWork();
    if (out.error != NativeLoopTransactionError::none) return out;
    auto& serving = *serving_;
    const auto open = serving.open_sessions.find(session_uuid);
    if (open == serving.open_sessions.end() || reason == pg::LedgerEndReason::none) {
      out.error = NativeLoopTransactionError::invalid_request;
      out.session_unknown = open == serving.open_sessions.end();
      return out;
    }
    // Roll back every transaction the session owns: CompleteWork rolled_back, no replacement (the ownership table is keyed
    // by the session UUID). A refused rollback is counted and the end record is still made (the transaction is then dead at
    // the next open's Restart); a FAILED write stops everything (the database is failed).
    std::vector<std::uint64_t> owned;
    for (const auto& entry : serving.transactions)
      if (entry.second.owner_key == session_uuid) owned.push_back(entry.first);
    for (const auto order : owned) {
      NativeLoopCompleteRequest rollback;
      rollback.owner_key = session_uuid;
      rollback.transaction_order = order;
      rollback.completion = NativeLoopCompletion::rollback;
      const auto r = CompleteNow(rollback);
      out.writes += r.writes;
      out.syncs += r.syncs;
      if (r.ok()) {
        ++out.transactions_rolled_back;
      } else {
        ++out.rollbacks_failed;
        out.detail = r.writer_error;
        if (r.error == NativeLoopTransactionError::writer_failed) {
          out.error = r.error;
          return out;
        }
      }
    }
    // The ended record: the opening record again, with the end time and the reason (never an overwrite).
    pg::SessionLedgerRecord record;
    record.database_uuid = serving.facts.served_facts ? serving.facts.served_facts->database_uuid : core::platform::Uuid{};
    record.record_uuid = session_uuid;
    auto ended = open->second;
    ended.ended_at_utc_ns = std::max<std::uint64_t>(NowUtcNs(), ended.opened_at_utc_ns);
    ended.end_reason = reason;
    record.body = ended;
    const auto r = AppendLedgerNow(record);
    out.writes += r.writes;
    out.syncs += r.syncs;
    if (!r.ok()) {
      out.error = r.error;
      out.detail = r.ledger_error + (r.record_error.empty() ? "" : "/" + r.record_error);
      return out;
    }
    serving.open_sessions.erase(open);
    out.ended_recorded = true;
    out.error = NativeLoopTransactionError::none;
    return out;
  } catch (...) {
    out = {};
    out.error = NativeLoopTransactionError::writer_refused;
    out.detail = "exception";
    return out;
  }
}

void NativeDatabaseLoop::EndAllSessionsAtClose() noexcept {
  try {
    if (!serving_ || AdmitTransactionWork() != NativeLoopTransactionError::none) return;  // restricted / failed: nothing is written
    std::vector<core::platform::Uuid> sessions;
    for (const auto& entry : serving_->open_sessions) sessions.push_back(entry.first);
    // 1. each session: its transactions rolled back, then its ended record (reason server_shutdown).
    for (const auto& session : sessions) {
      if (EndSessionNow(session, pg::LedgerEndReason::server_shutdown).error == NativeLoopTransactionError::writer_failed) return;
    }
    // 2. transactions that belong to no session (owned by an arbitrary key through the transaction requests): rolled back too.
    std::vector<std::pair<std::uint64_t, core::platform::Uuid>> rest;
    for (const auto& entry : serving_->transactions) rest.emplace_back(entry.first, entry.second.owner_key);
    for (const auto& [order, owner] : rest) {
      NativeLoopCompleteRequest rollback;
      rollback.owner_key = owner;
      rollback.transaction_order = order;
      rollback.completion = NativeLoopCompletion::rollback;
      if (CompleteNow(rollback).error == NativeLoopTransactionError::writer_failed) return;
    }
  } catch (...) {
  }
}

void NativeDatabaseLoop::HandleSession(SessionOpenWork& work) noexcept { work.done.set_value(OpenSessionNow(work.request)); }
void NativeDatabaseLoop::HandleSession(SessionEndWork& work) noexcept {
  work.done.set_value(EndSessionNow(work.request.session_uuid, work.request.reason));
}

std::future<NativeLoopSessionOpenResult> NativeDatabaseLoop::SubmitOpenSession(NativeLoopSessionOpenRequest request) {
  const auto refused = [](NativeLoopTransactionError error) {
    std::promise<NativeLoopSessionOpenResult> promise;
    NativeLoopSessionOpenResult result;
    result.error = error;
    promise.set_value(std::move(result));
    return promise.get_future();
  };
  if (on_loop_thread()) return refused(NativeLoopTransactionError::invalid_request);  // the loop never waits on itself
  SessionOpenWork work{std::move(request), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (const auto error = AdmitQueuedTransaction(); error != NativeLoopTransactionError::none) return refused(error);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
}

std::future<NativeLoopSessionEndResult> NativeDatabaseLoop::SubmitEndSession(NativeLoopSessionEndRequest request) {
  const auto refused = [](NativeLoopTransactionError error) {
    std::promise<NativeLoopSessionEndResult> promise;
    NativeLoopSessionEndResult result;
    result.error = error;
    promise.set_value(std::move(result));
    return promise.get_future();
  };
  if (on_loop_thread()) return refused(NativeLoopTransactionError::invalid_request);
  SessionEndWork work{std::move(request), {}};
  auto future = work.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (const auto error = AdmitQueuedTransaction(); error != NativeLoopTransactionError::none) return refused(error);
    queue_.emplace_back(std::move(work));
  }
  wake_.notify_one();
  return future;
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
// SB_SNAPSHOT_POINT_VIEW (owner 2026-11-21: the true snapshot-view reader entry, built before S4e). In durable_transaction
// mode the reads the work makes (ReadLoginMaterialOnLoopThread, ReadNamesSnapshotOnLoopThread) are NOT bound to the
// transaction's snapshot: they use the catalog visibility reader's committed entries, which compute visibility from the
// inventory read at that moment. That holds, correctly, for isolation read_committed_statement (each read sees the
// latest committed state) and it relied on the loop's serialisation. A durable scope has no captured value and no
// own-rows read yet; that is what still remains of the former SB_READ_TXN_SNAPSHOT_SEAM, for S4e.
// SB_NATIVE_ENGINE_READ_TRANSACTION mode snapshot_point: NO header, NO write, NO transaction number. The capture is
// the TIP counters (Next and the stored horizons) read through the write door's sink (reads only) once, held in
// memory for the work, like Firebird's in-memory system transaction 0. Needs only a served, not failed database
// (restricted included). THE VIEW: from the same chains read the capture also builds the inventory image and, from it,
// a visibility snapshot VALUE (mga::CreateSnapshotPointVisibility; fields and TIP states documented there): the reads
// the work makes through this loop (login material, names) decide every row version's visibility from that value, via
// the visibility reader's ...At entries, never from the inventory at read time. Memory only; no write.
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
    transaction::mga::VisibilitySnapshot captured_view;
    if (!WithTransactionSink(serving, out.begin, [&](storage::disk::CarefulWriteSink& sink) {
          const auto chains = pg::ReadTransactionChainsThroughSink(sink, serving.txn_binding);
          if (!chains.ok()) {
            out.begin.error = NativeLoopTransactionError::writer_refused;
            out.begin.writer_error = pg::TransactionWriterErrorName(chains.error);
            return;
          }
          const auto built = pg::BuildLocalTransactionInventoryFromImages(chains.page_size, chains.state_images, chains.header_images);
          if (!built.ok()) {
            out.begin.error = NativeLoopTransactionError::writer_refused;
            out.begin.writer_error = pg::TipPageErrorName(built.error);
            return;
          }
          auto view = transaction::mga::CreateSnapshotPointVisibility(built.inventory);
          if (!view.ok()) {
            out.begin.error = NativeLoopTransactionError::writer_refused;
            out.begin.writer_error = "snapshot_view_refused";
            return;
          }
          captured_view = std::move(view.snapshot);
          info.snapshot_next_transaction = chains.counters.next_transaction;
          info.snapshot_oit = chains.counters.horizons.oldest_interesting;
          info.snapshot_oat = chains.counters.horizons.oldest_active;
          info.snapshot_ost = chains.counters.horizons.oldest_snapshot;
          out.begin.error = NativeLoopTransactionError::none;
        }))
      return out;
    if (!out.begin.ok()) return out;
    out.snapshot_next_transaction = info.snapshot_next_transaction;
    info.snapshot_visible_through = captured_view.visible_through_local_transaction_id;
    info.snapshot_active_count = captured_view.active_excluded_local_transaction_ids.size();
    info.snapshot_in_doubt_count = captured_view.in_doubt_excluded_local_transaction_ids.size();
    // The captured value is the scope's view for exactly the work's duration (a scope inside a scope restores the outer).
    struct ScopeView {
      std::optional<transaction::mga::VisibilitySnapshot>& slot;
      std::optional<engine::internal_api::NativeNamesSnapshot>& names_slot;
      std::optional<transaction::mga::VisibilitySnapshot> outer;
      std::optional<engine::internal_api::NativeNamesSnapshot> outer_names;
      ~ScopeView() {
        slot = std::move(outer);
        names_slot = std::move(outer_names);
      }
    } scope_view{scope_view_, scope_names_, std::move(scope_view_), std::move(scope_names_)};
    scope_view_ = std::move(captured_view);
    scope_names_.reset();  // a scope's names rows belong to its own view (the outer scope's are restored at the end)
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
  return SubmitReadScope(std::move(work), mode).get();
}

// The queueing half (S4c: the login source queues under the names binding's lock and waits outside it).
std::future<NativeLoopReadScopeResult> NativeDatabaseLoop::SubmitReadScope(
    std::function<void(const NativeLoopReadTransaction&)> work, NativeLoopReadScopeMode mode) {
  std::promise<NativeLoopReadScopeResult> refused;
  const auto refuse = [&](NativeLoopTransactionError error) {
    NativeLoopReadScopeResult result;
    result.begin.error = error;
    refused.set_value(std::move(result));
    return refused.get_future();
  };
  if (on_loop_thread()) return refuse(NativeLoopTransactionError::database_not_serving);  // never waits on itself
  ReadScopeWork item{std::move(work), mode, {}};
  auto future = item.done.get_future();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto error = AdmitQueuedTransaction();
    // snapshot_point is a read: a restricted database serves it (Q309), only the queue bound and failed refuse.
    if (mode == NativeLoopReadScopeMode::snapshot_point && error == NativeLoopTransactionError::database_restricted)
      error = queue_.size() >= kQueueCapacity ? NativeLoopTransactionError::busy : NativeLoopTransactionError::none;
    if (error != NativeLoopTransactionError::none) return refuse(error);
    queue_.emplace_back(std::move(item));
  }
  wake_.notify_one();
  return future;
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
    if (auto* ledger = std::get_if<LedgerWork>(&work)) {
      HandleLedger(*ledger);
      continue;
    }
    if (auto* open_session = std::get_if<SessionOpenWork>(&work)) {
      HandleSession(*open_session);
      continue;
    }
    if (auto* end_session = std::get_if<SessionEndWork>(&work)) {
      HandleSession(*end_session);
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
  // S4e (owner: S4d answer 3): a graceful close first rolls back what is owned and ends the sessions (reason server_shutdown),
  // while the database is still serving; only then does the teardown begin. After a crash none of this happens: the slots are
  // dead at the next open and the session records are un-ended (orphaned).
  EndAllSessionsAtClose();
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
    // SB_SESSION_LEDGER: page-zero root kind 25. The open verification already refused a file without one; the
    // binding is built here, before the writer, because the writer's citation check names it.
    const storage::disk::FilespaceRootReference* ledger_root = nullptr;
    for (std::size_t i = 0; i < observed.identity.primary_root_count && i < observed.identity.primary_roots.size(); ++i)
      if (observed.identity.primary_roots[i].kind == 25 && !ledger_root) ledger_root = &observed.identity.primary_roots[i];
    if (!ledger_root) return reject("restart_binding", {{"error", "ledger_root_absent"}});
    pg::SessionLedgerBinding ledger_binding;
    ledger_binding.database_uuid = request.database_uuid;
    ledger_binding.root = *ledger_root;
    ledger_binding.read_only_database = binding.read_only_database;
    ledger_binding.allocation_root = binding.allocation_root;
    ledger_binding.issuer = binding.issuer;
    serving.ledger.emplace(ledger_binding);
    if (serving.ledger->page_size() == 0) return reject("restart_binding", {{"error", "ledger_binding_invalid"}});
    binding.session_ledger = &*serving.ledger;
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

    // SB_SESSION_LEDGER: the ledger is read once (no write; a torn last record is only noted) and indexed, for the
    // restarted and the restricted database alike (a restricted database reads its ledger, it cannot append).
    if (!serving.writer->failed()) {
      const auto loaded = serving.ledger->Load(sink);
      if (!loaded.ok())
        return reject("session_ledger", {{"error", pg::TransactionWriterErrorName(loaded.error)},
                                         {"rule", pg::TransactionWriterErrorName(loaded.rule)},
                                         {"page_error", Num(static_cast<unsigned>(loaded.page_error))},
                                         {"ordinal", Num(loaded.failing_ordinal)}});
      facts.ledger_records = loaded.records;
      facts.ledger_pages = loaded.pages;
      facts.ledger_torn_tail = loaded.torn_tail;
      // S4e: every session still open right after Load was left open by an earlier engine instance (a crash or a kill).
      facts.ledger_orphaned_sessions = serving.ledger->OpenSessionCount();
    }

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
    // SB_OPEN_SIZED_FROM_FILE: the hosted credential tier (security and configuration relations) sized from the file before
    // any credential read; a governed budget that will not admit it is its own refusal.
    {
      const auto tier = Host::SizeCredentialTierFromFile(serving.runtime, limits.fixed_root);
      if (tier != NativeHostedRuntimeError::none)
        return fail(Refusal(tier == NativeHostedRuntimeError::memory_failure ? "DATABASE.OPEN_NATIVE_CREDENTIAL_BUDGET_EXCEEDED"
                                                                             : StepCode("credential_tier"),
                            "credential_tier", {{"error", RuntimeErrorName(tier)}}));
      const auto& tier_limits = serving.runtime->credential_catalog_limits();
      serving.facts.open_credential_rows = tier_limits.maximum_rows;
      serving.facts.open_credential_base_pages = tier_limits.maximum_base_pages;
      serving.facts.open_credential_workspace_bytes = serving.runtime->credential_workspace_bytes();
    }
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
#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
    // SB_TEST_SEAM (2026-11-26): SB_OPEN_STAGE_TIMES=1 prints the time between consecutive open stages to stderr (a test binary only).
    const bool stage_times = std::getenv("SB_OPEN_STAGE_TIMES") != nullptr;
    auto stage_last = std::chrono::steady_clock::now();
    const auto stage_mark = [&](const char* name) {
      const auto now = std::chrono::steady_clock::now();
      if (stage_times)
        std::fprintf(stderr, "STAGE %s %.1f ms\n", name, std::chrono::duration<double, std::milli>(now - stage_last).count());
      stage_last = now;
    };
#else
    const auto stage_mark = [](const char*) {};
#endif
    const auto step = [&](const char* name, NativeHostedRuntimeError error) -> bool {
      stage_mark(name);
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
    // SB_OPEN_SIZED_FROM_FILE: the scan and the capture at the file's own size (see BuildLimits). A scan or capture that stops
    // because a SIZE bound filled is retried at twice the bounds; any other failure ends the open as before. The retry stops
    // when the governed memory budget will not admit the larger workspaces (memory_failure at a scale above the first, or the
    // seam's explicit ceiling in a test), with its own code, and when the scan already covers twice the file's pages (then
    // the bound is not what is wrong and the original refusal stands).
    std::uint32_t scale = 1;
    const auto cohort_filled = [&]() {
      using CE = db::NativeCatalogCohortError;
      const auto& c = read.catalogs();
      switch (c.capture_bound) {
        case db::NativeCatalogCaptureBound::rows:
        case db::NativeCatalogCaptureBound::text_bytes:
        case db::NativeCatalogCaptureBound::name_tree_cells:
        case db::NativeCatalogCaptureBound::physical_pages:
        case db::NativeCatalogCaptureBound::initial_control_pages:
        case db::NativeCatalogCaptureBound::transactions: return c.error == CE::resource_exhausted;
        case db::NativeCatalogCaptureBound::none: break;
        default: return false;  // filespaces, relation slots, the verification image bound, a missing tree workspace: not a scale
      }
      return c.error == CE::resource_exhausted ||
             (c.error == CE::relation_failure && (c.relation_error == db::NativeCatalogRelationLeaseError::resource_exhausted ||
                                                  c.visibility_error == db::NativePinnedCatalogReadError::resource_exhausted ||
                                                  c.relation_tree_error == storage::database::NativeBtreeTreeMemoryError::resource_exhausted)) ||
             ((c.error == CE::name_tree_failure || c.error == CE::comment_tree_failure) &&
              c.name_tree_error == storage::database::NativeBtreeTreeMemoryError::resource_exhausted);
    };
    // SB_OPEN_CAPTURE_EXACT: the capture's row, text, name-tree-cell and physical-page bounds come from what the scan step counted
    // for this very file (read.catalogs().needed_*), not from the doubled first guess (which reserved up to twice the need, and
    // 8 MiB of text per scale step); the first attempt with those bounds is the only one, and a bound that still fills (it cannot
    // for an unchanged file) falls back to the doubling below.
    bool exact_failed = false;
    std::uint64_t captured_bytes = 0;  // the capture workspace the open actually reserved
    const auto sized_catalog_step = [&](const char* name, bool with_capture) -> bool {
      for (bool grow = false;;) {
        if (grow) scale *= 2;
        grow = true;
        OpenLimits sized;
        if (!BuildLimits(request.page_size_bytes, request.total_pages, sized, scale)) {
          fail(Refusal(StepCode(name), name, {{"error", "limits_overflow"}, {"scale", Num(scale)}}));
          return false;
        }
        const bool use_exact = with_capture && !exact_failed && read.catalogs().ok();
        if (use_exact) {
          const auto& m = read.catalogs();
          sized.capture.maximum_rows = static_cast<std::size_t>(m.needed_rows) + 16;
          sized.capture.maximum_text_bytes = static_cast<std::size_t>(m.needed_text_bytes) + 4096;
          sized.capture.maximum_name_tree_cells = static_cast<std::size_t>(m.needed_name_tree_cells) + 16;
          sized.capture.maximum_physical_pages = static_cast<std::size_t>(m.needed_physical_pages) + 16;
        }
        const auto bytes = db::NativeCatalogCohortWorkspaceBytes(sized.scan) +
                           (with_capture ? db::NativeCatalogCaptureWorkspaceBytes(sized.capture) : 0) + sized.catalog.maximum_backing_bytes;
#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
        if (const auto ceiling = NativeCatalogSizingBudgetForTest(); ceiling != 0 && bytes > ceiling) {
          fail(Refusal("DATABASE.OPEN_NATIVE_CATALOG_BUDGET_EXCEEDED", name,
                       {{"scale", Num(scale)}, {"bytes_required", Num(bytes)}, {"budget", Num(ceiling)}}));
          return false;
        }
#endif
        stage_mark(name);
        const auto error = with_capture ? Host::ReadStartupCatalogs(read, limits.catalog, sized.scan, sized.capture)
                                        : Host::ReadStartupCatalogs(read, limits.catalog, sized.scan);
        if (error == NativeHostedRuntimeError::none) {
          serving.name_scan_limits = sized.scan;
          if (with_capture) captured_bytes = db::NativeCatalogCaptureWorkspaceBytes(sized.capture);
          return true;
        }
        if (error == NativeHostedRuntimeError::memory_failure && (scale > 1 || use_exact)) {
          fail(Refusal("DATABASE.OPEN_NATIVE_CATALOG_BUDGET_EXCEEDED", name,
                       {{"scale", Num(scale)}, {"bytes_required", Num(bytes)}, {"budget", "governed_memory"}}));
          return false;
        }
        if (use_exact && error == NativeHostedRuntimeError::catalog_failure && cohort_filled()) {
          exact_failed = true;
          grow = false;  // the same scale, now with the doubled first-guess bounds
          continue;
        }
        const bool covers_file = static_cast<std::uint64_t>(sized.scan.maximum_base_pages) >= 2 * request.total_pages;
        if (error != NativeHostedRuntimeError::catalog_failure || !cohort_filled() || covers_file || scale >= (1u << 20)) {
          fail(Refusal(StepCode(name), name, {{"error", RuntimeErrorName(error)}, {"scale", Num(scale)}}));
          return false;
        }
      }
    };
    if (!sized_catalog_step("catalog_scan", false)) return;
    if (!sized_catalog_step("catalog_capture", true)) return;
    {
      OpenLimits final_limits;
      if (BuildLimits(request.page_size_bytes, request.total_pages, final_limits, scale)) {
        serving.facts.open_catalog_scale = scale;
        // SB_OPEN_COHORT_STREAM: the scan's own workspace (the stream's window and the verifier keys) and the capture's, apart.
        serving.facts.open_catalog_scan_bytes = db::NativeCatalogCohortWorkspaceBytes(final_limits.scan);
        serving.facts.open_capture_bytes = captured_bytes;
        serving.facts.open_catalog_workspace_bytes = db::NativeCatalogCohortWorkspaceBytes(final_limits.scan) +
                                                     captured_bytes +
                                                     final_limits.catalog.maximum_backing_bytes;
      }
    }
    {
      stage_mark("catalog_capture_to_metadata");
      const auto metadata = Host::VerifyStartupCatalogMetadata(read);
      stage_mark("catalog_metadata_verification");
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
    stage_mark("before_open_verification");
    const auto verified = Host::VerifyStartupDatabaseFile(read, limits.verification);
    stage_mark("open_verification");
    if (!verified.ok())
      return fail(Refusal(
          verified.error == NativeOpenVerificationError::finding           ? "DATABASE.OPEN_VERIFICATION_FINDING"
          : verified.error == NativeOpenVerificationError::ledger_missing ? "DATABASE.OPEN_VERIFICATION_LEDGER_MISSING"
          : verified.error == NativeOpenVerificationError::ledger_invalid ? "DATABASE.OPEN_VERIFICATION_LEDGER_INVALID"
                                                                          : "DATABASE.OPEN_VERIFICATION_FAILED",
          "open_verification",
          {{"stage", VerificationStageName(verified.stage)},
           {"error", VerificationErrorName(verified.error)},
           {"source_error", Num(static_cast<unsigned>(verified.source_error))},
           {"ledger_finding", LedgerFindingName(verified.ledger_finding)},
           {"ledger_rule", pg::TransactionWriterErrorName(static_cast<pg::TransactionWriterError>(verified.ledger_rule))},
           {"ledger_page_error", Num(verified.ledger_page_error)},
           {"ledger_record_error", Num(verified.ledger_record_error)},
           {"ledger_ordinal", Num(verified.ledger_ordinal)},
           {"ledger_page", Num(verified.ledger_page)}}));

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
      served_state_cell_ = std::make_shared<engine::internal_api::NativeServedServingStateCell>();
      served_state_cell_->state.store(marker->serving_state, std::memory_order_release);
      marker->serving_state_cell = served_state_cell_;
      marker->names_source = std::make_shared<NativeLoopNamesSource>(names_binding_);
      marker->login_source = std::make_shared<NativeLoopLoginSource>(names_binding_);
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
