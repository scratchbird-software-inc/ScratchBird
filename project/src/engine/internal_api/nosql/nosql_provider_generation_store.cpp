// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_provider_generation_store.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kGenerationMagic = "SBNOSQLPG1";

std::mutex& StoreMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::vector<EngineNoSqlProviderGenerationMetadata>>&
GenerationCache() {
  static std::map<std::string, std::vector<EngineNoSqlProviderGenerationMetadata>>
      cache;
  return cache;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

std::string GenerationPath(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return {};
  }
  return context.database_path + ".sb.nosql_provider_generations";
}

std::uint64_t Fnva64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

bool IsHex(char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

bool IsValidUuid(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (!IsHex(value[i])) {
      return false;
    }
  }
  return true;
}

std::string StableGenerationUuid(const EngineRequestContext& context,
                                 const std::string& provider_id,
                                 const std::string& collection_uuid,
                                 std::uint64_t generation_id) {
  const std::string database_seed =
      !context.database_uuid.canonical.empty()
          ? context.database_uuid.canonical
          : EngineNoSqlProviderDatabaseIdentity(context);
  const std::string seed = database_seed + "|" + provider_id + "|" +
                           collection_uuid + "|" +
                           std::to_string(generation_id);
  std::string hex = Hex64(Fnva64(seed + ":left")) +
                    Hex64(Fnva64(seed + ":right"));
  hex[12] = '7';
  hex[16] = '8';
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
         hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
         hex.substr(20, 12);
}

std::uint64_t NonZeroEpoch(std::uint64_t epoch) { return epoch == 0 ? 1 : epoch; }

std::vector<std::pair<std::string, std::string>> MetadataPairs(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return {
      {"family", EngineNoSqlProviderFamilyName(metadata.family)},
      {"provider_id", metadata.provider_id},
      {"database_identity", metadata.database_identity},
      {"database_uuid", metadata.database_uuid},
      {"collection_uuid", metadata.collection_uuid},
      {"generation_uuid", metadata.generation_uuid},
      {"generation_id", std::to_string(metadata.generation_id)},
      {"descriptor_epoch", std::to_string(metadata.descriptor_epoch)},
      {"security_epoch", std::to_string(metadata.security_epoch)},
      {"redaction_epoch", std::to_string(metadata.redaction_epoch)},
      {"catalog_epoch", std::to_string(metadata.catalog_epoch)},
      {"publish_state", metadata.publish_state},
      {"validation_state", metadata.validation_state},
      {"backup_metadata_ref", metadata.backup_metadata_ref},
      {"restore_metadata_ref", metadata.restore_metadata_ref},
      {"repair_metadata_ref", metadata.repair_metadata_ref},
      {"support_bundle_evidence_id", metadata.support_bundle_evidence_id},
      {"provider_claims_transaction_finality_authority",
       BoolText(metadata.provider_claims_transaction_finality_authority)},
      {"provider_claims_visibility_authority",
       BoolText(metadata.provider_claims_visibility_authority)},
  };
}

std::map<std::string, std::string> PairMap(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::map<std::string, std::string> out;
  for (const auto& [key, value] : pairs) {
    out[key] = value;
  }
  return out;
}

std::string ValueOr(const std::map<std::string, std::string>& values,
                    const std::string& key,
                    const std::string& fallback = {}) {
  const auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

EngineNoSqlProviderGenerationMetadata MetadataFromPairs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  const auto values = PairMap(pairs);
  EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = EngineNoSqlProviderFamilyFromString(ValueOr(values, "family"));
  metadata.provider_id = ValueOr(values, "provider_id");
  metadata.database_identity = ValueOr(values, "database_identity");
  metadata.database_uuid = ValueOr(values, "database_uuid");
  metadata.collection_uuid = ValueOr(values, "collection_uuid");
  metadata.generation_uuid = ValueOr(values, "generation_uuid");
  metadata.generation_id = ParseU64(ValueOr(values, "generation_id"));
  metadata.descriptor_epoch = ParseU64(ValueOr(values, "descriptor_epoch"));
  metadata.security_epoch = ParseU64(ValueOr(values, "security_epoch"));
  metadata.redaction_epoch = ParseU64(ValueOr(values, "redaction_epoch"));
  metadata.catalog_epoch = ParseU64(ValueOr(values, "catalog_epoch"));
  metadata.publish_state = ValueOr(values, "publish_state", "unverified");
  metadata.validation_state = ValueOr(values, "validation_state", "unverified");
  metadata.backup_metadata_ref = ValueOr(values, "backup_metadata_ref");
  metadata.restore_metadata_ref = ValueOr(values, "restore_metadata_ref");
  metadata.repair_metadata_ref = ValueOr(values, "repair_metadata_ref");
  metadata.support_bundle_evidence_id =
      ValueOr(values, "support_bundle_evidence_id");
  metadata.provider_claims_transaction_finality_authority =
      ParseBool(ValueOr(values, "provider_claims_transaction_finality_authority"));
  metadata.provider_claims_visibility_authority =
      ParseBool(ValueOr(values, "provider_claims_visibility_authority"));
  return metadata;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineNoSqlProviderGenerationResult Failure(const EngineRequestContext& context,
                                            const std::string& operation_id,
                                            const char* detail) {
  (void)context;
  EngineNoSqlProviderGenerationResult result;
  result.ok = false;
  result.diagnostic = MakeInvalidRequestDiagnostic(operation_id, detail);
  result.evidence.push_back(std::string("provider_generation_refusal=") +
                            detail);
  result.evidence.push_back("provider_generation_fail_closed=true");
  result.evidence.push_back("provider_generation_finality_authority=false");
  result.evidence.push_back("provider_generation_visibility_authority=false");
  result.evidence.push_back(
      "provider_generation_mga_authority=engine_transaction_inventory");
  return result;
}

void AddCommonEvidence(EngineNoSqlProviderGenerationResult* result) {
  const auto& metadata = result->metadata;
  result->evidence.push_back("provider_generation_family=" +
                             std::string(EngineNoSqlProviderFamilyName(
                                 metadata.family)));
  result->evidence.push_back("provider_generation_provider_id=" +
                             metadata.provider_id);
  result->evidence.push_back("provider_generation_database_identity=" +
                             metadata.database_identity);
  result->evidence.push_back("provider_generation_collection_uuid=" +
                             metadata.collection_uuid);
  result->evidence.push_back("provider_generation_uuid=" +
                             metadata.generation_uuid);
  result->evidence.push_back("provider_generation_id=" +
                             std::to_string(metadata.generation_id));
  result->evidence.push_back("provider_generation_descriptor_epoch=" +
                             std::to_string(metadata.descriptor_epoch));
  result->evidence.push_back("provider_generation_security_epoch=" +
                             std::to_string(metadata.security_epoch));
  result->evidence.push_back("provider_generation_redaction_epoch=" +
                             std::to_string(metadata.redaction_epoch));
  result->evidence.push_back("provider_generation_catalog_epoch=" +
                             std::to_string(metadata.catalog_epoch));
  result->evidence.push_back("provider_generation_publish_state=" +
                             metadata.publish_state);
  result->evidence.push_back("provider_generation_validation_state=" +
                             metadata.validation_state);
  result->evidence.push_back("provider_generation_backup_metadata_ref=" +
                             metadata.backup_metadata_ref);
  result->evidence.push_back("provider_generation_restore_metadata_ref=" +
                             metadata.restore_metadata_ref);
  result->evidence.push_back("provider_generation_repair_metadata_ref=" +
                             metadata.repair_metadata_ref);
  result->evidence.push_back("provider_generation_support_bundle_evidence_id=" +
                             metadata.support_bundle_evidence_id);
  result->evidence.push_back("provider_generation_finality_authority=false");
  result->evidence.push_back("provider_generation_visibility_authority=false");
  result->evidence.push_back(
      "provider_generation_mga_authority=engine_transaction_inventory");
}

bool Matches(const EngineNoSqlProviderGenerationMetadata& metadata,
             EngineNoSqlProviderFamily family,
             const std::string& provider_id,
             const std::string& collection_uuid) {
  return metadata.family == family && metadata.provider_id == provider_id &&
         metadata.collection_uuid == collection_uuid;
}

std::string GenerationKey(const EngineNoSqlProviderGenerationMetadata& metadata) {
  return std::string(EngineNoSqlProviderFamilyName(metadata.family)) + "\x1f" +
         metadata.provider_id + "\x1f" + metadata.collection_uuid;
}

bool BoundToContext(const EngineRequestContext& context,
                    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!context.database_uuid.canonical.empty() &&
      !metadata.database_uuid.empty() &&
      IsValidUuid(context.database_uuid.canonical) &&
      IsValidUuid(metadata.database_uuid)) {
    return context.database_uuid.canonical == metadata.database_uuid;
  }
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  return metadata.database_identity.empty() || metadata.database_identity == identity;
}

bool HasLifecycleMetadata(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return metadata.family != EngineNoSqlProviderFamily::kUnknown &&
         (!metadata.database_identity.empty() ||
          !metadata.database_uuid.empty()) &&
         (metadata.database_uuid.empty() ||
          IsValidUuid(metadata.database_uuid)) &&
         !metadata.provider_id.empty() && !metadata.collection_uuid.empty() &&
         metadata.generation_id != 0 &&
         IsValidUuid(metadata.generation_uuid) &&
         !metadata.publish_state.empty() && !metadata.validation_state.empty() &&
         !metadata.backup_metadata_ref.empty() &&
         !metadata.restore_metadata_ref.empty() &&
         !metadata.repair_metadata_ref.empty() &&
         !metadata.support_bundle_evidence_id.empty();
}

bool MetadataRefMismatch(
    const EngineNoSqlProviderGenerationProof& proof,
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return (!proof.backup_metadata_ref.empty() &&
          proof.backup_metadata_ref != metadata.backup_metadata_ref) ||
         (!proof.restore_metadata_ref.empty() &&
          proof.restore_metadata_ref != metadata.restore_metadata_ref) ||
         (!proof.repair_metadata_ref.empty() &&
          proof.repair_metadata_ref != metadata.repair_metadata_ref) ||
         (!proof.support_bundle_evidence_id.empty() &&
          proof.support_bundle_evidence_id != metadata.support_bundle_evidence_id);
}

bool RewriteLocked(
    const EngineRequestContext& context,
    const std::vector<EngineNoSqlProviderGenerationMetadata>& generations) {
  const auto path = GenerationPath(context);
  if (path.empty()) { return true; }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { return false; }
  for (const auto& metadata : generations) {
    out << kGenerationMagic << "\tGENERATION\t"
        << EncodeCrudPairs(MetadataPairs(metadata)) << '\n';
  }
  out.flush();
  return static_cast<bool>(out);
}

bool ExistingFileNeedsRecordSeparator(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size == 0) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(static_cast<std::streamoff>(size - 1));
  char last = '\0';
  in.get(last);
  return in && last != '\n';
}

std::vector<EngineNoSqlProviderGenerationMetadata> LoadLocked(
    const EngineRequestContext& context) {
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  auto cache_it = GenerationCache().find(identity);
  if (cache_it != GenerationCache().end()) {
    return cache_it->second;
  }

  std::vector<EngineNoSqlProviderGenerationMetadata> loaded;
  std::map<std::string, EngineNoSqlProviderGenerationMetadata> latest;
  const auto path = GenerationPath(context);
  if (!path.empty()) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind(kGenerationMagic, 0) != 0) {
        continue;
      }
      const auto parts = Split(line, '\t');
      if (parts.size() < 3 || parts[1] != std::string("GENERATION")) {
        if (parts.size() >= 3 && parts[1] == std::string("DROP")) {
          auto metadata = MetadataFromPairs(DecodeCrudPairs(parts[2]));
          if (BoundToContext(context, metadata)) {
            latest.erase(GenerationKey(metadata));
          }
        }
        continue;
      }
      auto metadata = MetadataFromPairs(DecodeCrudPairs(parts[2]));
      if (!BoundToContext(context, metadata)) {
        continue;
      }
      metadata.database_identity = identity;
      latest[GenerationKey(metadata)] = std::move(metadata);
    }
  }
  for (auto& [key, metadata] : latest) {
    (void)key;
    loaded.push_back(std::move(metadata));
  }
  GenerationCache()[identity] = loaded;
  return loaded;
}

}  // namespace

std::string EngineNoSqlProviderDatabaseIdentity(
    const EngineRequestContext& context) {
  if (!context.database_path.empty()) {
    return context.database_path;
  }
  if (!context.database_uuid.canonical.empty()) {
    return context.database_uuid.canonical;
  }
  return "embedded_transient_nosql_provider";
}

EngineNoSqlProviderGenerationMetadata MakeDocumentProviderGenerationMetadata(
    const EngineRequestContext& context,
    const std::string& provider_id,
    const std::string& collection_uuid,
    std::uint64_t generation_id) {
  EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = EngineNoSqlProviderFamily::kDocument;
  metadata.provider_id = provider_id;
  metadata.database_identity = EngineNoSqlProviderDatabaseIdentity(context);
  metadata.database_uuid = IsValidUuid(context.database_uuid.canonical)
                               ? context.database_uuid.canonical
                               : GenerateCrudEngineUuid("database");
  metadata.collection_uuid = collection_uuid;
  metadata.generation_id = generation_id;
  metadata.generation_uuid = StableGenerationUuid(context,
                                                  provider_id,
                                                  collection_uuid,
                                                  generation_id);
  metadata.descriptor_epoch = NonZeroEpoch(context.resource_epoch);
  metadata.security_epoch = NonZeroEpoch(context.security_epoch);
  metadata.redaction_epoch = NonZeroEpoch(context.security_epoch);
  metadata.catalog_epoch = NonZeroEpoch(context.catalog_generation_id);
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.backup_metadata_ref =
      "backup.provider_generation:" + metadata.generation_uuid;
  metadata.restore_metadata_ref =
      "restore.provider_generation:" + metadata.generation_uuid;
  metadata.repair_metadata_ref =
      "repair.provider_generation:" + metadata.generation_uuid;
  metadata.support_bundle_evidence_id =
      "support.nosql_provider_generation:" + metadata.generation_uuid;
  return metadata;
}

EngineNoSqlProviderGenerationResult PublishNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  auto writable = metadata;
  writable.database_identity = EngineNoSqlProviderDatabaseIdentity(context);
  if (writable.database_uuid.empty() || !IsValidUuid(writable.database_uuid)) {
    writable.database_uuid = IsValidUuid(context.database_uuid.canonical)
                                 ? context.database_uuid.canonical
                                 : GenerateCrudEngineUuid("database");
  }
  if (writable.descriptor_epoch == 0) {
    writable.descriptor_epoch = NonZeroEpoch(context.resource_epoch);
  }
  if (writable.security_epoch == 0) {
    writable.security_epoch = NonZeroEpoch(context.security_epoch);
  }
  if (writable.redaction_epoch == 0) {
    writable.redaction_epoch = NonZeroEpoch(context.security_epoch);
  }
  if (writable.catalog_epoch == 0) {
    writable.catalog_epoch = NonZeroEpoch(context.catalog_generation_id);
  }
  if (!HasLifecycleMetadata(writable)) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationMetadataMissing);
  }
  if (!BoundToContext(context, writable)) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationIdentityMismatch);
  }
  if (writable.provider_claims_transaction_finality_authority ||
      writable.provider_claims_visibility_authority) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationAuthorityRefused);
  }

  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  const auto path = GenerationPath(context);
  if (!path.empty()) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
      return Failure(context,
                     "nosql.provider_generation.publish",
                     kNoSqlProviderGenerationUnavailable);
    }
    if (ExistingFileNeedsRecordSeparator(path)) {
      out << '\n';
    }
    out << kGenerationMagic << "\tGENERATION\t"
        << EncodeCrudPairs(MetadataPairs(writable)) << '\n';
    out.flush();
    if (!out) {
      return Failure(context,
                     "nosql.provider_generation.publish",
                     kNoSqlProviderGenerationUnavailable);
    }
  }

  auto generations = LoadLocked(context);
  generations.erase(
      std::remove_if(generations.begin(),
                     generations.end(),
                     [&](const EngineNoSqlProviderGenerationMetadata& existing) {
                       return Matches(existing,
                                      writable.family,
                                      writable.provider_id,
                                      writable.collection_uuid);
                     }),
      generations.end());
  generations.push_back(writable);
  GenerationCache()[identity] = generations;

  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.metadata = std::move(writable);
  AddCommonEvidence(&result);
  result.evidence.push_back("provider_generation_persisted=true");
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  result.evidence.push_back("provider_generation_support_bundle_ready=true");
  return result;
}

EngineNoSqlProviderGenerationResult LoadNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto loaded = LoadLocked(context);
  EngineNoSqlProviderGenerationResult result;
  for (const auto& metadata : loaded) {
    if (Matches(metadata, family, provider_id, collection_uuid)) {
      result.ok = true;
      result.diagnostic = OkDiagnostic();
      result.metadata = metadata;
      AddCommonEvidence(&result);
      result.evidence.push_back("provider_generation_loaded=true");
      return result;
    }
  }
  return Failure(context,
                 "nosql.provider_generation.load",
                 kNoSqlProviderGenerationUnavailable);
}

EngineNoSqlProviderGenerationResult ValidateNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlPhysicalProviderContract& contract) {
  if (!contract.provider_generation.required) {
    EngineNoSqlProviderGenerationResult result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back("provider_generation_required=false");
    return result;
  }
  if (!contract.provider_generation.proof_present) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationProofMissing);
  }
  auto loaded = LoadNoSqlProviderGeneration(context,
                                            contract.family,
                                            contract.provider_id,
                                            contract.provider_generation.collection_uuid);
  if (!loaded.ok) {
    return loaded;
  }

  const auto& metadata = loaded.metadata;
  const auto& proof = contract.provider_generation;
  const bool generation_mismatch =
      metadata.generation_id < proof.required_generation ||
      proof.available_generation < proof.required_generation ||
      (!proof.generation_uuid.empty() &&
       metadata.generation_uuid != proof.generation_uuid) ||
      (!proof.provider_id.empty() && metadata.provider_id != proof.provider_id) ||
      (!proof.database_uuid.empty() && IsValidUuid(proof.database_uuid) &&
       metadata.database_uuid != proof.database_uuid) ||
      (!proof.collection_uuid.empty() &&
       metadata.collection_uuid != proof.collection_uuid);
  if (generation_mismatch || !proof.visible_to_snapshot) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationStale);
  }

  const bool epoch_mismatch =
      (proof.descriptor_epoch != 0 &&
       metadata.descriptor_epoch != proof.descriptor_epoch) ||
      (proof.security_epoch != 0 &&
       metadata.security_epoch != proof.security_epoch) ||
      (proof.redaction_epoch != 0 &&
       metadata.redaction_epoch != proof.redaction_epoch) ||
      (proof.catalog_epoch != 0 &&
       metadata.catalog_epoch != proof.catalog_epoch);
  if (epoch_mismatch) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationEpochMismatch);
  }

  if (!proof.publish_state_bound || !proof.validation_state_bound ||
      metadata.publish_state != "published" ||
      metadata.validation_state != "validated") {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationStateUnvalidated);
  }

  if (!proof.backup_restore_repair_metadata_bound ||
      !proof.support_bundle_evidence_bound ||
      metadata.backup_metadata_ref.empty() ||
      metadata.restore_metadata_ref.empty() ||
      metadata.repair_metadata_ref.empty() ||
      metadata.support_bundle_evidence_id.empty() ||
      MetadataRefMismatch(proof, metadata)) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationMetadataMissing);
  }

  if (metadata.provider_claims_transaction_finality_authority ||
      metadata.provider_claims_visibility_authority ||
      proof.provider_claims_transaction_finality_authority ||
      proof.provider_claims_visibility_authority) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationAuthorityRefused);
  }

  loaded.evidence.push_back("provider_generation_validated=true");
  loaded.evidence.push_back("provider_generation_epoch_bound=true");
  loaded.evidence.push_back("provider_generation_backup_restore_repair_bound=true");
  loaded.evidence.push_back("provider_generation_support_bundle_ready=true");
  return loaded;
}

EngineNoSqlProviderGenerationResult RepairNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationRepairRequest& request) {
  if (!request.repair_admitted) {
    return Failure(context,
                   "nosql.provider_generation.repair",
                   kNoSqlProviderGenerationRepairAdmissionRequired);
  }
  if (request.authoritative_source_generations.empty()) {
    return Failure(context,
                   "nosql.provider_generation.repair",
                   kNoSqlProviderGenerationRepairSourceMissing);
  }

  for (auto metadata : request.authoritative_source_generations) {
    if (!Matches(metadata,
                 request.family,
                 request.provider_id,
                 request.collection_uuid)) {
      continue;
    }
    if (!BoundToContext(context, metadata)) {
      return Failure(context,
                     "nosql.provider_generation.repair",
                     kNoSqlProviderGenerationIdentityMismatch);
    }
    if (!HasLifecycleMetadata(metadata)) {
      return Failure(context,
                     "nosql.provider_generation.repair",
                     kNoSqlProviderGenerationMetadataMissing);
    }
    metadata.publish_state = "published";
    metadata.validation_state = "validated";
    auto repaired = PublishNoSqlProviderGeneration(context, metadata);
    if (repaired.ok) {
      repaired.evidence.push_back("provider_generation_repair_admitted=true");
      repaired.evidence.push_back("provider_generation_repair_source=authoritative");
      repaired.evidence.push_back("provider_generation_repair_published=true");
      repaired.evidence.push_back("provider_generation_descriptor_scan_fallback=false");
    }
    return repaired;
  }

  return Failure(context,
                 "nosql.provider_generation.repair",
                 kNoSqlProviderGenerationRepairSourceMissing);
}

EngineNoSqlProviderGenerationResult DropNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  auto generations = LoadLocked(context);
  const auto before = generations.size();
  generations.erase(
      std::remove_if(generations.begin(),
                     generations.end(),
                     [&](const EngineNoSqlProviderGenerationMetadata& existing) {
                       return Matches(existing, family, provider_id, collection_uuid);
                     }),
      generations.end());
  if (generations.size() == before) {
    return Failure(context,
                   "nosql.provider_generation.drop",
                   kNoSqlProviderGenerationUnavailable);
  }
  if (!RewriteLocked(context, generations)) {
    return Failure(context,
                   "nosql.provider_generation.drop",
                   kNoSqlProviderGenerationUnavailable);
  }
  GenerationCache()[identity] = generations;

  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back("provider_generation_drop=complete");
  result.evidence.push_back("provider_generation_persistent_state_removed=true");
  result.evidence.push_back("provider_generation_cache_stale=false");
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  result.evidence.push_back("provider_generation_finality_authority=false");
  result.evidence.push_back("provider_generation_visibility_authority=false");
  return result;
}

std::vector<EngineNoSqlProviderGenerationMetadata> ListNoSqlProviderGenerations(
    const EngineRequestContext& context) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  return LoadLocked(context);
}

EngineNoSqlProviderGenerationResult CleanupNoSqlProviderGenerations(
    const EngineRequestContext& context,
    bool drop_persistent_state) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  GenerationCache().erase(identity);
  if (drop_persistent_state) {
    const auto path = GenerationPath(context);
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back("provider_generation_cleanup=complete");
  result.evidence.push_back(std::string("provider_generation_drop_persistent_state=") +
                            BoolText(drop_persistent_state));
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  return result;
}

void AddNoSqlProviderGenerationEvidence(
    EngineApiResult* result,
    const EngineNoSqlProviderGenerationResult& generation) {
  for (const auto& item : generation.evidence) {
    AddApiBehaviorEvidence(result, "nosql_provider_generation", item);
  }
}

}  // namespace scratchbird::engine::internal_api
