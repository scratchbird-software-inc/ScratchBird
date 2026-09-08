#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using scratchbird::engine::sblr::SecAlterPolicySha;
using scratchbird::engine::sblr::SecAlterPolicyUuid;
using scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1;
using scratchbird::engine::sblr::SblrSecAlterPolicyRequestV1;
using scratchbird::engine::sblr::SblrSecAlterPolicyResultV1;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

SecAlterPolicyUuid Uuid(std::uint8_t seed) {
  SecAlterPolicyUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  value[6] = static_cast<std::uint8_t>((value[6] & 0x0f) | 0x70);
  value[8] = static_cast<std::uint8_t>((value[8] & 0x3f) | 0x80);
  return value;
}

SecAlterPolicySha Sha(std::uint8_t seed) {
  SecAlterPolicySha value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

SblrSecAlterPolicyDescriptorV1 Descriptor() {
  SblrSecAlterPolicyDescriptorV1 value;
  value.receipt = Uuid(1);
  value.occurrence = 11;
  value.policy_occurrence = 3;
  value.action = 1;
  value.policy_uuid = Uuid(20);
  value.expected_generation = 7;
  value.database_uuid = Uuid(40);
  value.owning_transaction_uuid = Uuid(60);
  value.owning_local_transaction_id = 19;
  value.statement_snapshot_uuid = Uuid(80);
  value.catalog_epoch_uuid = Uuid(100);
  value.catalog_generation = 23;
  value.security_context_uuid = Uuid(120);
  value.security_generation = 29;
  value.policy_snapshot_uuid = Uuid(140);
  value.policy_generation = 31;
  value.resource_grant_uuid = Uuid(160);
  value.resource_generation = 37;
  value.principal_uuid = Uuid(180);
  value.binding_uuid = Uuid(200);
  value.recovery_uuid = Uuid(220);
  value.binding_generation = 1;
  value.recovery_generation = 1;
  value.normalized_path_sha256 = Sha(5);
  value.syntax_demand_sha256 = Sha(45);
  value.authorization_evidence_sha256 = Sha(85);
  value.frozen_policy_record_sha256 = Sha(125);
  value.availability = 41;
  return value;
}

void RequireRequestCodec() {
  SblrSecAlterPolicyRequestV1 request;
  request.receipt = Uuid(1);
  request.occurrence = 11;
  request.policy_occurrence = 3;
  request.name_atoms = {{"security", false}, {"R\xc3\xa9sum\xc3\xa9 Policy", true}};

  const auto encoded =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyRequestV1(request);
  Require(encoded.size() == 896, "SAPQ exact extent was not emitted");
  SblrSecAlterPolicyRequestV1 decoded;
  std::string detail;
  Require(scratchbird::engine::sblr::DecodeSblrSecAlterPolicyRequestV1(
              encoded.data(), encoded.size(), &decoded, &detail),
          "canonical SAPQ was refused");
  Require(decoded.receipt == request.receipt &&
              decoded.occurrence == request.occurrence &&
              decoded.policy_occurrence == request.policy_occurrence &&
              decoded.command_identity == 1 && decoded.name_atoms.size() == 2 &&
              decoded.name_atoms[0].raw_utf8 == "security" &&
              !decoded.name_atoms[0].quoted && decoded.name_atoms[1].quoted,
          "SAPQ fields did not round trip");
  Require(scratchbird::engine::sblr::EncodeSblrSecAlterPolicyRequestV1(
              decoded) == encoded,
          "SAPQ re-encoding was not byte-identical");

  auto tampered = encoded;
  tampered[47] = 0x80;
  Require(!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyRequestV1(
              tampered.data(), tampered.size(), &decoded, &detail),
          "SAPQ invalid quote mask was accepted");
  tampered = encoded;
  tampered[832] ^= 1;
  Require(!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyRequestV1(
              tampered.data(), tampered.size(), &decoded, &detail),
          "SAPQ evidence drift was accepted");
  request.command_identity = 2;
  Require(scratchbird::engine::sblr::EncodeSblrSecAlterPolicyRequestV1(request)
              .empty(),
          "an unallocated policy action was encoded");
}

void RequireDescriptorCodec() {
  auto descriptor = Descriptor();
  const auto issued =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          descriptor, false);
  Require(issued.size() == 488, "SAPD exact extent was not emitted");
  SblrSecAlterPolicyDescriptorV1 decoded;
  std::string detail;
  Require(scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(
              issued.data(), issued.size(), &decoded, &detail, false),
          "canonical SAPD was refused");
  Require(decoded.policy_uuid == descriptor.policy_uuid &&
              decoded.expected_generation == descriptor.expected_generation &&
              decoded.owning_transaction_uuid ==
                  descriptor.owning_transaction_uuid &&
              decoded.syntax_demand_sha256 ==
                  descriptor.syntax_demand_sha256 &&
              decoded.availability == descriptor.availability,
          "SAPD fields did not round trip");

  const auto operand =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          decoded, true);
  Require(operand.size() == issued.size(), "SAPO exact extent was not emitted");
  Require(std::equal(issued.begin() + 4, issued.end(), operand.begin() + 4),
          "SAPD to SAPO changed authority bytes");
  Require(std::string(operand.begin(), operand.begin() + 4) == "SAPO",
          "SAPO magic was not emitted");
  Require(scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(
              operand.data(), operand.size(), &decoded, &detail, true),
          "canonical SAPO was refused");

  auto tampered = operand;
  tampered[200] ^= 1;
  Require(!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(
              tampered.data(), tampered.size(), &decoded, &detail, true),
          "SAPO authority drift was accepted");
  descriptor.recovery_uuid = descriptor.binding_uuid;
  Require(scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
              descriptor, false)
              .empty(),
          "SAPD colliding engine identities were accepted");
}

void RequireResultCodec() {
  const auto descriptor = Descriptor();
  const auto issued =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          descriptor, false);
  SblrSecAlterPolicyDescriptorV1 canonical_descriptor;
  std::string detail;
  Require(scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(
              issued.data(), issued.size(), &canonical_descriptor, &detail,
              false),
          "test SAPD did not canonicalize");

  SblrSecAlterPolicyResultV1 result;
  result.receipt = descriptor.receipt;
  result.policy_uuid = descriptor.policy_uuid;
  result.previous_generation = descriptor.expected_generation;
  result.generation = descriptor.expected_generation + 1;
  result.database_uuid = descriptor.database_uuid;
  result.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id = descriptor.owning_local_transaction_id;
  result.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  result.mutation_uuid = Uuid(11);
  result.cache_invalidation_generation = 43;
  result.security_generation = descriptor.security_generation + 1;
  result.descriptor_evidence = canonical_descriptor.descriptor_evidence;
  result.effect_evidence = Sha(165);
  result.availability = descriptor.availability;
  result.publication_barrier_uuid = Uuid(31);
  result.recovery_uuid = descriptor.recovery_uuid;

  const auto encoded =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyResultV1(result);
  Require(encoded.size() == 320, "SAPR exact extent was not emitted");
  SblrSecAlterPolicyResultV1 decoded;
  Require(scratchbird::engine::sblr::DecodeSblrSecAlterPolicyResultV1(
              encoded.data(), encoded.size(), &decoded, &detail),
          "canonical SAPR was refused");
  Require(decoded.policy_uuid == result.policy_uuid &&
              decoded.generation == result.generation &&
              decoded.previous_generation == result.previous_generation &&
              decoded.descriptor_evidence == result.descriptor_evidence &&
              decoded.publication_barrier_uuid ==
                  result.publication_barrier_uuid &&
              decoded.recovery_uuid == result.recovery_uuid,
          "SAPR fields did not round trip");
  Require(scratchbird::engine::sblr::EncodeSblrSecAlterPolicyResultV1(
              decoded) == encoded,
          "SAPR re-encoding was not byte-identical");

  auto tampered = encoded;
  tampered[224] ^= 1;
  Require(!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyResultV1(
              tampered.data(), tampered.size(), &decoded, &detail),
          "SAPR evidence drift was accepted");
  result.publication_barrier_uuid = result.mutation_uuid;
  Require(scratchbird::engine::sblr::EncodeSblrSecAlterPolicyResultV1(result)
              .empty(),
          "SAPR colliding publication identities were accepted");
}

void RequireSecurityPolicyShowCarrier() {
  namespace sblr = scratchbird::engine::sblr;

  auto envelope = sblr::MakeSblrEnvelope(
      "security.policy.show", "SBLR_SECURITY_POLICY_SHOW",
      "ia09.security_policy_show.exact_uuid_carrier");
  envelope.opcode_code = 1807;
  envelope.result_shape = "security_policy_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019d0000-0000-7000-8000-000000005821";
  envelope.registry_snapshot_uuid =
      "019d0000-0000-7000-8000-000000005822";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "security_policy_show_descriptor";
  operand.name = "policy";
  operand.value_kind = sblr::SblrValueKind::uuid_ref;
  const auto policy_uuid = Uuid(20);
  operand.value_body.assign(policy_uuid.begin(), policy_uuid.end());
  envelope.operands.push_back(operand);

  const auto canonical = sblr::ValidateSblrEnvelope(envelope);
  Require(canonical.ok,
          "canonical SHOW SECURITY POLICY UUID carrier was refused");

  auto malformed = envelope;
  malformed.operands.front().value_body.assign(16, 0);
  const auto zero_uuid = sblr::ValidateSblrEnvelope(malformed);
  Require(!zero_uuid.ok && !zero_uuid.diagnostics.empty() &&
              zero_uuid.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "zero SHOW SECURITY POLICY UUID was accepted");

  malformed = envelope;
  malformed.operands.front().name = "catalog";
  const auto wrong_slot = sblr::ValidateSblrEnvelope(malformed);
  Require(!wrong_slot.ok && !wrong_slot.diagnostics.empty() &&
              wrong_slot.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "alternate SHOW SECURITY POLICY operand slot was accepted");
}

}  // namespace

int main() {
  try {
    RequireRequestCodec();
    RequireDescriptorCodec();
    RequireResultCodec();
    RequireSecurityPolicyShowCarrier();
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
