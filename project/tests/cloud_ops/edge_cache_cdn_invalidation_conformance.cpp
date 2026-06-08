// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/edge_cache_cdn.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const api::EngineEdgeCacheCdnResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

api::EngineExternalEffectCommitEvidence CommitEvidence(bool committed) {
  api::EngineExternalEffectCommitEvidence evidence;
  evidence.transaction_uuid = "txn-edge-cache-committed";
  evidence.local_transaction_id = committed ? 77 : 0;
  evidence.transaction_inventory_generation = committed ? 12 : 0;
  evidence.commit_evidence_hash = committed ? "mga-commit-proof-hash-77" : "";
  evidence.finality_mode = "cluster_final";
  evidence.mga_commit_visible = committed;
  evidence.durable_commit_evidence = committed;
  return evidence;
}

api::EngineEdgeProviderProfile Provider(bool available) {
  api::EngineEdgeProviderProfile provider;
  provider.provider_profile_uuid = "edge-provider-local-signed-v1";
  provider.provider_family = "local_signed_stream";
  provider.provider_name = "local signed stream";
  provider.supported_purge_modes = {"purge", "soft_purge", "revalidate"};
  provider.max_tag_count = 2;
  provider.max_pending_outbox_records = 1;
  provider.max_attempts = 3;
  provider.retry_backoff_ms = 250;
  provider.provider_available = available;
  provider.signature_required = true;
  provider.signature_algorithm = "scratchbird-edge-signature-v1";
  provider.signature_key_ref = "kms-ref://edge-cache-signer";
  return provider;
}

api::EngineEdgeCacheTagRegistration TagRegistration(std::string tag_id) {
  api::EngineEdgeCacheTagRegistration tag;
  tag.cache_tag_descriptor_uuid = "edge-tag-descriptor-orders";
  tag.cache_tag_id = std::move(tag_id);
  tag.tag_class = "database_derived_page";
  tag.dependency_scope = "record";
  tag.internal_dependency_ref = "019e2000-0000-7000-8000-0000000000aa";
  tag.redaction_policy_uuid = "redaction-policy-safe-v1";
  tag.finality_mode = "cluster_final";
  tag.purge_required_flag = true;
  tag.external_provider_profile_uuid = "edge-provider-local-signed-v1";
  tag.object_scope_key = "orders-redacted-object";
  tag.creation_commit_evidence = CommitEvidence(true);
  return tag;
}

api::EngineEdgeInvalidationRequest InvalidationRequest(bool committed) {
  api::EngineEdgeInvalidationRequest request;
  request.commit_evidence = CommitEvidence(committed);
  request.cache_tag_ids = {"tag:orders:public:v1"};
  request.event_class = "record";
  request.finality_mode = "cluster_final";
  request.content_epoch_before = 41;
  request.content_epoch_before_present = true;
  request.content_epoch_after = 42;
  request.content_epoch_after_present = true;
  request.schema_epoch = 7;
  request.security_epoch = 8;
  request.resource_epoch = 9;
  request.route_epoch = 10;
  request.purge_mode = "purge";
  request.blocking_policy = "async_retry";
  request.redaction_policy_uuid = "redaction-policy-safe-v1";
  request.provider_profile_uuid = "edge-provider-local-signed-v1";
  request.source_object_ref = "redacted-object:orders";
  request.now_epoch_ms = 1000;
  return request;
}

}  // namespace

int main() {
  api::ResetEdgeCacheCdnStateForTests();

  api::EngineEdgeCacheCdnLimits limits;
  limits.max_tags_per_object = 2;
  limits.max_tags_per_invalidation = 2;
  limits.max_pending_outbox_records = 1;
  api::ConfigureEdgeCacheCdnLimits(limits);

  const auto provider = api::RegisterEdgeProviderProfile(Provider(false));
  Require(provider.ok, "edge provider profile registration failed");

  auto unsafe_tag = TagRegistration("019e2000-0000-7000-8000-0000000000bb");
  const auto unsafe_tag_result = api::RegisterEdgeCacheTag(unsafe_tag);
  Require(!unsafe_tag_result.ok &&
              HasDiagnostic(unsafe_tag_result, "SB-EDGE-TAG-UNSAFE"),
          "raw UUID edge tag was not refused");

  const auto tag = api::RegisterEdgeCacheTag(TagRegistration("tag:orders:public:v1"));
  Require(tag.ok && tag.tag_registered, "safe edge cache tag registration failed");
  Require(tag.tag.redacted_dependency_ref.find("019e2000") == std::string::npos,
          "edge tag exposed raw dependency UUID");

  const auto precommit = api::QueueEdgeCacheInvalidationAfterCommit(
      InvalidationRequest(false));
  Require(!precommit.ok &&
              HasDiagnostic(precommit, "SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED"),
          "pre-commit invalidation was not refused");
  Require(api::InspectEdgeInvalidations().invalidations.empty(),
          "pre-commit invalidation created an event");

  const auto queued = api::QueueEdgeCacheInvalidationAfterCommit(
      InvalidationRequest(true));
  Require(queued.ok && queued.invalidation_queued,
          "post-commit edge invalidation was not queued");
  Require(queued.invalidation.emitted_after_commit_evidence,
          "edge invalidation lacks after-commit evidence flag");
  Require(!queued.invalidation.signature_value.empty(),
          "edge invalidation signature metadata missing");
  Require(queued.invalidation.redacted_payload_metadata.find("019e2000") ==
              std::string::npos,
          "edge provider payload exposed raw dependency UUID");
  Require(queued.invalidation.redacted_payload_metadata.find("secret") ==
              std::string::npos,
          "edge provider payload exposed secret-shaped material");

  const auto duplicate = api::QueueEdgeCacheInvalidationAfterCommit(
      InvalidationRequest(true));
  Require(duplicate.ok && duplicate.deduplicated,
          "duplicate edge invalidation was not idempotently deduplicated");

  auto second_request = InvalidationRequest(true);
  second_request.content_epoch_after = 43;
  const auto backpressure = api::QueueEdgeCacheInvalidationAfterCommit(second_request);
  Require(!backpressure.ok &&
              HasDiagnostic(backpressure, "SB-EDGE-OUTBOX-BACKPRESSURE"),
          "edge outbox backpressure was not reported");

  auto fanout_request = InvalidationRequest(true);
  fanout_request.cache_tag_ids = {
      "tag:orders:public:v1", "tag:orders:public:v2", "tag:orders:public:v3"};
  const auto fanout = api::QueueEdgeCacheInvalidationAfterCommit(fanout_request);
  Require(!fanout.ok &&
              HasDiagnostic(fanout, "SB-EDGE-INVALIDATION-FANOUT-EXCEEDED"),
          "edge invalidation fanout limit was not enforced");

  api::EngineEdgeDispatchRequest dispatch;
  dispatch.provider_profile_uuid = "edge-provider-local-signed-v1";
  dispatch.now_epoch_ms = 2000;
  const auto failed_delivery = api::DispatchEdgeCacheInvalidations(dispatch);
  Require(!failed_delivery.ok &&
              HasDiagnostic(failed_delivery, "SB-EDGE-PROVIDER-DELIVERY-FAILED"),
          "unavailable provider did not produce retryable delivery failure");
  Require(failed_delivery.invalidation.delivery_state == "retry_pending",
          "failed provider delivery did not leave invalidation retry-pending");

  const auto provider_available = api::RegisterEdgeProviderProfile(Provider(true));
  Require(provider_available.ok, "edge provider availability update failed");
  dispatch.now_epoch_ms = 3000;
  const auto delivered = api::DispatchEdgeCacheInvalidations(dispatch);
  Require(delivered.ok && delivered.delivery_attempted,
          "available provider did not deliver retry-pending invalidation");
  Require(delivered.invalidation.delivery_state == "delivered",
          "edge invalidation did not reach delivered state");

  const auto inspect = api::InspectEdgeInvalidations();
  Require(inspect.ok && inspect.invalidations.size() == 1,
          "edge invalidation registry count mismatch");

  return EXIT_SUCCESS;
}
