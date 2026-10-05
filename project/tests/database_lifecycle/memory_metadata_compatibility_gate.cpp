// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_metadata_compatibility.hpp"
#include "memory_policy_config.hpp"
#include "page_cache.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
namespace page = scratchbird::storage::page;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

mem::MemoryMetadataOpenPolicy PolicyFor(mem::MemoryMetadataDomain domain) {
  mem::MemoryMetadataOpenPolicy policy;
  policy.expected_domain = domain;
  policy.current_format_version = 2;
  policy.oldest_supported_version = 1;
  policy.allow_legacy_upgrade = true;
  if (domain == mem::MemoryMetadataDomain::temp_workspace_manifest) {
    // Binary owner/fence records are version3. Text versions1/2 cannot be
    // upgraded by inventing the ownership information they never retained.
    policy.current_format_version = 3;
    policy.oldest_supported_version = 3;
    policy.allow_legacy_upgrade = false;
  }
  policy.require_authoritative_base_input = true;
  policy.require_payload_checksum = true;
  policy.protected_material_must_be_redacted = true;
  return policy;
}

mem::MemoryMetadataRecord RecordFor(mem::MemoryMetadataDomain domain, std::uint64_t version) {
  mem::MemoryMetadataRecord record;
  record.domain = domain;
  record.format_version = version;
  record.metadata_id = std::string(mem::MemoryMetadataDomainName(domain)) + "-metadata";
  record.schema_digest = std::string(mem::MemoryMetadataDomainName(domain)) + "-schema-digest";
  record.authoritative_base_input_present = true;
  record.payload_checksum_present = true;
  record.protected_material_redacted = true;
  return record;
}

void RequireCompatibilityEvidence(const mem::MemoryMetadataOpenResult& result,
                                  std::string_view action) {
  Require(EvidenceHas(result.evidence, "MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY"),
          "MMCH-044 evidence marker missing");
  Require(EvidenceHas(
              result.evidence,
              "memory_metadata.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_reference_wal_or_benchmark_authority"),
          "MMCH-044 authority boundary evidence missing");
  Require(EvidenceHas(result.evidence, action), "MMCH-044 action evidence missing");
}

// This helper checks version-policy decisions only. Real file recovery and
// preservation are independently exercised by temp_workspace_binary_owner_recovery.
void CurrentAndLegacyPolicyDecisionsForAllDomains() {
  for (const auto domain : {mem::MemoryMetadataDomain::memory_policy,
                           mem::MemoryMetadataDomain::temp_workspace_manifest,
                           mem::MemoryMetadataDomain::page_cache_metadata}) {
    const auto policy = PolicyFor(domain);
    const auto current = mem::ValidateMemoryMetadataOpen(
        policy, RecordFor(domain, policy.current_format_version));
    Require(current.ok(), "MMCH-044 current metadata policy decision failed");
    Require(current.action == mem::MemoryMetadataOpenAction::open_current,
            "MMCH-044 current metadata action mismatch");
    Require(current.upgraded_format_version == policy.current_format_version,
            "MMCH-044 current metadata version mismatch");
    RequireCompatibilityEvidence(current, "memory_metadata.action=open_current");

    for (std::uint64_t version = 1; version < policy.current_format_version; ++version) {
      const auto legacy = mem::ValidateMemoryMetadataOpen(policy, RecordFor(domain, version));
      if (domain == mem::MemoryMetadataDomain::temp_workspace_manifest) {
        Require(!legacy.ok() && legacy.fail_closed &&
                    legacy.action == mem::MemoryMetadataOpenAction::fail_closed &&
                    legacy.upgraded_format_version == 0 &&
                    legacy.diagnostic.diagnostic_code == "memory_metadata_unsupported_legacy_version",
                "MMCH-044 text temp manifest must not be admitted as an upgrade");
        RequireCompatibilityEvidence(legacy, "memory_metadata.action=fail_closed");
      } else {
        Require(legacy.ok(), "MMCH-044 supported legacy policy decision failed");
        Require(legacy.action == mem::MemoryMetadataOpenAction::upgrade_from_supported_legacy,
                "MMCH-044 legacy upgrade action mismatch");
        Require(legacy.upgraded_format_version == policy.current_format_version,
                "MMCH-044 legacy upgrade target mismatch");
        RequireCompatibilityEvidence(legacy,
                                     "memory_metadata.action=upgrade_from_supported_legacy");
      }
    }
  }
}

void UnsafeMetadataFailsClosed() {
  for (const auto domain : {mem::MemoryMetadataDomain::memory_policy,
                           mem::MemoryMetadataDomain::temp_workspace_manifest,
                           mem::MemoryMetadataDomain::page_cache_metadata}) {
    auto policy = PolicyFor(domain);

    auto future = RecordFor(domain, 99);
    auto result = mem::ValidateMemoryMetadataOpen(policy, future);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 future metadata version did not fail closed");
    Require(result.diagnostic.diagnostic_code == "memory_metadata_future_version",
            "MMCH-044 future version diagnostic changed");
    RequireCompatibilityEvidence(result, "memory_metadata.action=fail_closed");

    auto too_old = RecordFor(domain, 0);
    result = mem::ValidateMemoryMetadataOpen(policy, too_old);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 zero metadata version did not fail closed");

    auto missing_authority = RecordFor(domain, policy.current_format_version);
    missing_authority.authoritative_base_input_present = false;
    result = mem::ValidateMemoryMetadataOpen(policy, missing_authority);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 missing authoritative base input did not fail closed");
    Require(result.diagnostic.diagnostic_code ==
                "memory_metadata_missing_authoritative_base_input",
            "MMCH-044 missing authoritative input diagnostic changed");

    auto missing_checksum = RecordFor(domain, policy.current_format_version);
    missing_checksum.payload_checksum_present = false;
    result = mem::ValidateMemoryMetadataOpen(policy, missing_checksum);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 missing checksum did not fail closed");

    auto ambiguous = RecordFor(domain, policy.current_format_version);
    ambiguous.ambiguous_metadata = true;
    result = mem::ValidateMemoryMetadataOpen(policy, ambiguous);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 ambiguous metadata did not fail closed");

    auto unsafe_authority = RecordFor(domain, policy.current_format_version);
    unsafe_authority.parser_or_client_authority = true;
    result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 parser/client authority did not fail closed");
    Require(result.diagnostic.diagnostic_code == "memory_metadata_unsafe_authority",
            "MMCH-044 unsafe authority diagnostic changed");

    unsafe_authority = RecordFor(domain, policy.current_format_version);
    unsafe_authority.reference_authority = true;
    result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 reference authority did not fail closed");

    unsafe_authority = RecordFor(domain, policy.current_format_version);
    unsafe_authority.wal_authority = true;
    result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 WAL authority did not fail closed");

    unsafe_authority = RecordFor(domain, policy.current_format_version);
    unsafe_authority.recovery_authority_claimed = true;
    result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 recovery authority did not fail closed");

    auto unredacted = RecordFor(domain, policy.current_format_version);
    unredacted.protected_material_redacted = false;
    result = mem::ValidateMemoryMetadataOpen(policy, unredacted);
    Require(!result.ok() && result.fail_closed,
            "MMCH-044 unredacted protected material did not fail closed");

    auto missing_identity = RecordFor(domain, policy.current_format_version);
    missing_identity.metadata_id.clear();
    result = mem::ValidateMemoryMetadataOpen(policy, missing_identity);
    Require(!result.ok() && result.fail_closed &&
                result.diagnostic.diagnostic_code == "memory_metadata_missing_identity",
            "MMCH-044 missing metadata identity did not fail closed");

    auto missing_schema = RecordFor(domain, policy.current_format_version);
    missing_schema.schema_digest.clear();
    result = mem::ValidateMemoryMetadataOpen(policy, missing_schema);
    Require(!result.ok() && result.fail_closed &&
                result.diagnostic.diagnostic_code == "memory_metadata_missing_schema_digest",
            "MMCH-044 missing schema digest did not fail closed");

    auto wrong_domain = RecordFor(domain, policy.current_format_version);
    wrong_domain.domain = domain == mem::MemoryMetadataDomain::memory_policy
        ? mem::MemoryMetadataDomain::page_cache_metadata
        : mem::MemoryMetadataDomain::memory_policy;
    result = mem::ValidateMemoryMetadataOpen(policy, wrong_domain);
    Require(!result.ok() && result.fail_closed &&
                result.diagnostic.diagnostic_code == "memory_metadata_domain_mismatch",
            "MMCH-044 mismatched metadata domain did not fail closed");

    if (domain != mem::MemoryMetadataDomain::temp_workspace_manifest) {
      auto disabled = policy;
      disabled.allow_legacy_upgrade = false;
      result = mem::ValidateMemoryMetadataOpen(disabled, RecordFor(domain, 1));
      Require(!result.ok() && result.fail_closed &&
                  result.diagnostic.diagnostic_code == "memory_metadata_legacy_upgrade_disabled",
              "MMCH-044 disabled legacy policy did not fail closed");
    }
  }
}

void MetadataOwnersCarryFormatVersions() {
  mem::MemoryPolicyConfig memory_policy;
  mem::TempWorkspacePolicy temp_policy;
  page::PageCachePolicy cache_policy;
  Require(memory_policy.metadata_format_version == 2,
          "MMCH-044 memory policy metadata version missing");
  Require(temp_policy.metadata_format_version == 3,
          "MMCH-044 temp workspace metadata version missing");
  Require(cache_policy.metadata_format_version == 2,
          "MMCH-044 page cache metadata version missing");
}

}  // namespace

int main() {
  MetadataOwnersCarryFormatVersions();
  CurrentAndLegacyPolicyDecisionsForAllDomains();
  UnsafeMetadataFailsClosed();
  std::cout << "MMCH-044 decisions_do_not_open_rewrite_or_migrate_files; "
            << "authority_note=memory_metadata_compatibility_evidence_only;"
            << " metadata_is_not_transaction_finality_visibility_authorization_or_recovery_authority\n";
  return EXIT_SUCCESS;
}
