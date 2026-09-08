#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using SecAlterPolicyUuid = std::array<std::uint8_t, 16>;
using SecAlterPolicySha = std::array<std::uint8_t, 32>;

struct SblrSecAlterPolicyNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// SAPQ is syntax demand only. The parser may present the ACTIVATE action and
// one to three raw name atoms with quote bits. Policy, catalog, security,
// resource, authorization, recovery, and result authority remain engine-owned.
struct SblrSecAlterPolicyRequestV1 {
  SecAlterPolicyUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t policy_occurrence = 0;
  std::uint16_t command_identity = 1;  // 1 = ACTIVATE POLICY
  std::vector<SblrSecAlterPolicyNameAtomV1> name_atoms;
  SecAlterPolicySha evidence{};
};

// SAPD and SAPO share an exact header-neutral 400-byte body. SAPO is a
// literal magic-only projection of the engine-issued SAPD.
struct SblrSecAlterPolicyDescriptorV1 {
  SecAlterPolicyUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t policy_occurrence = 0;
  std::uint16_t action = 1;  // 1 = activate
  std::uint16_t flags = 0;
  SecAlterPolicyUuid policy_uuid{};
  std::uint64_t expected_generation = 0;
  SecAlterPolicyUuid database_uuid{};
  SecAlterPolicyUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  SecAlterPolicyUuid statement_snapshot_uuid{};
  SecAlterPolicyUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  SecAlterPolicyUuid security_context_uuid{};
  std::uint64_t security_generation = 0;
  SecAlterPolicyUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SecAlterPolicyUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  SecAlterPolicyUuid principal_uuid{};
  SecAlterPolicyUuid binding_uuid{};
  SecAlterPolicyUuid recovery_uuid{};
  std::uint64_t binding_generation = 0;
  std::uint64_t recovery_generation = 0;
  SecAlterPolicySha normalized_path_sha256{};
  SecAlterPolicySha syntax_demand_sha256{};
  SecAlterPolicySha authorization_evidence_sha256{};
  SecAlterPolicySha frozen_policy_record_sha256{};
  SecAlterPolicySha descriptor_evidence{};
  std::uint64_t availability = 0;
};

struct SblrSecAlterPolicyResultV1 {
  SecAlterPolicyUuid receipt{};
  SecAlterPolicyUuid policy_uuid{};
  std::uint64_t generation = 0;
  std::uint64_t previous_generation = 0;
  SecAlterPolicyUuid database_uuid{};
  SecAlterPolicyUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  SecAlterPolicyUuid statement_snapshot_uuid{};
  SecAlterPolicyUuid mutation_uuid{};
  std::uint64_t cache_invalidation_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint16_t action = 1;
  std::uint8_t status = 1;
  std::uint8_t publication_barrier = 1;
  SecAlterPolicySha descriptor_evidence{};
  SecAlterPolicySha effect_evidence{};
  SecAlterPolicySha result_evidence{};
  std::uint64_t availability = 0;
  SecAlterPolicyUuid publication_barrier_uuid{};
  SecAlterPolicyUuid recovery_uuid{};
};

std::vector<std::uint8_t> EncodeSblrSecAlterPolicyRequestV1(
    const SblrSecAlterPolicyRequestV1&);
bool DecodeSblrSecAlterPolicyRequestV1(
    const std::uint8_t*, std::size_t, SblrSecAlterPolicyRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrSecAlterPolicyDescriptorV1(
    const SblrSecAlterPolicyDescriptorV1&, bool operand_magic);
bool DecodeSblrSecAlterPolicyDescriptorV1(
    const std::uint8_t*, std::size_t, SblrSecAlterPolicyDescriptorV1*,
    std::string*, bool operand_magic);
std::vector<std::uint8_t> EncodeSblrSecAlterPolicyResultV1(
    const SblrSecAlterPolicyResultV1&);
bool DecodeSblrSecAlterPolicyResultV1(
    const std::uint8_t*, std::size_t, SblrSecAlterPolicyResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
