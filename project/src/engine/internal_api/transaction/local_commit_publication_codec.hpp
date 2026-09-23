// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction/local_commit_publication.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "hash_digest.hpp"
#include <charconv>
namespace scratchbird::engine::internal_api::local_publication_codec {
inline constexpr std::string_view kMagic = "SBMGAP02";
inline std::string Digest(std::string_view bytes) {
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(bytes.data()), bytes.size());
  return digest.ok() ? core::hash::HexLower(digest.digest) : std::string{};
}
inline bool Hash(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(),
      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline bool Number(const BinaryCatalogMetadata& fields, const std::string& key, std::uint64_t* output) {
  const auto it = fields.text.find(key);
  if (it == fields.text.end() || it->second.empty()) return false;
  const auto& text = it->second;
  const auto parsed = std::from_chars(text.data(), text.data()+text.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && std::to_string(*output) == text;
}
inline bool Frame(std::string* output, const BinaryCatalogMetadata& fields, std::string_view schema) {
  std::string record;
  if (!EncodeBinaryCatalogMetadata(fields, schema, &record)) return false;
  AppendBinaryU32(output, static_cast<std::uint32_t>(record.size()));
  *output += record;
  return true;
}
inline bool ReadFrame(std::span<const std::uint8_t> bytes, std::size_t* cursor,
                      std::string_view schema, BinaryCatalogMetadata* fields) {
  std::uint32_t size = 0;
  if (!ReadBinaryU32(bytes, cursor, &size) || size > bytes.size() - *cursor) return false;
  const auto record = std::string_view(reinterpret_cast<const char*>(bytes.data()+*cursor), size);
  if (!DecodeBinaryCatalogMetadata(record, schema, fields)) return false;
  *cursor += size;
  return true;
}
inline bool Valid(const LocalCommitPublicationMutation& value) {
  return core::uuid::IsEngineIdentityUuid(value.object_identity) &&
      core::uuid::IsEngineIdentityUuid(value.record_identity) &&
      (value.version_identity.is_nil() || core::uuid::IsEngineIdentityUuid(value.version_identity)) &&
      Hash(value.mutation_identity) && Hash(value.idempotency_key) &&
      Hash(value.precondition_sha256) && Hash(value.postcondition_sha256) &&
      !value.mutation_domain.empty() && !value.mutation_kind.empty() && !value.physical_identity.empty() &&
      value.finality_authority == "durable_transaction_inventory" && value.lifecycle_state == "commit_publish_ready";
}
inline bool Valid(const LocalCommitPublicationArtifact& value) {
  return !value.mutation_domain.empty() && !value.artifact_identity.empty() && Hash(value.postcondition_sha256);
}
inline BinaryCatalogMetadata Fields(const LocalCommitPublicationMutation& value) {
  BinaryCatalogMetadata fields;
  fields.text.emplace("mutation_identity", value.mutation_identity);
  fields.text.emplace("mutation_domain", value.mutation_domain);
  fields.text.emplace("mutation_kind", value.mutation_kind);
  fields.text.emplace("physical_identity", value.physical_identity);
  fields.text.emplace("idempotency_key", value.idempotency_key);
  fields.text.emplace("precondition_sha256", value.precondition_sha256);
  fields.text.emplace("postcondition_sha256", value.postcondition_sha256);
  fields.text.emplace("finality_authority", value.finality_authority);
  fields.text.emplace("lifecycle_state", value.lifecycle_state);
  fields.identities.emplace("object_identity", value.object_identity);
  fields.identities.emplace("record_identity", value.record_identity);
  fields.identities.emplace("version_identity", value.version_identity);
  fields.text.emplace("generation_before", std::to_string(value.generation_before));
  fields.text.emplace("generation_after", std::to_string(value.generation_after));
  return fields;
}
inline bool Decode(const BinaryCatalogMetadata& fields, LocalCommitPublicationMutation* value) {
  LocalCommitPublicationMutation decoded;
  if (!fields.text.contains("mutation_identity")) return false;
  decoded.mutation_identity = fields.text.at("mutation_identity");
  if (!fields.text.contains("mutation_domain")) return false;
  decoded.mutation_domain = fields.text.at("mutation_domain");
  if (!fields.text.contains("mutation_kind")) return false;
  decoded.mutation_kind = fields.text.at("mutation_kind");
  if (!fields.text.contains("physical_identity")) return false;
  decoded.physical_identity = fields.text.at("physical_identity");
  if (!fields.text.contains("idempotency_key")) return false;
  decoded.idempotency_key = fields.text.at("idempotency_key");
  if (!fields.text.contains("precondition_sha256")) return false;
  decoded.precondition_sha256 = fields.text.at("precondition_sha256");
  if (!fields.text.contains("postcondition_sha256")) return false;
  decoded.postcondition_sha256 = fields.text.at("postcondition_sha256");
  if (!fields.text.contains("finality_authority")) return false;
  decoded.finality_authority = fields.text.at("finality_authority");
  if (!fields.text.contains("lifecycle_state")) return false;
  decoded.lifecycle_state = fields.text.at("lifecycle_state");
  if (!fields.identities.contains("object_identity")) return false;
  decoded.object_identity = fields.identities.at("object_identity");
  if (!fields.identities.contains("record_identity")) return false;
  decoded.record_identity = fields.identities.at("record_identity");
  if (!fields.identities.contains("version_identity")) return false;
  decoded.version_identity = fields.identities.at("version_identity");
  if (!Number(fields, "generation_before", &decoded.generation_before)) return false;
  if (!Number(fields, "generation_after", &decoded.generation_after)) return false;
  if (!Valid(decoded)) return false;
  const auto canonical = Fields(decoded);
  if (canonical.text != fields.text || canonical.identities != fields.identities) return false;
  *value = std::move(decoded); return true;
}
inline BinaryCatalogMetadata Fields(const LocalCommitPublicationArtifact& value) {
  BinaryCatalogMetadata fields;
  fields.text.emplace("mutation_domain", value.mutation_domain);
  fields.text.emplace("artifact_identity", value.artifact_identity);
  fields.text.emplace("postcondition_sha256", value.postcondition_sha256);
  fields.text.emplace("durable_size_bytes", std::to_string(value.durable_size_bytes));
  return fields;
}
inline bool Decode(const BinaryCatalogMetadata& fields, LocalCommitPublicationArtifact* value) {
  LocalCommitPublicationArtifact decoded;
  if (!fields.text.contains("mutation_domain")) return false;
  decoded.mutation_domain = fields.text.at("mutation_domain");
  if (!fields.text.contains("artifact_identity")) return false;
  decoded.artifact_identity = fields.text.at("artifact_identity");
  if (!fields.text.contains("postcondition_sha256")) return false;
  decoded.postcondition_sha256 = fields.text.at("postcondition_sha256");
  if (!Number(fields, "durable_size_bytes", &decoded.durable_size_bytes)) return false;
  if (!Valid(decoded)) return false;
  const auto canonical = Fields(decoded);
  if (canonical.text != fields.text || canonical.identities != fields.identities) return false;
  *value = std::move(decoded); return true;
}
inline bool Encode(const EngineRequestContext& context,
    const std::vector<LocalCommitPublicationMutation>& mutations,
    const std::vector<LocalCommitPublicationArtifact>& artifacts,
    std::string* output) {
  if (!output || !context.local_transaction_id ||
      !core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(context.transaction_uuid)) return false;
  BinaryCatalogMetadata header;
  header.identities = {{"database_uuid", context.database_uuid}, {"transaction_uuid", context.transaction_uuid}};
  header.text = {{"publication_generation", std::to_string(context.local_transaction_id)},
                 {"mutations", std::to_string(mutations.size())}, {"artifacts", std::to_string(artifacts.size())}};
  std::string bytes(kMagic);
  if (!Frame(&bytes, header, "mga.publication.header.v2")) return false;
  for (const auto& value : mutations)
    if (!Valid(value) || !Frame(&bytes, Fields(value), "mga.publication.mutation.v2")) return false;
  for (const auto& value : artifacts)
    if (!Valid(value) || !Frame(&bytes, Fields(value), "mga.publication.artifact.v2")) return false;
  const auto digest = Digest(bytes);
  if (digest.empty()) return false;
  bytes += digest;
  *output = std::move(bytes);
  return true;
}
inline bool Decode(std::string_view bytes, const EngineRequestContext& context,
                   LocalCommitPublicationRecoveryResult* output) {
  if (!output || bytes.size() < kMagic.size()+64 || !bytes.starts_with(kMagic)) return false;
  const auto body = bytes.substr(0, bytes.size()-64);
  if (Digest(body) != bytes.substr(bytes.size()-64)) return false;
  const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  std::size_t cursor = kMagic.size();
  BinaryCatalogMetadata header;
  LocalCommitPublicationRecoveryResult decoded;
  std::uint64_t mutations = 0, artifacts = 0;
  if (!ReadFrame(input, &cursor, "mga.publication.header.v2", &header) ||
      !Number(header, "publication_generation", &decoded.publication_generation) ||
      decoded.publication_generation != context.local_transaction_id ||
      BinaryCatalogUuid(header, "database_uuid") != context.database_uuid ||
      BinaryCatalogUuid(header, "transaction_uuid") != context.transaction_uuid ||
      !Number(header, "mutations", &mutations) || !Number(header, "artifacts", &artifacts) ||
      mutations > input.size()/4 || artifacts > input.size()/4) return false;
  for (std::uint64_t n=0; n<mutations; ++n) {
    BinaryCatalogMetadata fields; LocalCommitPublicationMutation value;
    if (!ReadFrame(input, &cursor, "mga.publication.mutation.v2", &fields) || !Decode(fields, &value)) return false;
    decoded.mutations.push_back(std::move(value));
  }
  for (std::uint64_t n=0; n<artifacts; ++n) {
    BinaryCatalogMetadata fields; LocalCommitPublicationArtifact value;
    if (!ReadFrame(input, &cursor, "mga.publication.artifact.v2", &fields) || !Decode(fields, &value)) return false;
    decoded.artifacts.push_back(std::move(value));
  }
  if (cursor != input.size()) return false;
  std::string canonical;
  if (!Encode(context, decoded.mutations, decoded.artifacts, &canonical) || canonical != bytes) return false;
  decoded.manifest_sha256 = std::string(bytes.substr(bytes.size()-64));
  *output = std::move(decoded);
  return true;
}
} // namespace scratchbird::engine::internal_api::local_publication_codec
