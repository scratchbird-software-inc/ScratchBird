// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-LARGE-PAYLOAD-SEPARATION-ANCHOR
#include "large_payload.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::storage::page {
namespace {

namespace filespace = scratchbird::storage::filespace;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status LargePayloadOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status LargePayloadErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.kind == right.kind && left.value == right.value;
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, 1779620000000ull + seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string HashPayload(const std::vector<byte>& payload) {
  u64 hash = 1469598103934665603ULL;
  for (const byte value : payload) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "fnv1a64:" << hash;
  return out.str();
}

std::string BytesToString(const std::vector<byte>& payload) {
  return std::string(payload.begin(), payload.end());
}

std::string PageFamilyForLargePayloadFamily(LargePayloadFamily family) {
  switch (family) {
    case LargePayloadFamily::document: return "blob";
    case LargePayloadFamily::key_value: return "blob";
    case LargePayloadFamily::vector: return "vector";
    case LargePayloadFamily::text: return "blob";
    case LargePayloadFamily::blob: return "blob";
    case LargePayloadFamily::graph: return "graph";
  }
  return "blob";
}

LargePayloadEvidenceRecord BuildEvidence(LargePayloadStore* store,
                                         const LargePayloadDescriptor& descriptor,
                                         std::string action,
                                         std::string diagnostic_code,
                                         std::string reason,
                                         bool durable_state_changed,
                                         u64 authoritative_horizon = 0) {
  LargePayloadEvidenceRecord evidence;
  evidence.sequence = store == nullptr ? 0 : store->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.payload_uuid = descriptor.payload_uuid;
  evidence.family = descriptor.family;
  evidence.generation = descriptor.generation;
  evidence.local_transaction_id = descriptor.creator_local_transaction_id;
  evidence.authoritative_mga_horizon_local_transaction_id = authoritative_horizon;
  evidence.cache_hits = store == nullptr ? 0 : store->cache.hit_count;
  evidence.cache_misses = store == nullptr ? 0 : store->cache.miss_count;
  evidence.prefetch_requests = store == nullptr ? 0 : store->cache.prefetch_request_count;
  evidence.prefetch_loaded = store == nullptr ? 0 : store->cache.prefetch_loaded_count;
  evidence.evidence_token = "large_payload:" + evidence.action + ":" + std::to_string(evidence.sequence);
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_only = true;
  evidence.finality_authority = false;
  evidence.visibility_authority = false;
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

LargePayloadStoreResult RefuseStore(LargePayloadStore* store,
                                    const LargePayloadStoreRequest& request,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  LargePayloadStoreResult result;
  result.status = LargePayloadErrorStatus();
  result.descriptor.family = request.family;
  result.descriptor.owner_object_uuid = request.owner_object_uuid;
  result.descriptor.filespace_uuid = request.filespace_uuid;
  result.descriptor.creator_local_transaction_id = request.local_transaction_id;
  result.evidence = BuildEvidence(store,
                                  result.descriptor,
                                  "refuse_large_payload_store",
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  if (store != nullptr) {
    store->evidence.push_back(result.evidence);
  }
  return result;
}

LargePayloadReadResult RefuseRead(LargePayloadStore* store,
                                  const LargePayloadDescriptor& descriptor,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string detail) {
  LargePayloadReadResult result;
  result.status = LargePayloadErrorStatus();
  result.evidence = BuildEvidence(store,
                                  descriptor,
                                  "refuse_large_payload_read",
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  if (store != nullptr) {
    store->evidence.push_back(result.evidence);
  }
  return result;
}

LargePayloadRetireResult RefuseRetire(LargePayloadStore* store,
                                      const LargePayloadDescriptor& descriptor,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
  LargePayloadRetireResult result;
  result.status = LargePayloadErrorStatus();
  result.descriptor = descriptor;
  result.evidence = BuildEvidence(store,
                                  descriptor,
                                  "refuse_large_payload_retire",
                                  diagnostic_code,
                                  detail,
                                  false);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  if (store != nullptr) {
    store->evidence.push_back(result.evidence);
  }
  return result;
}

LargePayloadGenerationRecord* FindMutable(LargePayloadStore* store,
                                          const LargePayloadDescriptor& descriptor) {
  if (store == nullptr || descriptor.generation == 0 || !descriptor.payload_uuid.valid()) {
    return nullptr;
  }
  const auto found = std::find_if(store->generations.begin(),
                                  store->generations.end(),
                                  [&](const LargePayloadGenerationRecord& record) {
                                    return SameUuid(record.descriptor.payload_uuid, descriptor.payload_uuid) &&
                                           record.descriptor.generation == descriptor.generation;
                                  });
  return found == store->generations.end() ? nullptr : &(*found);
}

LargePayloadCacheEntry* FindCacheEntry(LargePayloadCache* cache,
                                       const LargePayloadDescriptor& descriptor) {
  if (cache == nullptr) { return nullptr; }
  const auto found = std::find_if(cache->entries.begin(),
                                  cache->entries.end(),
                                  [&](const LargePayloadCacheEntry& entry) {
                                    return entry.valid &&
                                           entry.generation == descriptor.generation &&
                                           SameUuid(entry.payload_uuid, descriptor.payload_uuid);
                                  });
  return found == cache->entries.end() ? nullptr : &(*found);
}

void PutCacheEntry(LargePayloadCache* cache,
                   const LargePayloadDescriptor& descriptor,
                   const std::vector<byte>& payload_bytes) {
  if (cache == nullptr || !descriptor.payload_uuid.valid() || descriptor.generation == 0) {
    return;
  }
  if (auto* existing = FindCacheEntry(cache, descriptor)) {
    existing->payload_bytes = payload_bytes;
    existing->content_hash = descriptor.content_hash;
    existing->valid = true;
    return;
  }
  LargePayloadCacheEntry entry;
  entry.payload_uuid = descriptor.payload_uuid;
  entry.generation = descriptor.generation;
  entry.payload_bytes = payload_bytes;
  entry.content_hash = descriptor.content_hash;
  entry.valid = true;
  cache->entries.push_back(std::move(entry));
}

void InvalidateCacheEntry(LargePayloadCache* cache,
                          const LargePayloadDescriptor& descriptor) {
  if (cache == nullptr) { return; }
  for (auto& entry : cache->entries) {
    if (entry.valid && entry.generation == descriptor.generation &&
        SameUuid(entry.payload_uuid, descriptor.payload_uuid)) {
      entry.valid = false;
      entry.payload_bytes.clear();
    }
  }
}

u64 NextGenerationForOwner(const LargePayloadStore& store,
                           const TypedUuid& generation_scope_uuid,
                           LargePayloadFamily family) {
  u64 max_generation = 0;
  for (const auto& record : store.generations) {
    if (record.descriptor.family == family &&
        SameUuid(record.descriptor.generation_scope_uuid, generation_scope_uuid)) {
      max_generation = std::max(max_generation, record.descriptor.generation);
    }
  }
  return max_generation + 1;
}

bool DescriptorMatchesRecord(const LargePayloadDescriptor& descriptor,
                             const LargePayloadGenerationRecord& record) {
  return SameUuid(descriptor.payload_uuid, record.descriptor.payload_uuid) &&
         descriptor.generation == record.descriptor.generation &&
         descriptor.byte_count == record.descriptor.byte_count &&
         descriptor.content_hash == record.descriptor.content_hash &&
         SameUuid(descriptor.generation_scope_uuid, record.descriptor.generation_scope_uuid) &&
         descriptor.family == record.descriptor.family;
}

bool VisibleAtSnapshot(const LargePayloadGenerationRecord& record, u64 snapshot_visible_through) {
  if (snapshot_visible_through == 0 ||
      record.descriptor.creator_local_transaction_id > snapshot_visible_through) {
    return false;
  }
  if (record.state == LargePayloadGenerationState::active) {
    return true;
  }
  if (record.state == LargePayloadGenerationState::retired) {
    return record.descriptor.retired_by_local_transaction_id != 0 &&
           snapshot_visible_through < record.descriptor.retired_by_local_transaction_id;
  }
  return false;
}

LargePayloadReadResult ReadCommittedPayloadBytes(LargePayloadStore* store,
                                                 const LargePayloadDescriptor& descriptor,
                                                 const LargePayloadGenerationRecord& record) {
  LargePayloadReadResult result;
  result.status = LargePayloadOkStatus();
  result.found = true;
  result.visible = true;
  if (record.descriptor.inline_payload) {
    result.payload_bytes = record.payload_bytes;
    return result;
  }

  OverflowReadRequest overflow_read;
  overflow_read.overflow_value_uuid = descriptor.overflow_value_uuid;
  const auto overflow = ReadOverflowValue(store->overflow_ledger, overflow_read);
  if (!overflow.ok()) {
    return RefuseRead(store,
                      descriptor,
                      "large_payload_read_overflow_refused",
                      "storage.page.large_payload.overflow_refused",
                      overflow.diagnostic.diagnostic_code);
  }
  result.payload_bytes = overflow.payload_bytes;
  return result;
}

std::map<std::string, std::string> ParseDescriptorFields(const std::string& text) {
  std::map<std::string, std::string> fields;
  std::stringstream stream(text);
  std::string field;
  while (std::getline(stream, field, ';')) {
    const auto equals = field.find('=');
    if (equals == std::string::npos) { continue; }
    fields[field.substr(0, equals)] = field.substr(equals + 1);
  }
  return fields;
}

u64 ParseU64(const std::string& value) {
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

}  // namespace

const char* LargePayloadFamilyName(LargePayloadFamily family) {
  switch (family) {
    case LargePayloadFamily::document: return "document";
    case LargePayloadFamily::key_value: return "key_value";
    case LargePayloadFamily::vector: return "vector";
    case LargePayloadFamily::text: return "text";
    case LargePayloadFamily::blob: return "blob";
    case LargePayloadFamily::graph: return "graph";
  }
  return "blob";
}

const char* LargePayloadGenerationStateName(LargePayloadGenerationState state) {
  switch (state) {
    case LargePayloadGenerationState::absent: return "absent";
    case LargePayloadGenerationState::active: return "active";
    case LargePayloadGenerationState::retired: return "retired";
    case LargePayloadGenerationState::gc_reclaimed: return "gc_reclaimed";
    case LargePayloadGenerationState::quarantine: return "quarantine";
  }
  return "absent";
}

LargePayloadFamily LargePayloadFamilyFromName(const std::string& name) {
  if (name == "document") { return LargePayloadFamily::document; }
  if (name == "key_value") { return LargePayloadFamily::key_value; }
  if (name == "vector") { return LargePayloadFamily::vector; }
  if (name == "text") { return LargePayloadFamily::text; }
  if (name == "graph") { return LargePayloadFamily::graph; }
  return LargePayloadFamily::blob;
}

std::string SerializeLargePayloadDescriptor(const LargePayloadDescriptor& descriptor) {
  std::ostringstream out;
  out << "SB_LARGE_PAYLOAD_DESCRIPTOR_V1"
      << ";family=" << LargePayloadFamilyName(descriptor.family)
      << ";payload_uuid=" << scratchbird::core::uuid::UuidToString(descriptor.payload_uuid.value)
      << ";owner_object_uuid=" << scratchbird::core::uuid::UuidToString(descriptor.owner_object_uuid.value)
      << ";generation_scope_uuid=" << scratchbird::core::uuid::UuidToString(descriptor.generation_scope_uuid.value)
      << ";filespace_uuid=" << scratchbird::core::uuid::UuidToString(descriptor.filespace_uuid.value)
      << ";overflow_value_uuid=" << scratchbird::core::uuid::UuidToString(descriptor.overflow_value_uuid.value)
      << ";generation=" << descriptor.generation
      << ";creator_local_tx=" << descriptor.creator_local_transaction_id
      << ";retired_by_local_tx=" << descriptor.retired_by_local_transaction_id
      << ";byte_count=" << descriptor.byte_count
      << ";content_hash=" << descriptor.content_hash
      << ";filespace_class=" << descriptor.filespace_class
      << ";page_family=" << descriptor.page_family
      << ";inline_payload=" << (descriptor.inline_payload ? "1" : "0");
  return out.str();
}

std::optional<LargePayloadDescriptor> ParseLargePayloadDescriptor(const std::string& text) {
  if (text.rfind("SB_LARGE_PAYLOAD_DESCRIPTOR_V1", 0) != 0) {
    return std::nullopt;
  }
  const auto fields = ParseDescriptorFields(text);
  LargePayloadDescriptor descriptor;
  const auto family = fields.find("family");
  if (family != fields.end()) { descriptor.family = LargePayloadFamilyFromName(family->second); }
  auto payload_uuid = fields.find("payload_uuid");
  if (payload_uuid != fields.end()) {
    const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::object, payload_uuid->second);
    if (parsed.ok()) { descriptor.payload_uuid = parsed.value; }
  }
  auto owner_uuid = fields.find("owner_object_uuid");
  if (owner_uuid != fields.end()) {
    const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::object, owner_uuid->second);
    if (parsed.ok()) { descriptor.owner_object_uuid = parsed.value; }
  }
  auto generation_scope_uuid = fields.find("generation_scope_uuid");
  if (generation_scope_uuid != fields.end()) {
    auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::row, generation_scope_uuid->second);
    if (!parsed.ok()) {
      parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::object, generation_scope_uuid->second);
    }
    if (parsed.ok()) { descriptor.generation_scope_uuid = parsed.value; }
  }
  auto filespace_uuid = fields.find("filespace_uuid");
  if (filespace_uuid != fields.end()) {
    const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::filespace, filespace_uuid->second);
    if (parsed.ok()) { descriptor.filespace_uuid = parsed.value; }
  }
  auto overflow_uuid = fields.find("overflow_value_uuid");
  if (overflow_uuid != fields.end()) {
    const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(UuidKind::object, overflow_uuid->second);
    if (parsed.ok()) { descriptor.overflow_value_uuid = parsed.value; }
  }
  descriptor.generation = ParseU64(fields.count("generation") ? fields.at("generation") : "");
  descriptor.creator_local_transaction_id = ParseU64(fields.count("creator_local_tx") ? fields.at("creator_local_tx") : "");
  descriptor.retired_by_local_transaction_id = ParseU64(fields.count("retired_by_local_tx") ? fields.at("retired_by_local_tx") : "");
  descriptor.byte_count = ParseU64(fields.count("byte_count") ? fields.at("byte_count") : "");
  if (fields.count("content_hash")) { descriptor.content_hash = fields.at("content_hash"); }
  if (fields.count("filespace_class")) { descriptor.filespace_class = fields.at("filespace_class"); }
  if (fields.count("page_family")) { descriptor.page_family = fields.at("page_family"); }
  if (fields.count("inline_payload")) { descriptor.inline_payload = fields.at("inline_payload") == "1"; }
  if (!descriptor.payload_uuid.valid() || descriptor.generation == 0 || descriptor.byte_count == 0) {
    return std::nullopt;
  }
  if (!descriptor.generation_scope_uuid.valid()) {
    descriptor.generation_scope_uuid = descriptor.owner_object_uuid;
  }
  if (!descriptor.inline_payload &&
      (!descriptor.owner_object_uuid.valid() || !descriptor.filespace_uuid.valid() ||
       !descriptor.overflow_value_uuid.valid() || descriptor.filespace_class.empty())) {
    return std::nullopt;
  }
  return descriptor;
}

LargePayloadStoreResult StoreLargePayloadGeneration(LargePayloadStore* store,
                                                    const LargePayloadStoreRequest& request) {
  if (store == nullptr) {
    return RefuseStore(nullptr,
                       request,
                       "large_payload_store_missing_store",
                       "storage.page.large_payload.missing_store",
                       "large payload store is required");
  }
  if (!request.database_uuid.valid() || !request.filespace_uuid.valid() ||
      !request.owner_object_uuid.valid() || !request.transaction_uuid.valid()) {
    return RefuseStore(store,
                       request,
                       "large_payload_store_invalid_identity",
                       "storage.page.large_payload.invalid_identity",
                       "database, filespace, owner, and transaction UUIDs are required");
  }
  if (request.local_transaction_id == 0 || !request.mga_write_admitted_by_transaction_inventory) {
    return RefuseStore(store,
                       request,
                       "large_payload_store_mga_authority_required",
                       "storage.page.large_payload.mga_authority_required",
                       "large payload writes require durable transaction inventory admission");
  }
  if (request.payload_bytes.empty()) {
    return RefuseStore(store,
                       request,
                       "large_payload_store_empty_payload",
                       "storage.page.large_payload.empty_payload",
                       "payload bytes must be non-empty");
  }

  const bool inline_payload = request.allow_inline_payload &&
                              request.payload_bytes.size() <= request.inline_threshold_bytes;
  LargePayloadDescriptor descriptor;
  descriptor.payload_uuid = GeneratedId(UuidKind::object, 300000 + store->next_evidence_sequence);
  descriptor.owner_object_uuid = request.owner_object_uuid;
  descriptor.generation_scope_uuid = request.generation_scope_uuid.valid()
                                         ? request.generation_scope_uuid
                                         : request.owner_object_uuid;
  descriptor.filespace_uuid = request.filespace_uuid;
  descriptor.family = request.family;
  descriptor.generation =
      NextGenerationForOwner(*store, descriptor.generation_scope_uuid, request.family);
  descriptor.creator_local_transaction_id = request.local_transaction_id;
  descriptor.byte_count = request.payload_bytes.size();
  descriptor.content_hash = HashPayload(request.payload_bytes);
  descriptor.inline_payload = inline_payload;

  if (inline_payload) {
    descriptor.filespace_class = "inline_hot_payload";
    descriptor.page_family = "row_data";
    descriptor.inline_text = BytesToString(request.payload_bytes);
  } else {
    descriptor.page_family = PageFamilyForLargePayloadFamily(request.family);
    filespace::FilespaceClassRequest class_request;
    class_request.database_uuid = request.database_uuid;
    class_request.filespace_uuid = request.filespace_uuid;
    class_request.owner_object_uuid = request.owner_object_uuid;
    class_request.object_class = filespace::FilespaceObjectClass::large_blob;
    class_request.page_family = descriptor.page_family;
    class_request.reason = "large_payload_separation";
    class_request.explicit_object_class = true;
    const auto decision = filespace::ResolveFilespaceClass(class_request);
    if (!decision.ok() || decision.filespace_class != filespace::FilespaceClass::large_blob) {
      return RefuseStore(store,
                         request,
                         "large_payload_store_filespace_class_refused",
                         "storage.page.large_payload.filespace_class_refused",
                         decision.diagnostic.diagnostic_code);
    }
    descriptor.filespace_class = filespace::FilespaceClassName(decision.filespace_class);

    OverflowPersistRequest overflow_request;
    overflow_request.row_uuid = request.owner_object_uuid;
    overflow_request.object_uuid = request.owner_object_uuid;
    overflow_request.transaction_uuid = request.transaction_uuid;
    overflow_request.local_transaction_id = request.local_transaction_id;
    overflow_request.generation = descriptor.generation;
    overflow_request.value_descriptor = SerializeLargePayloadDescriptor(descriptor);
    overflow_request.payload_bytes = request.payload_bytes;
    overflow_request.chunk_policy_uuid = request.chunk_policy_uuid.valid()
                                             ? request.chunk_policy_uuid
                                             : request.owner_object_uuid;
    overflow_request.chunk_size = request.chunk_size == 0 ? 4096 : request.chunk_size;
    const auto persisted = PersistOverflowValue(&store->overflow_ledger, overflow_request);
    if (!persisted.ok()) {
      return RefuseStore(store,
                         request,
                         "large_payload_store_overflow_refused",
                         "storage.page.large_payload.overflow_refused",
                         persisted.diagnostic.diagnostic_code);
    }
    OverflowCommitRequest overflow_commit;
    overflow_commit.overflow_value_uuid = persisted.overflow_value_uuid;
    overflow_commit.transaction_uuid = request.transaction_uuid;
    overflow_commit.local_transaction_id = request.local_transaction_id;
    overflow_commit.reason =
        "large payload generation follows engine MGA transaction inventory admission";
    const auto committed = CommitOverflowValue(&store->overflow_ledger, overflow_commit);
    if (!committed.ok()) {
      return RefuseStore(store,
                         request,
                         "large_payload_store_overflow_commit_refused",
                         "storage.page.large_payload.overflow_commit_refused",
                         committed.diagnostic.diagnostic_code);
    }
    descriptor.overflow_value_uuid = persisted.overflow_value_uuid;
  }

  if (request.retire_previous_generations) {
    for (auto& record : store->generations) {
      if (record.state == LargePayloadGenerationState::active &&
          record.descriptor.family == request.family &&
          SameUuid(record.descriptor.generation_scope_uuid,
                   descriptor.generation_scope_uuid)) {
        record.state = LargePayloadGenerationState::retired;
        record.descriptor.retired_by_local_transaction_id = request.local_transaction_id;
      }
    }
  }

  LargePayloadGenerationRecord record;
  record.descriptor = descriptor;
  record.state = LargePayloadGenerationState::active;
  if (inline_payload) {
    record.payload_bytes = request.payload_bytes;
  }
  store->generations.push_back(record);

  LargePayloadStoreResult result;
  result.status = LargePayloadOkStatus();
  result.stored = true;
  result.descriptor_only = !inline_payload;
  result.descriptor = descriptor;
  result.evidence = BuildEvidence(store,
                                  descriptor,
                                  inline_payload ? "store_inline_payload" : "store_large_payload_descriptor",
                                  "ok",
                                  request.reason.empty()
                                      ? "diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory"
                                      : request.reason,
                                  true);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                "ok",
                                                "storage.page.large_payload.stored",
                                                inline_payload ? "payload stored inline" : "large payload stored by pointer descriptor");
  store->evidence.push_back(result.evidence);
  return result;
}

LargePayloadReadResult ReadLargePayloadGeneration(LargePayloadStore* store,
                                                  const LargePayloadReadRequest& request) {
  if (store == nullptr) {
    return RefuseRead(nullptr,
                      request.descriptor,
                      "large_payload_read_missing_store",
                      "storage.page.large_payload.read_missing_store",
                      "large payload store is required");
  }
  auto* record = FindMutable(store, request.descriptor);
  if (record == nullptr) {
    return RefuseRead(store,
                      request.descriptor,
                      "large_payload_read_generation_not_found",
                      "storage.page.large_payload.generation_not_found",
                      "payload generation was not found");
  }
  if (!DescriptorMatchesRecord(request.descriptor, *record)) {
    return RefuseRead(store,
                      request.descriptor,
                      "large_payload_read_descriptor_mismatch",
                      "storage.page.large_payload.descriptor_mismatch",
                      "payload descriptor does not match stored generation");
  }
  if (!VisibleAtSnapshot(*record, request.observer_snapshot_visible_through_local_transaction_id)) {
    return RefuseRead(store,
                      request.descriptor,
                      "large_payload_read_not_visible_fail_closed",
                      "storage.page.large_payload.not_visible_fail_closed",
                      LargePayloadGenerationStateName(record->state));
  }

  LargePayloadReadResult result;
  result.status = LargePayloadOkStatus();
  result.found = true;
  result.visible = true;
  if (request.use_cache) {
    if (auto* cache_entry = FindCacheEntry(&store->cache, request.descriptor)) {
      ++store->cache.hit_count;
      result.cache_hit = true;
      result.payload_bytes = cache_entry->payload_bytes;
    } else {
      ++store->cache.miss_count;
      result.cache_hit = false;
      auto loaded = ReadCommittedPayloadBytes(store, request.descriptor, *record);
      if (!loaded.ok()) {
        return loaded;
      }
      result.payload_bytes = std::move(loaded.payload_bytes);
      if (request.prefetch_on_miss) {
        ++store->cache.prefetch_request_count;
        ++store->cache.prefetch_loaded_count;
        PutCacheEntry(&store->cache, request.descriptor, result.payload_bytes);
      }
    }
  } else {
    auto loaded = ReadCommittedPayloadBytes(store, request.descriptor, *record);
    if (!loaded.ok()) {
      return loaded;
    }
    result.payload_bytes = std::move(loaded.payload_bytes);
  }
  if (HashPayload(result.payload_bytes) != record->descriptor.content_hash) {
    return RefuseRead(store,
                      request.descriptor,
                      "large_payload_read_hash_mismatch",
                      "storage.page.large_payload.hash_mismatch",
                      "payload hash mismatch");
  }
  result.evidence = BuildEvidence(store,
                                  request.descriptor,
                                  result.cache_hit ? "read_large_payload_cache_hit" : "read_large_payload_cache_miss",
                                  "ok",
                                  request.reason.empty()
                                      ? "diagnostic_only=true;cache_evidence_not_visibility_authority"
                                      : request.reason,
                                  false);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                "ok",
                                                "storage.page.large_payload.read",
                                                result.cache_hit ? "cache hit" : "cache miss");
  store->evidence.push_back(result.evidence);
  return result;
}

LargePayloadPrefetchResult PrefetchLargePayloadGenerations(LargePayloadStore* store,
                                                           const LargePayloadPrefetchRequest& request) {
  LargePayloadPrefetchResult result;
  if (store == nullptr) {
    result.status = LargePayloadErrorStatus();
    result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                  "large_payload_prefetch_missing_store",
                                                  "storage.page.large_payload.prefetch_missing_store",
                                                  "large payload store is required");
    return result;
  }
  result.status = LargePayloadOkStatus();
  result.requested_count = request.descriptors.size();
  store->cache.prefetch_request_count += request.descriptors.size();
  for (const auto& descriptor : request.descriptors) {
    auto* record = FindMutable(store, descriptor);
    if (record == nullptr || !DescriptorMatchesRecord(descriptor, *record) ||
        !VisibleAtSnapshot(*record, request.observer_snapshot_visible_through_local_transaction_id)) {
      ++result.refused_count;
      continue;
    }
    auto loaded = ReadCommittedPayloadBytes(store, descriptor, *record);
    if (!loaded.ok()) {
      ++result.refused_count;
      continue;
    }
    PutCacheEntry(&store->cache, descriptor, loaded.payload_bytes);
    ++store->cache.prefetch_loaded_count;
    ++result.loaded_count;
  }
  LargePayloadDescriptor evidence_descriptor;
  if (!request.descriptors.empty()) {
    evidence_descriptor = request.descriptors.front();
  }
  result.evidence = BuildEvidence(store,
                                  evidence_descriptor,
                                  "prefetch_large_payload",
                                  "ok",
                                  request.reason.empty()
                                      ? "diagnostic_only=true;prefetch_evidence_not_visibility_authority"
                                      : request.reason,
                                  false);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                "ok",
                                                "storage.page.large_payload.prefetch",
                                                "large payload prefetch evaluated");
  store->evidence.push_back(result.evidence);
  return result;
}

LargePayloadRetireResult RetireLargePayloadGeneration(LargePayloadStore* store,
                                                      const LargePayloadRetireRequest& request) {
  if (store == nullptr) {
    return RefuseRetire(nullptr,
                        request.descriptor,
                        "large_payload_retire_missing_store",
                        "storage.page.large_payload.retire_missing_store",
                        "large payload store is required");
  }
  if (request.retiring_local_transaction_id == 0 ||
      !request.mga_write_admitted_by_transaction_inventory) {
    return RefuseRetire(store,
                        request.descriptor,
                        "large_payload_retire_mga_authority_required",
                        "storage.page.large_payload.retire_mga_authority_required",
                        "large payload retirement requires durable transaction inventory admission");
  }
  auto* record = FindMutable(store, request.descriptor);
  if (record == nullptr) {
    return RefuseRetire(store,
                        request.descriptor,
                        "large_payload_retire_generation_not_found",
                        "storage.page.large_payload.retire_generation_not_found",
                        "payload generation was not found");
  }
  if (!DescriptorMatchesRecord(request.descriptor, *record)) {
    return RefuseRetire(store,
                        request.descriptor,
                        "large_payload_retire_descriptor_mismatch",
                        "storage.page.large_payload.retire_descriptor_mismatch",
                        "payload descriptor does not match stored generation");
  }
  if (record->state != LargePayloadGenerationState::active) {
    return RefuseRetire(store,
                        request.descriptor,
                        "large_payload_retire_not_active_fail_closed",
                        "storage.page.large_payload.retire_not_active_fail_closed",
                        LargePayloadGenerationStateName(record->state));
  }

  record->state = LargePayloadGenerationState::retired;
  record->descriptor.retired_by_local_transaction_id = request.retiring_local_transaction_id;
  InvalidateCacheEntry(&store->cache, request.descriptor);

  LargePayloadRetireResult result;
  result.status = LargePayloadOkStatus();
  result.retired = true;
  result.descriptor = record->descriptor;
  result.evidence = BuildEvidence(store,
                                  record->descriptor,
                                  "retire_large_payload_generation",
                                  "ok",
                                  request.reason.empty()
                                      ? "diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory"
                                      : request.reason,
                                  true);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                "ok",
                                                "storage.page.large_payload.retired",
                                                "large payload generation retired by MGA transaction");
  store->evidence.push_back(result.evidence);
  return result;
}

LargePayloadGcResult CollectLargePayloadGarbage(LargePayloadStore* store,
                                                const LargePayloadGcRequest& request) {
  LargePayloadGcResult result;
  if (store == nullptr || !request.cleanup_horizon_authoritative ||
      request.authoritative_mga_horizon_local_transaction_id == 0) {
    result.status = LargePayloadErrorStatus();
    result.fail_closed_count = 1;
    result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                  "large_payload_gc_mga_horizon_required",
                                                  "storage.page.large_payload.gc_mga_horizon_required",
                                                  "GC requires authoritative MGA cleanup horizon");
    return result;
  }

  result.status = LargePayloadOkStatus();
  LargePayloadDescriptor evidence_descriptor;
  for (auto& record : store->generations) {
    if (!evidence_descriptor.payload_uuid.valid()) {
      evidence_descriptor = record.descriptor;
    }
    if (record.state == LargePayloadGenerationState::active) {
      ++result.retained_count;
      continue;
    }
    if (record.state == LargePayloadGenerationState::retired) {
      if (record.descriptor.retired_by_local_transaction_id == 0) {
        record.state = LargePayloadGenerationState::quarantine;
        ++result.fail_closed_count;
        continue;
      }
      if (record.descriptor.retired_by_local_transaction_id <
          request.authoritative_mga_horizon_local_transaction_id) {
        record.state = LargePayloadGenerationState::gc_reclaimed;
        record.payload_bytes.clear();
        InvalidateCacheEntry(&store->cache, record.descriptor);
        ++result.reclaimed_count;
      } else {
        ++result.retained_count;
      }
      continue;
    }
    if (record.state == LargePayloadGenerationState::quarantine) {
      ++result.fail_closed_count;
    }
  }
  result.evidence = BuildEvidence(store,
                                  evidence_descriptor,
                                  "gc_large_payload_generations",
                                  "ok",
                                  request.reason.empty()
                                      ? "diagnostic_only=true;mga_horizon_bounds_gc=true"
                                      : request.reason,
                                  result.reclaimed_count != 0,
                                  request.authoritative_mga_horizon_local_transaction_id);
  result.diagnostic = MakeLargePayloadDiagnostic(result.status,
                                                "ok",
                                                "storage.page.large_payload.gc",
                                                "large payload GC evaluated");
  store->evidence.push_back(result.evidence);
  return result;
}

const LargePayloadGenerationRecord* FindLargePayloadGeneration(const LargePayloadStore& store,
                                                               const LargePayloadDescriptor& descriptor) {
  const auto found = std::find_if(store.generations.begin(),
                                  store.generations.end(),
                                  [&](const LargePayloadGenerationRecord& record) {
                                    return SameUuid(record.descriptor.payload_uuid, descriptor.payload_uuid) &&
                                           record.descriptor.generation == descriptor.generation;
                                  });
  return found == store.generations.end() ? nullptr : &(*found);
}

DiagnosticRecord MakeLargePayloadDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.large_payload",
                        status.ok() ? "" : "fail closed and retry after engine MGA authority is available");
}

}  // namespace scratchbird::storage::page
