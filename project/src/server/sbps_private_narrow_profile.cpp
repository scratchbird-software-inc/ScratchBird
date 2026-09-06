// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbps_private_narrow_profile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::server::sbps::private_narrow {
namespace {

constexpr const char* kProfileInvalid = "PARSER_SERVER_IPC.PROFILE_INVALID";
constexpr const char* kActivationInvalid =
    "PARSER_SERVER_IPC.ACTIVATION_RECORD_INVALID";
constexpr const char* kCapabilityInvalid =
    "PARSER_SERVER_IPC.CAPABILITY_INVALID";
constexpr const char* kEvidenceInvalid = "PARSER_SERVER_IPC.EVIDENCE_INVALID";
constexpr const char* kEvidenceRevoked = "PARSER_SERVER_IPC.EVIDENCE_REVOKED";
constexpr const char* kEndpointInvalid =
    "PARSER_SERVER_IPC.ENDPOINT_DESCRIPTOR_INVALID";
constexpr const char* kMessageUnregistered =
    "PARSER_SERVER_IPC.MESSAGE_UNREGISTERED";
constexpr std::string_view kAdmissionState =
    "implementation_evidence_gated";
constexpr std::string_view kAuthLayout =
    "SBPS-AUTH-METADATA-PAYLOAD-BINARY-LAYOUT-V1";
constexpr std::string_view kAuthCorpus =
    "SBPS-AUTH-METADATA-PAYLOAD-CONFORMANCE-MANIFEST-V1";
constexpr std::string_view kExecutionLayout =
    "SBPS-EXECUTION-EVENT-PAYLOAD-BINARY-LAYOUT-V1";
constexpr std::string_view kExecutionCorpus =
    "SBPS-EXECUTION-EVENT-PAYLOAD-CONFORMANCE-MANIFEST-V1";
constexpr std::string_view kBindingLayout =
    "SBPS-NARROW-QUERY-BINDING-COORDINATION-BINARY-LAYOUT-V1";
constexpr std::string_view kStatementContextLayout =
    "SBPS-NARROW-STATEMENT-CONTEXT-COORDINATION-BINARY-LAYOUT-V1";
constexpr std::string_view kContextualTextLayout =
    "SBPS-CONTEXTUAL-TEXT-LITERAL-COORDINATION-BINARY-LAYOUT-V2";
constexpr std::string_view kNarrowCorpus =
    "SBPS-NARROW-PRIVATE-PROFILE-CONFORMANCE-MANIFEST-V1";
constexpr std::string_view kContextualTextCorpus =
    "CONTEXTUAL-TEXT-LITERAL-PROFILE-CONFORMANCE-MANIFEST-V2";

constexpr std::array<PairV1, kRequiredPairCountV1> kRequiredPairs{{
    {1, 1001},   {2, 1002},   {3, 2001},   {10, 1010}, {11, 1011},
    {12, 1012},  {20, 1020},  {21, 1021},  {42, 1042}, {43, 1043},
    {44, 1044},  {45, 1045},  {46, 1046},  {47, 1047}, {50, 1050},
    {51, 1051},  {60, 2001},  {61, 1061},  {70, 1070}, {71, 1071},
    {72, 1072},  {73, 1073},  {74, 1074},  {694, 7707},
    {695, 7708}, {696, 7709}, {697, 7710}, {698, 7711},
    {699, 7712},
    {702, 7715}, {703, 7716}, {704, 7717}, {705, 7718},
    {706, 7719}, {707, 7720},
    {728, 7741}, {729, 7742}, {730, 7743}, {731, 7744},
    {732, 7745}, {733, 7746}, {734, 7747}, {735, 7748},
    {736, 7749}, {737, 7750}, {738, 7751}, {739, 7752},
}};

constexpr std::array<PairV1, kForbiddenPairCountV1> kForbiddenPairs{{
    {30, 1030}, {31, 1031}, {32, 1032}, {33, 1033}, {34, 1034},
    {35, 1035}, {36, 1036}, {40, 1040}, {41, 1041}, {80, 1080},
    {81, 1081}, {82, 1082}, {83, 1083}, {84, 1084}, {85, 1085},
    {86, 1086}, {87, 1087}, {88, 1088},
}};

constexpr std::array<PairV1, kSuccessOnlyPairCountV1> kSuccessOnlyPairs{{
    {43, 1043}, {45, 1045}, {47, 1047}, {51, 1051}, {61, 1061},
    {71, 1071}, {73, 1073}, {695, 7708}, {697, 7710}, {699, 7712},
    {703, 7716}, {705, 7718},
    {707, 7720}, {729, 7742}, {731, 7744},
    {733, 7746}, {735, 7748}, {737, 7750}, {739, 7752},
}};

constexpr std::array<std::string_view, 3> kRequiredRoles{{
    "sb_server", "sb_listener", "parser_worker"}};

constexpr std::array<std::string_view, 7> kRequiredCorpora{{
    "SBPS-AUTH-METADATA-PAYLOAD-CONFORMANCE-MANIFEST-V1",
    "SBPS-EXECUTION-EVENT-PAYLOAD-CONFORMANCE-MANIFEST-V1",
    "SBPS-NARROW-PRIVATE-PROFILE-CONFORMANCE-MANIFEST-V1",
    "CONTEXTUAL-TEXT-LITERAL-PROFILE-CONFORMANCE-MANIFEST-V2",
    "SBPS-PROFILE-ACTIVATION-LIFECYCLE-CONFORMANCE-MANIFEST-V1",
    "LISTENER-ENGINE-IPC-ADAPTER-CONFORMANCE-MANIFEST-V1",
    "SBLR-BULK-IMPORT-STREAM-TRANSPORT-CONFORMANCE-V1",
}};

ProfileDiagnosticV1 Ok() {
  ProfileDiagnosticV1 result;
  result.status = ProfileValidationStatusV1::ok;
  return result;
}

ProfileDiagnosticV1 Error(ProfileValidationStatusV1 status,
                          std::string diagnostic,
                          std::string field,
                          std::string detail) {
  ProfileDiagnosticV1 result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic);
  result.field = std::move(field);
  result.detail = std::move(detail);
  return result;
}

int HexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

UuidV1 ParseUuid(std::string_view text) {
  UuidV1 result{};
  std::size_t nibble = 0;
  for (const auto value : text) {
    if (value == '-') continue;
    const auto digit = HexDigit(value);
    if (digit < 0 || nibble >= 32) return {};
    if ((nibble & 1u) == 0) {
      result[nibble / 2] = static_cast<byte>(digit << 4);
    } else {
      result[nibble / 2] |= static_cast<byte>(digit);
    }
    ++nibble;
  }
  return nibble == 32 ? result : UuidV1{};
}

Hash256V1 ParseHash(std::string_view text) {
  Hash256V1 result{};
  if (text.size() != 64) return result;
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto high = HexDigit(text[index * 2]);
    const auto low = HexDigit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return {};
    result[index] = static_cast<byte>((high << 4) | low);
  }
  return result;
}

bool AnyNonzero(const UuidV1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool AnyNonzero(const Hash256V1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool UuidVersion7(const UuidV1& value) {
  return AnyNonzero(value) && (value[6] & 0xf0u) == 0x70u &&
         (value[8] & 0xc0u) == 0x80u;
}

bool BitmapZero(const CapabilityBitmapV1& bitmap) {
  return std::all_of(bitmap.begin(), bitmap.end(),
                     [](byte octet) { return octet == 0; });
}

std::string UuidText(const UuidV1& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(kHex[(uuid[index] >> 4u) & 0x0fu]);
    result.push_back(kHex[uuid[index] & 0x0fu]);
  }
  return result;
}

std::string HashText(const Hash256V1& hash) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto octet : hash) {
    result.push_back(kHex[(octet >> 4u) & 0x0fu]);
    result.push_back(kHex[octet & 0x0fu]);
  }
  return result;
}

std::string Decimal(std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error != std::errc{}) return {};
  return std::string(buffer.data(), end);
}

bool ActivationTextValueValid(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char octet) {
           return octet >= 0x20u && octet <= 0x7eu && octet != '=';
         });
}

std::pair<std::string_view, std::string_view> AuthorityFor(PairV1 pair) {
  if (pair.payload_schema_id == 7707 || pair.payload_schema_id == 7708) {
    return {kBindingLayout, kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7709 || pair.payload_schema_id == 7710) {
    return {kStatementContextLayout, kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7711 || pair.payload_schema_id == 7712) {
    return {kContextualTextLayout, kContextualTextCorpus};
  }
  if (pair.payload_schema_id >= 7715 && pair.payload_schema_id <= 7718) {
    return {"SBPS-BULK-IMPORT-STREAM-PAYLOAD-LAYOUT-V1",
            "SBLR-BULK-IMPORT-STREAM-TRANSPORT-CONFORMANCE-V1"};
  }
  if (pair.payload_schema_id == 7719 || pair.payload_schema_id == 7720) {
    return {"SBPS-BULK-IMPORT-STREAM-PAYLOAD-LAYOUT-V1",
            "SBLR-BULK-IMPORT-STREAM-TRANSPORT-CONFORMANCE-V1"};
  }
  if (pair.payload_schema_id >= 7741 && pair.payload_schema_id <= 7744) {
    return {"SBLR-OPTIMIZER-STATS-COORDINATION-V1", kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7745 || pair.payload_schema_id == 7746) {
    return {"SBLR-PARSE-TEXT-COORDINATION-V1", kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7747 || pair.payload_schema_id == 7748) {
    return {"SBLR-CATALOG-EPOCH-CHECK-COORDINATION-V1", kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7749 || pair.payload_schema_id == 7750) {
    return {"SBLR-DATABASE-ATTACH-COORDINATION-V1", kNarrowCorpus};
  }
  if (pair.payload_schema_id == 7751 || pair.payload_schema_id == 7752) {
    return {"SBLR-SOURCE-ARTIFACT-EXTERNAL-REFERENCE-V1", kNarrowCorpus};
  }
  if ((pair.payload_schema_id >= 1042 && pair.payload_schema_id <= 1074) &&
      pair.payload_schema_id != 2001) {
    return {kExecutionLayout, kExecutionCorpus};
  }
  return {kAuthLayout, kAuthCorpus};
}

template <typename Range>
bool SortedUnique(const Range& values) {
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
}

bool ExactPairs(const std::vector<PairV1>& actual,
                const auto& expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

std::vector<PairV1> PairUniverse() {
  std::vector<PairV1> result(kRequiredPairs.begin(), kRequiredPairs.end());
  result.insert(result.end(), kForbiddenPairs.begin(), kForbiddenPairs.end());
  std::sort(result.begin(), result.end());
  return result;
}

bool PairIn(const std::vector<PairV1>& values, PairV1 pair) {
  return std::binary_search(values.begin(), values.end(), pair);
}

}  // namespace

std::vector<byte> SerializeActivationRecordV1(
    const ActivationRecordV1& record) {
  const std::array<std::string, 12> fields{{
      Decimal(record.record_version),
      Decimal(record.protocol_major),
      Decimal(record.protocol_minor),
      Decimal(record.pair.message_code),
      Decimal(record.pair.payload_schema_id),
      record.admission_state,
      UuidText(record.activation_set_uuid),
      record.approved_profile_id,
      HashText(record.registry_snapshot_sha256),
      record.layout_authority_id,
      record.conformance_corpus_id,
      record.implementation_evidence_id,
  }};
  if (std::any_of(fields.begin(), fields.end(), [](const auto& field) {
        return !ActivationTextValueValid(field);
      })) {
    return {};
  }
  std::vector<byte> result;
  std::size_t reserve = fields.size();
  for (const auto& field : fields) reserve += field.size();
  result.reserve(reserve);
  for (const auto& field : fields) {
    result.insert(result.end(), field.begin(), field.end());
    result.push_back(0);
  }
  return result;
}

CoreProfileRecordV1 CorePrivateNarrowProfileRecordV1() {
  CoreProfileRecordV1 record;
  record.profile_id = kProfileIdV1;
  record.profile_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000001");
  record.activation_set_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000002");
  record.implementation_evidence_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000003");
  record.endpoint_profile_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000004");
  record.implementation_evidence_id = kImplementationEvidenceIdV1;
  record.registry_snapshot_sha256 = ParseHash(kRegistrySnapshotSha256V1);
  record.protocol_major = kProtocolMajorV1;
  record.protocol_minor = kProtocolMinorV1;
  record.parser_api_major = kParserApiMajorV1;
  record.parser_api_minor = kParserApiMinorV1;
  record.approved = true;
  record.immutable = true;
  record.generic_query_admission = false;
  record.pair_universe = PairUniverse();
  record.required_pairs.assign(kRequiredPairs.begin(), kRequiredPairs.end());
  record.forbidden_pairs.assign(kForbiddenPairs.begin(), kForbiddenPairs.end());
  record.success_only_pairs.assign(kSuccessOnlyPairs.begin(),
                                   kSuccessOnlyPairs.end());
  record.semantic_refusal_pair = {60, 2001};
  for (const auto pair : kRequiredPairs) {
    const auto [layout, corpus] = AuthorityFor(pair);
    ActivationRecordV1 activation;
    activation.pair = pair;
    activation.admission_state = std::string(kAdmissionState);
    activation.activation_set_uuid = record.activation_set_uuid;
    activation.approved_profile_id = record.profile_id;
    activation.registry_snapshot_sha256 = record.registry_snapshot_sha256;
    activation.layout_authority_id = std::string(layout);
    activation.conformance_corpus_id = std::string(corpus);
    activation.implementation_evidence_id = record.implementation_evidence_id;
    activation.exact_nul_serialization = SerializeActivationRecordV1(activation);
    record.candidate_activation_records.push_back(std::move(activation));
  }
  return record;
}

EvidenceRecordV1 CorePendingPrivateNarrowEvidenceV1() {
  EvidenceRecordV1 evidence;
  evidence.implementation_evidence_id = kImplementationEvidenceIdV1;
  evidence.evidence_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000003");
  evidence.activation_set_uuid =
      ParseUuid("019d3b15-6e00-7000-8000-000000000002");
  evidence.approved_profile_id = kProfileIdV1;
  evidence.protocol_major = kProtocolMajorV1;
  evidence.protocol_minor = kProtocolMinorV1;
  evidence.registry_snapshot_sha256 = ParseHash(kRegistrySnapshotSha256V1);
  evidence.status = EvidenceStatusV1::pending;
  evidence.usable = false;
  evidence.revoked = false;
  for (const auto role : kRequiredRoles) {
    RoleEvidenceV1 row;
    row.role = role;
    evidence.role_artifacts.push_back(std::move(row));
  }
  for (const auto corpus : kRequiredCorpora) {
    CorpusEvidenceV1 row;
    row.conformance_corpus_id = corpus;
    evidence.corpus_results.push_back(std::move(row));
  }
  return evidence;
}

ProfileDiagnosticV1 ValidateCorePrivateNarrowProfileRecordV1(
    const CoreProfileRecordV1& record) {
  const auto expected = CorePrivateNarrowProfileRecordV1();
  if (record.profile_id != expected.profile_id ||
      record.profile_uuid != expected.profile_uuid ||
      record.activation_set_uuid != expected.activation_set_uuid ||
      record.implementation_evidence_uuid !=
          expected.implementation_evidence_uuid ||
      record.endpoint_profile_uuid != expected.endpoint_profile_uuid ||
      record.implementation_evidence_id !=
          expected.implementation_evidence_id ||
      record.registry_snapshot_sha256 !=
          expected.registry_snapshot_sha256 ||
      !UuidVersion7(record.profile_uuid) ||
      !UuidVersion7(record.activation_set_uuid) ||
      !UuidVersion7(record.implementation_evidence_uuid) ||
      !UuidVersion7(record.endpoint_profile_uuid) ||
      record.protocol_major != kProtocolMajorV1 ||
      record.protocol_minor != kProtocolMinorV1 ||
      record.parser_api_major != kParserApiMajorV1 ||
      record.parser_api_minor != kParserApiMinorV1 || !record.approved ||
      !record.immutable || record.generic_query_admission) {
    return Error(ProfileValidationStatusV1::profile_invalid,
                 kProfileInvalid, "profile_identity",
                 "private_narrow_profile_identity_or_state_drifted");
  }
  if (!BitmapZero(record.offered_capability_bitmap) ||
      !BitmapZero(record.accepted_capability_bitmap)) {
    return Error(ProfileValidationStatusV1::capability_invalid,
                 kCapabilityInvalid, "capability_bitmap",
                 "private_narrow_profile_capability_bitmap_is_nonzero");
  }
  if (!SortedUnique(record.pair_universe) ||
      record.pair_universe.size() != kPairUniverseCountV1 ||
      record.pair_universe != expected.pair_universe ||
      !ExactPairs(record.required_pairs, kRequiredPairs) ||
      !ExactPairs(record.forbidden_pairs, kForbiddenPairs) ||
      !ExactPairs(record.success_only_pairs, kSuccessOnlyPairs) ||
      record.semantic_refusal_pair != PairV1{60, 2001}) {
    return Error(ProfileValidationStatusV1::profile_invalid,
                 kProfileInvalid, "pair_sets",
                 "required_forbidden_or_success_pair_set_drifted");
  }
  if (!record.actual_active_pairs.empty()) {
    return Error(ProfileValidationStatusV1::profile_invalid,
                 kProfileInvalid, "actual_active_pairs",
                 "core_pending_record_must_retain_empty_active_projection");
  }
  if (record.candidate_activation_records.size() !=
      kRequiredPairCountV1) {
    return Error(ProfileValidationStatusV1::activation_record_invalid,
                 kActivationInvalid, "candidate_activation_records",
                 "activation_record_count_invalid");
  }
  for (std::size_t index = 0;
       index < record.candidate_activation_records.size(); ++index) {
    const auto& activation = record.candidate_activation_records[index];
    const auto [layout, corpus] = AuthorityFor(kRequiredPairs[index]);
    if (activation.record_version != 1 ||
        activation.protocol_major != kProtocolMajorV1 ||
        activation.protocol_minor != kProtocolMinorV1 ||
        activation.pair != kRequiredPairs[index] ||
        activation.admission_state != kAdmissionState ||
        activation.activation_set_uuid != record.activation_set_uuid ||
        activation.approved_profile_id != record.profile_id ||
        activation.registry_snapshot_sha256 !=
            record.registry_snapshot_sha256 ||
        activation.layout_authority_id != layout ||
        activation.conformance_corpus_id != corpus ||
        activation.implementation_evidence_id !=
            record.implementation_evidence_id ||
        activation.exact_nul_serialization.empty() ||
        activation.exact_nul_serialization !=
            SerializeActivationRecordV1(activation) ||
        activation.exact_nul_serialization.back() != 0 ||
        std::count(activation.exact_nul_serialization.begin(),
                   activation.exact_nul_serialization.end(), 0) != 12) {
      return Error(ProfileValidationStatusV1::activation_record_invalid,
                   kActivationInvalid, "candidate_activation_records",
                   "activation_record_or_nul_serialization_drifted");
    }
  }
  return Ok();
}

ProfileDiagnosticV1 ValidatePrivateNarrowEvidenceV1(
    const CoreProfileRecordV1& profile,
    const EvidenceRecordV1& evidence) {
  auto profile_outcome = ValidateCorePrivateNarrowProfileRecordV1(profile);
  if (!profile_outcome.ok()) return profile_outcome;
  if (evidence.implementation_evidence_id !=
          profile.implementation_evidence_id ||
      evidence.evidence_uuid != profile.implementation_evidence_uuid ||
      evidence.activation_set_uuid != profile.activation_set_uuid ||
      evidence.approved_profile_id != profile.profile_id ||
      evidence.protocol_major != profile.protocol_major ||
      evidence.protocol_minor != profile.protocol_minor ||
      evidence.registry_snapshot_sha256 !=
          profile.registry_snapshot_sha256) {
    return Error(ProfileValidationStatusV1::evidence_invalid,
                 kEvidenceInvalid, "evidence_identity",
                 "evidence_tuple_does_not_match_profile");
  }
  if (evidence.revoked || evidence.status == EvidenceStatusV1::revoked ||
      evidence.status == EvidenceStatusV1::expired) {
    return Error(ProfileValidationStatusV1::evidence_revoked,
                 kEvidenceRevoked, "evidence_status",
                 "evidence_is_revoked_or_expired");
  }
  if (evidence.status == EvidenceStatusV1::pending || !evidence.usable) {
    return Error(ProfileValidationStatusV1::evidence_pending,
                 kEvidenceInvalid, "evidence_status",
                 "implementation_evidence_is_pending_or_not_usable");
  }
  if (evidence.status != EvidenceStatusV1::usable ||
      evidence.role_artifacts.size() != kRequiredRoles.size() ||
      evidence.corpus_results.size() != kRequiredCorpora.size() ||
      !AnyNonzero(evidence.endpoint_generation_evidence_sha256) ||
      !AnyNonzero(evidence.aggregate_evidence_sha256)) {
    return Error(ProfileValidationStatusV1::evidence_invalid,
                 kEvidenceInvalid, "evidence_completeness",
                 "usable_evidence_is_incomplete");
  }
  std::set<std::string_view> roles;
  for (const auto& role : evidence.role_artifacts) {
    if (!roles.insert(role.role).second || !role.verified ||
        !AnyNonzero(role.implementation_artifact_sha256) ||
        !AnyNonzero(role.executable_fixture_sha256) ||
        !AnyNonzero(role.verification_result_sha256)) {
      return Error(ProfileValidationStatusV1::evidence_invalid,
                   kEvidenceInvalid, "role_artifacts",
                   "role_evidence_is_duplicate_or_incomplete");
    }
  }
  const std::set<std::string_view> expected_roles(kRequiredRoles.begin(),
                                                   kRequiredRoles.end());
  if (roles != expected_roles) {
    return Error(ProfileValidationStatusV1::evidence_invalid,
                 kEvidenceInvalid, "role_artifacts",
                 "role_evidence_set_is_not_exact");
  }
  std::set<std::string_view> corpora;
  for (const auto& corpus : evidence.corpus_results) {
    if (!corpora.insert(corpus.conformance_corpus_id).second ||
        !corpus.passed || !AnyNonzero(corpus.manifest_sha256) ||
        !AnyNonzero(corpus.executable_fixture_set_sha256) ||
        !AnyNonzero(corpus.result_sha256)) {
      return Error(ProfileValidationStatusV1::evidence_invalid,
                   kEvidenceInvalid, "corpus_results",
                   "corpus_evidence_is_duplicate_or_incomplete");
    }
  }
  const std::set<std::string_view> expected_corpora(kRequiredCorpora.begin(),
                                                     kRequiredCorpora.end());
  if (corpora != expected_corpora) {
    return Error(ProfileValidationStatusV1::evidence_invalid,
                 kEvidenceInvalid, "corpus_results",
                 "corpus_evidence_set_is_not_exact");
  }
  return Ok();
}

ActivationResultV1 BuildPrivateNarrowActivationStateV1(
    const CoreProfileRecordV1& profile,
    const EvidenceRecordV1& evidence,
    const EndpointActivationRequestV1& request) {
  ActivationResultV1 result;
  result.outcome = ValidateCorePrivateNarrowProfileRecordV1(profile);
  if (!result.outcome.ok()) return result;
  result.outcome = ValidatePrivateNarrowEvidenceV1(profile, evidence);
  if (!result.outcome.ok()) return result;
  if (!BitmapZero(request.offered_capability_bitmap) ||
      !BitmapZero(request.accepted_capability_bitmap)) {
    result.outcome = Error(ProfileValidationStatusV1::capability_invalid,
                           kCapabilityInvalid, "capability_bitmap",
                           "activation_proposed_a_nonzero_capability_bit");
    return result;
  }
  if (request.endpoint_profile_uuid != profile.endpoint_profile_uuid ||
      !UuidVersion7(request.live_endpoint_uuid) ||
      request.live_endpoint_uuid == profile.endpoint_profile_uuid ||
      request.live_endpoint_uuid == profile.profile_uuid ||
      request.live_endpoint_uuid == profile.activation_set_uuid ||
      request.live_endpoint_uuid == profile.implementation_evidence_uuid ||
      request.live_endpoint_generation == 0 ||
      request.approved_profile_id != profile.profile_id ||
      request.activation_set_uuid != profile.activation_set_uuid ||
      request.protocol_major != profile.protocol_major ||
      request.protocol_minor != profile.protocol_minor) {
    result.outcome = Error(ProfileValidationStatusV1::endpoint_invalid,
                           kEndpointInvalid, "endpoint_binding",
                           "endpoint_profile_or_live_generation_is_invalid");
    return result;
  }
  if (request.proposed_active_pairs != profile.required_pairs) {
    result.outcome = Error(ProfileValidationStatusV1::profile_invalid,
                           kProfileInvalid, "proposed_active_pairs",
                           "active_pair_set_is_not_the_exact_required_set");
    return result;
  }
  result.state.active = true;
  result.state.endpoint_profile_uuid = request.endpoint_profile_uuid;
  result.state.live_endpoint_uuid = request.live_endpoint_uuid;
  result.state.live_endpoint_generation = request.live_endpoint_generation;
  result.state.approved_profile_id = request.approved_profile_id;
  result.state.protocol_major = request.protocol_major;
  result.state.protocol_minor = request.protocol_minor;
  result.state.active_pairs = request.proposed_active_pairs;
  result.outcome = Ok();
  return result;
}

DispatchDecisionV1 AdmitPrivateNarrowDispatchV1(
    const CoreProfileRecordV1& profile,
    const ActivationStateV1& state,
    const DispatchKeyV1& key) {
  DispatchDecisionV1 result;
  result.outcome = ValidateCorePrivateNarrowProfileRecordV1(profile);
  if (!result.outcome.ok()) return result;
  const PairV1 pair{key.message_code, key.payload_schema_id};
  if (!state.active || state.endpoint_profile_uuid != profile.endpoint_profile_uuid ||
      state.approved_profile_id != profile.profile_id ||
      state.protocol_major != profile.protocol_major ||
      state.protocol_minor != profile.protocol_minor ||
      state.active_pairs != profile.required_pairs ||
      key.endpoint_uuid != state.live_endpoint_uuid ||
      key.endpoint_generation != state.live_endpoint_generation ||
      key.approved_profile_id != state.approved_profile_id ||
      key.protocol_major != state.protocol_major ||
      key.protocol_minor != state.protocol_minor ||
      !PairIn(state.active_pairs, pair)) {
    result.outcome = Error(ProfileValidationStatusV1::message_unregistered,
                           kMessageUnregistered, "dispatch_key",
                           "endpoint_profile_generation_or_exact_pair_inactive");
    return result;
  }
  result.admitted = true;
  result.success_only = PairIn(profile.success_only_pairs, pair);
  result.semantic_refusal = pair == profile.semantic_refusal_pair;
  result.outcome = Ok();
  return result;
}

const char* ProfileValidationStatusNameV1(ProfileValidationStatusV1 status) {
  switch (status) {
    case ProfileValidationStatusV1::ok:
      return "ok";
    case ProfileValidationStatusV1::core_record_invalid:
      return "core_record_invalid";
    case ProfileValidationStatusV1::activation_record_invalid:
      return "activation_record_invalid";
    case ProfileValidationStatusV1::profile_invalid:
      return "profile_invalid";
    case ProfileValidationStatusV1::capability_invalid:
      return "capability_invalid";
    case ProfileValidationStatusV1::evidence_pending:
      return "evidence_pending";
    case ProfileValidationStatusV1::evidence_invalid:
      return "evidence_invalid";
    case ProfileValidationStatusV1::evidence_revoked:
      return "evidence_revoked";
    case ProfileValidationStatusV1::endpoint_invalid:
      return "endpoint_invalid";
    case ProfileValidationStatusV1::message_unregistered:
      return "message_unregistered";
  }
  return "unknown";
}

}  // namespace scratchbird::server::sbps::private_narrow
