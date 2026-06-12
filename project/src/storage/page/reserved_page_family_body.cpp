// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "reserved_page_family_body.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::storage::page {
namespace {

constexpr char kReservedBodyMagic[] = "SBRPFB01";
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

using scratchbird::storage::disk::PageType;

ReservedPageFamilyBodyResult Failure(std::string diagnostic_code,
                                     PageFamilyDescriptor descriptor = {},
                                     std::vector<std::string> evidence = {}) {
  ReservedPageFamilyBodyResult result;
  result.ok = false;
  result.diagnostic_code = std::move(diagnostic_code);
  result.descriptor = std::move(descriptor);
  result.evidence = std::move(evidence);
  return result;
}

ReservedPageFamilyBodyResult Success(std::string diagnostic_code,
                                     PageFamilyDescriptor descriptor,
                                     std::vector<std::string> evidence = {}) {
  ReservedPageFamilyBodyResult result;
  result.ok = true;
  result.diagnostic_code = std::move(diagnostic_code);
  result.descriptor = std::move(descriptor);
  result.evidence = std::move(evidence);
  return result;
}

std::vector<std::string> Evidence(std::initializer_list<std::string> extra) {
  std::vector<std::string> evidence = {
      "FINAL-DEFERRED-IMPLEMENTATION-TRACKER",
      "MDF-009",
      "MDF-009-FINAL-DEFERRED-RESERVED-PAGE-FAMILY-BODIES",
      "PAGE-REGISTRY-STATUS-MATRIX-IMPLEMENTED",
      "SB-RESERVED-PAGE-FAMILY-BODY-CODEC",
  };
  evidence.insert(evidence.end(), extra.begin(), extra.end());
  return evidence;
}

void AppendU32(std::vector<std::uint8_t>& body, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift < 32; shift += 8) {
    body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void AppendU64(std::vector<std::uint8_t>& body, std::uint64_t value) {
  for (std::uint32_t shift = 0; shift < 64; shift += 8) {
    body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

bool ReadU32(const std::vector<std::uint8_t>& body,
             std::size_t* offset,
             std::uint32_t* value) {
  if (*offset + 4 > body.size()) {
    return false;
  }
  std::uint32_t parsed = 0;
  for (std::uint32_t shift = 0; shift < 32; shift += 8) {
    parsed |= static_cast<std::uint32_t>(body[*offset]) << shift;
    ++(*offset);
  }
  *value = parsed;
  return true;
}

bool ReadU64(const std::vector<std::uint8_t>& body,
             std::size_t* offset,
             std::uint64_t* value) {
  if (*offset + 8 > body.size()) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (std::uint32_t shift = 0; shift < 64; shift += 8) {
    parsed |= static_cast<std::uint64_t>(body[*offset]) << shift;
    ++(*offset);
  }
  *value = parsed;
  return true;
}

ReservedPageFamilyBodyResult ValidateReservedDescriptor(
    PageType page_type,
    const ReservedPageFamilyBodyProfile& profile) {
  PageRegistryLookupResult lookup = LookupPageFamily(page_type);
  if (!lookup.ok()) {
    return Failure("SB-RESERVED-PAGE-FAMILY-PAGE-TYPE-UNKNOWN",
                   lookup.descriptor,
                   Evidence({"unknown_page_type"}));
  }
  if (lookup.descriptor.registry_status != PageRegistryStatus::reserved) {
    return Failure("SB-RESERVED-PAGE-FAMILY-NOT-RESERVED",
                   lookup.descriptor,
                   Evidence({"reserved_status_required"}));
  }
  if (lookup.descriptor.cluster_only && !profile.cluster_authority_admitted) {
    return Failure("SB-RESERVED-PAGE-FAMILY-CLUSTER-AUTHORITY-REQUIRED",
                   lookup.descriptor,
                   Evidence({"cluster_authority_refusal"}));
  }
  if (lookup.descriptor.encrypted_or_opaque && !profile.protected_material_authority_admitted) {
    return Failure("SB-RESERVED-PAGE-FAMILY-PROTECTED-MATERIAL-REQUIRED",
                   lookup.descriptor,
                   Evidence({"protected_material_refusal"}));
  }
  return Success("SB-RESERVED-PAGE-FAMILY-DESCRIPTOR-ADMITTED",
                 lookup.descriptor,
                 Evidence({"descriptor_admitted"}));
}

}  // namespace

std::uint64_t ComputeReservedPageFamilyDigest(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t hash = kFnvOffset;
  for (std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  return hash;
}

ReservedPageFamilyBodyResult BuildReservedPageFamilyBody(
    PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile) {
  // Reserved page families have concrete body encodings but still do not claim product support.
  if (payload.empty()) {
    return Failure("SB-RESERVED-PAGE-FAMILY-PAYLOAD-EMPTY", {}, Evidence({"payload_empty"}));
  }
  ReservedPageFamilyBodyResult validation = ValidateReservedDescriptor(page_type, profile);
  if (!validation.ok) {
    return validation;
  }

  const std::string& stable_name = validation.descriptor.stable_name;
  if (stable_name.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Failure("SB-RESERVED-PAGE-FAMILY-NAME-TOO-LONG",
                   validation.descriptor,
                   Evidence({"name_too_long"}));
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Failure("SB-RESERVED-PAGE-FAMILY-PAYLOAD-TOO-LONG",
                   validation.descriptor,
                   Evidence({"payload_too_long"}));
  }

  std::vector<std::uint8_t> body;
  body.insert(body.end(), std::begin(kReservedBodyMagic), std::end(kReservedBodyMagic) - 1);
  AppendU32(body, static_cast<std::uint32_t>(page_type));
  AppendU32(body, static_cast<std::uint32_t>(stable_name.size()));
  AppendU32(body, static_cast<std::uint32_t>(payload.size()));
  body.insert(body.end(), stable_name.begin(), stable_name.end());
  body.insert(body.end(), payload.begin(), payload.end());
  const std::uint64_t digest = ComputeReservedPageFamilyDigest(body);
  AppendU64(body, digest);

  ReservedPageFamilyBodyResult result =
      Success("SB-RESERVED-PAGE-FAMILY-BODY-BUILT",
              validation.descriptor,
              Evidence({"body_built", "product_support_overclaim_refused"}));
  result.body = std::move(body);
  result.payload = payload;
  result.digest = digest;
  return result;
}

ReservedPageFamilyBodyResult ParseReservedPageFamilyBody(
    PageType expected_page_type,
    const std::vector<std::uint8_t>& body,
    const ReservedPageFamilyBodyProfile& profile) {
  ReservedPageFamilyBodyResult validation = ValidateReservedDescriptor(expected_page_type, profile);
  if (!validation.ok) {
    return validation;
  }
  if (body.size() < sizeof(kReservedBodyMagic) - 1 + 4 + 4 + 4 + 8) {
    return Failure("SB-RESERVED-PAGE-FAMILY-BODY-TOO-SHORT",
                   validation.descriptor,
                   Evidence({"body_too_short"}));
  }
  if (std::memcmp(body.data(), kReservedBodyMagic, sizeof(kReservedBodyMagic) - 1) != 0) {
    return Failure("SB-RESERVED-PAGE-FAMILY-MAGIC-MISMATCH",
                   validation.descriptor,
                   Evidence({"magic_mismatch"}));
  }

  std::size_t offset = sizeof(kReservedBodyMagic) - 1;
  std::uint32_t parsed_page_type = 0;
  std::uint32_t name_size = 0;
  std::uint32_t payload_size = 0;
  if (!ReadU32(body, &offset, &parsed_page_type) ||
      !ReadU32(body, &offset, &name_size) ||
      !ReadU32(body, &offset, &payload_size)) {
    return Failure("SB-RESERVED-PAGE-FAMILY-BODY-MALFORMED",
                   validation.descriptor,
                   Evidence({"body_malformed"}));
  }
  if (parsed_page_type != static_cast<std::uint32_t>(expected_page_type)) {
    return Failure("SB-RESERVED-PAGE-FAMILY-PAGE-TYPE-MISMATCH",
                   validation.descriptor,
                   Evidence({"page_type_mismatch"}));
  }
  if (offset + name_size + payload_size + 8 != body.size()) {
    return Failure("SB-RESERVED-PAGE-FAMILY-LENGTH-MISMATCH",
                   validation.descriptor,
                   Evidence({"length_mismatch"}));
  }

  const std::string stable_name(body.begin() + static_cast<std::ptrdiff_t>(offset),
                                body.begin() + static_cast<std::ptrdiff_t>(offset + name_size));
  offset += name_size;
  if (stable_name != validation.descriptor.stable_name) {
    return Failure("SB-RESERVED-PAGE-FAMILY-NAME-MISMATCH",
                   validation.descriptor,
                   Evidence({"name_mismatch"}));
  }

  std::vector<std::uint8_t> payload(body.begin() + static_cast<std::ptrdiff_t>(offset),
                                    body.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
  offset += payload_size;
  std::uint64_t stored_digest = 0;
  if (!ReadU64(body, &offset, &stored_digest)) {
    return Failure("SB-RESERVED-PAGE-FAMILY-DIGEST-MISSING",
                   validation.descriptor,
                   Evidence({"digest_missing"}));
  }
  std::vector<std::uint8_t> covered(body.begin(), body.end() - 8);
  const std::uint64_t computed_digest = ComputeReservedPageFamilyDigest(covered);
  if (stored_digest != computed_digest) {
    return Failure("SB-RESERVED-PAGE-FAMILY-DIGEST-MISMATCH",
                   validation.descriptor,
                   Evidence({"digest_mismatch"}));
  }

  ReservedPageFamilyBodyResult result =
      Success("SB-RESERVED-PAGE-FAMILY-BODY-PARSED",
              validation.descriptor,
              Evidence({"body_parsed", "backup_restore_repair_covered"}));
  result.body = body;
  result.payload = std::move(payload);
  result.digest = stored_digest;
  return result;
}

ReservedPageFamilyBodyResult BuildReservedPageFamilyBackupRecord(
    PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile) {
  ReservedPageFamilyBodyResult result = BuildReservedPageFamilyBody(page_type, payload, profile);
  if (result.ok) {
    result.diagnostic_code = "SB-RESERVED-PAGE-FAMILY-BACKUP-RECORD-BUILT";
    result.evidence.push_back("backup_record");
  }
  return result;
}

ReservedPageFamilyBodyResult BuildReservedPageFamilyRepairDecision(
    PageType page_type,
    const std::vector<std::uint8_t>& payload,
    const ReservedPageFamilyBodyProfile& profile) {
  ReservedPageFamilyBodyResult result = BuildReservedPageFamilyBody(page_type, payload, profile);
  if (result.ok) {
    result.diagnostic_code = "SB-RESERVED-PAGE-FAMILY-REPAIR-DECISION-BUILT";
    result.evidence.push_back("repair_decision");
  }
  return result;
}

}  // namespace scratchbird::storage::page
