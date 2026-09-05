// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "server/sbps_private_narrow_profile.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

namespace profile = scratchbird::server::sbps::private_narrow;
using scratchbird::core::platform::byte;

static_assert(profile::kPairUniverseCountV1 == 63);
static_assert(profile::kRequiredPairCountV1 == 45);
static_assert(profile::kForbiddenPairCountV1 == 18);
static_assert(profile::kSuccessOnlyPairCountV1 == 18);

void Require(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}

profile::UuidV1 Uuid(std::uint16_t discriminator) {
  profile::UuidV1 uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9d;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[14] = static_cast<byte>((discriminator >> 8u) & 0xffu);
  uuid[15] = static_cast<byte>(discriminator & 0xffu);
  return uuid;
}

profile::Hash256V1 Hash(byte discriminator) {
  profile::Hash256V1 hash{};
  hash.fill(discriminator);
  return hash;
}

profile::EvidenceRecordV1 SyntheticUsableEvidenceFixture() {
  auto evidence = profile::CorePendingPrivateNarrowEvidenceV1();
  evidence.status = profile::EvidenceStatusV1::usable;
  evidence.usable = true;
  byte discriminator = 1;
  for (auto& role : evidence.role_artifacts) {
    role.implementation_artifact_sha256 = Hash(discriminator++);
    role.executable_fixture_sha256 = Hash(discriminator++);
    role.verification_result_sha256 = Hash(discriminator++);
    role.verified = true;
  }
  for (auto& corpus : evidence.corpus_results) {
    corpus.manifest_sha256 = Hash(discriminator++);
    corpus.executable_fixture_set_sha256 = Hash(discriminator++);
    corpus.result_sha256 = Hash(discriminator++);
    corpus.passed = true;
  }
  evidence.endpoint_generation_evidence_sha256 = Hash(discriminator++);
  evidence.aggregate_evidence_sha256 = Hash(discriminator++);
  return evidence;
}

profile::EndpointActivationRequestV1 ActivationRequest(
    const profile::CoreProfileRecordV1& core) {
  profile::EndpointActivationRequestV1 request;
  request.endpoint_profile_uuid = core.endpoint_profile_uuid;
  request.live_endpoint_uuid = Uuid(0x9001);
  request.live_endpoint_generation = 1;
  request.approved_profile_id = core.profile_id;
  request.activation_set_uuid = core.activation_set_uuid;
  request.protocol_major = core.protocol_major;
  request.protocol_minor = core.protocol_minor;
  request.proposed_active_pairs = core.required_pairs;
  return request;
}

void CoreRecordAndPendingEvidenceStayInactive() {
  const auto core = profile::CorePrivateNarrowProfileRecordV1();
  Require(profile::ValidateCorePrivateNarrowProfileRecordV1(core).ok(),
          "compiled Core private profile record failed validation");
  Require(core.pair_universe.size() == 63 &&
              core.required_pairs.size() == 45 &&
              core.forbidden_pairs.size() == 18 &&
              core.candidate_activation_records.size() == 45 &&
              core.actual_active_pairs.empty(),
          "compiled Core pair projection drifted");
  const auto contains = [](const auto& pairs, std::uint16_t message,
                           std::uint32_t schema) {
    return std::find(pairs.begin(), pairs.end(),
                     profile::PairV1{message, schema}) != pairs.end();
  };
  for (const auto [message, schema] : {
           profile::PairV1{702, 7715}, profile::PairV1{703, 7716},
           profile::PairV1{704, 7717}, profile::PairV1{705, 7718},
           profile::PairV1{706, 7719}, profile::PairV1{707, 7720}}) {
    Require(contains(core.pair_universe, message, schema) &&
                contains(core.required_pairs, message, schema) &&
                !contains(core.forbidden_pairs, message, schema),
            "one exact bulk-import message/schema pair is not in the "
            "compiled SFPS1 required universe");
  }
  for (const auto [request_message, request_schema] : {
           profile::PairV1{702, 7715}, profile::PairV1{704, 7717},
           profile::PairV1{706, 7719}}) {
    Require(!contains(core.success_only_pairs, request_message,
                      request_schema),
            "a parser-to-server bulk-import request was marked success-only");
  }
  for (const auto [ack_message, ack_schema] : {
           profile::PairV1{703, 7716}, profile::PairV1{705, 7718},
           profile::PairV1{707, 7720}}) {
    Require(contains(core.success_only_pairs, ack_message, ack_schema),
            "a server-to-parser bulk-import ACK lacks success-only role");
  }
  for (const auto [message, schema] : {
           profile::PairV1{728, 7741}, profile::PairV1{729, 7742},
           profile::PairV1{730, 7743}, profile::PairV1{731, 7744}}) {
    Require(contains(core.pair_universe, message, schema) &&
                contains(core.required_pairs, message, schema) &&
                !contains(core.forbidden_pairs, message, schema),
            "one exact optimizer-statistics message/schema pair is not in "
            "the compiled SFPS1 required universe");
  }
  for (const auto [request_message, request_schema] : {
           profile::PairV1{728, 7741}, profile::PairV1{730, 7743}}) {
    Require(!contains(core.success_only_pairs, request_message,
                      request_schema),
            "an optimizer-statistics request was marked success-only");
  }
  for (const auto [result_message, result_schema] : {
           profile::PairV1{729, 7742}, profile::PairV1{731, 7744}}) {
    Require(contains(core.success_only_pairs, result_message, result_schema),
            "an optimizer-statistics result lacks success-only role");
  }
  for (const auto [message, schema] : {
           profile::PairV1{732, 7745}, profile::PairV1{733, 7746}}) {
    Require(contains(core.pair_universe, message, schema) &&
                contains(core.required_pairs, message, schema) &&
                !contains(core.forbidden_pairs, message, schema),
            "one exact PARSE TEXT bind message/schema pair is not in the "
            "compiled SFPS1 required universe");
  }
  Require(!contains(core.success_only_pairs, 732, 7745),
          "the PARSE TEXT bind request was marked success-only");
  Require(contains(core.success_only_pairs, 733, 7746),
          "the PARSE TEXT bind result lacks success-only role");
  for (const auto [message, schema] : {
           profile::PairV1{734, 7747}, profile::PairV1{735, 7748}}) {
    Require(contains(core.pair_universe, message, schema) &&
                contains(core.required_pairs, message, schema) &&
                !contains(core.forbidden_pairs, message, schema),
            "one exact CATALOG EPOCH CHECK bind pair is not in the "
            "compiled SFPS1 required universe");
  }
  Require(!contains(core.success_only_pairs, 734, 7747),
          "the CATALOG EPOCH CHECK bind request was marked success-only");
  Require(contains(core.success_only_pairs, 735, 7748),
          "the CATALOG EPOCH CHECK bind result lacks success-only role");
  for (const auto [message, schema] : {
           profile::PairV1{736, 7749}, profile::PairV1{737, 7750}}) {
    Require(contains(core.pair_universe, message, schema) &&
                contains(core.required_pairs, message, schema) &&
                !contains(core.forbidden_pairs, message, schema),
            "one exact DATABASE ATTACH bind pair is not in the compiled "
            "SFPS1 required universe");
  }
  Require(!contains(core.success_only_pairs, 736, 7749),
          "the DATABASE ATTACH bind request was marked success-only");
  Require(contains(core.success_only_pairs, 737, 7750),
          "the DATABASE ATTACH bind result lacks success-only role");
  for (const auto& record : core.candidate_activation_records) {
    Require(!record.exact_nul_serialization.empty() &&
                record.exact_nul_serialization.back() == 0 &&
                record.exact_nul_serialization ==
                    profile::SerializeActivationRecordV1(record),
            "activation record NUL serialization drifted");
  }

  const auto pending = profile::CorePendingPrivateNarrowEvidenceV1();
  const auto evidence =
      profile::ValidatePrivateNarrowEvidenceV1(core, pending);
  Require(evidence.status == profile::ProfileValidationStatusV1::evidence_pending,
          "manifest-listed pending evidence was not fail-closed");
  const auto activation = profile::BuildPrivateNarrowActivationStateV1(
      core, pending, ActivationRequest(core));
  Require(!activation.ok() && !activation.state.active &&
              activation.state.active_pairs.empty(),
          "pending evidence created an active pair projection");
}

void CoreAndEvidenceNegativeDrift() {
  const auto core = profile::CorePrivateNarrowProfileRecordV1();
  auto capability = core;
  capability.offered_capability_bitmap[0] = 1;
  Require(profile::ValidateCorePrivateNarrowProfileRecordV1(capability).status ==
              profile::ProfileValidationStatusV1::capability_invalid,
          "nonzero capability bit was admitted");

  auto complement = core;
  complement.forbidden_pairs.pop_back();
  Require(!profile::ValidateCorePrivateNarrowProfileRecordV1(complement).ok(),
          "45/18 complement drift was admitted");

  auto nul_drift = core;
  nul_drift.candidate_activation_records.front()
      .exact_nul_serialization.back() = 'x';
  Require(profile::ValidateCorePrivateNarrowProfileRecordV1(nul_drift).status ==
              profile::ProfileValidationStatusV1::activation_record_invalid,
          "activation-record trailing NUL drift was admitted");

  auto incomplete = SyntheticUsableEvidenceFixture();
  incomplete.role_artifacts.front().verification_result_sha256 = {};
  Require(profile::ValidatePrivateNarrowEvidenceV1(core, incomplete).status ==
              profile::ProfileValidationStatusV1::evidence_invalid,
          "incomplete usable evidence was admitted");

  auto revoked = SyntheticUsableEvidenceFixture();
  revoked.status = profile::EvidenceStatusV1::revoked;
  revoked.revoked = true;
  const auto activation = profile::BuildPrivateNarrowActivationStateV1(
      core, revoked, ActivationRequest(core));
  Require(activation.outcome.status ==
              profile::ProfileValidationStatusV1::evidence_revoked &&
              activation.state.active_pairs.empty(),
          "revoked evidence retained active pairs");
}

void SyntheticUsableFixtureProvesExactDispatchOnly() {
  const auto core = profile::CorePrivateNarrowProfileRecordV1();
  const auto evidence = SyntheticUsableEvidenceFixture();
  const auto activation = profile::BuildPrivateNarrowActivationStateV1(
      core, evidence, ActivationRequest(core));
  Require(activation.ok() && activation.state.active &&
              activation.state.active_pairs == core.required_pairs,
          "synthetic complete evidence fixture did not activate exact pairs");

  const auto key = [&](std::uint16_t message, std::uint32_t schema) {
    profile::DispatchKeyV1 value;
    value.endpoint_uuid = activation.state.live_endpoint_uuid;
    value.endpoint_generation = activation.state.live_endpoint_generation;
    value.approved_profile_id = activation.state.approved_profile_id;
    value.protocol_major = activation.state.protocol_major;
    value.protocol_minor = activation.state.protocol_minor;
    value.message_code = message;
    value.payload_schema_id = schema;
    return value;
  };
  const auto execute = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(42, 1042));
  Require(execute.admitted && !execute.success_only &&
              !execute.semantic_refusal,
          "exact 42/1042 dispatch failed");
  const auto result = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(43, 1043));
  Require(result.admitted && result.success_only &&
              !result.semantic_refusal,
          "43/1043 success-only overlay failed");
  const auto contextual_issue = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(698, 7711));
  Require(contextual_issue.admitted && !contextual_issue.success_only &&
              !contextual_issue.semantic_refusal,
          "698/7711 contextual-TEXT issue dispatch failed");
  const auto contextual_result = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(699, 7712));
  Require(contextual_result.admitted && contextual_result.success_only &&
              !contextual_result.semantic_refusal,
          "699/7712 contextual-TEXT success-only overlay failed");
  const auto refusal = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(60, 2001));
  Require(refusal.admitted && !refusal.success_only &&
              refusal.semantic_refusal,
          "60/2001 refusal identity failed");
  const auto chunk = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(702, 7715));
  Require(chunk.admitted && !chunk.success_only,
          "702/7715 chunk dispatch failed");
  const auto seal_ack = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(705, 7718));
  Require(seal_ack.admitted && seal_ack.success_only,
          "705/7718 seal ACK dispatch failed");
  const auto bind_ack = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(707, 7720));
  Require(bind_ack.admitted && bind_ack.success_only,
          "707/7720 bind ACK dispatch failed");
  const auto epoch_bind = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(734, 7747));
  Require(epoch_bind.admitted && !epoch_bind.success_only,
          "734/7747 catalog-epoch bind dispatch failed");
  const auto epoch_bind_result = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(735, 7748));
  Require(epoch_bind_result.admitted && epoch_bind_result.success_only,
          "735/7748 catalog-epoch bind result dispatch failed");
  const auto database_attach_bind = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(736, 7749));
  Require(database_attach_bind.admitted && !database_attach_bind.success_only,
          "736/7749 database-attach bind dispatch failed");
  const auto database_attach_bind_result =
      profile::AdmitPrivateNarrowDispatchV1(
          core, activation.state, key(737, 7750));
  Require(database_attach_bind_result.admitted &&
              database_attach_bind_result.success_only,
          "737/7750 database-attach bind result dispatch failed");

  const auto mixed_pair = profile::AdmitPrivateNarrowDispatchV1(
      core, activation.state, key(42, 1043));
  Require(!mixed_pair.admitted && mixed_pair.outcome.status ==
              profile::ProfileValidationStatusV1::message_unregistered,
          "message-only/schema-only dispatch was admitted");
  auto wrong_generation = key(42, 1042);
  ++wrong_generation.endpoint_generation;
  Require(!profile::AdmitPrivateNarrowDispatchV1(
               core, activation.state, wrong_generation)
               .admitted,
          "cross-generation dispatch was admitted");
}

}  // namespace

int main() {
  try {
    CoreRecordAndPendingEvidenceStayInactive();
    CoreAndEvidenceNegativeDrift();
    SyntheticUsableFixtureProvesExactDispatchOnly();
  } catch (const std::exception& error) {
    (void)error;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
