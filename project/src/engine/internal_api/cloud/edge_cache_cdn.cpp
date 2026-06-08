// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/edge_cache_cdn.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::vector<EngineEdgeProviderProfile>& Providers() {
  static std::vector<EngineEdgeProviderProfile> providers;
  return providers;
}

std::vector<EngineEdgeCacheTagRecord>& Tags() {
  static std::vector<EngineEdgeCacheTagRecord> tags;
  return tags;
}

std::vector<EngineEdgeInvalidationRecord>& Invalidations() {
  static std::vector<EngineEdgeInvalidationRecord> invalidations;
  return invalidations;
}

EngineEdgeCacheCdnLimits& Limits() {
  static EngineEdgeCacheCdnLimits limits;
  return limits;
}

std::uint64_t& StreamSequence() {
  static std::uint64_t sequence = 0;
  return sequence;
}

std::mutex& EdgeMutex() {
  static std::mutex mutex;
  return mutex;
}

EngineApiDiagnostic Diagnostic(std::string code, std::string detail, bool error = true) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.message_key = diagnostic.code;
  diagnostic.detail = std::move(detail);
  diagnostic.error = error;
  return diagnostic;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableId(std::string_view prefix, std::string_view material) {
  return std::string(prefix) + Hex64(Fnv1a64(material));
}

std::string HashText(std::string_view material) {
  return "hash:v1:fnv1a64:" + Hex64(Fnv1a64(material));
}

std::string Lower(std::string value) {
  for (auto& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool ContainsSecretMarker(std::string_view text) {
  const std::string lower = Lower(std::string(text));
  const std::vector<std::string> markers = {
      "secret=", "password", "passwd", "pwd=", "credential", "bearer ",
      "token=", "apikey", "api_key", "private_key", "key_material",
      "plaintext", "cleartext", "kms_plaintext", "protected_material"};
  for (const auto& marker : markers) {
    if (lower.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool IsHex(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsRawUuidText(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return false;
      }
    } else if (!IsHex(text[i])) {
      return false;
    }
  }
  return true;
}

bool SafeAsciiTag(std::string_view text, std::uint64_t max_length) {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == ':') {
      continue;
    }
    return false;
  }
  return true;
}

bool OneOf(std::string_view value, const std::vector<std::string>& values) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool IsValidTagClass(std::string_view value) {
  static const std::vector<std::string> values = {
      "static_asset_hint",
      "database_derived_page",
      "object_fragment",
      "tenant_scoped_content",
      "public_cluster_final_content",
      "branch_or_local_content",
      "diagnostic_content"};
  return OneOf(value, values);
}

bool IsDbDerivedTagClass(std::string_view value) {
  static const std::vector<std::string> values = {
      "database_derived_page",
      "object_fragment",
      "tenant_scoped_content",
      "public_cluster_final_content",
      "branch_or_local_content"};
  return OneOf(value, values);
}

bool IsValidDependencyScope(std::string_view value) {
  static const std::vector<std::string> values = {
      "database", "schema", "table", "record", "query", "resource",
      "route", "tenant", "security_policy", "finality"};
  return OneOf(value, values);
}

bool IsValidFinalityMode(std::string_view value) {
  static const std::vector<std::string> values = {
      "local_current", "local_final", "branch_final", "cluster_pending",
      "cluster_final", "archive_snapshot", "diagnostic_only"};
  return OneOf(value, values);
}

bool IsValidEventClass(std::string_view value) {
  static const std::vector<std::string> values = {
      "catalog", "record", "resource", "security", "route", "finality",
      "tenant", "policy"};
  return OneOf(value, values);
}

bool IsValidBlockingPolicy(std::string_view value) {
  static const std::vector<std::string> values = {
      "block_success", "warn_and_retry", "async_retry", "best_effort"};
  return OneOf(value, values);
}

bool IsSupportedProviderFamily(std::string_view value) {
  static const std::vector<std::string> values = {
      "local_signed_stream", "cdn_tag_purge", "cdn_revalidate"};
  return OneOf(value, values);
}

bool HasAfterCommitEvidence(const EngineExternalEffectCommitEvidence& evidence) {
  return evidence.mga_commit_visible &&
         evidence.durable_commit_evidence &&
         !evidence.transaction_uuid.empty() &&
         evidence.local_transaction_id > 0 &&
         evidence.transaction_inventory_generation > 0 &&
         !evidence.commit_evidence_hash.empty() &&
         IsValidFinalityMode(evidence.finality_mode);
}

EngineEdgeProviderProfile* FindProviderLocked(const std::string& provider_profile_uuid) {
  for (auto& provider : Providers()) {
    if (provider.provider_profile_uuid == provider_profile_uuid) {
      return &provider;
    }
  }
  return nullptr;
}

const EngineEdgeProviderProfile* FindProviderConstLocked(
    const std::string& provider_profile_uuid) {
  for (const auto& provider : Providers()) {
    if (provider.provider_profile_uuid == provider_profile_uuid) {
      return &provider;
    }
  }
  return nullptr;
}

EngineEdgeCacheTagRecord* FindTagLocked(const std::string& cache_tag_id) {
  for (auto& tag : Tags()) {
    if (tag.cache_tag_id == cache_tag_id) {
      return &tag;
    }
  }
  return nullptr;
}

EngineEdgeInvalidationRecord* FindInvalidationByIdempotencyLocked(
    const std::string& idempotency_key) {
  for (auto& invalidation : Invalidations()) {
    if (invalidation.idempotency_key == idempotency_key) {
      return &invalidation;
    }
  }
  return nullptr;
}

std::uint64_t TagCountForObjectLocked(const std::string& object_scope_key) {
  std::uint64_t count = 0;
  for (const auto& tag : Tags()) {
    if (!object_scope_key.empty() && tag.object_scope_key == object_scope_key) {
      ++count;
    }
  }
  return count;
}

bool ProviderSupportsPurgeMode(const EngineEdgeProviderProfile& provider,
                               std::string_view purge_mode) {
  return OneOf(purge_mode, provider.supported_purge_modes);
}

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::string Join(const std::vector<std::string>& values, std::string_view separator) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << separator;
    }
    out << values[i];
  }
  return out.str();
}

std::string RedactedDependencyRef(std::string_view internal_dependency_ref) {
  if (internal_dependency_ref.empty()) {
    return "opaque:none";
  }
  return StableId("opaque:", std::string("edge-dependency|") +
                                 std::string(internal_dependency_ref));
}

std::string SignatureFor(const EngineEdgeProviderProfile& provider,
                         std::string_view payload_hash,
                         std::string_view stream_sequence) {
  return StableId("sig:v1:",
                  provider.provider_profile_uuid + "|" +
                      provider.signature_algorithm + "|" +
                      provider.signature_key_ref + "|" +
                      std::string(payload_hash) + "|" +
                      std::string(stream_sequence));
}

std::string CanonicalPayloadMetadata(const EngineEdgeInvalidationRequest& request,
                                     const EngineEdgeProviderProfile& provider,
                                     const std::vector<EngineEdgeCacheTagRecord>& tags,
                                     const std::vector<std::string>& tag_ids,
                                     std::string_view stream_sequence) {
  std::vector<std::string> redacted_dependencies;
  redacted_dependencies.reserve(tags.size());
  for (const auto& tag : tags) {
    redacted_dependencies.push_back(tag.redacted_dependency_ref);
  }
  redacted_dependencies = SortedUnique(std::move(redacted_dependencies));

  std::ostringstream out;
  out << "edge_payload_version=1\n";
  out << "export_authority=external_effect_only\n";
  out << "provider_profile_uuid=" << provider.provider_profile_uuid << '\n';
  out << "provider_family=" << provider.provider_family << '\n';
  out << "event_class=" << request.event_class << '\n';
  out << "finality_mode=" << request.finality_mode << '\n';
  out << "cache_tag_ids=" << Join(tag_ids, ",") << '\n';
  out << "redacted_dependency_refs=" << Join(redacted_dependencies, ",") << '\n';
  out << "schema_epoch=" << request.schema_epoch << '\n';
  out << "security_epoch=" << request.security_epoch << '\n';
  out << "resource_epoch=" << request.resource_epoch << '\n';
  out << "route_epoch=" << request.route_epoch << '\n';
  out << "content_epoch_before=";
  if (request.content_epoch_before_present) {
    out << request.content_epoch_before;
  } else {
    out << "null";
  }
  out << '\n';
  out << "content_epoch_after=";
  if (request.content_epoch_after_present) {
    out << request.content_epoch_after;
  } else {
    out << "null";
  }
  out << '\n';
  out << "purge_mode=" << request.purge_mode << '\n';
  out << "blocking_policy=" << request.blocking_policy << '\n';
  out << "redaction_policy_uuid=" << request.redaction_policy_uuid << '\n';
  out << "payload_redacted=true\n";
  out << "stream_sequence=" << stream_sequence << '\n';
  return out.str();
}

std::string IdempotencyKeyFor(const EngineEdgeInvalidationRequest& request,
                              const std::vector<std::string>& tag_ids) {
  std::ostringstream out;
  out << "edge-idempotency-v1|";
  out << request.provider_profile_uuid << '|';
  out << request.commit_evidence.transaction_uuid << '|';
  out << request.commit_evidence.local_transaction_id << '|';
  out << request.commit_evidence.transaction_inventory_generation << '|';
  out << request.event_class << '|';
  out << Join(tag_ids, ",") << '|';
  out << request.purge_mode << '|';
  out << request.content_epoch_before_present << ':'
      << request.content_epoch_before << '|';
  out << request.content_epoch_after_present << ':'
      << request.content_epoch_after << '|';
  out << request.redaction_policy_uuid << '|';
  out << request.finality_mode;
  return StableId("edge-idem-", out.str());
}

EngineEdgeCacheCdnResult Failure(std::string code, std::string detail) {
  EngineEdgeCacheCdnResult result;
  result.diagnostics.push_back(Diagnostic(std::move(code), std::move(detail)));
  return result;
}

bool RedactionPolicyUnsafe(std::string_view redaction_policy_uuid) {
  if (redaction_policy_uuid.empty()) {
    return true;
  }
  const std::string lower = Lower(std::string(redaction_policy_uuid));
  return ContainsSecretMarker(redaction_policy_uuid) ||
         lower.find("unsafe") != std::string::npos ||
         lower.find("none") == 0;
}

bool PayloadContainsUnsafeMaterial(const std::string& payload) {
  if (ContainsSecretMarker(payload)) {
    return true;
  }
  std::istringstream in(payload);
  std::string line;
  while (std::getline(in, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if ((key == "cache_tag_ids" || key == "redacted_dependency_refs") &&
        IsRawUuidText(value)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void ConfigureEdgeCacheCdnLimits(const EngineEdgeCacheCdnLimits& limits) {
  std::lock_guard<std::mutex> lock(EdgeMutex());
  Limits().max_tags_per_object = std::max<std::uint64_t>(1, limits.max_tags_per_object);
  Limits().max_tags_per_invalidation =
      std::max<std::uint64_t>(1, limits.max_tags_per_invalidation);
  Limits().max_pending_outbox_records =
      std::max<std::uint64_t>(1, limits.max_pending_outbox_records);
}

EngineEdgeCacheCdnResult RegisterEdgeProviderProfile(
    const EngineEdgeProviderProfile& profile) {
  if (profile.provider_profile_uuid.empty() ||
      ContainsSecretMarker(profile.provider_profile_uuid)) {
    return Failure("SB-EDGE-PROVIDER-MISSING",
                   "provider_profile_uuid_required_and_secret_free");
  }
  if (!IsSupportedProviderFamily(profile.provider_family) || !profile.supported) {
    return Failure("SB-EDGE-PROVIDER-UNSUPPORTED",
                   "provider_family_not_supported");
  }
  if (profile.tag_max_length == 0 || profile.max_tag_count == 0 ||
      profile.max_pending_outbox_records == 0 || profile.max_attempts == 0) {
    return Failure("SB-EDGE-PROVIDER-UNSUPPORTED",
                   "provider_limits_must_be_nonzero");
  }
  if (profile.signature_required &&
      (profile.signature_algorithm != "scratchbird-edge-signature-v1" ||
       profile.signature_key_ref.empty() ||
       ContainsSecretMarker(profile.signature_key_ref))) {
    return Failure("SB-EDGE-SIGNATURE-CONFIG-INVALID",
                   "signature_algorithm_and_key_reference_required");
  }
  for (const auto& purge_mode : profile.supported_purge_modes) {
    if (!OneOf(purge_mode,
               std::vector<std::string>{"purge", "soft_purge", "revalidate",
                                        "ttl_shorten", "no_store"})) {
      return Failure("SB-EDGE-PROVIDER-UNSUPPORTED",
                     "provider_purge_mode_not_registered");
    }
  }

  std::lock_guard<std::mutex> lock(EdgeMutex());
  if (auto* existing = FindProviderLocked(profile.provider_profile_uuid)) {
    *existing = profile;
  } else {
    Providers().push_back(profile);
  }
  EngineEdgeCacheCdnResult result;
  result.ok = true;
  result.provider = profile;
  return result;
}

EngineEdgeCacheCdnResult RegisterEdgeCacheTag(
    const EngineEdgeCacheTagRegistration& registration) {
  if (!HasAfterCommitEvidence(registration.creation_commit_evidence)) {
    return Failure("SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED",
                   "tag_registry_descriptor_requires_committed_mga_evidence");
  }
  if (registration.cache_tag_descriptor_uuid.empty() ||
      registration.cache_tag_id.empty() ||
      ContainsSecretMarker(registration.cache_tag_id) ||
      ContainsSecretMarker(registration.cache_tag_descriptor_uuid)) {
    return Failure("SB-EDGE-TAG-UNSAFE",
                   "tag_descriptor_and_external_tag_required");
  }
  if (!IsValidTagClass(registration.tag_class) ||
      !IsValidDependencyScope(registration.dependency_scope)) {
    return Failure("SB-EDGE-TAG-UNSAFE",
                   "tag_class_or_dependency_scope_not_registered");
  }
  if (IsDbDerivedTagClass(registration.tag_class) &&
      !IsValidFinalityMode(registration.finality_mode)) {
    return Failure("SB-EDGE-FINALITY-LABEL-REQUIRED",
                   "db_derived_tag_requires_finality_label");
  }
  if (registration.tag_class == "branch_or_local_content" &&
      registration.allow_branch_global_cache) {
    return Failure("SB-EDGE-FINALITY-LABEL-REQUIRED",
                   "branch_or_local_content_global_cache_refused");
  }
  if (RedactionPolicyUnsafe(registration.redaction_policy_uuid)) {
    return Failure("SB-EDGE-REDACTION-UNSAFE",
                   "safe_redaction_policy_required");
  }
  if (registration.external_provider_profile_uuid.empty()) {
    return Failure("SB-EDGE-PROVIDER-MISSING",
                   "provider_profile_required_for_exported_cache_tag");
  }

  std::lock_guard<std::mutex> lock(EdgeMutex());
  const auto* provider = FindProviderConstLocked(registration.external_provider_profile_uuid);
  if (provider == nullptr) {
    return Failure("SB-EDGE-PROVIDER-MISSING",
                   "provider_profile_not_registered");
  }
  if (!provider->supported || !IsSupportedProviderFamily(provider->provider_family)) {
    return Failure("SB-EDGE-PROVIDER-UNSUPPORTED",
                   "provider_profile_not_supported");
  }
  if (registration.allow_raw_uuid_export == false &&
      IsRawUuidText(registration.cache_tag_id)) {
    return Failure("SB-EDGE-TAG-UNSAFE",
                   "raw_uuid_external_tag_refused");
  }
  if (!SafeAsciiTag(registration.cache_tag_id, provider->tag_max_length)) {
    return Failure("SB-EDGE-TAG-UNSAFE",
                   "external_tag_charset_or_length_invalid");
  }
  if (registration.ttl_present && registration.ttl_ms == 0) {
    return Failure("SB-EDGE-TAG-UNSAFE",
                   "ttl_zero_invalid_when_present");
  }

  if (auto* existing = FindTagLocked(registration.cache_tag_id)) {
    EngineEdgeCacheCdnResult result;
    result.ok = true;
    result.deduplicated = true;
    result.tag = *existing;
    return result;
  }

  const std::string redacted_dependency =
      RedactedDependencyRef(registration.internal_dependency_ref);
  const std::string object_scope = registration.object_scope_key.empty()
                                       ? redacted_dependency
                                       : registration.object_scope_key;
  if (TagCountForObjectLocked(object_scope) >= Limits().max_tags_per_object) {
    return Failure("SB-EDGE-INVALIDATION-FANOUT-EXCEEDED",
                   "maximum_edge_cache_tags_per_object_exceeded");
  }

  EngineEdgeCacheTagRecord tag;
  tag.cache_tag_descriptor_uuid = registration.cache_tag_descriptor_uuid;
  tag.cache_tag_id = registration.cache_tag_id;
  tag.tag_class = registration.tag_class;
  tag.dependency_scope = registration.dependency_scope;
  tag.redacted_dependency_ref = redacted_dependency;
  tag.redaction_policy_uuid = registration.redaction_policy_uuid;
  tag.finality_mode = registration.finality_mode;
  tag.ttl_ms = registration.ttl_ms;
  tag.ttl_present = registration.ttl_present;
  tag.purge_required_flag = registration.purge_required_flag;
  tag.external_provider_profile_uuid = registration.external_provider_profile_uuid;
  tag.object_scope_key = object_scope;
  tag.created_by_transaction_uuid =
      registration.creation_commit_evidence.transaction_uuid;
  tag.created_by_local_transaction_id =
      registration.creation_commit_evidence.local_transaction_id;
  tag.emitted_only_after_commit_evidence = true;
  Tags().push_back(tag);

  EngineEdgeCacheCdnResult result;
  result.ok = true;
  result.tag_registered = true;
  result.tag = tag;
  return result;
}

EngineEdgeCacheCdnResult QueueEdgeCacheInvalidationAfterCommit(
    const EngineEdgeInvalidationRequest& request) {
  if (!HasAfterCommitEvidence(request.commit_evidence)) {
    return Failure("SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED",
                   "durable_mga_commit_evidence_required");
  }
  if (!IsValidEventClass(request.event_class) ||
      !IsValidFinalityMode(request.finality_mode) ||
      !IsValidBlockingPolicy(request.blocking_policy)) {
    return Failure("SB-EDGE-FINALITY-LABEL-REQUIRED",
                   "event_class_finality_or_blocking_policy_invalid");
  }
  if (RedactionPolicyUnsafe(request.redaction_policy_uuid)) {
    return Failure("SB-EDGE-REDACTION-UNSAFE",
                   "safe_redaction_policy_required");
  }
  if (request.provider_profile_uuid.empty()) {
    return Failure("SB-EDGE-PROVIDER-MISSING",
                   "provider_profile_required_for_invalidation");
  }

  std::vector<EngineEdgeCacheTagRecord> selected_tags;
  EngineEdgeProviderProfile provider;
  std::vector<std::string> tag_ids = SortedUnique(request.cache_tag_ids);

  {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    const auto* found_provider = FindProviderConstLocked(request.provider_profile_uuid);
    if (found_provider == nullptr) {
      return Failure("SB-EDGE-PROVIDER-MISSING",
                     "provider_profile_not_registered");
    }
    provider = *found_provider;
    if (!provider.supported || !IsSupportedProviderFamily(provider.provider_family) ||
        !ProviderSupportsPurgeMode(provider, request.purge_mode)) {
      return Failure("SB-EDGE-PROVIDER-UNSUPPORTED",
                     "provider_or_purge_mode_not_supported");
    }
    if (provider.signature_required &&
        (provider.signature_key_ref.empty() ||
         provider.signature_algorithm != "scratchbird-edge-signature-v1" ||
         ContainsSecretMarker(provider.signature_key_ref))) {
      return Failure("SB-EDGE-SIGNATURE-CONFIG-INVALID",
                     "signature_configuration_invalid");
    }
    const std::uint64_t effective_tag_limit =
        std::min<std::uint64_t>(Limits().max_tags_per_invalidation,
                                provider.max_tag_count);
    if (tag_ids.empty() || tag_ids.size() > effective_tag_limit) {
      return Failure("SB-EDGE-INVALIDATION-FANOUT-EXCEEDED",
                     "maximum_edge_invalidation_fanout_exceeded");
    }

    for (const auto& tag_id : tag_ids) {
      auto* tag = FindTagLocked(tag_id);
      if (tag == nullptr || tag->external_provider_profile_uuid != request.provider_profile_uuid) {
        return Failure("SB-EDGE-TAG-UNSAFE",
                       "tag_missing_or_provider_mismatch");
      }
      if (tag->redaction_policy_uuid != request.redaction_policy_uuid) {
        return Failure("SB-EDGE-REDACTION-UNSAFE",
                       "tag_redaction_policy_mismatch");
      }
      selected_tags.push_back(*tag);
    }
  }

  const std::string idempotency_key = IdempotencyKeyFor(request, tag_ids);
  {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    if (auto* existing = FindInvalidationByIdempotencyLocked(idempotency_key)) {
      EngineEdgeCacheCdnResult result;
      result.ok = true;
      result.deduplicated = true;
      result.invalidation = *existing;
      return result;
    }
  }

  std::string stream_sequence;
  {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    stream_sequence = std::to_string(++StreamSequence());
  }

  const std::string metadata =
      CanonicalPayloadMetadata(request, provider, selected_tags, tag_ids, stream_sequence);
  if (PayloadContainsUnsafeMaterial(metadata)) {
    return Failure("SB-EDGE-REDACTION-UNSAFE",
                   "redacted_payload_metadata_contains_unsafe_material");
  }
  const std::string payload_hash = HashText(metadata);
  const std::string signature = provider.signature_required
                                    ? SignatureFor(provider, payload_hash, stream_sequence)
                                    : "";

  EngineExternalEffectOutboxAdmission admission;
  admission.commit_evidence = request.commit_evidence;
  admission.source_object_ref = request.source_object_ref;
  admission.effect_class = "edge_cdn_invalidation";
  admission.provider_profile_uuid = request.provider_profile_uuid;
  admission.idempotency_key = idempotency_key;
  admission.redaction_policy_uuid = request.redaction_policy_uuid;
  admission.payload_hash = payload_hash;
  admission.payload_metadata = metadata + "payload_hash=" + payload_hash + "\n" +
                               "signature_algorithm=" + provider.signature_algorithm + "\n" +
                               "signature_key_ref=" + provider.signature_key_ref + "\n" +
                               "signature_value=" + signature + "\n";
  admission.now_epoch_ms = request.now_epoch_ms;
  admission.max_pending_records =
      std::min<std::uint64_t>(Limits().max_pending_outbox_records,
                              provider.max_pending_outbox_records);

  auto outbox_result = AdmitExternalEffectAfterCommit(admission);
  if (!outbox_result.ok) {
    EngineEdgeCacheCdnResult result;
    result.diagnostics = std::move(outbox_result.diagnostics);
    return result;
  }
  if (outbox_result.deduplicated) {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    if (auto* existing = FindInvalidationByIdempotencyLocked(idempotency_key)) {
      EngineEdgeCacheCdnResult result;
      result.ok = true;
      result.deduplicated = true;
      result.invalidation = *existing;
      result.outbox_record = outbox_result.record;
      return result;
    }
  }

  EngineEdgeInvalidationRecord invalidation;
  invalidation.edge_invalidation_uuid =
      StableId("edge-invalidation-", idempotency_key + "|" + stream_sequence);
  invalidation.cache_tag_id_vector = tag_ids;
  invalidation.event_class = request.event_class;
  invalidation.finality_mode = request.finality_mode;
  invalidation.content_epoch_before = request.content_epoch_before;
  invalidation.content_epoch_before_present = request.content_epoch_before_present;
  invalidation.content_epoch_after = request.content_epoch_after;
  invalidation.content_epoch_after_present = request.content_epoch_after_present;
  invalidation.purge_mode = request.purge_mode;
  invalidation.blocking_policy = request.blocking_policy;
  invalidation.redaction_policy_uuid = request.redaction_policy_uuid;
  invalidation.payload_hash = payload_hash;
  invalidation.provider_profile_uuid = request.provider_profile_uuid;
  invalidation.idempotency_key = idempotency_key;
  invalidation.stream_sequence = stream_sequence;
  invalidation.signature_algorithm = provider.signature_algorithm;
  invalidation.signature_key_ref = provider.signature_key_ref;
  invalidation.signature_value = signature;
  invalidation.redacted_payload_metadata = admission.payload_metadata;
  invalidation.outbox_event_uuid = outbox_result.record.outbox_event_uuid;
  invalidation.source_transaction_uuid = request.commit_evidence.transaction_uuid;
  invalidation.local_transaction_id = request.commit_evidence.local_transaction_id;
  invalidation.transaction_inventory_generation =
      request.commit_evidence.transaction_inventory_generation;
  invalidation.emitted_after_commit_evidence = true;
  invalidation.payload_redacted = true;
  invalidation.delivery_state = outbox_result.record.final_state;

  {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    Invalidations().push_back(invalidation);
  }

  EngineEdgeCacheCdnResult result;
  result.ok = true;
  result.invalidation_queued = true;
  result.invalidation = invalidation;
  result.outbox_record = outbox_result.record;
  return result;
}

EngineEdgeCacheCdnResult DispatchEdgeCacheInvalidations(
    const EngineEdgeDispatchRequest& request) {
  EngineEdgeProviderProfile provider;
  {
    std::lock_guard<std::mutex> lock(EdgeMutex());
    const auto* found = FindProviderConstLocked(request.provider_profile_uuid);
    if (found == nullptr) {
      return Failure("SB-EDGE-PROVIDER-MISSING",
                     "provider_profile_not_registered");
    }
    provider = *found;
  }

  const auto outbox_records = InspectExternalEffectOutbox();
  std::uint64_t attempted = 0;
  EngineEdgeCacheCdnResult result;
  result.ok = true;

  for (const auto& record : outbox_records) {
    if (record.provider_profile_uuid != request.provider_profile_uuid ||
        record.effect_class != "edge_cdn_invalidation" ||
        (record.final_state != "pending" && record.final_state != "retry_pending")) {
      continue;
    }
    if (attempted >= request.max_records) {
      break;
    }
    ++attempted;

    EngineExternalEffectDeliveryAttempt attempt;
    attempt.idempotency_key = record.idempotency_key;
    attempt.provider_success = provider.provider_available;
    attempt.retryable_failure = true;
    attempt.diagnostic_code = "SB-EDGE-PROVIDER-DELIVERY-FAILED";
    attempt.now_epoch_ms = request.now_epoch_ms;
    attempt.max_attempts = provider.max_attempts;
    attempt.retry_backoff_ms = provider.retry_backoff_ms;
    auto delivery = RecordExternalEffectDeliveryAttempt(attempt);
    result.delivery_attempted = true;
    result.outbox_record = delivery.record;
    if (!delivery.ok) {
      result.ok = false;
      result.diagnostics.insert(result.diagnostics.end(),
                                delivery.diagnostics.begin(),
                                delivery.diagnostics.end());
    }
    {
      std::lock_guard<std::mutex> lock(EdgeMutex());
      if (auto* invalidation =
              FindInvalidationByIdempotencyLocked(record.idempotency_key)) {
        invalidation->delivery_state = delivery.record.final_state;
        invalidation->last_diagnostic_code = delivery.record.last_diagnostic_code;
        result.invalidation = *invalidation;
      }
    }
  }

  if (attempted == 0) {
    result.delivery_attempted = false;
  }
  return result;
}

EngineEdgeCacheCdnResult InspectEdgeCacheTags() {
  std::lock_guard<std::mutex> lock(EdgeMutex());
  EngineEdgeCacheCdnResult result;
  result.ok = true;
  result.tags = Tags();
  return result;
}

EngineEdgeCacheCdnResult InspectEdgeInvalidations() {
  std::lock_guard<std::mutex> lock(EdgeMutex());
  EngineEdgeCacheCdnResult result;
  result.ok = true;
  result.invalidations = Invalidations();
  return result;
}

void ResetEdgeCacheCdnStateForTests() {
  std::lock_guard<std::mutex> lock(EdgeMutex());
  Providers().clear();
  Tags().clear();
  Invalidations().clear();
  Limits() = EngineEdgeCacheCdnLimits{};
  StreamSequence() = 0;
  ResetExternalEffectOutboxForTests();
}

}  // namespace scratchbird::engine::internal_api
