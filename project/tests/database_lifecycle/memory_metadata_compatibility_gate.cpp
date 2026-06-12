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

void CurrentAndLegacyOpenForAllDomains() {
  for (const auto domain : {mem::MemoryMetadataDomain::memory_policy,
                           mem::MemoryMetadataDomain::temp_workspace_manifest,
                           mem::MemoryMetadataDomain::page_cache_metadata}) {
    const auto current =
        mem::ValidateMemoryMetadataOpen(PolicyFor(domain), RecordFor(domain, 2));
    Require(current.ok(), "MMCH-044 current metadata open failed");
    Require(current.action == mem::MemoryMetadataOpenAction::open_current,
            "MMCH-044 current metadata action mismatch");
    Require(current.upgraded_format_version == 2,
            "MMCH-044 current metadata version mismatch");
    RequireCompatibilityEvidence(current, "memory_metadata.action=open_current");

    const auto legacy =
        mem::ValidateMemoryMetadataOpen(PolicyFor(domain), RecordFor(domain, 1));
    Require(legacy.ok(), "MMCH-044 supported legacy metadata upgrade failed");
    Require(legacy.action == mem::MemoryMetadataOpenAction::upgrade_from_supported_legacy,
            "MMCH-044 legacy upgrade action mismatch");
    Require(legacy.upgraded_format_version == 2,
            "MMCH-044 legacy upgrade target mismatch");
    RequireCompatibilityEvidence(legacy,
                                 "memory_metadata.action=upgrade_from_supported_legacy");
  }
}

void UnsafeMetadataFailsClosed() {
  auto policy = PolicyFor(mem::MemoryMetadataDomain::memory_policy);

  auto future = RecordFor(mem::MemoryMetadataDomain::memory_policy, 99);
  auto result = mem::ValidateMemoryMetadataOpen(policy, future);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 future metadata version did not fail closed");
  Require(result.diagnostic.diagnostic_code == "memory_metadata_future_version",
          "MMCH-044 future version diagnostic changed");
  RequireCompatibilityEvidence(result, "memory_metadata.action=fail_closed");

  auto too_old = RecordFor(mem::MemoryMetadataDomain::memory_policy, 0);
  result = mem::ValidateMemoryMetadataOpen(policy, too_old);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 zero metadata version did not fail closed");

  auto missing_authority = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  missing_authority.authoritative_base_input_present = false;
  result = mem::ValidateMemoryMetadataOpen(policy, missing_authority);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 missing authoritative base input did not fail closed");
  Require(result.diagnostic.diagnostic_code ==
              "memory_metadata_missing_authoritative_base_input",
          "MMCH-044 missing authoritative input diagnostic changed");

  auto missing_checksum = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  missing_checksum.payload_checksum_present = false;
  result = mem::ValidateMemoryMetadataOpen(policy, missing_checksum);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 missing checksum did not fail closed");

  auto ambiguous = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  ambiguous.ambiguous_metadata = true;
  result = mem::ValidateMemoryMetadataOpen(policy, ambiguous);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 ambiguous metadata did not fail closed");

  auto unsafe_authority = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  unsafe_authority.parser_or_client_authority = true;
  result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 parser/client authority did not fail closed");
  Require(result.diagnostic.diagnostic_code == "memory_metadata_unsafe_authority",
          "MMCH-044 unsafe authority diagnostic changed");

  unsafe_authority = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  unsafe_authority.reference_authority = true;
  result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 reference authority did not fail closed");

  unsafe_authority = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  unsafe_authority.wal_authority = true;
  result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 WAL authority did not fail closed");

  unsafe_authority = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  unsafe_authority.recovery_authority_claimed = true;
  result = mem::ValidateMemoryMetadataOpen(policy, unsafe_authority);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 recovery authority did not fail closed");

  auto unredacted = RecordFor(mem::MemoryMetadataDomain::memory_policy, 2);
  unredacted.protected_material_redacted = false;
  result = mem::ValidateMemoryMetadataOpen(policy, unredacted);
  Require(!result.ok() && result.fail_closed,
          "MMCH-044 unredacted protected material did not fail closed");
}

void MetadataOwnersCarryFormatVersions() {
  mem::MemoryPolicyConfig memory_policy;
  mem::TempWorkspacePolicy temp_policy;
  page::PageCachePolicy cache_policy;
  Require(memory_policy.metadata_format_version == 2,
          "MMCH-044 memory policy metadata version missing");
  Require(temp_policy.metadata_format_version == 2,
          "MMCH-044 temp workspace metadata version missing");
  Require(cache_policy.metadata_format_version == 2,
          "MMCH-044 page cache metadata version missing");
}

}  // namespace

int main() {
  MetadataOwnersCarryFormatVersions();
  CurrentAndLegacyOpenForAllDomains();
  UnsafeMetadataFailsClosed();
  std::cout << "MMCH-044 authority_note=memory_metadata_compatibility_evidence_only;"
            << " metadata_is_not_transaction_finality_visibility_authorization_or_recovery_authority\n";
  return EXIT_SUCCESS;
}
