// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_metadata_compatibility.hpp"

#include <string>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "memory_metadata.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_reference_wal_or_benchmark_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

MemoryMetadataOpenResult MakeFailure(const MemoryMetadataOpenPolicy& policy,
                                     const MemoryMetadataRecord& record,
                                     std::string diagnostic_code,
                                     std::string message_key,
                                     std::string reason) {
  MemoryMetadataOpenResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.action = MemoryMetadataOpenAction::fail_closed;
  result.upgraded_format_version = 0;
  result.diagnostic = MakeDiagnostic(StatusCode::memory_invalid_request,
                                     Severity::error,
                                     Subsystem::memory,
                                     std::move(diagnostic_code),
                                     std::move(message_key),
                                     {{"domain", MemoryMetadataDomainName(record.domain)},
                                      {"expected_domain", MemoryMetadataDomainName(policy.expected_domain)},
                                      {"format_version", std::to_string(record.format_version)},
                                      {"current_format_version", std::to_string(policy.current_format_version)},
                                      {"reason", reason}},
                                     {},
                                     "core.memory.metadata_compatibility",
                                     "fail closed and reopen only from authoritative base metadata");
  result.evidence.push_back("MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("memory_metadata.fail_closed=true");
  result.evidence.push_back("memory_metadata.domain=" +
                            std::string(MemoryMetadataDomainName(record.domain)));
  result.evidence.push_back("memory_metadata.action=fail_closed");
  result.evidence.push_back("memory_metadata.reason=" + std::move(reason));
  result.evidence.push_back("memory_metadata.protected_material_redacted=" +
                            std::string(record.protected_material_redacted ? "true" : "false"));
  return result;
}

void AddCommonEvidence(const MemoryMetadataOpenPolicy& policy,
                       const MemoryMetadataRecord& record,
                       MemoryMetadataOpenResult* result) {
  result->evidence.push_back("MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY");
  result->evidence.push_back(kAuthorityBoundary);
  result->evidence.push_back("memory_metadata.domain=" +
                             std::string(MemoryMetadataDomainName(record.domain)));
  result->evidence.push_back("memory_metadata.metadata_id=" + record.metadata_id);
  result->evidence.push_back("memory_metadata.format_version=" +
                             std::to_string(record.format_version));
  result->evidence.push_back("memory_metadata.current_format_version=" +
                             std::to_string(policy.current_format_version));
  result->evidence.push_back("memory_metadata.authoritative_base_input_present=" +
                             std::string(record.authoritative_base_input_present ? "true" : "false"));
  result->evidence.push_back("memory_metadata.payload_checksum_present=" +
                             std::string(record.payload_checksum_present ? "true" : "false"));
  result->evidence.push_back("memory_metadata.protected_material_redacted=" +
                             std::string(record.protected_material_redacted ? "true" : "false"));
}

}  // namespace

const char* MemoryMetadataDomainName(MemoryMetadataDomain domain) {
  switch (domain) {
    case MemoryMetadataDomain::memory_policy:
      return "memory_policy";
    case MemoryMetadataDomain::temp_workspace_manifest:
      return "temp_workspace_manifest";
    case MemoryMetadataDomain::page_cache_metadata:
      return "page_cache_metadata";
  }
  return "unknown";
}

const char* MemoryMetadataOpenActionName(MemoryMetadataOpenAction action) {
  switch (action) {
    case MemoryMetadataOpenAction::open_current:
      return "open_current";
    case MemoryMetadataOpenAction::upgrade_from_supported_legacy:
      return "upgrade_from_supported_legacy";
    case MemoryMetadataOpenAction::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

MemoryMetadataOpenResult ValidateMemoryMetadataOpen(
    const MemoryMetadataOpenPolicy& policy,
    const MemoryMetadataRecord& record) {
  if (record.domain != policy.expected_domain) {
    return MakeFailure(policy, record, "memory_metadata_domain_mismatch",
                       "core.memory.metadata.domain_mismatch",
                       "domain_mismatch");
  }
  if (record.metadata_id.empty()) {
    return MakeFailure(policy, record, "memory_metadata_missing_identity",
                       "core.memory.metadata.missing_identity",
                       "missing_metadata_id");
  }
  if (record.schema_digest.empty()) {
    return MakeFailure(policy, record, "memory_metadata_missing_schema_digest",
                       "core.memory.metadata.missing_schema_digest",
                       "missing_schema_digest");
  }
  if (record.parser_or_client_authority || record.reference_authority ||
      record.wal_authority || record.recovery_authority_claimed) {
    return MakeFailure(policy, record, "memory_metadata_unsafe_authority",
                       "core.memory.metadata.unsafe_authority",
                       "parser_client_reference_wal_or_recovery_authority_claimed");
  }
  if (policy.require_authoritative_base_input &&
      !record.authoritative_base_input_present) {
    return MakeFailure(policy, record, "memory_metadata_missing_authoritative_base_input",
                       "core.memory.metadata.missing_authoritative_base_input",
                       "authoritative_base_input_required");
  }
  if (policy.require_payload_checksum && !record.payload_checksum_present) {
    return MakeFailure(policy, record, "memory_metadata_missing_payload_checksum",
                       "core.memory.metadata.missing_payload_checksum",
                       "payload_checksum_required");
  }
  if (record.ambiguous_metadata) {
    return MakeFailure(policy, record, "memory_metadata_ambiguous",
                       "core.memory.metadata.ambiguous",
                       "ambiguous_metadata");
  }
  if (policy.protected_material_must_be_redacted &&
      !record.protected_material_redacted) {
    return MakeFailure(policy, record, "memory_metadata_protected_material_unredacted",
                       "core.memory.metadata.protected_material_unredacted",
                       "protected_material_must_be_redacted");
  }
  if (record.format_version == 0 ||
      record.format_version < policy.oldest_supported_version) {
    return MakeFailure(policy, record, "memory_metadata_unsupported_legacy_version",
                       "core.memory.metadata.unsupported_legacy_version",
                       "format_version_too_old");
  }
  if (record.format_version > policy.current_format_version) {
    return MakeFailure(policy, record, "memory_metadata_future_version",
                       "core.memory.metadata.future_version",
                       "format_version_newer_than_engine");
  }
  if (record.format_version == policy.current_format_version) {
    MemoryMetadataOpenResult result;
    result.status = OkStatus();
    result.action = MemoryMetadataOpenAction::open_current;
    result.upgraded_format_version = policy.current_format_version;
    result.diagnostic = MakeDiagnostic(StatusCode::ok,
                                       Severity::info,
                                       Subsystem::memory,
                                       "memory_metadata_open_current",
                                       "core.memory.metadata.open_current",
                                       {{"domain", MemoryMetadataDomainName(record.domain)},
                                        {"format_version", std::to_string(record.format_version)}},
                                       {},
                                       "core.memory.metadata_compatibility",
                                       {});
    AddCommonEvidence(policy, record, &result);
    result.evidence.push_back("memory_metadata.action=open_current");
    result.evidence.push_back("memory_metadata.fail_closed=false");
    return result;
  }
  if (!policy.allow_legacy_upgrade) {
    return MakeFailure(policy, record, "memory_metadata_legacy_upgrade_disabled",
                       "core.memory.metadata.legacy_upgrade_disabled",
                       "legacy_upgrade_disabled");
  }

  MemoryMetadataOpenResult result;
  result.status = OkStatus();
  result.action = MemoryMetadataOpenAction::upgrade_from_supported_legacy;
  result.upgraded_format_version = policy.current_format_version;
  result.diagnostic = MakeDiagnostic(StatusCode::ok,
                                     Severity::info,
                                     Subsystem::memory,
                                     "memory_metadata_upgrade_supported_legacy",
                                     "core.memory.metadata.upgrade_supported_legacy",
                                     {{"domain", MemoryMetadataDomainName(record.domain)},
                                      {"from_format_version", std::to_string(record.format_version)},
                                      {"to_format_version", std::to_string(policy.current_format_version)}},
                                     {},
                                     "core.memory.metadata_compatibility",
                                     {});
  AddCommonEvidence(policy, record, &result);
  result.evidence.push_back("memory_metadata.action=upgrade_from_supported_legacy");
  result.evidence.push_back("memory_metadata.fail_closed=false");
  result.evidence.push_back("memory_metadata.upgraded_format_version=" +
                            std::to_string(result.upgraded_format_version));
  return result;
}

}  // namespace scratchbird::core::memory
