// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_local_service_startup_credentials.hpp"
#include "native_local_service_startup_configuration.hpp"
#include "native_local_service_agent_configuration.hpp"
#include "process_crypto_owner.hpp"
#include "hosted_process_admission.hpp"
#include <optional>
#include "native_owned_publication_watermark.hpp"
#include "native_catalog_cohort_lease_reader.hpp"
#include "engine/internal_api/security/native_local_service_authentication.hpp"
#include "engine/internal_api/security/native_local_service_session.hpp"
#include "engine/internal_api/catalog/native_catalog_metadata_verification.hpp"
#include "engine/internal_api/verification/native_verification_policy_selection.hpp"
#include "engine/internal_api/transaction/native_startup_transaction_history.hpp"
#include "engine/internal_api/verification/native_startup_management_definition.hpp"
#include "engine/internal_api/transaction/native_transaction_policy_selection.hpp"
#include "engine/internal_api/verification/native_startup_storage_identity.hpp"
#include "engine/internal_api/verification/native_startup_storage_scan.hpp"

namespace scratchbird::server {

struct NativeHostedRuntimeLimits {
  storage::database::NativeOwnedCheckpointMemoryLimits writer;
  storage::database::NativeCatalogRelationMemoryLimits catalog;
  std::size_t database_crypto_bytes = 0;
  engine::internal_api::NativeRuntimeAuthorizationLimits authorization;
};
enum class NativeHostedRuntimeError {
  none, invalid_request, wrong_owner, process_unavailable, ownership_failure,
  custody_failure, bootstrap_failure, identity_failure, memory_failure,
  crypto_failure, adoption_failure, preparation_failure, busy, closed,
  credential_failure, resource_exhausted, process_admission_failure, synchronization_failure,
  authorization_preparation_failure, authorization_failure, configuration_failure, agent_configuration_failure,
  memory_envelope_failure, agent_policy_failure, cancelled, catalog_failure,
  catalog_verification_failure, verification_policy_failure,
  management_definition_failure, transaction_policy_failure
};
enum class NativeAgentPolicyError {
  none, unavailable, registry_invalid, unknown_agent, invalid_property,
  descriptor_mismatch, generation_mismatch, schema_refused, missing_policy_family,
  metric_contract_mismatch, dependency_missing, dependency_cycle,
  cluster_admission_required, lifecycle_invariant, resource_exhausted
};
// Validation of retained configuration only. Metric availability, resource
// grants, service sessions and lifecycle/worker admission are separate gates.
struct NativeAgentPolicyValidation {
  NativeAgentPolicyError error=NativeAgentPolicyError::unavailable;
  core::platform::Uuid failing_object_uuid;
  std::array<char,128> diagnostic_code{};
  std::array<char,128> field{};
  std::size_t policy_count=0,registration_count=0;
  bool ok() const noexcept {return error==NativeAgentPolicyError::none;}
};
// SEARCH_KEY: SB_OPEN_VERIFICATION_READ_ONLY (owner direction 2026-11-05).
// Verification of the primary database file on open is L5's first startup
// duty. It only reads: no transaction, publication, phase record or
// allocation is written, because writing to a corrupt database may corrupt it
// further. A refusal is returned to the caller, which reports it (the report
// channel is not yet specified); policy then closes the database or enters
// restricted repair. "finding" is an integrity problem in the file;
// source_mismatch means the file disagrees with what this open already
// captured. Both mean the file is not in a known good state.
enum class NativeOpenVerificationError {
  none, not_ready, busy, cancelled, invalid_request, policy_refused, memory_failure,
  crypto_failure, io_failure, source_failure, source_mismatch, finding
};
enum class NativeOpenVerificationStage { none, identity, allocation, inventory, catalog_routes, complete };
struct NativeOpenVerificationLimits {
  // One selected-checkpoint read at a time; each stage releases its read.
  storage::database::NativeSelectedCheckpointMemoryLimits selected;
  // Secondary allocation-chain read workspace; at least one page.
  std::size_t map_backing_bytes = 0;
};
// Copied values only. No custody, phase, readiness or repair authority.
struct NativeOpenVerificationObservation {
  engine::internal_api::NativeStartupStorageIdentityObservation identity;
  engine::internal_api::NativeStartupStorageScanObservation allocation, inventory;
  engine::internal_api::NativeStartupCatalogRouteObservation routes;
  std::size_t workspace_bytes = 0;
};
struct NativeOpenVerificationResult {
  NativeOpenVerificationError error = NativeOpenVerificationError::not_ready;
  // The stage that refused, or complete.
  NativeOpenVerificationStage stage = NativeOpenVerificationStage::none;
  // The selected-source refusal behind a stage's error, for the report.
  storage::database::NativeOwnedSourceError source_error = storage::database::NativeOwnedSourceError::none;
  std::optional<NativeOpenVerificationObservation> observation;
  bool ok() const noexcept {return error==NativeOpenVerificationError::none&&observation.has_value();}
};
class NativeHostedDatabaseRuntime;
struct NativeHostedRuntimeOpenResult;
struct NativeHostedRuntimeCloseResult {
  NativeHostedRuntimeError error = NativeHostedRuntimeError::invalid_request;
  NativeHostedDatabaseCustody::CloseResult custody;
  core::runtime::RuntimeCryptoPoolResult crypto;
  NativeDatabaseMemoryEnvelopeError memory_envelope_error=NativeDatabaseMemoryEnvelopeError::none;
  bool original_route_released = false;
  bool ok() const noexcept { return error == NativeHostedRuntimeError::none && original_route_released; }
};

// SEARCH_KEY: NATIVE_PROBE_ACCEPTS_PRODUCTION_LINEAGE
// Which bootstrap lineage the route probe may decode at offset 0. The native
// lineage writes a FilespaceBootstrap preamble; the production create path
// writes a DatabaseHeader (SBDBV001). Accepting the latter is a caller policy
// and must be stated, never inferred: it was previously derived from whether
// the caller supplied its own ownership lock, which silently coupled lineage
// acceptance to resource provenance and turns a correct native-first open into
// a bootstrap_failure.
enum class NativeBootstrapLineagePolicy : std::uint8_t {
  // Decode a native FilespaceBootstrap preamble only. The default, so a caller
  // that says nothing cannot widen what the engine will mount.
  native_only,
  // Also accept a production DatabaseHeader lineage. Native decode is still
  // attempted first, so native-lineage callers are unaffected.
  accept_production_lineage
};

// Owning native host resource path. The original route, selected inode,
// complete writer cohort, database crypto and read preparation use the actual
// process budget. A successful Open establishes physical custody only; it does
// not activate configuration, authenticate a session or publish readiness.
// Lifecycle entry is affine to the constructing process/thread. The process
// owner and selected manager outlive this owner and every credential read.
class NativeHostedDatabaseRuntime final {
 public:
  using Uuid = core::platform::Uuid;
  NativeHostedDatabaseRuntime(const NativeHostedDatabaseRuntime&) = delete;
  NativeHostedDatabaseRuntime& operator=(const NativeHostedDatabaseRuntime&) = delete;
  ~NativeHostedDatabaseRuntime() {
    // The final owner uses the same preallocated close protocol. An error may
    // never silently force release; explicit Close permits reconciliation.
    if (!CloseImpl(*this).ok()) std::terminate();
  }
  static NativeHostedRuntimeOpenResult Open(ServerProcessCryptoOwner&,
      const DatabaseOwnershipRequest&, NativeHostedRuntimeLimits,
      std::vector<std::unique_ptr<storage::disk::FileDevice>>&,
      NativeBootstrapLineagePolicy =
          NativeBootstrapLineagePolicy::native_only) noexcept;
  // SEARCH_KEY: NATIVE_HOSTED_OPEN_UNDER_HELD_PROCESS_ADMISSION
  // Same open for a caller that already holds the single process-to-database
  // admission, which Open() cannot serve because it reserves internally and
  // TryReserve is one-shot. The reservation is BORROWED, not taken: the
  // reference is proof of custody only, and the caller keeps the right (and
  // the duty) to commit or release it. Passing a reset reservation is refused
  // rather than opening unadmitted.
  static NativeHostedRuntimeOpenResult OpenUnderHeldAdmission(ServerProcessCryptoOwner&,
      const DatabaseOwnershipRequest&, NativeHostedRuntimeLimits,
      std::vector<std::unique_ptr<storage::disk::FileDevice>>&,
      const HostedProcessAdmission::Reservation&,
      std::shared_ptr<DatabaseOwnershipLock> held_ownership = nullptr,
      NativeBootstrapLineagePolicy =
          NativeBootstrapLineagePolicy::native_only) noexcept;
  static NativeHostedRuntimeCloseResult Close(std::shared_ptr<NativeHostedDatabaseRuntime>) noexcept;
 private:
  // Shared body. Owns the reservation when Open reserved it, borrows when the
  // caller already holds one.
  static NativeHostedRuntimeOpenResult OpenImpl(ServerProcessCryptoOwner&,
      const DatabaseOwnershipRequest&, NativeHostedRuntimeLimits,
      std::vector<std::unique_ptr<storage::disk::FileDevice>>&,
      std::optional<HostedProcessAdmission::Reservation>&&,
      std::shared_ptr<DatabaseOwnershipLock>,
      NativeBootstrapLineagePolicy) noexcept;
 public:

  class CredentialRead final {
    static std::shared_ptr<NativeHostedDatabaseRuntime> TakeOwner(CredentialRead& other) noexcept {
      other.CheckOwner();
      if (other.mutating_) std::terminate();
      return std::move(other.owner_);
    }
    void CheckOwner() const noexcept {
      if(process_id_!=CurrentProcess()||thread_!=std::this_thread::get_id())std::terminate();
    }
    // First member: reject a foreign move before transferring any governed
    // workspace, provider result, operation or protected source handle.
    std::shared_ptr<NativeHostedDatabaseRuntime> owner_;
    std::shared_ptr<StartupCredentialSource> source_;
    const std::uint64_t process_id_=CurrentProcess();
    const std::thread::id thread_=std::this_thread::get_id();
    bool mutating_ = false;
   public:
    CredentialRead() = default;
    CredentialRead(const CredentialRead&) = delete;
    CredentialRead& operator=(const CredentialRead&) = delete;
    CredentialRead(CredentialRead&& other) noexcept
        : owner_(TakeOwner(other)), source_(std::move(other.source_)),error(other.error),
          crypto(std::move(other.crypto)),custody_error(other.custody_error),
          writer_error(other.writer_error),route_error(other.route_error),
          inputs_(std::move(other.inputs_)),
          preparation_(std::move(other.preparation_)),
          authorization_preparation_(std::move(other.authorization_preparation_)),
          authorization_(other.authorization_),configuration_(other.configuration_),agents_(other.agents_),
          agent_policies_(other.agent_policies_),
          page_zero_anchor_(std::exchange(other.page_zero_anchor_,{})),credentials_(std::move(other.credentials_)),
          catalogs_(std::exchange(other.catalogs_,{})),catalog_backing_bytes_(other.catalog_backing_bytes_),
          catalog_capture_(std::move(other.catalog_capture_)),
          catalog_metadata_(std::exchange(other.catalog_metadata_,{})),
          verification_policies_(std::exchange(other.verification_policies_,{})),
          management_definition_(std::exchange(other.management_definition_,{})),
          transaction_policies_(std::exchange(other.transaction_policies_,{})),
          open_verification_(std::exchange(other.open_verification_,{})),
          authentication_(std::move(other.authentication_)),session_(std::move(other.session_)) {
      other.authorization_={};other.configuration_={};other.agents_={};other.agent_policies_={};
      other.session_.reset();other.authentication_.reset();
      other.credentials_->selection={};other.error=NativeHostedRuntimeError::closed;
    }
    CredentialRead& operator=(CredentialRead&&) = delete;
    ~CredentialRead() { Reset(); }
    NativeHostedRuntimeError error = NativeHostedRuntimeError::invalid_request;
    core::runtime::RuntimeCryptoPoolResult crypto;
    NativeHostedCustodyError custody_error=NativeHostedCustodyError::none;
    storage::database::NativeWriterError writer_error=storage::database::NativeWriterError::none;
    storage::disk::RouteSourceTransitionError route_error=storage::disk::RouteSourceTransitionError::none;
    const NativeRuntimePrincipalCredentialPreparation& preparation() const noexcept { CheckOwner();return preparation_; }
    const NativeLocalServiceStartupCredentialsResult& credentials() const noexcept { CheckOwner();return *credentials_; }
    const engine::internal_api::NativeRuntimeAuthorizationPreparedResult& authorization_preparation() const noexcept {
      CheckOwner();return authorization_preparation_;
    }
    // Exact retained native grant data. No provider/policy activation, session
    // or BEGIN authority is issued by this read. The view borrows this owner.
    const engine::internal_api::NativeRuntimeAuthorizationResult& authorization() const noexcept {
      CheckOwner();return authorization_;
    }
    const NativeLocalServiceStartupConfigurationResult& configuration() const noexcept {
      CheckOwner();return configuration_;
    }
    const NativeLocalServiceAgentConfigurationResult& agents() const noexcept {
      CheckOwner();return agents_;
    }
    const NativeAgentPolicyValidation& agent_policies() const noexcept {
      CheckOwner();return agent_policies_;
    }
    // Page zero's identity as the startup observation retained it, after the
    // device guards were handed off. Every later read of this runtime must
    // find exactly this identity. Nothing is written or published.
    const std::optional<storage::database::NativeFixedRootIdentity>& page_zero_anchor() const noexcept {
      CheckOwner();return page_zero_anchor_;
    }
    std::optional<engine::internal_api::NativeLocalServiceAuthenticationObservation> authentication() const noexcept {
      CheckOwner();return NativeHostedDatabaseRuntime::ObserveAuthentication(*this);
    }
    std::optional<engine::internal_api::NativeLocalServiceSessionObservation> session() const noexcept {
      CheckOwner();return NativeHostedDatabaseRuntime::ObserveSession(*this);
    }
    const storage::database::NativeCatalogCohortInspection& catalogs() const noexcept {
      CheckOwner();return catalogs_;
    }
    const storage::database::NativeCatalogCapture& catalog_capture() const noexcept {
      CheckOwner();return catalog_capture_;
    }
    const engine::internal_api::NativeCatalogMetadataVerification& catalog_metadata() const noexcept {
      CheckOwner();return catalog_metadata_;
    }
    const engine::internal_api::NativeVerificationPolicyDeclarations& verification_policies() const noexcept {
      CheckOwner();return verification_policies_;
    }
    // The retained clean result of the read-only open verification, if any.
    const std::optional<NativeOpenVerificationObservation>& open_verification() const noexcept {
      CheckOwner();return open_verification_;
    }
    const engine::internal_api::NativeStartupManagementDefinition& management_definition() const noexcept {
      CheckOwner();return management_definition_;
    }
    const engine::internal_api::NativeTransactionPolicyDeclarations& transaction_policies() const noexcept {
      CheckOwner();return transaction_policies_;
    }
    bool ok() const noexcept { CheckOwner();return error == NativeHostedRuntimeError::none && credentials_->ok()&&authorization_.ok()&&configuration_.ok()&&agents_.ok(); }
    void Reset() noexcept {
      CheckOwner();if (mutating_) std::terminate();
      mutating_=true;
      auto owner=std::move(owner_);
      if(owner){if(owner->entering_)std::terminate();owner->entering_=true;}
      // Callback observers see a retired read before any resource can invoke
      // them. Keep the original host pinned and busy through the last cleanup.
      error=NativeHostedRuntimeError::closed;authorization_={};configuration_={};agents_={};agent_policies_={};
      session_.reset();authentication_.reset();
      open_verification_.reset();
      catalogs_={};catalog_backing_bytes_=0;
      catalog_metadata_={};
      verification_policies_={};
      management_definition_={};
      transaction_policies_={};
      catalog_capture_={};
      credentials_->selection={};
      page_zero_anchor_.reset();
      credentials_->comparison.reset();
      authorization_preparation_.workspace={};
      preparation_.workspace = {};
      credentials_->operation.Reset();
      inputs_.reset();
      source_.reset();
      if(owner) {
        if(!owner->read_active_)std::terminate();
        owner->read_active_=false;owner->entering_=false;
      }
      owner.reset();mutating_=false;
    }
   private:
    friend class NativeHostedDatabaseRuntime;
    // The startup observation is retained: page zero's identity.
    bool anchored() const noexcept {return page_zero_anchor_.has_value();}
    struct Inputs {
      const NativeLocalServiceStartupCredentialsRequest request;
      const core::memory::MemoryTag tag;
      Inputs(NativeLocalServiceStartupCredentialsRequest&& command,const core::memory::MemoryTag& label)
          :request(std::move(command)),tag(label) {}
    };
    // Heap-backed labels and the last stop-state references must not retire
    // when Read returns with device guards still held. Unique ownership also
    // makes moves leave no allocating/deallocating input cleanup behind.
    std::unique_ptr<Inputs> inputs_;
    // Reverse destruction drops all selected/source fences before either owner.
    NativeRuntimePrincipalCredentialPreparation preparation_;
    engine::internal_api::NativeRuntimeAuthorizationPreparedResult authorization_preparation_;
    engine::internal_api::NativeRuntimeAuthorizationResult authorization_;
    NativeLocalServiceStartupConfigurationResult configuration_;
    NativeLocalServiceAgentConfigurationResult agents_;
    NativeAgentPolicyValidation agent_policies_;
    std::optional<storage::database::NativeFixedRootIdentity> page_zero_anchor_;
    std::optional<NativeLocalServiceStartupCredentialsResult> credentials_{std::in_place};
    storage::database::NativeCatalogCohortInspection catalogs_;
    std::size_t catalog_backing_bytes_=0;
    storage::database::NativeCatalogCapture catalog_capture_;
    engine::internal_api::NativeCatalogMetadataVerification catalog_metadata_;
    engine::internal_api::NativeVerificationPolicyDeclarations verification_policies_;
    engine::internal_api::NativeStartupManagementDefinition management_definition_;
    engine::internal_api::NativeTransactionPolicyDeclarations transaction_policies_;
    std::optional<NativeOpenVerificationObservation> open_verification_;
    // Retire the engine decision before the credential generation, original
    // operation and governed grant views it borrows. No capability escapes.
    std::optional<engine::internal_api::NativeLocalServiceAuthentication> authentication_;
    std::optional<engine::internal_api::NativeLocalServiceSession> session_;
  };
  static CredentialRead ReadStartupCredentials(std::shared_ptr<NativeHostedDatabaseRuntime>,
      NativeLocalServiceStartupCredentialsRequest, std::shared_ptr<StartupCredentialSource>,
      const core::memory::MemoryTag&, const core::memory::ScopedProtectedBuffer&);

  // Transfer the exact initial writer reservation into the private comparison
  // owner, then retire its selected device guards. Retained catalog/grant views,
  // protected generation, original route and all governed backing stay pinned.
  // After guard retirement, validate the stored agent policy bodies against
  // the registry and schemas before counter observation/resource preparation.
  // A refusal retains the exact source for diagnosis and retry. No provider,
  // session, lifecycle verification, worker or BEGIN authority is activated.
  // Reentrant retirement refuses; destroying/moving this acquiring-thread owner
  // during retirement terminates before any custody transfer.
  static NativeHostedRuntimeError RetainStartupObservation(CredentialRead&) noexcept;

  // Scan every declared native catalog role, its full navigation/base-leaf
  // coverage, metadata chains and committed row visibility. No full engine
  // startup-verification receipt or service admission is issued by this scan.
  static NativeHostedRuntimeError ReadStartupCatalogs(CredentialRead&,
      storage::database::NativeSelectedCheckpointMemoryLimits,
      const storage::database::NativeCatalogRelationMemoryLimits&,
      storage::database::NativeCatalogCaptureLimits={}) noexcept;

  // Engine-owned name and local namespace verification over the retained
  // capture, after service authentication. Scratch uses the
  // installed database allocator; no physical guard or heap fallback is used
  // by the verifier. This component cannot admit agents or normal service.
  static NativeHostedRuntimeError VerifyStartupCatalogMetadata(CredentialRead&) noexcept;

  // Bind the requested verification rules to the exact retained native catalog.
  // Actual activation and authorization remain separate engine obligations.
  static NativeHostedRuntimeError ResolveStartupVerificationPolicies(CredentialRead&,
      const engine::internal_api::NativeVerificationPolicyRequest&) noexcept;
  // Normal startup uses only the actual persisted coordinator's typed policy
  // references and closed class/level selection, with no caller substitution.
  static NativeHostedRuntimeError ResolveConfiguredStartupVerificationPolicies(CredentialRead&) noexcept;
  // Resolve the cataloged local verification descriptor and every exact
  // profile through actual retained native source and installed memory policy.
  static NativeHostedRuntimeError ResolveStartupManagementDefinition(CredentialRead&) noexcept;
  static NativeHostedRuntimeError ResolveStartupTransactionPolicies(CredentialRead&) noexcept;


  // Apply the selected database hard ceiling to the actual process ledger,
  // including already admitted bootstrap credit. Requires the original writer
  // operation and credential/grant observation, with device guards retired.
  // This constraint is one resource prerequisite, not full policy/provider
  // activation, an authenticated session, a transaction or readiness.
  static NativeDatabaseMemoryEnvelopeResult ConstrainStartupMemory(CredentialRead&) noexcept;

  // After reservation constraints, install the selected context/zeroing/failure
  // behavior in the SAME allocator that owns bootstrap storage. Refusal keeps
  // the constraint and source for retry. No session or engine epoch is issued.
  static NativeDatabaseAllocationBehaviorResult InstallStartupAllocationBehavior(CredentialRead&) noexcept;

  // Execute the built-in engine verifier under the actual installed database
  // memory policy. The original writer reservation and protected source stay
  // pinned. A successful decision is a prerequisite to native service attach;
  // it does not activate MGA or start workers/listeners.
  static engine::internal_api::NativeLocalServiceAuthenticationResult AuthenticateStartupService(
      CredentialRead&, const core::memory::ScopedProtectedBuffer&) noexcept;

  // Attach only the authenticated internal startup/recovery service to this
  // original native owner. The returned observation is not a client context.
  static engine::internal_api::NativeLocalServiceSessionResult AttachStartupServiceSession(CredentialRead&) noexcept;

  // SB_OPEN_VERIFICATION_READ_ONLY: verify the opened primary file before any
  // startup transaction exists. Call after the startup verification policies
  // resolve. Reads through the retained custody operation with its own memory
  // grant; writes nothing. A clean result is retained in the read.
  static NativeOpenVerificationResult VerifyStartupDatabaseFile(CredentialRead&,const NativeOpenVerificationLimits&) noexcept;
  Uuid database_uuid() const noexcept { return SameOwner() ? database_ : Uuid{}; }
  Uuid primary_filespace_uuid() const noexcept { return SameOwner() ? primary_ : Uuid{}; }
  Uuid engine_instance_uuid() const noexcept {
    return SameOwner()&&writer_adopted_&&!writer_retired_&&!closed_?engine_instance_:Uuid{};
  }
  // Descriptive facts only. They cannot grant an operation, session or BEGIN.
  std::span<const storage::database::NativeWriterMemberCloseReceipt> close_receipts() const noexcept {
    return SameOwner() ? std::span<const storage::database::NativeWriterMemberCloseReceipt>{close_receipts_} :
        std::span<const storage::database::NativeWriterMemberCloseReceipt>{};
  }
 private:
  static std::optional<engine::internal_api::NativeLocalServiceSessionObservation>
      ObserveSession(const CredentialRead& read) noexcept {
    if(!read.session_)return std::nullopt;
    return read.session_->observation_;
  }
  static std::optional<engine::internal_api::NativeLocalServiceAuthenticationObservation>
      ObserveAuthentication(const CredentialRead& read) noexcept {
    if(!read.authentication_)return std::nullopt;
    return read.authentication_->observation_;
  }
  static NativeAgentPolicyValidation ValidateAgentPolicies(const NativeLocalServiceAgentConfigurationResult&) noexcept;
  static NativeHostedRuntimeCloseResult CloseImpl(NativeHostedDatabaseRuntime&) noexcept;
  explicit NativeHostedDatabaseRuntime(ServerProcessCryptoOwner& process) : process_(&process) {}
  static std::uint64_t CurrentProcess() noexcept {
    return static_cast<std::uint64_t>(
#if defined(_WIN32)
        ::_getpid()
#else
        ::getpid()
#endif
        );
  }
  bool SameOwner() const noexcept {
    return process_id_==CurrentProcess()&&thread_==std::this_thread::get_id();
  }
  NativeBootstrapMemoryResult Acquire(std::size_t bytes, NativeBootstrapMemoryKind kind) noexcept {
    const auto operation=core::uuid::IssueRuntimeIdentityV7();
    const auto owner=core::uuid::IssueRuntimeIdentityV7();
    const auto context=core::uuid::IssueRuntimeIdentityV7();
    if (!operation || !owner || !context) return {NativeBootstrapMemoryError::invalid_request};
    return process_->AcquireNativeBootstrapResource({database_,*operation,*owner,*context,bytes,kind});
  }
  static storage::database::NativeStorageMemoryBinding StorageBinding(
      const core::memory::ReservationBackedMemoryResource& resource) noexcept {
    const auto& request=resource.request();
    return {Uuid{request.binary_ownership[core::memory::MemoryBinaryScopeKind::database]},
        Uuid{request.binary_operation_uuid},Uuid{request.binary_ownership[core::memory::MemoryBinaryScopeKind::owner]},
        Uuid{request.binary_ownership[core::memory::MemoryBinaryScopeKind::context]}};
  }
  const std::uint64_t process_id_ = CurrentProcess();
  const std::thread::id thread_ = std::this_thread::get_id();
  ServerProcessCryptoOwner* const process_;
  NativeHostedRuntimeLimits limits_;
  std::optional<HostedProcessAdmission::Reservation> process_admission_;
  Uuid database_,primary_,engine_instance_;
  bool entering_ = false, read_active_ = false, writer_adopted_ = false, writer_retired_ = false, closed_ = false;
  std::vector<storage::database::NativeWriterMemberCloseReceipt> close_receipts_;
  core::runtime::RuntimeCryptoPoolOwner database_crypto_;
  core::hash::CryptoMemoryBinding crypto_binding_;
  storage::database::NativeStorageMemory reading_,catalog_,authorization_;
  // Retire writer/cohort before crypto backing or the original route owner.
  std::shared_ptr<NativeHostedDatabaseCustody> custody_;
};

struct NativeHostedRuntimeOpenResult {
  NativeHostedRuntimeError error = NativeHostedRuntimeError::invalid_request;
  DatabaseOwnershipResult ownership;
  NativeHostedDatabaseCustody::ProbeResult probe;
  NativeHostedDatabaseCustody::AdoptionResult adoption;
  NativeBootstrapMemoryResult memory;
  core::runtime::RuntimeCryptoPoolResult crypto;
  HostedProcessAdmissionError process_admission_error = HostedProcessAdmissionError::none;
  NativeHostedCustodyError custody_error = NativeHostedCustodyError::none;
  storage::disk::RouteSourceTransitionError route_error = storage::disk::RouteSourceTransitionError::none;
  storage::database::NativeStorageMemoryError memory_binding_error = storage::database::NativeStorageMemoryError::none;
  std::error_code synchronization_error;
  // Also returned on failure: reconcile/Close this original owner explicitly.
  std::shared_ptr<NativeHostedDatabaseRuntime> runtime;
  bool ok() const noexcept { return error == NativeHostedRuntimeError::none && bool(runtime); }
};

} // namespace scratchbird::server
