// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_delete_durable_store.hpp"
#include "mga_relation_store/mga_update_durable_frame_store_internal.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "dml/delete_recovery_authority.hpp"
#include "dml/delete_durable_authority_codec.hpp"
#include "api_diagnostics.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
namespace detail = mga_update_durable_detail;
namespace mga = scratchbird::transaction::mga;
using Chain = std::vector<std::vector<std::uint8_t>>;
using State = w::TypedUpdateJournalState;
using Disposition = EngineDmlDeleteRecoveryDispositionV1;
using Publication = MgaDmlDeletePublicationStatusV1;
constexpr std::size_t kMaximumChainBytes = 4 * w::kTypedDeleteJournalWithResultBytes;

bool Diagnostic(EngineApiDiagnostic* out, const char* detail_text) {
  if (out) *out = MakeEngineApiDiagnostic("DML.DELETE_FAILED",
      "sblr.dml_delete_rows.durable_store", detail_text, true);
  return false;
}
bool FenceNativeHistory(const EngineRequestContext& context) {
  const auto marker_path = context.database_path + ".sb.mga_savepoints";
  return scratchbird::storage::disk::SyncFilesystemPath(marker_path, true).ok() &&
      scratchbird::storage::disk::SyncParentDirectoryPath(marker_path).ok();
}
bool ReadChain(const std::string& path, Chain* chain,
               w::TypedDeleteJournalRecord* head, bool* missing) {
  std::error_code error;
  *missing = !std::filesystem::exists(path, error);
  if (error) return false;
  if (*missing) return true;
  const auto size = std::filesystem::file_size(path, error);
  if (error || !size || size > kMaximumChainBytes) return false;
  std::ifstream input(path, std::ios::binary);
  std::size_t remaining = size;
  while (remaining) {
    if (chain->size() == 4 || remaining < w::kTypedDeleteJournalHeaderBytes) return false;
    std::vector<std::uint8_t> bytes(w::kTypedDeleteJournalHeaderBytes);
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input) return false;
    std::uint32_t extent = 0;
    if (!detail::DmlUpdateDurableReadU32(bytes, 8, &extent) ||
        (extent != w::kTypedDeleteJournalWithoutResultBytes &&
         extent != w::kTypedDeleteJournalWithResultBytes) || extent > remaining) return false;
    bytes.resize(extent);
    input.read(reinterpret_cast<char*>(bytes.data() + w::kTypedDeleteJournalHeaderBytes),
               extent - w::kTypedDeleteJournalHeaderBytes);
    w::TypedDeleteJournalRecord decoded;
    w::TypedDeleteCarrierError codec_error;
    if (!input || !w::DecodeAndValidateTypedDeleteJournal(bytes,
          chain->empty() ? nullptr : head, &decoded, &codec_error)) return false;
    *head = std::move(decoded);
    chain->push_back(std::move(bytes));
    remaining -= extent;
  }
  return input.peek() == std::char_traits<char>::eof() && !input.bad();
}

// All path creation and byte preparation happens before the statement barrier.
bool WriteFileFenced(const std::string& path, const Chain& chain) {
#if defined(_WIN32)
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  for (const auto& bytes : chain) {
    DWORD written = 0;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) { ok = false; break; }
  }
  if (ok) ok = FlushFileBuffers(file) != 0;
  if (!CloseHandle(file)) ok = false;
#else
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (file < 0) return false;
  bool ok = true;
  for (const auto& bytes : chain) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) { ok = false; break; }
      offset += static_cast<std::size_t>(written);
    }
    if (!ok) break;
  }
  if (ok) ok = ::fsync(file) == 0;
  if (::close(file) != 0) ok = false;
#endif
  return ok;
}
}

struct MgaDmlDeleteDurableStoreV1::Impl {
  EngineRequestContext context;
  w::TypedUpdateUuid descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  std::string path, temporary, pending, pending_temporary, authority, authority_temporary;
  std::unique_ptr<detail::DmlUpdateDurableFileLock> lock;
  Chain chain, staged;
  w::TypedDeleteJournalRecord head, staged_head;
  bool uncertain = false;
#if !defined(_WIN32)
  int directory_fd = -1;
  ~Impl() { if (directory_fd >= 0) ::close(directory_fd); }
#endif

  bool RenameFenced(const std::string& from, const std::string& to) noexcept {
#if defined(_WIN32)
    return MoveFileExA(from.c_str(), to.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0 && ::fsync(directory_fd) == 0;
#endif
  }
  bool RemovePendingFenced() {
    if (staged.empty()) return true;
#if defined(_WIN32)
    if (!DeleteFileA(pending.c_str())) return false;
#else
    if (::unlink(pending.c_str()) != 0 || ::fsync(directory_fd) != 0) return false;
#endif
    staged.clear();
    return true;
  }
  bool Matches(const w::TypedDeleteJournalRecord& value) const {
    return value.descriptor.descriptor_uuid == descriptor_uuid &&
        value.descriptor.descriptor_generation == descriptor_generation &&
        detail::DmlUpdateDurableTypedUuidText(value.database_uuid) == context.database_uuid &&
        detail::DmlUpdateDurableTypedUuidText(value.owning_transaction_uuid) == context.transaction_uuid &&
        value.owning_local_transaction_id == context.local_transaction_id &&
        detail::DmlUpdateDurableTypedUuidText(value.authenticated_statement_receipt_uuid) == context.statement_receipt_uuid;
  }
  bool ActiveInventory() const {
    const auto inventory = scratchbird::storage::database::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    if (!inventory.ok()) return false;
    const auto transaction = mga::LookupLocalTransaction(inventory.snapshot->inventory,
        mga::MakeLocalTransactionId(context.local_transaction_id));
    w::TypedUpdateUuid transaction_uuid{};
    if (!detail::DmlUpdateDurableTypedUuid(context.transaction_uuid, &transaction_uuid)) return false;
    return transaction.ok() && transaction.entry.state == mga::TransactionState::active &&
        std::equal(transaction_uuid.begin(), transaction_uuid.end(),
                   transaction.entry.identity.transaction_uuid.value.bytes.begin()) &&
        scratchbird::storage::database::RevalidateLocalTransactionInventorySnapshot(*inventory.snapshot).ok();
  }
  Publication PublishNoAlloc() noexcept {
    if (!RenameFenced(pending, path)) {
      uncertain = true;
      return Publication::publication_uncertain;
    }
    chain.swap(staged);
    // Leave decoded head untouched: successful publication seals this owner.
    // Reopening validates the terminal bytes before any subsequent action.
    uncertain = true;
    return Publication::published;
  }
};

MgaDmlDeleteDurableStoreV1::MgaDmlDeleteDurableStoreV1(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
MgaDmlDeleteDurableStoreV1::~MgaDmlDeleteDurableStoreV1() = default;

std::unique_ptr<MgaDmlDeleteDurableStoreV1> MgaDmlDeleteDurableStoreV1::Open(
    const EngineRequestContext& context, const w::TypedUpdateUuid& descriptor_uuid,
    std::uint64_t generation, EngineApiDiagnostic* diagnostic) {
  const auto refuse = [&](const char* why) -> std::unique_ptr<MgaDmlDeleteDurableStoreV1> {
    Diagnostic(diagnostic, why); return {};
  };
  if (context.database_path.empty() || !context.local_transaction_id || !generation ||
      detail::DmlUpdateDurableZero(descriptor_uuid) || context.cluster_transaction_active || context.route_fence_present)
    return refuse("local_owned_descriptor_required");
  w::TypedUpdateUuid checked_uuid{};
  for (const auto* identity : {&context.database_uuid, &context.transaction_uuid,
      &context.statement_receipt_uuid, &context.statement_snapshot_uuid,
      &context.statement_metadata_snapshot_uuid}) {
    if (!detail::DmlUpdateDurableTypedUuid(identity->canonical, &checked_uuid) ||
        detail::DmlUpdateDurableZero(checked_uuid)) return refuse("complete_UUID_owner_required");
  }
  if (!context.catalog_generation_id || !context.datatype_registry_generation)
    return refuse("owner_generations_required");
  const auto inventory = scratchbird::storage::database::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
  if (!inventory.ok()) return refuse("owning_database_inventory_required");
  const auto owner = mga::LookupLocalTransaction(inventory.snapshot->inventory,
      mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!owner.ok() || !detail::DmlUpdateDurableTypedUuid(context.transaction_uuid, &checked_uuid) ||
      !std::equal(checked_uuid.begin(), checked_uuid.end(), owner.entry.identity.transaction_uuid.value.bytes.begin()) ||
      !scratchbird::storage::database::RevalidateLocalTransactionInventorySnapshot(*inventory.snapshot).ok())
    return refuse("owning_transaction_inventory_required");
  auto impl = std::make_unique<Impl>();
  impl->context = context; impl->descriptor_uuid = descriptor_uuid;
  impl->descriptor_generation = generation;
  const auto directory = context.database_path + ".sb.mga_delete_operations.v1";
  if (!detail::DmlUpdateDurableEnsureDirectory(directory)) return refuse("directory_fence_failed");
  // A durable new directory also requires the parent directory entry fence.
  const auto parent = std::filesystem::path(directory).parent_path();
  if (!detail::DmlUpdateDurableEnsureDirectory(parent.empty() ? "." : parent.string()))
    return refuse("parent_directory_fence_failed");
  impl->path = directory + "/" + detail::DmlUpdateDurableTypedUuidText(descriptor_uuid) +
      "." + std::to_string(generation) + ".ddjr";
  impl->temporary = impl->path + ".writing";
  impl->pending = impl->path + ".publication";
  impl->pending_temporary = impl->pending + ".writing";
  impl->authority = impl->path + ".authority";
  impl->authority_temporary = impl->authority + ".writing";
  impl->lock = std::make_unique<detail::DmlUpdateDurableFileLock>(impl->path);
  if (!impl->lock->ok()) return refuse("operation_lock_failed");
#if !defined(_WIN32)
  impl->directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (impl->directory_fd < 0) return refuse("directory_open_failed");
#endif
  bool missing = false, pending_missing = false;
  if (!ReadChain(impl->path, &impl->chain, &impl->head, &missing) ||
      (!missing && !impl->Matches(impl->head))) return refuse("durable_chain_invalid");
  if (!ReadChain(impl->pending, &impl->staged, &impl->staged_head, &pending_missing))
    return refuse("staged_chain_invalid");
  if (!pending_missing && (missing || impl->head.lifecycle_state != State::prepared ||
      impl->staged_head.lifecycle_state != State::published ||
      impl->staged.size() != impl->chain.size() + 1 ||
      !std::equal(impl->chain.begin(), impl->chain.end(), impl->staged.begin())))
    return refuse("staged_chain_not_exact_successor");
  if (diagnostic) *diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return std::unique_ptr<MgaDmlDeleteDurableStoreV1>(new MgaDmlDeleteDurableStoreV1(std::move(impl)));
}

std::span<const std::vector<std::uint8_t>> MgaDmlDeleteDurableStoreV1::chain() const {
  return impl_->chain;
}

bool MgaDmlDeleteDurableStoreV1::LoadAuthorityBundle(
    DmlDeleteDurableAuthorityBundleV1* out, EngineApiDiagnostic* diagnostic) const {
  const auto& p = *impl_;
  if (!out || p.uncertain) return Diagnostic(diagnostic, "bundle_output_or_reopen_required");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(p.authority, error);
  if (error || !std::filesystem::is_regular_file(status))
    return Diagnostic(diagnostic, "bundle_regular_file_required");
  const auto size = std::filesystem::file_size(p.authority, error);
  if (error || size < 272 || size > kDmlDeleteDurableAuthorityMaximumBytesV1)
    return Diagnostic(diagnostic, "bundle_extent_invalid");
  std::vector<std::uint8_t> bytes(size);
  std::ifstream input(p.authority, std::ios::binary);
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input || input.peek() != std::char_traits<char>::eof() || input.bad())
    return Diagnostic(diagnostic, "bundle_read_failed");
  DmlDeleteDurableAuthorityBundleV1 value;
  if (!DecodeDmlDeleteDurableAuthorityBundleV1(bytes, &value, diagnostic)) return false;
  if (value.descriptor.descriptor_uuid != p.descriptor_uuid ||
      value.descriptor.descriptor_generation != p.descriptor_generation ||
      !MatchesDmlDeleteDurableAuthorityOwnerV1(p.context, value) ||
      (!p.chain.empty() && (value.descriptor.exact_bytes != p.head.descriptor.exact_bytes ||
       (p.head.statement_savepoint_generation && p.head.statement_savepoint_uuid != value.reserved_statement_savepoint_uuid))))
    return Diagnostic(diagnostic, "bundle_owner_or_DDJR_mismatch");
  *out = std::move(value);
  if (diagnostic) *diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return true;
}

bool MgaDmlDeleteDurableStoreV1::StoreAuthorityBundle(
    const DmlDeleteDurableAuthorityBundleV1& input, EngineApiDiagnostic* diagnostic) {
  auto& p = *impl_;
  if (p.uncertain || input.descriptor.descriptor_uuid != p.descriptor_uuid ||
      input.descriptor.descriptor_generation != p.descriptor_generation ||
      !MatchesDmlDeleteDurableAuthorityOwnerV1(p.context, input) || !p.ActiveInventory())
    return Diagnostic(diagnostic, "bundle_owner_or_active_inventory_required");
  std::vector<std::uint8_t> bytes;
  if (!EncodeDmlDeleteDurableAuthorityBundleV1(input, &bytes, diagnostic)) return false;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(p.authority, error);
  if (error && error != std::errc::no_such_file_or_directory)
    return Diagnostic(diagnostic, "bundle_status_failed");
  if (std::filesystem::exists(status)) {
    DmlDeleteDurableAuthorityBundleV1 existing;
    if (!LoadAuthorityBundle(&existing, diagnostic)) return false;
    if (existing.exact_bytes != bytes) return Diagnostic(diagnostic, "immutable_bundle_conflict");
    // A prior rename may have succeeded while its directory fence failed.
    // Exact retry still needs the file and directory fences before success.
    if (!scratchbird::storage::disk::SyncFilesystemPath(p.authority, true).ok() ||
        !scratchbird::storage::disk::SyncParentDirectoryPath(p.authority).ok()) {
      p.uncertain = true;
      return Diagnostic(diagnostic, "bundle_retry_fence_uncertain_reopen_required");
    }
  } else {
    if (!p.chain.empty()) return Diagnostic(diagnostic, "bundle_must_precede_bound");
    if (!WriteFileFenced(p.authority_temporary, Chain{std::move(bytes)}))
      return Diagnostic(diagnostic, "bundle_write_failed");
    if (!p.RenameFenced(p.authority_temporary, p.authority)) {
      p.uncertain = true;
      return Diagnostic(diagnostic, "bundle_fence_uncertain_reopen_required");
    }
  }
  if (diagnostic) *diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return true;
}

bool MgaDmlDeleteDurableStoreV1::Append(const w::TypedDeleteJournalRecord& next,
                                      EngineApiDiagnostic* diagnostic) {
  auto& p = *impl_;
  auto success = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  if (p.uncertain || next.lifecycle_state == State::published || !p.Matches(next))
    return Diagnostic(diagnostic, "owner_or_successor_refused");
  std::vector<std::uint8_t> bytes;
  w::TypedDeleteCarrierError codec_error;
  w::TypedDeleteJournalRecord decoded;
  if (!w::EncodeTypedDeleteJournal(next, p.chain.empty() ? nullptr : &p.head, &bytes, &codec_error) ||
      !w::DecodeAndValidateTypedDeleteJournal(bytes, p.chain.empty() ? nullptr : &p.head, &decoded, &codec_error))
    return Diagnostic(diagnostic, "noncanonical_successor");
  std::error_code bundle_error;
  const auto bundle_status = std::filesystem::symlink_status(p.authority, bundle_error);
  if (bundle_error && bundle_error != std::errc::no_such_file_or_directory)
    return Diagnostic(diagnostic, "bundle_status_failed");
  if (std::filesystem::exists(bundle_status)) {
    DmlDeleteDurableAuthorityBundleV1 bundle;
    if (!LoadAuthorityBundle(&bundle, diagnostic)) return false;
    if (bundle.descriptor.exact_bytes != decoded.descriptor.exact_bytes ||
        (decoded.statement_savepoint_generation && decoded.statement_savepoint_uuid != bundle.reserved_statement_savepoint_uuid))
      return Diagnostic(diagnostic, "DDJR_successor_bundle_mismatch");
  }
  auto replacement = p.chain;
  replacement.push_back(std::move(bytes));
  if (next.statement_savepoint_generation && !FenceNativeHistory(p.context))
    return Diagnostic(diagnostic, "native_history_fence_failed");
  const auto observed = ObserveDmlDeleteRecoveryAuthorityV1(p.context, replacement);
  if (!observed.ok || !p.ActiveInventory() ||
      (next.lifecycle_state == State::bound && observed.disposition != Disposition::abandon_unexecuted) ||
      ((next.lifecycle_state == State::intent || next.lifecycle_state == State::prepared) &&
       observed.disposition != Disposition::rollback_statement) ||
      (next.lifecycle_state == State::aborted && observed.disposition != Disposition::aborted))
    return Diagnostic(diagnostic, "MGA_successor_authority_refused");
  if (!p.staged.empty() && next.lifecycle_state != State::aborted)
    return Diagnostic(diagnostic, "prepared_publication_already_staged");
  if (!WriteFileFenced(p.temporary, replacement)) return Diagnostic(diagnostic, "successor_write_failed");
  if (!p.RemovePendingFenced() || !p.RenameFenced(p.temporary, p.path)) {
    p.uncertain = true;
    return Diagnostic(diagnostic, "successor_fence_uncertain_reopen_required");
  }
  p.chain.swap(replacement); p.head = std::move(decoded);
  if (diagnostic) *diagnostic = std::move(success);
  return true;
}

bool MgaDmlDeleteDurableStoreV1::StagePublication(EngineApiDiagnostic* diagnostic) {
  auto& p = *impl_;
  auto success = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  if (p.uncertain || p.chain.empty() || p.head.lifecycle_state != State::prepared)
    return Diagnostic(diagnostic, "prepared_result_required");
  const auto observed = ObserveDmlDeleteRecoveryAuthorityV1(p.context, p.chain);
  if (!observed.ok || observed.disposition != Disposition::rollback_statement || !p.ActiveInventory())
    return Diagnostic(diagnostic, "active_statement_boundary_required");
  if (!p.staged.empty()) {
    if (diagnostic) *diagnostic = std::move(success);
    return true;
  }
  auto next = p.head;
  next.lifecycle_state = State::published; ++next.journal_sequence;
  next.prior_record_sha256 = p.head.record_evidence_sha256;
  std::vector<std::uint8_t> bytes;
  w::TypedDeleteCarrierError codec_error;
  w::TypedDeleteJournalRecord decoded;
  if (!w::EncodeTypedDeleteJournal(next, &p.head, &bytes, &codec_error) ||
      !w::DecodeAndValidateTypedDeleteJournal(bytes, &p.head, &decoded, &codec_error))
    return Diagnostic(diagnostic, "publication_encoding_failed");
  auto staged = p.chain; staged.push_back(std::move(bytes));
  if (!WriteFileFenced(p.pending_temporary, staged)) return Diagnostic(diagnostic, "publication_stage_write_failed");
  if (!p.RenameFenced(p.pending_temporary, p.pending)) {
    p.uncertain = true;
    return Diagnostic(diagnostic, "publication_stage_fence_uncertain_reopen_required");
  }
  p.staged.swap(staged); p.staged_head = std::move(decoded);
  if (diagnostic) *diagnostic = std::move(success);
  return true;
}

MgaDmlDeletePublicationStatusV1 MgaDmlDeleteDurableStoreV1::ReleaseAndPublish() {
  auto& p = *impl_;
  if (p.uncertain || p.staged.empty()) return Publication::refused;
  const auto observed = ObserveDmlDeleteRecoveryAuthorityV1(p.context, p.chain);
  if (!observed.ok || observed.disposition != Disposition::rollback_statement || !p.ActiveInventory())
    return Publication::refused;
  const auto key = MgaSavepointUuidKey(detail::DmlUpdateDurableTypedUuidText(p.head.statement_savepoint_uuid));
  // Release itself owns its MGA append/fence. Once it succeeds, nothing below
  // may encode, allocate, cancel or roll back the published statement.
  const auto released = ReleaseMgaSavepointMarker(p.context, key);
  if (released.error) { p.uncertain = true; return Publication::release_uncertain; }
  return p.PublishNoAlloc();
}

MgaDmlDeletePublicationStatusV1 MgaDmlDeleteDurableStoreV1::CompleteReleasedPublication() {
  auto& p = *impl_;
  if (p.uncertain || p.chain.empty()) return Publication::refused;
  // A previous release may have written its complete marker but lost the
  // synchronization acknowledgement. Fence the native history before using
  // it to finish publication; readable page-cache bytes alone are not proof
  // that the selected durable barrier completed.
  if (!FenceNativeHistory(p.context)) return Publication::refused;
  const auto observed = ObserveDmlDeleteRecoveryAuthorityV1(p.context, p.chain);
  if (!observed.ok) return Publication::refused;
  if (observed.disposition == Disposition::published) return Publication::published;
  if (observed.disposition != Disposition::publish_prepared_result || p.staged.empty())
    return Publication::refused;
  return p.PublishNoAlloc();
}

MgaDmlDeleteAbortStatusV1 MgaDmlDeleteDurableStoreV1::AbortBeforePublication() {
  using Abort = MgaDmlDeleteAbortStatusV1;
  auto& p = *impl_;
  if (p.uncertain || p.chain.empty()) return Abort::refused;
  if (p.head.statement_savepoint_generation && !FenceNativeHistory(p.context))
    return Abort::refused;
  const auto observed = ObserveDmlDeleteRecoveryAuthorityV1(p.context, p.chain);
  if (!observed.ok) return Abort::refused;
  if (observed.disposition == Disposition::aborted) return Abort::aborted;
  if (!p.ActiveInventory() ||
      (observed.disposition != Disposition::abandon_unexecuted &&
       observed.disposition != Disposition::rollback_statement &&
       observed.disposition != Disposition::statement_already_rewound)) return Abort::refused;
  // Prepare journal material before asking MGA to rewind. Exceptions after a
  // rewind still leave retained marker history for idempotent reopened retry.
  auto aborted = p.head;
  aborted.lifecycle_state = State::aborted; ++aborted.journal_sequence;
  aborted.prior_record_sha256 = p.head.record_evidence_sha256;
  aborted.prior_result.reset();
  if (p.head.statement_savepoint_generation) {
    const auto key = MgaSavepointUuidKey(detail::DmlUpdateDurableTypedUuidText(p.head.statement_savepoint_uuid));
    if (observed.disposition == Disposition::rollback_statement &&
        RollbackToMgaSavepointMarker(p.context, key).error) {
      p.uncertain = true;
      return Abort::rewind_uncertain;
    }
    const auto marker = ObserveMgaSavepointMarker(p.context, key, p.head.statement_savepoint_generation);
    if (!marker.ok || (!marker.rolled_back && marker.lifecycle != MgaSavepointMarkerLifecycle::invalidated)) {
      p.uncertain = true;
      return Abort::rewind_uncertain;
    }
    if (marker.lifecycle == MgaSavepointMarkerLifecycle::active &&
        ReleaseMgaSavepointMarker(p.context, key).error) {
      p.uncertain = true;
      return Abort::release_uncertain;
    }
  }
  EngineApiDiagnostic diagnostic;
  if (!Append(aborted, &diagnostic)) return Abort::journal_uncertain;
  return Abort::aborted;
}
}  // namespace scratchbird::engine::internal_api
