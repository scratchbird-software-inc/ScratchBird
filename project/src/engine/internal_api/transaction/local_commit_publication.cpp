// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction/local_commit_publication.hpp"
#include "transaction/local_commit_publication_codec.hpp"
#include "engine/authority_hash_material.hpp"

#include "dml/transactional_index_provider.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include "whole_store_crash_injection.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace core_hash = scratchbird::core::hash;
using scratchbird::core::platform::byte;
using scratchbird::storage::disk::SyncFilesystemPath;
using scratchbird::storage::disk::SyncParentDirectoryPath;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;



EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic Refuse(std::string detail) {
  return MakeEngineApiDiagnostic(
      "SB_DIAG_MGA_PD_COMMIT_PAGE_BARRIER_BLOCKED",
      "mga.page_durability.commit_page_barrier_blocked",
      std::move(detail),
      true);
}

std::string Sha256(std::string_view bytes) {
  const auto digest = core_hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(bytes.data()), bytes.size());
  return digest.ok() ? core_hash::HexLower(digest.digest) : std::string{};
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string ArtifactPostcondition(const std::filesystem::path& path,
                                  std::uint64_t size) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  const auto digest = core_hash::ComputeSha256Stream(input, size);
  return digest.ok() ? core_hash::HexLower(digest.digest) : std::string{};
}

std::string ManifestPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_transaction_publication." +
         std::to_string(context.local_transaction_id) + ".v2";
}

bool IsPublicationManifest(const std::string& filename,
                           const std::string& database_filename) {
  return filename.rfind(database_filename + ".sb.mga_transaction_publication.", 0) == 0;
}

std::string DomainForArtifact(const std::string& identity) {
  if (identity == "database_file") return "physical_page_allocation_overflow";
  if (identity.find("row_versions") != std::string::npos ||
      identity.find(".rows") != std::string::npos) return "row_version";
  if (identity.find("index") != std::string::npos) return "index";
  if (identity.find("metadata") != std::string::npos ||
      identity.find("descriptor") != std::string::npos) return "catalog";
  if (identity.find("large_value") != std::string::npos) return "overflow";
  if (identity.find("event_sequence_allocator") != std::string::npos) return "allocation";
  if (identity.find("trigger") != std::string::npos) return "trigger_side_effect";
  return "auxiliary";
}

std::vector<std::filesystem::path> PublicationArtifacts(
    const EngineRequestContext& context,
    bool* scan_ok) {
  if (scan_ok != nullptr) *scan_ok = false;
  const std::filesystem::path database(context.database_path);
  std::vector<std::filesystem::path> paths;
  std::error_code ec;
  if (std::filesystem::is_regular_file(database, ec)) paths.push_back(database);
  if (ec) return {};

  const auto parent = database.parent_path().empty()
                          ? std::filesystem::path(".")
                          : database.parent_path();
  const std::string database_filename = database.filename().string();
  for (std::filesystem::directory_iterator it(parent, ec), end;
       !ec && it != end; it.increment(ec)) {
    const auto filename = it->path().filename().string();
    if (filename.rfind(database_filename + ".sb.", 0) != 0 ||
        filename == database_filename + ".sb.txn_publish" ||
        filename.find(".tmp.") != std::string::npos ||
        IsPublicationManifest(filename, database_filename)) {
      continue;
    }
    std::error_code type_error;
    if (it->is_regular_file(type_error)) {
      paths.push_back(it->path());
    } else if (!type_error && it->is_directory(type_error) &&
               filename == database_filename + ".sb.mga_relation_scope") {
      for (std::filesystem::recursive_directory_iterator nested(it->path(), type_error), nested_end;
           !type_error && nested != nested_end; nested.increment(type_error)) {
        std::error_code nested_type_error;
        if (nested->is_regular_file(nested_type_error)) paths.push_back(nested->path());
        if (nested_type_error) return {};
      }
    }
    if (type_error) return {};
  }
  if (ec) return {};
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  if (scan_ok != nullptr) *scan_ok = true;
  return paths;
}

std::string ArtifactIdentity(const EngineRequestContext& context,
                             const std::filesystem::path& path) {
  const std::filesystem::path database(context.database_path);
  if (path == database) return "database_file";
  const auto parent = database.parent_path().empty()
                          ? std::filesystem::path(".")
                          : database.parent_path();
  std::error_code ec;
  const auto relative = std::filesystem::relative(path, parent, ec);
  return ec ? path.filename().string() : relative.generic_string();
}

std::string PairMaterial(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::ostringstream material;
  for (const auto& [key, value] : pairs) {
    material << key.size() << ':' << key << value.size() << ':' << value << ';';
  }
  return material.str();
}

// Typed digest material. A UUID occupies exactly sixteen binary bytes and
// cannot alias a text field or a different tuple of values.
std::string NativeMaterial(std::initializer_list<AuthorityHashField> fields) {
  std::string bytes = "SBMGAM02";
  AppendBinaryU32(&bytes, static_cast<std::uint32_t>(fields.size()));
  for (const auto& field : fields) {
    if (const auto* identity = std::get_if<EngineUuid>(&field)) {
      bytes.push_back(2);
      bytes.append(reinterpret_cast<const char*>(identity->bytes.data()), 16);
    } else {
      bytes.push_back(1);
      if (!AppendBinaryString(&bytes, std::get<std::string_view>(field))) return {};
    }
  }
  return bytes;
}

LocalCommitPublicationMutation Mutation(
    std::string domain,
    std::string kind,
    EngineUuid object,
    EngineUuid record,
    std::string physical,
    std::uint64_t generation_before,
    std::uint64_t generation_after,
    std::string precondition,
    std::string postcondition,
    const EngineRequestContext& context,
    EngineUuid version = {}) {
  LocalCommitPublicationMutation mutation;
  mutation.mutation_domain = std::move(domain);
  mutation.mutation_kind = std::move(kind);
  mutation.object_identity = std::move(object);
  mutation.record_identity = record;
  mutation.version_identity = version;
  mutation.physical_identity = std::move(physical);
  mutation.generation_before = generation_before;
  mutation.generation_after = generation_after;
  mutation.precondition_sha256 = Sha256(precondition);
  mutation.postcondition_sha256 = Sha256(postcondition);
  const std::string identity_material = NativeMaterial({
      std::string_view("SBMGA_MUTATION_ID_V2"), context.transaction_uuid,
      std::to_string(context.local_transaction_id), mutation.mutation_domain,
      mutation.mutation_kind, mutation.object_identity, mutation.record_identity,
      mutation.version_identity, std::to_string(mutation.generation_after)});
  mutation.mutation_identity = Sha256(identity_material);
  mutation.idempotency_key = Sha256(NativeMaterial({std::string_view("SBMGA_IDEMPOTENCY_V2"), identity_material}));
  return mutation;
}

std::vector<LocalCommitPublicationMutation> TransactionMutations(
    const EngineRequestContext& context,
    const MgaRelationStoreState& state,
    const scratchbird::core::index::PersistentSecondaryIndexDeltaLedger& ledger) {
  std::vector<LocalCommitPublicationMutation> mutations;
  const auto transaction_id = context.local_transaction_id;
  for (const auto& table : state.relation_metadata.tables) {
    if (table.creator_tx != transaction_id) continue;
    const std::string postcondition = NativeMaterial({table.table_uuid,
        std::to_string(table.event_sequence), PairMaterial(table.columns)});
    mutations.push_back(Mutation(
        "catalog", "table_metadata_publish", table.table_uuid,
        table.table_uuid, "mga_relation_metadata", 0, table.event_sequence,
        "absent", postcondition, context));
  }
  for (const auto& index : state.relation_metadata.indexes) {
    if (index.creator_tx != transaction_id) continue;
    const std::string postcondition = NativeMaterial({index.index_uuid, index.table_uuid,
        std::to_string(index.event_sequence)});
    mutations.push_back(Mutation(
        "catalog", "index_metadata_publish", index.table_uuid,
        index.index_uuid, "mga_relation_metadata", 0, index.event_sequence,
        "absent", postcondition, context));
  }
  for (const auto& descriptor :
       state.relation_metadata.sealed_relation_descriptor_snapshots) {
    if (descriptor.creator_tx != transaction_id) continue;
    const std::string postcondition = NativeMaterial({descriptor.relation_uuid,
        descriptor.relation_descriptor_uuid, std::to_string(descriptor.relation_descriptor_generation),
        PairMaterial(descriptor.descriptor_fields)});
    mutations.push_back(Mutation(
        "catalog", "relation_descriptor_publish", descriptor.relation_uuid,
        descriptor.relation_descriptor_uuid, "mga_relation_descriptors", 0,
        descriptor.relation_descriptor_generation, "absent", postcondition,
        context));
  }
  for (const auto& row : state.row_versions) {
    if (row.creator_tx != transaction_id) continue;
    const std::string kind = row.deleted
                                 ? "delete_row_version"
                                 : (row.previous_version_uuid.is_nil()
                                        ? "insert_row_version"
                                        : "update_row_version");
    const std::string precondition = row.previous_version_uuid.is_nil() ? "absent" :
        NativeMaterial({row.previous_version_uuid, std::to_string(row.previous_sequence)});
    const std::string postcondition = NativeMaterial({row.table_uuid, row.row_uuid,
        row.version_uuid, std::to_string(row.sequence),
        std::string_view(row.deleted ? "deleted" : "live"), PairMaterial(row.values)});
    mutations.push_back(Mutation(
        "row_version", kind, row.table_uuid, row.row_uuid,
        "database_page_or_mga_row_segment", row.previous_sequence,
        row.sequence == 0 ? row.event_sequence : row.sequence, precondition,
        postcondition, context, row.version_uuid));
  }
  for (const auto& entry : state.index_entries) {
    if (entry.creator_tx != transaction_id) continue;
    const std::string postcondition = NativeMaterial({entry.index_uuid, entry.table_uuid,
        entry.row_uuid, entry.version_uuid, entry.key_value, entry.payload_value});
    mutations.push_back(Mutation(
        "index", entry.entry_kind.empty() ? "index_entry_publish" : entry.entry_kind,
        entry.index_uuid, entry.row_uuid,
        "database_page_or_mga_index_segment", 0,
        entry.sequence == 0 ? entry.event_sequence : entry.sequence, "absent",
        postcondition, context, entry.version_uuid));
  }
  for (const auto& record : ledger.records) {
    if (record.delta.local_transaction_id != transaction_id) continue;
    const auto delta_uuid = record.delta.delta_id.value;
    const auto index_uuid = record.delta.index_uuid.value;
    const auto row_uuid = record.delta.row_uuid.value;
    mutations.push_back(Mutation(
        "index", "secondary_index_delta", index_uuid, delta_uuid,
        "mga_secondary_index_delta_ledger", 0, 1, "absent",
        NativeMaterial({index_uuid, row_uuid, record.delta.key_payload, std::string_view("precommit_uncommitted")}),
        context));
  }
  std::sort(mutations.begin(), mutations.end(),
            [](const auto& left, const auto& right) {
              return left.mutation_identity < right.mutation_identity;
            });
  return mutations;
}

}  // namespace

const char* LocalCommitPublicationRecoveryClassName(
    LocalCommitPublicationRecoveryClass recovery_class) {
  switch (recovery_class) {
    case LocalCommitPublicationRecoveryClass::retryable_unpublished:
      return "retryable_unpublished";
    case LocalCommitPublicationRecoveryClass::committed_by_inventory:
      return "committed_by_inventory";
    case LocalCommitPublicationRecoveryClass::abandoned_by_rollback:
      return "abandoned_by_rollback";
    case LocalCommitPublicationRecoveryClass::in_doubt:
      return "in_doubt";
    case LocalCommitPublicationRecoveryClass::corrupt_manifest:
      return "corrupt_manifest";
  }
  return "corrupt_manifest";
}

LocalCommitPublicationResult RunLocalCommitPageBarrier(
    const EngineRequestContext& context) {
  LocalCommitPublicationResult result;
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.is_nil()) {
    result.diagnostic = Refuse("transaction_identity_required");
    return result;
  }

  // Freeze and validate the final logical write set before touching inventory
  // finality.  The canonical relation-store loader validates row chains,
  // catalog records, scoped segments, large-value locators, and transaction
  // ownership while the transaction is still active.
  const auto state = LoadMgaRelationStoreState(context);
  if (!state.ok) {
    result.diagnostic = Refuse("transaction_write_set_unclassifiable:" +
                               state.diagnostic.code);
    return result;
  }

  // Every registry-admitted native provider must prove that each transaction-
  // owned row mutation has the matching insert/retire index-version bytes
  // before durable inventory finality can be published.  This is validation
  // of candidate-access state, never a second finality decision.
  const auto index_validation =
      ValidateTransactionalIndexMutationSetForCommit(
          context, BuildMgaRelationReadView(state.state));
  if (!index_validation.ok) {
    result.diagnostic = Refuse(
        "transactional_index_mutation_set_incomplete:" +
        index_validation.diagnostic.code + ":" +
        index_validation.diagnostic.detail);
    return result;
  }

  // The delta ledger is decoded before any finality mutation.  A malformed or
  // torn auxiliary artifact therefore blocks the barrier while the transaction
  // remains active.  Its precommit state is intentional: recovery and readers
  // classify it through inventory finality, never through this derived flag.
  const auto ledger = LoadMgaSecondaryIndexDeltaLedger(context);
  if (!ledger.ok) {
    result.diagnostic = Refuse("secondary_index_delta_ledger_unclassifiable:" +
                               ledger.diagnostic.code);
    return result;
  }
  result.mutations = TransactionMutations(context, state.state, ledger.ledger);
  for (const auto& mutation : result.mutations) {
    if (mutation.mutation_identity.empty() || mutation.idempotency_key.empty() ||
        mutation.precondition_sha256.empty() ||
        mutation.postcondition_sha256.empty()) {
      result.diagnostic = Refuse("transaction_mutation_descriptor_incomplete");
      return result;
    }
  }

  bool artifact_scan_ok = false;
  const auto paths = PublicationArtifacts(context, &artifact_scan_ok);
  if (!artifact_scan_ok) {
    result.diagnostic = Refuse("durability_domain_scan_failed");
    return result;
  }
  if (paths.empty()) {
    result.diagnostic = Refuse("database_durability_domain_missing");
    return result;
  }
  for (const auto& path : paths) {
    LocalCommitPublicationArtifact artifact;
    artifact.artifact_identity = ArtifactIdentity(context, path);
    artifact.mutation_domain = DomainForArtifact(artifact.artifact_identity);
    const auto synced = SyncFilesystemPath(path.string(), true);
    if (!synced.ok()) {
      result.diagnostic = Refuse("artifact_sync_failed:" + path.string() + ":" +
                                 synced.diagnostic.diagnostic_code);
      return result;
    }
    if (artifact.mutation_domain == "row_version") {
      scratchbird::core::platform::MaybeCrashAtWholeStoreRealDmlBoundary(
          "page_sync");
    } else if (artifact.mutation_domain == "index") {
      scratchbird::core::platform::MaybeCrashAtWholeStoreRealDmlBoundary(
          "index_sync");
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      result.diagnostic = Refuse("artifact_size_unavailable:" + path.string());
      return result;
    }
    artifact.durable_size_bytes = static_cast<std::uint64_t>(size);
    artifact.postcondition_sha256 = ArtifactPostcondition(
        path, static_cast<std::uint64_t>(size));
    if (artifact.postcondition_sha256.empty()) {
      result.diagnostic = Refuse("artifact_postcondition_hash_failed:" + path.string());
      return result;
    }
    result.artifacts.push_back(std::move(artifact));
  }

  result.publication_generation = context.local_transaction_id;
  std::string manifest;
  if (!local_publication_codec::Encode(context, result.mutations, result.artifacts, &manifest)) {
    result.diagnostic = Refuse("manifest_binary_encoding_failed");
    return result;
  }
  result.manifest_sha256 = manifest.substr(manifest.size() - 64);
  result.manifest_path = ManifestPath(context);
  const std::string temporary = result.manifest_path + ".tmp." +
                                std::to_string(context.local_transaction_id);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      result.diagnostic = Refuse("manifest_temporary_open_failed");
      return result;
    }
    output.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
    output.close();
    if (!output) {
      result.diagnostic = Refuse("manifest_temporary_write_failed");
      return result;
    }
  }
  const auto temp_sync = SyncFilesystemPath(temporary, true);
  if (!temp_sync.ok()) {
    result.diagnostic = Refuse("manifest_temporary_sync_failed");
    return result;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, result.manifest_path, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(result.manifest_path, remove_ec);
    ec.clear();
    std::filesystem::rename(temporary, result.manifest_path, ec);
  }
  if (ec) {
    result.diagnostic = Refuse("manifest_atomic_publish_failed:" + ec.message());
    return result;
  }
  const auto parent_sync = SyncParentDirectoryPath(result.manifest_path);
  if (!parent_sync.ok()) {
    result.diagnostic = Refuse("manifest_parent_sync_failed");
    return result;
  }
  scratchbird::core::platform::MaybeCrashAtWholeStoreRealDmlBoundary(
      "mutation_manifest_publication");

  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

LocalCommitPublicationRecoveryResult ClassifyLocalCommitPublicationForRecovery(
    const EngineRequestContext& context,
    const scratchbird::transaction::mga::LocalTransactionInventory& inventory) {
  LocalCommitPublicationRecoveryResult result;
  const std::string manifest_path = ManifestPath(context);
  const std::string encoded = ReadFile(manifest_path);
  if (encoded.empty()) {
    result.diagnostic = Refuse("publication_manifest_missing_or_empty");
    result.stable_reason = "durable publication manifest is missing or empty";
    return result;
  }
  if (!local_publication_codec::Decode(encoded, context, &result)) {
    result.diagnostic = Refuse("publication_manifest_binary_invalid");
    result.stable_reason = "manifest identity, framing, or checksum is invalid";
    return result;
  }

  const auto transaction = LookupLocalTransaction(
      inventory, MakeLocalTransactionId(context.local_transaction_id));
  if (!transaction.ok()) {
    result.recovery_class = LocalCommitPublicationRecoveryClass::in_doubt;
    result.diagnostic = Refuse("publication_inventory_identity_missing");
    result.stable_reason = "manifest has no matching durable transaction inventory entry";
    return result;
  }
  result.recovery_class = ClassifyLocalCommitPublicationInventoryOutcome(transaction.entry);
  switch (result.recovery_class) {
    case LocalCommitPublicationRecoveryClass::committed_by_inventory:
      result.stable_reason = "inventory finality commits every manifested artifact";
      break;
    case LocalCommitPublicationRecoveryClass::abandoned_by_rollback:
      result.stable_reason = "inventory finality abandons every manifested artifact";
      break;
    case LocalCommitPublicationRecoveryClass::retryable_unpublished:
      result.stable_reason = "transaction remains open and the publication barrier may be retried";
      break;
    default:
      result.stable_reason = "inventory requires prepared, failed or ambiguous outcome review";
      break;
  }
  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

}  // namespace scratchbird::engine::internal_api
