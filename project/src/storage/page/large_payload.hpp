// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-LARGE-PAYLOAD-SEPARATION-ANCHOR
#include "filespace_lifecycle.hpp"
#include "overflow_persistence.hpp"
#include "runtime_platform.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u64 kLargePayloadDefaultInlineThresholdBytes = 4096;

enum class LargePayloadFamily : u32 {
  document,
  key_value,
  vector,
  text,
  blob,
  graph
};

enum class LargePayloadGenerationState : u32 {
  absent,
  active,
  retired,
  gc_reclaimed,
  quarantine
};

struct LargePayloadDescriptor {
  TypedUuid payload_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid generation_scope_uuid;
  TypedUuid filespace_uuid;
  TypedUuid overflow_value_uuid;
  LargePayloadFamily family = LargePayloadFamily::blob;
  u64 generation = 0;
  u64 creator_local_transaction_id = 0;
  u64 retired_by_local_transaction_id = 0;
  u64 byte_count = 0;
  std::string content_hash;
  std::string filespace_class;
  std::string page_family;
  bool inline_payload = false;
  std::string inline_text;
};

struct LargePayloadGenerationRecord {
  LargePayloadDescriptor descriptor;
  LargePayloadGenerationState state = LargePayloadGenerationState::absent;
  std::vector<byte> payload_bytes;
};

struct LargePayloadEvidenceRecord {
  u64 sequence = 0;
  std::string action;
  TypedUuid payload_uuid;
  LargePayloadFamily family = LargePayloadFamily::blob;
  u64 generation = 0;
  u64 local_transaction_id = 0;
  u64 authoritative_mga_horizon_local_transaction_id = 0;
  u64 cache_hits = 0;
  u64 cache_misses = 0;
  u64 prefetch_requests = 0;
  u64 prefetch_loaded = 0;
  std::string evidence_token;
  std::string reason;
  std::string diagnostic_code;
  bool diagnostic_only = true;
  bool finality_authority = false;
  bool visibility_authority = false;
  bool durable_state_changed = false;
};

struct LargePayloadCacheEntry {
  TypedUuid payload_uuid;
  u64 generation = 0;
  std::vector<byte> payload_bytes;
  std::string content_hash;
  bool valid = false;
};

struct LargePayloadCache {
  std::vector<LargePayloadCacheEntry> entries;
  u64 hit_count = 0;
  u64 miss_count = 0;
  u64 prefetch_request_count = 0;
  u64 prefetch_loaded_count = 0;
};

struct LargePayloadStore {
  std::vector<LargePayloadGenerationRecord> generations;
  OverflowLedger overflow_ledger;
  LargePayloadCache cache;
  std::vector<LargePayloadEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct LargePayloadStoreRequest {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid generation_scope_uuid;
  TypedUuid transaction_uuid;
  TypedUuid chunk_policy_uuid;
  u64 local_transaction_id = 0;
  LargePayloadFamily family = LargePayloadFamily::blob;
  std::vector<byte> payload_bytes;
  u64 inline_threshold_bytes = kLargePayloadDefaultInlineThresholdBytes;
  u32 chunk_size = 4096;
  std::string reason;
  bool allow_inline_payload = true;
  bool retire_previous_generations = true;
  bool mga_write_admitted_by_transaction_inventory = false;
};

struct LargePayloadStoreResult {
  Status status;
  bool stored = false;
  bool descriptor_only = false;
  LargePayloadDescriptor descriptor;
  LargePayloadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && stored; }
};

struct LargePayloadReadRequest {
  LargePayloadDescriptor descriptor;
  u64 observer_snapshot_visible_through_local_transaction_id = 0;
  bool use_cache = true;
  bool prefetch_on_miss = false;
  std::string reason;
};

struct LargePayloadReadResult {
  Status status;
  bool found = false;
  bool visible = false;
  bool cache_hit = false;
  std::vector<byte> payload_bytes;
  LargePayloadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && found && visible; }
};

struct LargePayloadPrefetchRequest {
  std::vector<LargePayloadDescriptor> descriptors;
  u64 observer_snapshot_visible_through_local_transaction_id = 0;
  std::string reason;
};

struct LargePayloadPrefetchResult {
  Status status;
  u64 requested_count = 0;
  u64 loaded_count = 0;
  u64 refused_count = 0;
  LargePayloadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct LargePayloadGcRequest {
  u64 authoritative_mga_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  std::string reason;
};

struct LargePayloadGcResult {
  Status status;
  u64 reclaimed_count = 0;
  u64 retained_count = 0;
  u64 fail_closed_count = 0;
  LargePayloadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct LargePayloadRetireRequest {
  LargePayloadDescriptor descriptor;
  u64 retiring_local_transaction_id = 0;
  bool mga_write_admitted_by_transaction_inventory = false;
  std::string reason;
};

struct LargePayloadRetireResult {
  Status status;
  bool retired = false;
  LargePayloadDescriptor descriptor;
  LargePayloadEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && retired; }
};

const char* LargePayloadFamilyName(LargePayloadFamily family);
const char* LargePayloadGenerationStateName(LargePayloadGenerationState state);
LargePayloadFamily LargePayloadFamilyFromName(const std::string& name);
std::string SerializeLargePayloadDescriptor(const LargePayloadDescriptor& descriptor);
std::optional<LargePayloadDescriptor> ParseLargePayloadDescriptor(const std::string& text);

LargePayloadStoreResult StoreLargePayloadGeneration(LargePayloadStore* store,
                                                    const LargePayloadStoreRequest& request);
LargePayloadReadResult ReadLargePayloadGeneration(LargePayloadStore* store,
                                                  const LargePayloadReadRequest& request);
LargePayloadPrefetchResult PrefetchLargePayloadGenerations(LargePayloadStore* store,
                                                           const LargePayloadPrefetchRequest& request);
LargePayloadRetireResult RetireLargePayloadGeneration(LargePayloadStore* store,
                                                      const LargePayloadRetireRequest& request);
LargePayloadGcResult CollectLargePayloadGarbage(LargePayloadStore* store,
                                                const LargePayloadGcRequest& request);
const LargePayloadGenerationRecord* FindLargePayloadGeneration(const LargePayloadStore& store,
                                                               const LargePayloadDescriptor& descriptor);
DiagnosticRecord MakeLargePayloadDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::storage::page
