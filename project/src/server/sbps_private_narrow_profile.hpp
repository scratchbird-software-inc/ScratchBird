// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Pure Core-record validation for the isolated private SBPS narrow profile.
// This file does not publish an endpoint or alter the existing SBPS registry.
// The manifest-listed Core evidence record is pending, so constructing state
// from `CorePendingPrivateNarrowEvidenceV1()` always leaves active pairs empty.

#include "runtime_platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server::sbps::private_narrow {

using scratchbird::core::platform::byte;
using UuidV1 = std::array<byte, 16>;
using Hash256V1 = std::array<byte, 32>;
using CapabilityBitmapV1 = std::array<byte, 32>;

inline constexpr char kProfileIdV1[] =
    "SBPS-V1-PROFILE-019d3b15-6e00-7000-8000-000000000001";
inline constexpr char kImplementationEvidenceIdV1[] =
    "SBPS-V1-EVIDENCE-019d3b15-6e00-7000-8000-000000000003";
inline constexpr char kRegistrySnapshotSha256V1[] =
    "bbbaf6f102a88d82feff5344b094091ff8a6a14ef89aad4a00c62f736e1ec8a4";
inline constexpr std::uint16_t kProtocolMajorV1 = 1;
inline constexpr std::uint16_t kProtocolMinorV1 = 0;
inline constexpr std::uint32_t kParserApiMajorV1 = 1;
inline constexpr std::uint32_t kParserApiMinorV1 = 0;
inline constexpr std::size_t kPairUniverseCountV1 = 47;
inline constexpr std::size_t kRequiredPairCountV1 = 29;
inline constexpr std::size_t kForbiddenPairCountV1 = 18;
inline constexpr std::size_t kSuccessOnlyPairCountV1 = 10;

struct PairV1 {
  std::uint16_t message_code = 0;
  std::uint32_t payload_schema_id = 0;

  friend bool operator==(const PairV1&, const PairV1&) = default;
  friend bool operator<(const PairV1& left, const PairV1& right) {
    return left.message_code < right.message_code ||
           (left.message_code == right.message_code &&
            left.payload_schema_id < right.payload_schema_id);
  }
};

struct ActivationRecordV1 {
  std::uint16_t record_version = 1;
  std::uint16_t protocol_major = 1;
  std::uint16_t protocol_minor = 0;
  PairV1 pair;
  std::string admission_state;
  UuidV1 activation_set_uuid{};
  std::string approved_profile_id;
  Hash256V1 registry_snapshot_sha256{};
  std::string layout_authority_id;
  std::string conformance_corpus_id;
  std::string implementation_evidence_id;
  std::vector<byte> exact_nul_serialization;
};

struct CoreProfileRecordV1 {
  std::string profile_id;
  UuidV1 profile_uuid{};
  UuidV1 activation_set_uuid{};
  UuidV1 implementation_evidence_uuid{};
  UuidV1 endpoint_profile_uuid{};
  std::string implementation_evidence_id;
  Hash256V1 registry_snapshot_sha256{};
  std::uint16_t protocol_major = 0;
  std::uint16_t protocol_minor = 0;
  std::uint32_t parser_api_major = 0;
  std::uint32_t parser_api_minor = 0;
  bool approved = false;
  bool immutable = false;
  bool generic_query_admission = false;
  CapabilityBitmapV1 offered_capability_bitmap{};
  CapabilityBitmapV1 accepted_capability_bitmap{};
  std::vector<PairV1> pair_universe;
  std::vector<PairV1> required_pairs;
  std::vector<PairV1> forbidden_pairs;
  std::vector<PairV1> success_only_pairs;
  PairV1 semantic_refusal_pair;
  std::vector<ActivationRecordV1> candidate_activation_records;
  // Active projections are authoritative, not inferred from candidate rows.
  std::vector<PairV1> actual_active_pairs;
};

enum class EvidenceStatusV1 : std::uint8_t {
  pending = 0,
  usable,
  revoked,
  expired,
};

struct RoleEvidenceV1 {
  std::string role;
  Hash256V1 implementation_artifact_sha256{};
  Hash256V1 executable_fixture_sha256{};
  Hash256V1 verification_result_sha256{};
  bool verified = false;
};

struct CorpusEvidenceV1 {
  std::string conformance_corpus_id;
  Hash256V1 manifest_sha256{};
  Hash256V1 executable_fixture_set_sha256{};
  Hash256V1 result_sha256{};
  bool passed = false;
};

struct EvidenceRecordV1 {
  std::string implementation_evidence_id;
  UuidV1 evidence_uuid{};
  UuidV1 activation_set_uuid{};
  std::string approved_profile_id;
  std::uint16_t protocol_major = 0;
  std::uint16_t protocol_minor = 0;
  Hash256V1 registry_snapshot_sha256{};
  EvidenceStatusV1 status = EvidenceStatusV1::pending;
  bool usable = false;
  bool revoked = false;
  std::vector<RoleEvidenceV1> role_artifacts;
  std::vector<CorpusEvidenceV1> corpus_results;
  Hash256V1 endpoint_generation_evidence_sha256{};
  Hash256V1 aggregate_evidence_sha256{};
};

struct EndpointActivationRequestV1 {
  UuidV1 endpoint_profile_uuid{};
  UuidV1 live_endpoint_uuid{};
  std::uint64_t live_endpoint_generation = 0;
  std::string approved_profile_id;
  UuidV1 activation_set_uuid{};
  std::uint16_t protocol_major = 0;
  std::uint16_t protocol_minor = 0;
  CapabilityBitmapV1 offered_capability_bitmap{};
  CapabilityBitmapV1 accepted_capability_bitmap{};
  std::vector<PairV1> proposed_active_pairs;
};

enum class ProfileValidationStatusV1 : std::uint8_t {
  ok = 0,
  core_record_invalid,
  activation_record_invalid,
  profile_invalid,
  capability_invalid,
  evidence_pending,
  evidence_invalid,
  evidence_revoked,
  endpoint_invalid,
  message_unregistered,
};

struct ProfileDiagnosticV1 {
  ProfileValidationStatusV1 status =
      ProfileValidationStatusV1::core_record_invalid;
  std::string diagnostic_code;
  std::string field;
  std::string detail;

  [[nodiscard]] bool ok() const {
    return status == ProfileValidationStatusV1::ok;
  }
};

struct ActivationStateV1 {
  bool active = false;
  UuidV1 endpoint_profile_uuid{};
  UuidV1 live_endpoint_uuid{};
  std::uint64_t live_endpoint_generation = 0;
  std::string approved_profile_id;
  std::uint16_t protocol_major = 0;
  std::uint16_t protocol_minor = 0;
  std::vector<PairV1> active_pairs;
};

struct ActivationResultV1 {
  ProfileDiagnosticV1 outcome;
  ActivationStateV1 state;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

struct DispatchKeyV1 {
  UuidV1 endpoint_uuid{};
  std::uint64_t endpoint_generation = 0;
  std::string approved_profile_id;
  std::uint16_t protocol_major = 0;
  std::uint16_t protocol_minor = 0;
  std::uint16_t message_code = 0;
  std::uint32_t payload_schema_id = 0;
};

struct DispatchDecisionV1 {
  bool admitted = false;
  bool success_only = false;
  bool semantic_refusal = false;
  ProfileDiagnosticV1 outcome;
};

CoreProfileRecordV1 CorePrivateNarrowProfileRecordV1();
EvidenceRecordV1 CorePendingPrivateNarrowEvidenceV1();

ProfileDiagnosticV1 ValidateCorePrivateNarrowProfileRecordV1(
    const CoreProfileRecordV1& record);

ProfileDiagnosticV1 ValidatePrivateNarrowEvidenceV1(
    const CoreProfileRecordV1& profile,
    const EvidenceRecordV1& evidence);

ActivationResultV1 BuildPrivateNarrowActivationStateV1(
    const CoreProfileRecordV1& profile,
    const EvidenceRecordV1& evidence,
    const EndpointActivationRequestV1& request);

DispatchDecisionV1 AdmitPrivateNarrowDispatchV1(
    const CoreProfileRecordV1& profile,
    const ActivationStateV1& state,
    const DispatchKeyV1& key);

std::vector<byte> SerializeActivationRecordV1(
    const ActivationRecordV1& record);

const char* ProfileValidationStatusNameV1(ProfileValidationStatusV1 status);

}  // namespace scratchbird::server::sbps::private_narrow
