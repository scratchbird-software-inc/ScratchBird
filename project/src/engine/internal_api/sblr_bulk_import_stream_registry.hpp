#pragma once

#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr std::uint64_t kBulkImportStreamMaximumBytesV1 = 17179869184ull;
inline constexpr std::uint64_t kBulkImportStreamMaximumChunksV1 = 262144ull;
inline constexpr std::uint32_t kBulkImportStreamMaximumChunkBytesV1 = 8388608u;
inline constexpr std::uint64_t kBulkImportStreamMaximumRowsV1 = 1048576ull;

enum class BulkImportStreamState : std::uint8_t {
  allocated = 0,
  receiving = 1,
  sealed = 2,
  executing = 3,
  published = 4,
  evidenced = 5,
  result_recorded = 6,
  refused = 7,
  aborted = 8,
};

enum class BulkImportStreamAbortReason : std::uint32_t {
  none = 0,
  chunk_conflict = 1,
  seal_conflict = 2,
  corrupt_journal = 3,
  corrupt_spool = 4,
  recovery_conflict = 5,
  execution_aborted = 6,
};

struct BulkImportStreamAllocation {
  engine::sblr::BulkImportUuid authenticated_receipt_uuid{};
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  std::uint64_t structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  engine::sblr::BulkImportSha descriptor_evidence{};
  engine::sblr::BulkImportSha authority_evidence_sha256{};
  engine::sblr::BulkImportSha syntax_demand_sha256{};
  engine::sblr::BulkImportUuid durable_spool_uuid{};
  std::uint64_t durable_spool_generation = 0;
  engine::sblr::BulkImportUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  engine::sblr::BulkImportUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  engine::sblr::BulkImportUuid statement_snapshot_uuid{};
  engine::sblr::BulkImportUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  engine::sblr::BulkImportUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  engine::sblr::BulkImportUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  engine::sblr::BulkImportUuid route_snapshot_uuid{};
  std::uint64_t route_generation = 0;
  engine::sblr::BulkImportUuid recovery_operation_uuid{};
  std::uint64_t recovery_generation = 0;
  engine::sblr::BulkImportUuid row_shape_uuid{};
  std::uint64_t row_shape_generation = 0;
  engine::sblr::BulkImportSha column_descriptor_set_sha256{};
  engine::sblr::BulkImportSha import_policy_bundle_sha256{};
  engine::sblr::BulkImportUuid resource_grant_uuid{};
  std::uint64_t resource_grant_generation = 0;
  bool cluster_bound = false;
  std::uint64_t cluster_epoch = 0;
  engine::sblr::BulkImportUuid cluster_fence_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t effective_maximum_stream_bytes = kBulkImportStreamMaximumBytesV1;
  std::uint64_t effective_maximum_chunk_count = kBulkImportStreamMaximumChunksV1;
  std::uint32_t effective_maximum_chunk_bytes = kBulkImportStreamMaximumChunkBytesV1;
  std::uint64_t effective_maximum_rows = kBulkImportStreamMaximumRowsV1;
  std::uint32_t effective_maximum_target_columns = 65535u;
};

struct BulkImportPublicationRecord {
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  engine::sblr::BulkImportSha recovery_key_sha256{};
  engine::sblr::BulkImportUuid durable_publication_uuid{};
  std::uint64_t durable_publication_generation = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t rejected_rows = 0;
  engine::sblr::BulkImportSha postcondition_evidence_sha256{};
};

struct BulkImportExecutorEvidenceRecord {
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  engine::sblr::BulkImportUuid durable_publication_uuid{};
  std::uint64_t durable_publication_generation = 0;
  std::uint64_t executor_availability_generation = 0;
  engine::sblr::BulkImportSha executor_evidence_sha256{};
};

struct BulkImportAcceptedChunk {
  std::uint64_t sequence = 0;
  std::uint64_t byte_offset = 0;
  std::uint32_t payload_bytes = 0;
  engine::sblr::BulkImportSha previous_chain_sha{};
  engine::sblr::BulkImportSha payload_sha{};
  engine::sblr::BulkImportSha chain_sha{};
  std::vector<std::uint8_t> ack_wire;
};

struct BulkImportSealSnapshot {
  bool present = false;
  std::uint64_t final_chunk_count = 0;
  std::uint64_t total_stream_bytes = 0;
  engine::sblr::BulkImportSha final_chain_sha{};
  engine::sblr::BulkImportSha content_sha{};
  engine::sblr::BulkImportSha request_evidence{};
  std::vector<std::uint8_t> ack_wire;
};

struct BulkImportStreamEntry {
  BulkImportStreamAllocation allocation;
  BulkImportStreamState state = BulkImportStreamState::allocated;
  std::uint64_t received_chunks = 0;
  std::uint64_t received_bytes = 0;
  engine::sblr::BulkImportSha chain_sha{};
  engine::sblr::BulkImportSha content_sha{};
  std::vector<BulkImportAcceptedChunk> accepted_chunks;
  BulkImportSealSnapshot seal;
  engine::sblr::BulkImportSha recovery_key_sha256{};
  BulkImportPublicationRecord publication;
  BulkImportExecutorEvidenceRecord executor_evidence;
  BulkImportStreamAbortReason abort_reason = BulkImportStreamAbortReason::none;
  std::vector<std::uint8_t> result_wire;
  std::uint64_t journal_records = 0;
  std::uint64_t journal_bytes = 0;
};

struct BulkImportChunk {
  engine::sblr::BulkImportUuid authenticated_receipt_uuid{};
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  std::uint64_t structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  engine::sblr::BulkImportSha descriptor_evidence{};
  std::uint64_t sequence = 0;
  std::uint64_t byte_offset = 0;
  engine::sblr::BulkImportSha previous_chain_sha{};
  engine::sblr::BulkImportSha payload_sha{};
  engine::sblr::BulkImportSha chain_sha{};
  std::vector<std::uint8_t> payload;
};

struct BulkImportSeal {
  engine::sblr::BulkImportUuid authenticated_receipt_uuid{};
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  engine::sblr::BulkImportSha descriptor_evidence{};
  std::uint64_t final_chunk_count = 0;
  std::uint64_t total_stream_bytes = 0;
  engine::sblr::BulkImportSha final_chain_sha{};
  engine::sblr::BulkImportSha content_sha{};
  engine::sblr::BulkImportSha seal_request_evidence{};
};

struct BulkImportStreamRegistryResult {
  bool ok = false;
  BulkImportStreamState state = BulkImportStreamState::allocated;
  std::string error;
  std::vector<std::uint8_t> response_wire;
  bool replayed = false;
  bool durable = false;
  BulkImportStreamAllocation allocation;
};
using BulkImportAllocationFactory = std::function<bool(BulkImportStreamAllocation*)>;

struct BulkImportSealedSpoolSnapshot {
  engine::sblr::BulkImportUuid stream_uuid{};
  std::uint64_t stream_generation = 0;
  engine::sblr::BulkImportUuid authenticated_receipt_uuid{};
  engine::sblr::BulkImportSha descriptor_evidence{};
  std::uint64_t total_stream_bytes = 0;
  std::uint64_t final_chunk_count = 0;
  engine::sblr::BulkImportSha final_chain_sha{};
  engine::sblr::BulkImportSha content_sha{};
};

using BulkImportSpoolReader =
    std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>;

class SblrBulkImportStreamRegistry {
 public:
  explicit SblrBulkImportStreamRegistry(std::filesystem::path root);
  ~SblrBulkImportStreamRegistry();

  SblrBulkImportStreamRegistry(const SblrBulkImportStreamRegistry&) = delete;
  SblrBulkImportStreamRegistry& operator=(const SblrBulkImportStreamRegistry&) = delete;

  bool healthy() const;
  const std::string& startup_error() const;

  BulkImportStreamRegistryResult Allocate(const BulkImportStreamAllocation&);
  BulkImportStreamRegistryResult AllocateOrReplay(const engine::sblr::BulkImportUuid&,
                                                  std::uint64_t, std::uint32_t,
                                                  const engine::sblr::BulkImportSha&,
                                                  const BulkImportAllocationFactory&);
  BulkImportStreamRegistryResult Append(const BulkImportChunk&);
  BulkImportStreamRegistryResult Seal(const BulkImportSeal&);
  BulkImportStreamRegistryResult BeginExecution(const engine::sblr::BulkImportUuid&,
                                                std::uint64_t);
  BulkImportStreamRegistryResult Publish(const BulkImportPublicationRecord&);
  BulkImportStreamRegistryResult RecordEvidence(const BulkImportExecutorEvidenceRecord&);
  BulkImportStreamRegistryResult RecordResult(const engine::sblr::BulkImportUuid&,
                                              std::uint64_t,
                                              std::vector<std::uint8_t>);
  BulkImportStreamRegistryResult AbortBeforePublication(
      const engine::sblr::BulkImportUuid&, std::uint64_t,
      BulkImportStreamAbortReason, std::string);
  BulkImportStreamRegistryResult Recover(const engine::sblr::BulkImportUuid&,
                                         BulkImportStreamEntry*) const;
  BulkImportStreamRegistryResult ReadSealedSpool(
      const engine::sblr::BulkImportUuid&, std::uint64_t,
      const engine::sblr::BulkImportUuid&,
      const engine::sblr::BulkImportSha&, const BulkImportSpoolReader&,
      BulkImportSealedSpoolSnapshot*) const;

 private:
  enum class LoadOutcome : std::uint8_t { loaded, absent, corrupt, io_error };

  std::filesystem::path root_;
  int root_fd_ = -1;
  int lock_fd_ = -1;
  bool healthy_ = false;
  std::string startup_error_;
  mutable bool recovery_required_ = false;
  mutable std::uint64_t temporary_ordinal_ = 0;
  // Recursive ownership permits AllocateOrReplay to invoke the existing
  // durable allocator while retaining the registry lock across the
  // absence-check and factory validation.
  mutable std::recursive_mutex mutex_;
  mutable std::map<engine::sblr::BulkImportUuid, BulkImportStreamEntry> entries_;

  std::string BaseName(const engine::sblr::BulkImportUuid&) const;
  std::string MetaName(const engine::sblr::BulkImportUuid&) const;
  std::string SpoolName(const engine::sblr::BulkImportUuid&) const;
  std::string JournalName(const engine::sblr::BulkImportUuid&) const;
  bool SafeRegularFile(const std::string&, bool allow_absent, std::uint64_t*) const;
  bool Save(const BulkImportStreamEntry&) const;
  LoadOutcome Load(const engine::sblr::BulkImportUuid&,
                   BulkImportStreamEntry*,
                   std::string*) const;
  bool AppendJournal(BulkImportStreamEntry*,
                     std::uint8_t record_type,
                     const std::vector<std::uint8_t>&,
                     std::string*) const;
  BulkImportStreamRegistryResult EnsureLoaded(const engine::sblr::BulkImportUuid&,
                                              BulkImportStreamEntry**) const;
  BulkImportStreamRegistryResult Abort(BulkImportStreamEntry*,
                                       BulkImportStreamAbortReason,
                                       std::string);
};

}  // namespace scratchbird::engine::internal_api
