// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "cloud/external_effect_outbox.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLOUD_EDGE_CACHE_CDN
// Edge cache/CDN invalidation is an advisory external-effect export. It never
// supplies transaction, visibility, durability, recovery, or security authority.

struct EngineEdgeCacheCdnLimits {
  std::uint64_t max_tags_per_object = 32;
  std::uint64_t max_tags_per_invalidation = 64;
  std::uint64_t max_pending_outbox_records = 1024;
};

struct EngineEdgeProviderProfile {
  std::string provider_profile_uuid;
  std::string provider_family = "local_signed_stream";
  std::string provider_name = "local_signed_stream";
  std::vector<std::string> supported_purge_modes = {"purge", "soft_purge", "revalidate"};
  std::string tag_charset = "safe_ascii";
  std::uint64_t tag_max_length = 128;
  std::uint64_t max_tag_count = 64;
  std::uint64_t max_pending_outbox_records = 1024;
  std::uint64_t max_attempts = 3;
  std::uint64_t retry_backoff_ms = 1000;
  bool supported = true;
  bool provider_available = true;
  bool signature_required = true;
  std::string signature_algorithm = "scratchbird-edge-signature-v1";
  std::string signature_key_ref;
  bool ack_required = false;
};

struct EngineEdgeCacheTagRegistration {
  std::string cache_tag_descriptor_uuid;
  std::string cache_tag_id;
  std::string tag_class;
  std::string dependency_scope;
  std::string internal_dependency_ref;
  std::string redaction_policy_uuid;
  std::string finality_mode;
  std::uint64_t ttl_ms = 0;
  bool ttl_present = false;
  bool purge_required_flag = false;
  std::string external_provider_profile_uuid;
  std::string object_scope_key;
  bool allow_raw_uuid_export = false;
  bool allow_branch_global_cache = false;
  EngineExternalEffectCommitEvidence creation_commit_evidence;
};

struct EngineEdgeCacheTagRecord {
  std::string cache_tag_descriptor_uuid;
  std::string cache_tag_id;
  std::string tag_class;
  std::string dependency_scope;
  std::string redacted_dependency_ref;
  std::string redaction_policy_uuid;
  std::string finality_mode;
  std::uint64_t ttl_ms = 0;
  bool ttl_present = false;
  bool purge_required_flag = false;
  std::string external_provider_profile_uuid;
  std::string object_scope_key;
  std::string created_by_transaction_uuid;
  std::uint64_t created_by_local_transaction_id = 0;
  bool emitted_only_after_commit_evidence = true;
};

struct EngineEdgeInvalidationRequest {
  EngineExternalEffectCommitEvidence commit_evidence;
  std::vector<std::string> cache_tag_ids;
  std::string event_class = "record";
  std::string finality_mode;
  std::uint64_t content_epoch_before = 0;
  bool content_epoch_before_present = false;
  std::uint64_t content_epoch_after = 0;
  bool content_epoch_after_present = false;
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::string purge_mode = "purge";
  std::string blocking_policy = "async_retry";
  std::string redaction_policy_uuid;
  std::string provider_profile_uuid;
  std::string source_object_ref;
  std::uint64_t now_epoch_ms = 0;
};

struct EngineEdgeInvalidationRecord {
  std::string edge_invalidation_uuid;
  std::vector<std::string> cache_tag_id_vector;
  std::string event_class;
  std::string finality_mode;
  std::uint64_t content_epoch_before = 0;
  bool content_epoch_before_present = false;
  std::uint64_t content_epoch_after = 0;
  bool content_epoch_after_present = false;
  std::string purge_mode;
  std::string blocking_policy;
  std::string redaction_policy_uuid;
  std::string payload_hash;
  std::string provider_profile_uuid;
  std::string idempotency_key;
  std::string stream_sequence;
  std::string signature_algorithm;
  std::string signature_key_ref;
  std::string signature_value;
  std::string redacted_payload_metadata;
  std::string outbox_event_uuid;
  std::string source_transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t transaction_inventory_generation = 0;
  bool emitted_after_commit_evidence = false;
  bool payload_redacted = true;
  std::string delivery_state = "pending";
  std::string last_diagnostic_code;
};

struct EngineEdgeDispatchRequest {
  std::string provider_profile_uuid;
  std::uint64_t now_epoch_ms = 0;
  std::uint64_t max_records = 64;
};

struct EngineEdgeCacheCdnResult {
  bool ok = false;
  bool deduplicated = false;
  bool tag_registered = false;
  bool invalidation_queued = false;
  bool delivery_attempted = false;
  EngineEdgeProviderProfile provider;
  EngineEdgeCacheTagRecord tag;
  EngineEdgeInvalidationRecord invalidation;
  EngineExternalEffectOutboxRecord outbox_record;
  std::vector<EngineEdgeCacheTagRecord> tags;
  std::vector<EngineEdgeInvalidationRecord> invalidations;
  std::vector<EngineApiDiagnostic> diagnostics;
};

void ConfigureEdgeCacheCdnLimits(const EngineEdgeCacheCdnLimits& limits);
EngineEdgeCacheCdnResult RegisterEdgeProviderProfile(
    const EngineEdgeProviderProfile& profile);
EngineEdgeCacheCdnResult RegisterEdgeCacheTag(
    const EngineEdgeCacheTagRegistration& registration);
EngineEdgeCacheCdnResult QueueEdgeCacheInvalidationAfterCommit(
    const EngineEdgeInvalidationRequest& request);
EngineEdgeCacheCdnResult DispatchEdgeCacheInvalidations(
    const EngineEdgeDispatchRequest& request);
EngineEdgeCacheCdnResult InspectEdgeCacheTags();
EngineEdgeCacheCdnResult InspectEdgeInvalidations();
void ResetEdgeCacheCdnStateForTests();

}  // namespace scratchbird::engine::internal_api
