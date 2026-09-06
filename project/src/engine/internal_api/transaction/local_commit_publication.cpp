// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction/local_commit_publication.hpp"

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
using scratchbird::transaction::mga::TransactionState;

constexpr std::string_view kManifestMagic = "SBMGA_LOCAL_COMMIT_PUBLICATION_V1";

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
  constexpr std::size_t kProbeBytes = 4096;
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  const auto head_size = static_cast<std::size_t>(
      std::min<std::uint64_t>(size, kProbeBytes));
  std::string material = "SBMGA_ARTIFACT_FENCE_V1\t" + std::to_string(size) + "\t";
  std::string probe(head_size, '\0');
  input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
  if (static_cast<std::size_t>(input.gcount()) != probe.size()) return {};
  material.append(probe);
  if (size > kProbeBytes) {
    const auto tail_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(size - kProbeBytes, kProbeBytes));
    probe.assign(tail_size, '\0');
    input.clear();
    input.seekg(static_cast<std::streamoff>(size - tail_size), std::ios::beg);
    input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
    if (static_cast<std::size_t>(input.gcount()) != probe.size()) return {};
    material.append(probe);
  }
  return Sha256(material);
}

std::string EncodeField(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(ch >> 4) & 0xf]);
      encoded.push_back(kHex[ch & 0xf]);
    }
  }
  return encoded;
}

int Hex(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

bool IsSha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(),
                     [](const char ch) { return Hex(ch) >= 0; });
}

bool DecodeField(std::string_view value, std::string* decoded) {
  if (decoded == nullptr) return false;
  decoded->clear();
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      decoded->push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size()) return false;
    const int high = Hex(value[i + 1]);
    const int low = Hex(value[i + 2]);
    if (high < 0 || low < 0) return false;
    decoded->push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const auto end = line.find('\t', begin);
    if (end == std::string::npos) {
      fields.push_back(line.substr(begin));
      break;
    }
    fields.push_back(line.substr(begin, end - begin));
    begin = end + 1;
  }
  return fields;
}

std::string ManifestPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_transaction_publication." +
         std::to_string(context.local_transaction_id) + ".v1";
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

bool ParseU64(std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

std::string PairMaterial(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::ostringstream material;
  for (const auto& [key, value] : pairs) {
    material << key.size() << ':' << key << value.size() << ':' << value << ';';
  }
  return material.str();
}

LocalCommitPublicationMutation Mutation(
    std::string domain,
    std::string kind,
    std::string object,
    std::string record,
    std::string physical,
    std::uint64_t generation_before,
    std::uint64_t generation_after,
    std::string precondition,
    std::string postcondition,
    const EngineRequestContext& context) {
  LocalCommitPublicationMutation mutation;
  mutation.mutation_domain = std::move(domain);
  mutation.mutation_kind = std::move(kind);
  mutation.object_identity = std::move(object);
  mutation.record_identity = std::move(record);
  mutation.physical_identity = std::move(physical);
  mutation.generation_before = generation_before;
  mutation.generation_after = generation_after;
  mutation.precondition_sha256 = Sha256(precondition);
  mutation.postcondition_sha256 = Sha256(postcondition);
  const std::string identity_material =
      "SBMGA_MUTATION_ID_V1\t" + context.transaction_uuid.canonical + "\t" +
      std::to_string(context.local_transaction_id) + "\t" +
      mutation.mutation_domain + "\t" + mutation.mutation_kind + "\t" +
      mutation.object_identity + "\t" + mutation.record_identity + "\t" +
      std::to_string(mutation.generation_after);
  mutation.mutation_identity = Sha256(identity_material);
  mutation.idempotency_key = Sha256("SBMGA_IDEMPOTENCY_V1\t" + identity_material);
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
    const std::string postcondition = table.table_uuid + "\t" +
                                      std::to_string(table.event_sequence) + "\t" +
                                      PairMaterial(table.columns);
    mutations.push_back(Mutation(
        "catalog", "table_metadata_publish", table.table_uuid,
        table.table_uuid, "mga_relation_metadata", 0, table.event_sequence,
        "absent", postcondition, context));
  }
  for (const auto& index : state.relation_metadata.indexes) {
    if (index.creator_tx != transaction_id) continue;
    const std::string postcondition = index.index_uuid + "\t" + index.table_uuid +
                                      "\t" + std::to_string(index.event_sequence);
    mutations.push_back(Mutation(
        "catalog", "index_metadata_publish", index.table_uuid,
        index.index_uuid, "mga_relation_metadata", 0, index.event_sequence,
        "absent", postcondition, context));
  }
  for (const auto& descriptor :
       state.relation_metadata.sealed_relation_descriptor_snapshots) {
    if (descriptor.creator_tx != transaction_id) continue;
    const std::string postcondition = descriptor.relation_uuid + "\t" +
        descriptor.relation_descriptor_uuid + "\t" +
        std::to_string(descriptor.relation_descriptor_generation) + "\t" +
        PairMaterial(descriptor.descriptor_fields);
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
                                 : (row.previous_version_uuid.empty()
                                        ? "insert_row_version"
                                        : "update_row_version");
    const std::string precondition = row.previous_version_uuid.empty()
                                         ? "absent"
                                         : row.previous_version_uuid + "\t" +
                                               std::to_string(row.previous_sequence);
    const std::string postcondition = row.table_uuid + "\t" + row.row_uuid +
        "\t" + row.version_uuid + "\t" + std::to_string(row.sequence) +
        "\t" + (row.deleted ? "deleted" : "live") + "\t" +
        PairMaterial(row.values);
    mutations.push_back(Mutation(
        "row_version", kind, row.table_uuid, row.row_uuid + ":" + row.version_uuid,
        "database_page_or_mga_row_segment", row.previous_sequence,
        row.sequence == 0 ? row.event_sequence : row.sequence, precondition,
        postcondition, context));
  }
  for (const auto& entry : state.index_entries) {
    if (entry.creator_tx != transaction_id) continue;
    const std::string postcondition = entry.index_uuid + "\t" + entry.table_uuid +
        "\t" + entry.row_uuid + "\t" + entry.version_uuid + "\t" +
        entry.key_value + "\t" + entry.payload_value;
    mutations.push_back(Mutation(
        "index", entry.entry_kind.empty() ? "index_entry_publish" : entry.entry_kind,
        entry.index_uuid, entry.row_uuid + ":" + entry.version_uuid,
        "database_page_or_mga_index_segment", 0,
        entry.sequence == 0 ? entry.event_sequence : entry.sequence, "absent",
        postcondition, context));
  }
  for (const auto& record : ledger.records) {
    if (record.delta.local_transaction_id != transaction_id) continue;
    const std::string delta_uuid = scratchbird::core::uuid::UuidToString(
        record.delta.delta_id.value);
    const std::string index_uuid = scratchbird::core::uuid::UuidToString(
        record.delta.index_uuid.value);
    const std::string row_uuid = scratchbird::core::uuid::UuidToString(
        record.delta.row_uuid.value);
    mutations.push_back(Mutation(
        "index", "secondary_index_delta", index_uuid, delta_uuid,
        "mga_secondary_index_delta_ledger", 0, 1, "absent",
        index_uuid + "\t" + row_uuid + "\t" + record.delta.key_payload +
            "\tprecommit_uncommitted",
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
      context.transaction_uuid.canonical.empty()) {
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
  std::ostringstream body;
  body << kManifestMagic << '\t' << result.publication_generation << '\t'
       << EncodeField(context.transaction_uuid.canonical) << '\t'
       << result.mutations.size() << '\t' << result.artifacts.size() << '\n';
  for (const auto& mutation : result.mutations) {
    body << "MUTATION\t" << mutation.mutation_identity << '\t'
         << EncodeField(mutation.mutation_domain) << '\t'
         << EncodeField(mutation.mutation_kind) << '\t'
         << EncodeField(mutation.object_identity) << '\t'
         << EncodeField(mutation.record_identity) << '\t'
         << EncodeField(mutation.physical_identity) << '\t'
         << mutation.generation_before << '\t' << mutation.generation_after << '\t'
         << mutation.idempotency_key << '\t' << mutation.precondition_sha256 << '\t'
         << mutation.postcondition_sha256 << '\t'
         << EncodeField(mutation.finality_authority) << '\t'
         << EncodeField(mutation.lifecycle_state) << '\n';
  }
  for (const auto& artifact : result.artifacts) {
    body << "ARTIFACT\t" << EncodeField(artifact.mutation_domain) << '\t'
         << EncodeField(artifact.artifact_identity) << '\t'
         << artifact.durable_size_bytes << '\t'
         << artifact.postcondition_sha256 << '\n';
  }
  const std::string body_bytes = body.str();
  result.manifest_sha256 = Sha256(body_bytes);
  if (result.manifest_sha256.empty()) {
    result.diagnostic = Refuse("manifest_hash_failed");
    return result;
  }
  const std::string manifest = body_bytes + "SEAL\t" + result.manifest_sha256 + "\n";
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
  std::istringstream input(encoded);
  std::string line;
  std::string body;
  std::string transaction_uuid;
  std::uint64_t declared_mutations = 0;
  std::uint64_t declared_artifacts = 0;
  if (!std::getline(input, line)) {
    result.diagnostic = Refuse("publication_manifest_header_missing");
    return result;
  }
  body += line + "\n";
  const auto header = SplitTabs(line);
  if (header.size() != 5 || header[0] != kManifestMagic ||
      !ParseU64(header[1], &result.publication_generation) ||
      !DecodeField(header[2], &transaction_uuid) ||
      !ParseU64(header[3], &declared_mutations) ||
      !ParseU64(header[4], &declared_artifacts) ||
      result.publication_generation != context.local_transaction_id ||
      transaction_uuid != context.transaction_uuid.canonical) {
    result.diagnostic = Refuse("publication_manifest_header_invalid");
    result.stable_reason = "manifest identity or generation does not match the transaction";
    return result;
  }
  std::string seal;
  while (std::getline(input, line)) {
    const auto fields = SplitTabs(line);
    if (fields.size() == 2 && fields[0] == "SEAL") {
      seal = fields[1];
      break;
    }
    if (fields.size() == 14 && fields[0] == "MUTATION") {
      LocalCommitPublicationMutation mutation;
      mutation.mutation_identity = fields[1];
      if (!IsSha256(fields[1]) ||
          !DecodeField(fields[2], &mutation.mutation_domain) ||
          !DecodeField(fields[3], &mutation.mutation_kind) ||
          !DecodeField(fields[4], &mutation.object_identity) ||
          !DecodeField(fields[5], &mutation.record_identity) ||
          !DecodeField(fields[6], &mutation.physical_identity) ||
          !ParseU64(fields[7], &mutation.generation_before) ||
          !ParseU64(fields[8], &mutation.generation_after) ||
          !IsSha256(fields[9]) || !IsSha256(fields[10]) ||
          !IsSha256(fields[11]) ||
          !DecodeField(fields[12], &mutation.finality_authority) ||
          !DecodeField(fields[13], &mutation.lifecycle_state) ||
          mutation.finality_authority != "durable_transaction_inventory" ||
          mutation.lifecycle_state != "commit_publish_ready") {
        result.diagnostic = Refuse("publication_manifest_mutation_invalid");
        return result;
      }
      mutation.idempotency_key = fields[9];
      mutation.precondition_sha256 = fields[10];
      mutation.postcondition_sha256 = fields[11];
      result.mutations.push_back(std::move(mutation));
      body += line + "\n";
      continue;
    }
    if (fields.size() != 5 || fields[0] != "ARTIFACT") {
      result.diagnostic = Refuse("publication_manifest_artifact_invalid");
      return result;
    }
    LocalCommitPublicationArtifact artifact;
    if (!DecodeField(fields[1], &artifact.mutation_domain) ||
        !DecodeField(fields[2], &artifact.artifact_identity) ||
        !ParseU64(fields[3], &artifact.durable_size_bytes) ||
        !IsSha256(fields[4])) {
      result.diagnostic = Refuse("publication_manifest_artifact_invalid");
      return result;
    }
    artifact.postcondition_sha256 = fields[4];
    result.artifacts.push_back(std::move(artifact));
    body += line + "\n";
  }
  std::string trailing;
  if (std::getline(input, trailing) || !IsSha256(seal) ||
      result.mutations.size() != declared_mutations ||
      result.artifacts.size() != declared_artifacts || seal != Sha256(body)) {
    result.diagnostic = Refuse("publication_manifest_seal_invalid");
    result.stable_reason = "manifest is torn or its checksum is invalid";
    return result;
  }
  result.manifest_sha256 = seal;

  const auto transaction = LookupLocalTransaction(
      inventory, MakeLocalTransactionId(context.local_transaction_id));
  if (!transaction.ok()) {
    result.recovery_class = LocalCommitPublicationRecoveryClass::in_doubt;
    result.diagnostic = Refuse("publication_inventory_identity_missing");
    result.stable_reason = "manifest has no matching durable transaction inventory entry";
    return result;
  }
  switch (transaction.entry.state) {
    case TransactionState::committed:
    case TransactionState::archived:
      result.recovery_class = LocalCommitPublicationRecoveryClass::committed_by_inventory;
      result.stable_reason = "inventory finality commits every manifested artifact";
      break;
    case TransactionState::rolled_back:
    case TransactionState::failed_terminal:
      result.recovery_class = LocalCommitPublicationRecoveryClass::abandoned_by_rollback;
      result.stable_reason = "inventory finality abandons every manifested artifact";
      break;
    case TransactionState::prepared:
    case TransactionState::limbo:
      result.recovery_class = LocalCommitPublicationRecoveryClass::in_doubt;
      result.stable_reason = "inventory requires prepared or in-doubt resolution";
      break;
    default:
      result.recovery_class = LocalCommitPublicationRecoveryClass::retryable_unpublished;
      result.stable_reason = "transaction remains open and the publication barrier may be retried";
      break;
  }
  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

}  // namespace scratchbird::engine::internal_api
