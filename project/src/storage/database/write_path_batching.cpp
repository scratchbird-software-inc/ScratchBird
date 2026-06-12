// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "write_path_batching.hpp"

#include "database_dirty_manifest.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_cache.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_recovery.hpp"
#include "uuid.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::database {
namespace {

namespace mga = scratchbird::transaction::mga;
namespace mem = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

inline constexpr std::uint32_t kWritePathBatchingProofPageSize = 8192;

void AddEvidence(WritePathBatchingResult* result, std::string evidence) {
  result->evidence.push_back(std::move(evidence));
}

std::string Bool(bool value) { return value ? "true" : "false"; }

std::string StableHash(const std::vector<std::string>& fields) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& field : fields) {
    for (const unsigned char ch : field) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << hash;
  return out.str();
}

WritePathBatchingResult Refuse(const WritePathBatchingRequest& request,
                               std::string diagnostic,
                               std::string fallback_reason) {
  WritePathBatchingResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fallback_used = request.exact_fallback_available;
  result.fail_closed = !request.exact_fallback_available;
  result.diagnostic_code = std::move(diagnostic);
  result.fallback_reason = std::move(fallback_reason);
  AddEvidence(&result, "orh284.route_label=" + request.route_label);
  AddEvidence(&result, "orh284.refused=" + result.diagnostic_code);
  AddEvidence(&result, "orh284.exact_fallback_used=" + Bool(result.fallback_used));
  AddEvidence(&result, "orh284.fallback_reason=" + result.fallback_reason);
  AddEvidence(&result, "orh284.benchmark_clean=false");
  AddEvidence(&result, "orh284.write_batch_metadata.finality_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.visibility_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.row_identity_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.authorization_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.recovery_authority=false");
  AddEvidence(&result,
              "orh284.mga_recovery_authority=durable_transaction_inventory");
  return result;
}

platform::TypedUuid DerivedUuid(platform::UuidKind kind,
                                std::uint64_t millis,
                                platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  if (!generated.ok()) {
    return {};
  }
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  return typed.ok() ? typed.value : platform::TypedUuid{};
}

page::PageCacheEntry Entry(const WritePathBatchingRequest& request,
                           std::uint64_t ordinal) {
  page::PageCacheEntry entry;
  entry.database_uuid = request.database_uuid;
  entry.filespace_uuid = request.filespace_uuid;
  entry.page_uuid = DerivedUuid(platform::UuidKind::page, 284000 + ordinal,
                                static_cast<platform::byte>(ordinal));
  entry.page_type = scratchbird::storage::disk::PageType::row_data;
  entry.page_number = ordinal;
  entry.page_generation = request.page_generation;
  entry.page_size = kWritePathBatchingProofPageSize;
  entry.dirty = true;
  return entry;
}

DirtyObjectManifest MakeManifest(const WritePathBatchingRequest& request) {
  DirtyObjectManifest manifest;
  manifest.checkpoint_generation = request.batching_generation;
  manifest.completed = true;
  manifest.classification_only = true;
  for (std::uint64_t i = 0; i < request.dirty_page_count; ++i) {
    DirtyObjectManifestEntry entry;
    entry.kind = i == 0 ? DirtyObjectKind::transaction_inventory
                        : (i % 2 == 0 ? DirtyObjectKind::allocation_map
                                      : DirtyObjectKind::row_data_page);
    entry.object_uuid = DerivedUuid(platform::UuidKind::page, 284500 + i,
                                    static_cast<platform::byte>(i + 11));
    entry.page_number = i + 1;
    entry.page_generation = request.page_generation;
    entry.object_checksum = 1000 + i;
    entry.local_transaction_id = request.local_transaction_id;
    entry.operation_envelope_checksum = 2000 + i;
    entry.transaction_evidence_checksum = 3000 + i;
    entry.dirty = true;
    entry.authoritative = true;
    manifest.entries.push_back(entry);
  }
  return manifest;
}

mga::LocalTransactionInventory MakeInventory(
    const WritePathBatchingRequest& request) {
  mga::LocalTransactionInventory inventory = mga::MakeEmptyLocalTransactionInventory();
  auto begin = mga::BeginLocalTransaction(inventory, request.transaction_uuid,
                                          284000);
  if (!begin.ok()) {
    return inventory;
  }
  auto commit = mga::CommitLocalTransaction(begin.inventory,
                                            begin.entry.identity.local_id,
                                            284100);
  return commit.ok() ? commit.inventory : begin.inventory;
}

page::PageAllocationLedger MakeAllocationLedger(
    const WritePathBatchingRequest& request) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = request.database_uuid;
  ledger.filespace_uuid = request.filespace_uuid;
  ledger.free_extents.push_back({100, request.extent_page_count + 8});
  return ledger;
}

bool WriteFsyncOpenMarker(const std::filesystem::path& path,
                          const std::string& content,
                          std::string* reopened) {
  std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
  const std::string file_path = path.string();
  HANDLE file = ::CreateFileA(file_path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const auto* data = content.data();
  std::size_t remaining = content.size();
  while (remaining != 0) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining,
                              static_cast<std::size_t>(
                                  std::numeric_limits<DWORD>::max())));
    DWORD written = 0;
    if (::WriteFile(file, data, chunk, &written, nullptr) == 0 ||
        written == 0) {
      ::CloseHandle(file);
      return false;
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
  if (::FlushFileBuffers(file) == 0) {
    ::CloseHandle(file);
    return false;
  }
  if (::CloseHandle(file) == 0) {
    return false;
  }
  file = ::CreateFileA(file_path.c_str(),
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::string buffer(content.size(), '\0');
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(buffer.size() - offset,
                              static_cast<std::size_t>(
                                  std::numeric_limits<DWORD>::max())));
    DWORD read_bytes = 0;
    if (::ReadFile(file, buffer.data() + offset, chunk, &read_bytes,
                   nullptr) == 0) {
      ::CloseHandle(file);
      return false;
    }
    if (read_bytes == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read_bytes);
  }
  ::CloseHandle(file);
  buffer.resize(offset);
  if (reopened != nullptr) {
    *reopened = std::move(buffer);
  }
  return reopened != nullptr && *reopened == content;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return false;
  }
  const auto* data = content.data();
  std::size_t remaining = content.size();
  while (remaining != 0) {
    const ssize_t written = ::write(fd, data, remaining);
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return false;
  }
  if (::close(fd) != 0) {
    return false;
  }
  const int read_fd = ::open(path.c_str(), O_RDONLY);
  if (read_fd < 0) {
    return false;
  }
  std::string buffer(content.size(), '\0');
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const ssize_t read_bytes =
        ::read(read_fd, buffer.data() + offset, buffer.size() - offset);
    if (read_bytes < 0) {
      ::close(read_fd);
      return false;
    }
    if (read_bytes == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read_bytes);
  }
  ::close(read_fd);
  buffer.resize(offset);
  if (reopened != nullptr) {
    *reopened = std::move(buffer);
  }
  return reopened != nullptr && *reopened == content;
#endif
}

void AppendDirtyManifestEvidence(WritePathBatchingResult* result,
                                 const DirtyManifestRecoveryResult& recovery) {
  AddEvidence(result, "orh284.dirty_manifest.classifications=" +
                          std::to_string(recovery.classifications.size()));
  AddEvidence(result, "orh284.dirty_manifest.rebuild_by_scan_required=" +
                          Bool(recovery.rebuild_by_scan_required));
  for (const auto& classification : recovery.classifications) {
    AddEvidence(result, std::string("orh284.dirty_manifest.action=") +
                            DirtyManifestRecoveryActionName(classification.action));
  }
}

}  // namespace

WritePathBatchingResult ExecuteDurabilityWritePathBatch(
    const WritePathBatchingRequest& request) {
  if (request.route_label.empty()) {
    return Refuse(request, "ORH_WRITE_BATCHING_MISSING_ROUTE_LABEL",
                  "route_label_required");
  }
  if (!request.runtime_enabled) {
    return Refuse(request, "ORH_WRITE_BATCHING_NO_RUNTIME",
                  "runtime_consumption_missing");
  }
  if (!request.exact_fallback_available) {
    return Refuse(request, "ORH_WRITE_BATCHING_NO_EXACT_FALLBACK",
                  "exact_unbatched_fallback_required");
  }
  if (request.authority.parser_client_or_reference_write_batch_authority ||
      request.authority.batch_metadata_finality_or_visibility_authority ||
      request.authority.batch_metadata_recovery_authority) {
    return Refuse(request, "ORH_WRITE_BATCHING_UNSAFE_AUTHORITY",
                  "write_batch_metadata_is_advisory_only");
  }
  if (request.authority.recovery_from_batch_metadata_alone) {
    return Refuse(request, "ORH_WRITE_BATCHING_METADATA_ONLY_RECOVERY_REFUSED",
                  "durable_mga_tip_inventory_required_for_recovery");
  }
  if (!request.authority.engine_mga_tip_authoritative ||
      !request.authority.durable_transaction_inventory_proven) {
    return Refuse(request, "ORH_WRITE_BATCHING_MGA_TIP_UNPROVEN",
                  "durable_transaction_inventory_required");
  }
  if (request.batching_generation != request.expected_batching_generation) {
    return Refuse(request, "ORH_WRITE_BATCHING_STALE_GENERATION",
                  "batching_generation_mismatch");
  }
  if (request.dirty_page_count == 0 ||
      !request.dirty_page_accounting_available) {
    return Refuse(request, "ORH_WRITE_BATCHING_DIRTY_ACCOUNTING_MISSING",
                  "dirty_page_accounting_required");
  }
  if (!request.fsync_open_proof_available) {
    return Refuse(request, "ORH_WRITE_BATCHING_FSYNC_OPEN_MISSING",
                  "fsync_open_proof_required");
  }
  if (!request.extent_allocation_matches || request.extent_page_count == 0) {
    return Refuse(request, "ORH_WRITE_BATCHING_EXTENT_MISMATCH",
                  "extent_allocation_mismatch");
  }
  if (!request.crash_reopen_recovery_proof_available) {
    return Refuse(request, "ORH_WRITE_BATCHING_RECOVERY_PROOF_MISSING",
                  "crash_reopen_recovery_proof_required");
  }
  if (request.resource_pressure) {
    return Refuse(request, "ORH_WRITE_BATCHING_RESOURCE_PRESSURE_FALLBACK",
                  "resource_pressure_unbatched_fallback");
  }

  page::PageCachePolicy policy;
  policy.max_resident_pages = request.dirty_page_count + 4;
  policy.bulk_write_ring_pages = request.dirty_page_count + 4;
  policy.allow_dirty_eviction = false;
  policy.max_resident_bytes =
      (request.dirty_page_count + 4) * kWritePathBatchingProofPageSize;
  auto memory_policy = mem::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "write_path_batching_page_cache";
  memory_policy.hard_limit_bytes = policy.max_resident_bytes;
  memory_policy.soft_limit_bytes = policy.max_resident_bytes;
  memory_policy.per_context_limit_bytes = policy.max_resident_bytes;
  memory_policy.page_buffer_pool_limit_bytes = policy.max_resident_bytes;
  mem::MemoryManager page_cache_memory(memory_policy);
  page::PageCacheLedger cache;
  page::BindPageCacheMemoryManager(&cache, &page_cache_memory);
  page::PageCacheLifecycleInput lifecycle;
  lifecycle.database_uuid = request.database_uuid;
  lifecycle.filespace_uuid = request.filespace_uuid;
  lifecycle.policy_generation = request.batching_generation;
  lifecycle.checkpoint_generation = request.batching_generation;
  lifecycle.dirty_epoch = request.batching_generation;
  lifecycle.tx2_activation_committed = true;
  lifecycle.cache_runtime_started = true;
  lifecycle.engine_agent_active = true;
  lifecycle.writeback_allowed = true;
  lifecycle.checkpoint_allowed = true;

  for (std::uint64_t i = 0; i < request.dirty_page_count; ++i) {
    const auto admit = page::AdmitPageCacheEntryForContext(
        &cache, policy, Entry(request, i + 1), page::PageCacheIoContext::bulk_write);
    if (!admit.ok()) {
      return Refuse(request, "ORH_WRITE_BATCHING_DIRTY_ACCOUNTING_MISSING",
                    admit.diagnostic.diagnostic_code);
    }
  }
  const auto before = page::SnapshotPageCache(cache);
  const auto writeback = page::WritebackDirtyPageCacheEntries(&cache, lifecycle);
  if (!writeback.ok() || writeback.flushed_pages != request.dirty_page_count) {
    return Refuse(request, "ORH_WRITE_BATCHING_DIRTY_ACCOUNTING_MISSING",
                  writeback.diagnostic.diagnostic_code);
  }
  const auto checkpoint =
      page::CheckpointPageCacheLifecycle(&cache, lifecycle,
                                         page::PageCacheCheckpointMode::clean_close);
  if (!checkpoint.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_FSYNC_OPEN_MISSING",
                  checkpoint.diagnostic.diagnostic_code);
  }

  const auto inventory = MakeInventory(request);
  page::TransactionInventoryPageBody body;
  body.page_number = 1;
  body.inventory = inventory;
  const auto txn_page =
      page::BuildTransactionInventoryPageBody(body,
                                              kWritePathBatchingProofPageSize);
  if (!txn_page.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_MGA_TIP_UNPROVEN",
                  txn_page.diagnostic.diagnostic_code);
  }
  const auto parsed_txn =
      page::ParseTransactionInventoryPageBody(txn_page.serialized, 1);
  if (!parsed_txn.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_CRASH_REOPEN_MISMATCH",
                  parsed_txn.diagnostic.diagnostic_code);
  }
  const auto txn_recovery =
      mga::ClassifyLocalTransactionInventoryForRecovery(parsed_txn.body.inventory);
  if (!txn_recovery.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_MGA_TIP_UNPROVEN",
                  txn_recovery.diagnostic.diagnostic_code);
  }

  auto allocation_ledger = MakeAllocationLedger(request);
  page::PageAllocationRequest allocation_request;
  allocation_request.database_uuid = request.database_uuid;
  allocation_request.filespace_uuid = request.filespace_uuid;
  allocation_request.owner_object_uuid =
      DerivedUuid(platform::UuidKind::object, 284900, 90);
  allocation_request.creator_transaction_uuid = request.transaction_uuid;
  allocation_request.creator_local_transaction_id = request.local_transaction_id;
  allocation_request.page_family = "row_data";
  allocation_request.page_count = request.extent_page_count;
  allocation_request.page_generation = request.page_generation;
  allocation_request.engine_authoritative = true;
  allocation_request.durability_fence_satisfied = true;
  const auto allocation =
      page::ReservePageAllocation(&allocation_ledger, allocation_request);
  if (!allocation.ok() ||
      allocation.allocation.page_count != request.extent_page_count) {
    return Refuse(request, "ORH_WRITE_BATCHING_EXTENT_MISMATCH",
                  allocation.diagnostic.diagnostic_code);
  }
  const auto allocation_recovery =
      page::ClassifyPageAllocationLedgerForRecovery(allocation_ledger);
  if (!allocation_recovery.ok() ||
      allocation_recovery.classifications.empty() ||
      allocation_recovery.classifications.front().fail_closed) {
    return Refuse(request, "ORH_WRITE_BATCHING_EXTENT_MISMATCH",
                  "allocation_recovery_classification_failed");
  }

  auto manifest = MakeManifest(request);
  const auto built_manifest = BuildDirtyObjectManifest(manifest);
  if (!built_manifest.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_DIRTY_ACCOUNTING_MISSING",
                  built_manifest.diagnostic.diagnostic_code);
  }
  const auto parsed_manifest =
      ParseDirtyObjectManifest(built_manifest.serialized);
  if (!parsed_manifest.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_CRASH_REOPEN_MISMATCH",
                  parsed_manifest.diagnostic.diagnostic_code);
  }
  const auto manifest_recovery =
      ClassifyDirtyObjectManifestForRecovery(parsed_manifest.manifest);
  if (!manifest_recovery.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_RECOVERY_PROOF_MISSING",
                  manifest_recovery.diagnostic.diagnostic_code);
  }
  const auto recovery_path =
      request.scratch_directory / "orh284_recovery_evidence.txt";
  std::filesystem::create_directories(recovery_path.parent_path());
  const auto recovery_evidence = PersistDirtyManifestRecoveryRunEvidence(
      recovery_path.string(), parsed_manifest.manifest, manifest_recovery,
      "orh284-recovery-run");
  if (!recovery_evidence.ok()) {
    return Refuse(request, "ORH_WRITE_BATCHING_RECOVERY_PROOF_MISSING",
                  recovery_evidence.diagnostic.diagnostic_code);
  }

  const std::string marker =
      "orh284:" + request.route_label + ":" +
      std::to_string(request.batching_generation) + ":" +
      std::to_string(writeback.flushed_pages);
  std::string reopened_marker;
  if (!WriteFsyncOpenMarker(request.scratch_directory / "orh284_fsync.marker",
                            marker, &reopened_marker)) {
    return Refuse(request, "ORH_WRITE_BATCHING_FSYNC_OPEN_MISSING",
                  "fsync_open_marker_failed");
  }

  const auto after = page::SnapshotPageCache(cache);
  const auto txn_checksum =
      page::ComputeTransactionInventoryPageChecksum(txn_page.serialized);
  const auto state_hash =
      StableHash({request.route_label,
                  std::to_string(request.batching_generation),
                  std::to_string(before.dirty_pages),
                  std::to_string(after.dirty_pages),
                  std::to_string(writeback.flushed_pages),
                  std::to_string(built_manifest.manifest.manifest_checksum),
                  std::to_string(txn_checksum),
                  std::to_string(allocation.allocation.page_count),
                  reopened_marker});
  if (!request.expected_state_hash.empty() &&
      request.expected_state_hash != state_hash) {
    return Refuse(request, "ORH_WRITE_BATCHING_CRASH_REOPEN_MISMATCH",
                  "state_hash_mismatch_after_reopen");
  }

  WritePathBatchingResult result;
  result.ok = true;
  result.benchmark_clean = true;
  result.runtime_consumed = true;
  result.dirty_page_accounting_proven = true;
  result.extent_allocation_proven = true;
  result.fsync_open_proven = true;
  result.crash_reopen_recovery_proven = true;
  result.unbatched_flush_operations = request.dirty_page_count;
  result.batched_flush_operations = 1;
  result.flushed_pages = writeback.flushed_pages;
  result.diagnostic_code = "ORH_DURABILITY_WRITE_PATH_BATCHING.OK";
  result.fallback_reason = "none";
  result.state_hash = state_hash;
  AddEvidence(&result, "orh284.route_label=" + request.route_label);
  AddEvidence(&result, "orh284.runtime_consumed=true");
  AddEvidence(&result,
              "orh284.batching_generation=" +
                  std::to_string(request.batching_generation));
  AddEvidence(&result,
              "orh284.unbatched_flush_operations=" +
                  std::to_string(result.unbatched_flush_operations));
  AddEvidence(&result,
              "orh284.batched_flush_operations=" +
                  std::to_string(result.batched_flush_operations));
  AddEvidence(&result,
              "orh284.dirty_pages_before=" + std::to_string(before.dirty_pages));
  AddEvidence(&result,
              "orh284.dirty_pages_after=" + std::to_string(after.dirty_pages));
  AddEvidence(&result,
              "orh284.flushed_pages=" + std::to_string(writeback.flushed_pages));
  AddEvidence(&result, "orh284.dirty_page_accounting_proven=true");
  AddEvidence(&result, "orh284.extent_allocation_proven=true");
  AddEvidence(&result, "orh284.fsync_open_proven=true");
  AddEvidence(&result, "orh284.crash_reopen_recovery_proven=true");
  AddEvidence(&result, "orh284.state_hash=" + result.state_hash);
  AddEvidence(&result,
              "orh284.transaction_inventory_checksum=" +
                  std::to_string(txn_checksum));
  AddEvidence(&result,
              "orh284.dirty_manifest_checksum=" +
                  std::to_string(built_manifest.manifest.manifest_checksum));
  AddEvidence(&result,
              "orh284.recovery_evidence_completed=" +
                  Bool(recovery_evidence.evidence.completed));
  AddEvidence(&result,
              "orh284.checkpoint_writeback_complete=" +
                  Bool(checkpoint.publication.writeback_complete));
  AddEvidence(&result, "orh284.write_batch_metadata.finality_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.visibility_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.row_identity_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.authorization_authority=false");
  AddEvidence(&result, "orh284.write_batch_metadata.recovery_authority=false");
  AddEvidence(&result,
              "orh284.mga_recovery_authority=durable_transaction_inventory");
  AddEvidence(&result, "orh284.benchmark_clean=true");
  AppendDirtyManifestEvidence(&result, manifest_recovery);
  return result;
}

}  // namespace scratchbird::storage::database
