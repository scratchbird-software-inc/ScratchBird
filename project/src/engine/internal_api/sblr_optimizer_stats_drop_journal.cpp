// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_optimizer_stats_drop_journal.hpp"

#include "api_diagnostics.hpp"
#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include "engine/optimizer/optimizer_statistics_invalidation.hpp"
#include "engine/internal_api/transaction/transaction_api.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "transaction/mga/transaction_inventory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

namespace drop = scratchbird::engine::sblr;
namespace mga = scratchbird::transaction::mga;
namespace storage_db = scratchbird::storage::database;

constexpr std::size_t kRecordBytes = 472;
constexpr std::size_t kMaximumRecordCount = 262144;
constexpr std::uint64_t kInitialStatisticsEpoch = 1;
constexpr std::string_view kRecordDomain =
    "ScratchBird.SblrOptimizerStatsDropJournalRecord.V1";

std::mutex g_process_journal_mutex;
std::atomic<std::uint64_t> g_uuid_ordinal{1};

struct JournalRecord {
  drop::SblrOptimizerStatsDropUuidV1 database_uuid{};
  drop::SblrOptimizerStatsDropUuidV1 effect_uuid{};
  drop::SblrOptimizerStatsDropSha256V1 descriptor_sha256{};
  std::uint64_t prior_statistics_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t record_generation = 0;
  drop::SblrOptimizerStatsDropUuidV1 durable_publication_uuid{};
  drop::SblrOptimizerStatsDropUuidV1 owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  drop::SblrOptimizerStatsDropSha256V1 previous_chain_sha256{};
  drop::SblrOptimizerStatsDropSha256V1 result_sha256{};
  drop::SblrOptimizerStatsDropSha256V1 record_evidence_sha256{};
  std::vector<std::uint8_t> canonical_result_bytes;
};

enum class TransactionVisibility {
  visible,
  invisible_terminal,
  hidden_future,
  unresolved_other,
  invalid,
};

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}

template <std::size_t N>
bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
void Put(std::vector<std::uint8_t>* bytes,
         const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

template <std::size_t N>
void Get(const std::uint8_t* bytes, std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes, N, value->begin());
}

void PutLe(std::vector<std::uint8_t>* bytes, std::uint64_t value,
           std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

std::uint64_t GetLe(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

drop::SblrOptimizerStatsDropSha256V1 Hash(
    const std::uint8_t* bytes, std::size_t size) {
  std::vector<std::uint8_t> material;
  if (size != 0) material.assign(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

drop::SblrOptimizerStatsDropSha256V1 Hash(
    const std::vector<std::uint8_t>& bytes) {
  return Hash(bytes.data(), bytes.size());
}

std::string UuidText(const drop::SblrOptimizerStatsDropUuidV1& value) {
  scratchbird::core::platform::Uuid uuid;
  uuid.bytes = value;
  return scratchbird::core::uuid::UuidToString(uuid);
}

drop::SblrOptimizerStatsDropUuidV1 UuidBytes(std::string_view value) {
  drop::SblrOptimizerStatsDropUuidV1 bytes{};
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(value));
  if (parsed.ok()) bytes = parsed.value.bytes;
  return bytes;
}

drop::SblrOptimizerStatsDropUuidV1 NewUuid() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (std::uint64_t attempt = 0; attempt < 32; ++attempt) {
    const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object,
        now + g_uuid_ordinal.fetch_add(1, std::memory_order_relaxed));
    if (generated.ok()) return generated.value.value.bytes;
  }
  return {};
}

std::string JournalPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.sblr_optimizer_stats_epoch.v1";
}

bool ContextValid(const EngineRequestContext& context) {
  return context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         context.statement_transaction_inventory_snapshot != nullptr &&
         !context.database_path.empty() && !context.database_uuid.canonical.empty() &&
         !context.transaction_uuid.canonical.empty() &&
         context.local_transaction_id != 0;
}

std::vector<std::uint8_t> EncodeRecord(const JournalRecord& record) {
  if (!NonZero(record.database_uuid) || !NonZero(record.effect_uuid) ||
      !NonZero(record.descriptor_sha256) ||
      record.prior_statistics_epoch == 0 ||
      record.prior_statistics_epoch == std::numeric_limits<std::uint64_t>::max() ||
      record.statistics_epoch != record.prior_statistics_epoch + 1 ||
      record.record_generation == 0 ||
      !NonZero(record.durable_publication_uuid) ||
      record.durable_publication_uuid == record.effect_uuid ||
      !NonZero(record.owning_transaction_uuid) ||
      record.owning_local_transaction_id == 0 ||
      record.canonical_result_bytes.size() !=
          drop::kSblrOptimizerStatsDropResultBytes) {
    return {};
  }
  const auto result_hash = Hash(record.canonical_result_bytes);
  if (NonZero(record.result_sha256) && record.result_sha256 != result_hash) {
    return {};
  }
  std::vector<std::uint8_t> bytes{'O', 'S', 'D', 'J'};
  PutLe(&bytes, 1, 2);
  PutLe(&bytes, kRecordBytes, 2);
  PutLe(&bytes, kRecordBytes, 4);
  PutLe(&bytes, 1, 4);
  Put(&bytes, record.database_uuid);
  Put(&bytes, record.effect_uuid);
  Put(&bytes, record.descriptor_sha256);
  PutLe(&bytes, record.prior_statistics_epoch, 8);
  PutLe(&bytes, record.statistics_epoch, 8);
  PutLe(&bytes, record.record_generation, 8);
  Put(&bytes, record.durable_publication_uuid);
  Put(&bytes, record.owning_transaction_uuid);
  PutLe(&bytes, record.owning_local_transaction_id, 8);
  Put(&bytes, record.previous_chain_sha256);
  Put(&bytes, result_hash);
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), record.canonical_result_bytes.begin(),
               record.canonical_result_bytes.end());
  bytes.insert(bytes.end(), 8, 0);
  std::vector<std::uint8_t> material(kRecordDomain.begin(),
                                     kRecordDomain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  const auto evidence = Hash(material);
  if (NonZero(record.record_evidence_sha256) &&
      record.record_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), bytes.begin() + 208);
  return bytes;
}

bool DecodeRecord(const std::uint8_t* bytes, std::size_t size,
                  JournalRecord* record) {
  if (bytes == nullptr || record == nullptr || size != kRecordBytes ||
      !std::equal(bytes, bytes + 4, "OSDJ") || GetLe(bytes + 4, 2) != 1 ||
      GetLe(bytes + 6, 2) != kRecordBytes ||
      GetLe(bytes + 8, 4) != kRecordBytes || GetLe(bytes + 12, 4) != 1 ||
      !Zero(bytes + 464, bytes + 472)) {
    return false;
  }
  JournalRecord value;
  Get(bytes + 16, &value.database_uuid);
  Get(bytes + 32, &value.effect_uuid);
  Get(bytes + 48, &value.descriptor_sha256);
  value.prior_statistics_epoch = GetLe(bytes + 80, 8);
  value.statistics_epoch = GetLe(bytes + 88, 8);
  value.record_generation = GetLe(bytes + 96, 8);
  Get(bytes + 104, &value.durable_publication_uuid);
  Get(bytes + 120, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 136, 8);
  Get(bytes + 144, &value.previous_chain_sha256);
  Get(bytes + 176, &value.result_sha256);
  Get(bytes + 208, &value.record_evidence_sha256);
  value.canonical_result_bytes.assign(bytes + 240, bytes + 464);
  if (value.canonical_result_bytes.size() !=
      drop::kSblrOptimizerStatsDropResultBytes) {
    return false;
  }
  auto material_bytes = std::vector<std::uint8_t>(bytes, bytes + size);
  std::fill(material_bytes.begin() + 208, material_bytes.begin() + 240, 0);
  std::vector<std::uint8_t> material(kRecordDomain.begin(),
                                     kRecordDomain.end());
  material.insert(material.end(), material_bytes.begin(),
                  material_bytes.end());
  if (value.record_evidence_sha256 != Hash(material) ||
      value.result_sha256 != Hash(value.canonical_result_bytes) ||
      EncodeRecord(value) != std::vector<std::uint8_t>(bytes, bytes + size)) {
    return false;
  }
  *record = std::move(value);
  return true;
}

// A record needs 240 bytes before its 224-byte result and retains eight
// canonical zero bytes for a future compatible minor extension.
static_assert(kRecordBytes == 472,
              "optimizer statistics journal layout extent drift");

struct LoadedJournal {
  bool ok = false;
  bool absent = false;
  std::vector<JournalRecord> records;
  drop::SblrOptimizerStatsDropSha256V1 last_chain{};
  std::string detail;
};

LoadedJournal DecodeJournal(const std::vector<std::uint8_t>& bytes,
                            const drop::SblrOptimizerStatsDropUuidV1&
                                database_uuid) {
  LoadedJournal loaded;
  if (bytes.empty()) {
    loaded.ok = true;
    loaded.absent = true;
    return loaded;
  }
  if (bytes.size() % kRecordBytes != 0 ||
      bytes.size() / kRecordBytes > kMaximumRecordCount) {
    loaded.detail = "journal_extent_invalid";
    return loaded;
  }
  drop::SblrOptimizerStatsDropSha256V1 prior_chain{};
  for (std::size_t offset = 0; offset < bytes.size();
       offset += kRecordBytes) {
    JournalRecord record;
    if (!DecodeRecord(bytes.data() + offset, kRecordBytes, &record) ||
        record.database_uuid != database_uuid ||
        record.record_generation != loaded.records.size() + 1 ||
        record.previous_chain_sha256 != prior_chain) {
      loaded.detail = "journal_record_invalid";
      return loaded;
    }
    prior_chain = record.record_evidence_sha256;
    loaded.records.push_back(std::move(record));
  }
  loaded.ok = true;
  loaded.last_chain = prior_chain;
  return loaded;
}

TransactionVisibility RecordVisibility(
    const EngineRequestContext& context, const JournalRecord& record) {
  const auto& snapshot = *context.statement_transaction_inventory_snapshot;
  const auto found = mga::LookupLocalTransaction(
      snapshot.inventory,
      mga::MakeLocalTransactionId(record.owning_local_transaction_id));
  if (!found.ok()) {
    return record.owning_local_transaction_id >=
                   snapshot.inventory.next_local_transaction_id
               ? TransactionVisibility::hidden_future
               : TransactionVisibility::invalid;
  }
  if (found.entry.identity.transaction_uuid.value.bytes !=
      record.owning_transaction_uuid) {
    return TransactionVisibility::invalid;
  }
  using State = mga::TransactionState;
  if (found.entry.state == State::committed ||
      found.entry.state == State::archived) {
    return TransactionVisibility::visible;
  }
  if (found.entry.state == State::rolled_back ||
      found.entry.state == State::failed_terminal) {
    return TransactionVisibility::invisible_terminal;
  }
  const bool own = record.owning_local_transaction_id ==
                       context.local_transaction_id &&
                   UuidText(record.owning_transaction_uuid) ==
                       context.transaction_uuid.canonical;
  if (own && (found.entry.state == State::active ||
              found.entry.state == State::preparing ||
              found.entry.state == State::prepared ||
              found.entry.state == State::committing)) {
    return TransactionVisibility::visible;
  }
  if (found.entry.state == State::created ||
      found.entry.state == State::active ||
      found.entry.state == State::preparing ||
      found.entry.state == State::prepared ||
      found.entry.state == State::committing ||
      found.entry.state == State::rolling_back ||
      found.entry.state == State::limbo ||
      found.entry.state == State::recovering ||
      found.entry.state == State::read_only_active) {
    return TransactionVisibility::unresolved_other;
  }
  return TransactionVisibility::invalid;
}

SblrOptimizerStatsEpochSnapshotV1 VisibleEpoch(
    const EngineRequestContext& context, const LoadedJournal& loaded) {
  SblrOptimizerStatsEpochSnapshotV1 result;
  result.statistics_epoch = kInitialStatisticsEpoch;
  result.journal_generation = loaded.records.size();
  result.journal_chain_sha256 = loaded.last_chain;
  if (!ContextValid(context) || !loaded.ok) {
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH", "sblr.optimizer_stats_drop.journal_invalid",
        loaded.detail.empty() ? "statement_authority_invalid" : loaded.detail);
    return result;
  }
  for (const auto& record : loaded.records) {
    switch (RecordVisibility(context, record)) {
      case TransactionVisibility::visible:
        if (record.prior_statistics_epoch != result.statistics_epoch ||
            record.statistics_epoch != result.statistics_epoch + 1) {
          result.diagnostic = Diagnostic(
              "MGA.AUTHORITY_MISMATCH",
              "sblr.optimizer_stats_drop.visible_epoch_fork",
              "visible_statistics_epoch_chain_conflict");
          return result;
        }
        result.statistics_epoch = record.statistics_epoch;
        break;
      case TransactionVisibility::unresolved_other:
        ++result.unresolved_other_transaction_count;
        break;
      case TransactionVisibility::hidden_future:
      case TransactionVisibility::invisible_terminal:
        break;
      case TransactionVisibility::invalid:
        result.diagnostic = Diagnostic(
            "MGA.AUTHORITY_MISMATCH",
            "sblr.optimizer_stats_drop.transaction_lineage_invalid",
            "journal_transaction_identity_invalid");
        return result;
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

bool ReadAll(int fd, std::vector<std::uint8_t>* bytes) {
#if defined(_WIN32)
  (void)fd;
  (void)bytes;
  return false;
#else
  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uint64_t>(metadata.st_size) >
          kRecordBytes * kMaximumRecordCount) {
    return false;
  }
  bytes->assign(static_cast<std::size_t>(metadata.st_size), 0);
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const auto count = ::pread(fd, bytes->data() + offset,
                               bytes->size() - offset,
                               static_cast<off_t>(offset));
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
#endif
}

bool AppendAll(int fd, const std::vector<std::uint8_t>& bytes,
               std::size_t offset) {
#if defined(_WIN32)
  (void)fd;
  (void)bytes;
  (void)offset;
  return false;
#else
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::pwrite(fd, bytes.data() + written,
                                bytes.size() - written,
                                static_cast<off_t>(offset + written));
    if (count <= 0) return false;
    written += static_cast<std::size_t>(count);
  }
  return ::fdatasync(fd) == 0;
#endif
}

bool InvalidateDerivedCache(std::uint64_t statistics_epoch,
                            const drop::SblrOptimizerStatsDropSha256V1&
                                descriptor_sha256) {
  optimizer::OptimizerStatisticsInvalidationRequest request;
  request.kind = optimizer::OptimizerStatisticsInvalidationKind::kStatsRefresh;
  request.authority.engine_runtime_scope = true;
  request.authority.optimizer_cache_owner = true;
  request.authority.analyze_generation_authority = true;
  request.stats_epoch = statistics_epoch;
  request.evidence_digest =
      "sha256:" + scratchbird::core::hash::HexLower(descriptor_sha256);
  request.reason = "engine.op.optimizer_stats_drop";
  const auto invalidated = optimizer::DispatchOptimizerStatisticsInvalidation(
      request, &optimizer::GlobalOptimizerPinnedStatsDescriptorCache());
  return invalidated.accepted;
}

}  // namespace

SblrOptimizerStatsEpochSnapshotV1 InspectSblrOptimizerStatsEpochV1(
    const EngineRequestContext& context) {
  std::lock_guard<std::mutex> process_guard(g_process_journal_mutex);
  if (!ContextValid(context)) {
    SblrOptimizerStatsEpochSnapshotV1 result;
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH", "sblr.optimizer_stats_drop.inspect_invalid",
        "statement_inventory_authority_required");
    return result;
  }
  const auto database_uuid = UuidBytes(context.database_uuid.canonical);
  if (!NonZero(database_uuid)) {
    SblrOptimizerStatsEpochSnapshotV1 result;
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH", "sblr.optimizer_stats_drop.inspect_invalid",
        "database_uuid_invalid");
    return result;
  }
#if defined(_WIN32)
  std::ifstream input(JournalPath(context), std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if (input) {
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
  }
#else
  const int fd = ::open(JournalPath(context).c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT) {
    LoadedJournal empty;
    empty.ok = true;
    empty.absent = true;
    return VisibleEpoch(context, empty);
  }
  if (fd < 0 || ::flock(fd, LOCK_SH) != 0) {
    if (fd >= 0) ::close(fd);
    SblrOptimizerStatsEpochSnapshotV1 result;
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_read_failed",
        "journal_open_or_lock_failed");
    return result;
  }
  std::vector<std::uint8_t> bytes;
  const bool read_ok = ReadAll(fd, &bytes);
  (void)::flock(fd, LOCK_UN);
  (void)::close(fd);
  if (!read_ok) {
    SblrOptimizerStatsEpochSnapshotV1 result;
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_read_failed",
        "journal_read_failed");
    return result;
  }
#endif
  return VisibleEpoch(context, DecodeJournal(bytes, database_uuid));
}

SblrOptimizerStatsDropPublicationV1 PublishSblrOptimizerStatsDropV1(
    const EngineRequestContext& context,
    const drop::SblrOptimizerStatsDropDescriptorV1& descriptor) {
  SblrOptimizerStatsDropPublicationV1 result;
  const auto descriptor_bytes = drop::EncodeSblrOptimizerStatsDropDescriptorV1(
      descriptor);
  if (!ContextValid(context) || descriptor_bytes.empty()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND.INVALID", "sblr.optimizer_stats_drop.publish_invalid",
        "canonical_descriptor_and_statement_authority_required");
    return result;
  }
  const auto database_uuid = UuidBytes(context.database_uuid.canonical);
  if (!NonZero(database_uuid) ||
      descriptor.owning_transaction_uuid !=
          UuidBytes(context.transaction_uuid.canonical) ||
      descriptor.owning_local_transaction_id != context.local_transaction_id ||
      descriptor.inventory_generation !=
          context.statement_transaction_inventory_snapshot->inventory
              .next_local_transaction_id) {
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.optimizer_stats_drop.publish_authority_mismatch",
        "database_transaction_or_inventory_identity_mismatch");
    return result;
  }

  // The inventory publication journal is database-wide.  Its quick identity
  // may change because another transaction begins, commits, or rolls back;
  // that is a revalidation signal, not proof that this statement's exact
  // transaction authority is stale.  Serialize with transaction publication,
  // retain the cheap unchanged path, and pay for a new strong snapshot only
  // after the global fence changes.  The descriptor remains bound to the
  // original statement inventory generation while visibility below consumes
  // the newly authenticated immutable snapshot.
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  EngineRequestContext publication_context = context;
  const auto fence = storage_db::RevalidateLocalTransactionInventorySnapshot(
      *context.statement_transaction_inventory_snapshot);
  if (!fence.ok()) {
    const auto current =
        storage_db::AcquireStrongLocalTransactionInventorySnapshot(
            context.database_path);
    if (!current.ok()) {
      result.diagnostic = Diagnostic(
          "MGA.AUTHORITY_MISMATCH",
          "sblr.optimizer_stats_drop.inventory_revalidation_failed",
          current.diagnostic.diagnostic_code.empty()
              ? fence.diagnostic.remediation_hint
              : current.diagnostic.diagnostic_code);
      return result;
    }
    publication_context.statement_transaction_inventory_snapshot =
        current.snapshot;
  }
  const auto exact_transaction = mga::LookupLocalTransaction(
      publication_context.statement_transaction_inventory_snapshot->inventory,
      mga::MakeLocalTransactionId(descriptor.owning_local_transaction_id));
  if (!exact_transaction.ok() ||
      exact_transaction.entry.state != mga::TransactionState::active) {
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION_INVALID",
        "sblr.optimizer_stats_drop.transaction_not_active",
        "exact_owning_transaction_is_not_active");
    return result;
  }
  if (!exact_transaction.entry.identity.transaction_uuid.valid() ||
      exact_transaction.entry.identity.transaction_uuid.value.bytes !=
          descriptor.owning_transaction_uuid) {
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.optimizer_stats_drop.transaction_identity_changed",
        "exact_owning_transaction_uuid_mismatch");
    return result;
  }

  std::lock_guard<std::mutex> process_guard(g_process_journal_mutex);
#if defined(_WIN32)
  result.diagnostic = Diagnostic(
      "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_write_failed",
      "windows_durable_append_not_implemented");
  return result;
#else
  const std::string path = JournalPath(context);
  const bool existed = std::filesystem::exists(path);
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
  if (fd < 0 || ::flock(fd, LOCK_EX) != 0) {
    if (fd >= 0) ::close(fd);
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_write_failed",
        "journal_open_or_lock_failed");
    return result;
  }
  std::vector<std::uint8_t> bytes;
  if (!ReadAll(fd, &bytes)) {
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_write_failed",
        "journal_read_before_append_failed");
    return result;
  }
  const auto loaded = DecodeJournal(bytes, database_uuid);
  if (!loaded.ok) {
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.optimizer_stats_drop.journal_corrupt", loaded.detail);
    return result;
  }
  for (const auto& record : loaded.records) {
    if (record.effect_uuid != descriptor.effect_uuid) continue;
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    if (record.descriptor_sha256 != descriptor.descriptor_sha256 ||
        record.owning_transaction_uuid != descriptor.owning_transaction_uuid ||
        record.owning_local_transaction_id !=
            descriptor.owning_local_transaction_id) {
      result.diagnostic = Diagnostic(
          "MGA.AUTHORITY_MISMATCH",
          "sblr.optimizer_stats_drop.effect_identity_collision",
          "effect_uuid_reused_with_different_authority");
      return result;
    }
    if (!InvalidateDerivedCache(record.statistics_epoch,
                                record.descriptor_sha256)) {
      result.diagnostic = Diagnostic(
          "SBLR.EXECUTION_FAILED",
          "sblr.optimizer_stats_drop.cache_invalidation_failed",
          "exact_replay_cache_invalidation_failed");
      return result;
    }
    result.ok = true;
    result.exact_replay = true;
    result.statistics_epoch = record.statistics_epoch;
    result.journal_generation = record.record_generation;
    result.canonical_result_bytes = record.canonical_result_bytes;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto visible = VisibleEpoch(publication_context, loaded);
  if (!visible.ok || visible.unresolved_other_transaction_count != 0 ||
      visible.statistics_epoch != descriptor.expected_statistics_epoch ||
      visible.journal_generation != descriptor.expected_journal_generation ||
      descriptor.proposed_effect_generation != loaded.records.size() + 1 ||
      descriptor.next_statistics_epoch != visible.statistics_epoch + 1) {
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.optimizer_stats_drop.publication_fence_changed",
        visible.unresolved_other_transaction_count != 0
            ? "concurrent_statistics_writer_present"
            : "statistics_epoch_or_journal_generation_changed");
    return result;
  }
  JournalRecord record;
  record.database_uuid = database_uuid;
  record.effect_uuid = descriptor.effect_uuid;
  record.descriptor_sha256 = descriptor.descriptor_sha256;
  record.prior_statistics_epoch = descriptor.expected_statistics_epoch;
  record.statistics_epoch = descriptor.next_statistics_epoch;
  record.record_generation = descriptor.proposed_effect_generation;
  record.durable_publication_uuid = NewUuid();
  record.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  record.owning_local_transaction_id =
      descriptor.owning_local_transaction_id;
  record.previous_chain_sha256 = loaded.last_chain;
  drop::SblrOptimizerStatsDropResultV1 publication;
  publication.effect_uuid = descriptor.effect_uuid;
  publication.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  publication.durable_publication_uuid = record.durable_publication_uuid;
  publication.prior_statistics_epoch = descriptor.expected_statistics_epoch;
  publication.statistics_epoch = descriptor.next_statistics_epoch;
  publication.effect_generation = descriptor.proposed_effect_generation;
  publication.catalog_generation = descriptor.catalog_generation;
  publication.security_epoch = descriptor.security_epoch;
  publication.resource_epoch = descriptor.resource_epoch;
  publication.inventory_generation = descriptor.inventory_generation;
  publication.cache_invalidation_generation = descriptor.next_statistics_epoch;
  publication.flags = descriptor.flags;
  publication.status = drop::kSblrOptimizerStatsDropPublishedStatus;
  publication.executor_availability_generation =
      descriptor.executor_availability_generation;
  publication.publication_barrier_generation =
      descriptor.proposed_effect_generation;
  record.canonical_result_bytes =
      drop::EncodeSblrOptimizerStatsDropResultV1(publication);
  record.result_sha256 = Hash(record.canonical_result_bytes);
  const auto encoded = EncodeRecord(record);
  if (!NonZero(record.durable_publication_uuid) ||
      record.canonical_result_bytes.empty() || encoded.empty() ||
      !AppendAll(fd, encoded, bytes.size())) {
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED", "sblr.optimizer_stats_drop.journal_write_failed",
        "durable_epoch_publication_failed");
    return result;
  }
  (void)::flock(fd, LOCK_UN);
  (void)::close(fd);
  if (!existed) {
    const std::filesystem::path parent =
        std::filesystem::path(path).parent_path();
    const int parent_fd = ::open(parent.empty() ? "." : parent.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd >= 0) {
      (void)::fsync(parent_fd);
      (void)::close(parent_fd);
    }
  }
  if (!InvalidateDerivedCache(record.statistics_epoch,
                              record.descriptor_sha256)) {
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED",
        "sblr.optimizer_stats_drop.cache_invalidation_failed",
        "durable_epoch_published_cache_invalidation_recovery_required");
    return result;
  }
  result.ok = true;
  result.statistics_epoch = record.statistics_epoch;
  result.journal_generation = record.record_generation;
  result.canonical_result_bytes = record.canonical_result_bytes;
  result.diagnostic = OkDiagnostic();
  return result;
#endif
}

}  // namespace scratchbird::engine::internal_api
