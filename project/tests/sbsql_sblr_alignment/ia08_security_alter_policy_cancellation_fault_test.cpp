#include "engine/internal_api/sblr_sec_alter_policy_coordinator.hpp"

#include <cstdint>

namespace {

scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 Descriptor() {
  scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 value;
  auto uuid = [](std::uint8_t seed) {
    scratchbird::engine::sblr::SecAlterPolicyUuid out{};
    for (std::size_t index = 0; index < out.size(); ++index) {
      out[index] = static_cast<std::uint8_t>(seed + index);
    }
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x70);
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3f) | 0x80);
    return out;
  };
  auto sha = [](std::uint8_t seed) {
    scratchbird::engine::sblr::SecAlterPolicySha out{};
    for (std::size_t index = 0; index < out.size(); ++index) {
      out[index] = static_cast<std::uint8_t>(seed + index);
    }
    return out;
  };
  value.receipt = uuid(1);
  value.occurrence = 1;
  value.policy_occurrence = 1;
  value.policy_uuid = uuid(20);
  value.expected_generation = 3;
  value.database_uuid = uuid(40);
  value.owning_transaction_uuid = uuid(60);
  value.owning_local_transaction_id = 7;
  value.statement_snapshot_uuid = uuid(80);
  value.catalog_epoch_uuid = uuid(100);
  value.catalog_generation = 11;
  value.security_context_uuid = uuid(120);
  value.security_generation = 13;
  value.policy_snapshot_uuid = uuid(140);
  value.policy_generation = 17;
  value.resource_grant_uuid = uuid(160);
  value.resource_generation = 19;
  value.principal_uuid = uuid(180);
  value.binding_uuid = uuid(200);
  value.recovery_uuid = uuid(220);
  value.binding_generation = 1;
  value.recovery_generation = 1;
  value.normalized_path_sha256 = sha(1);
  value.syntax_demand_sha256 = sha(40);
  value.authorization_evidence_sha256 = sha(80);
  value.frozen_policy_record_sha256 = sha(120);
  value.availability = 23;
  return value;
}

}  // namespace

int main() {
  scratchbird::engine::internal_api::SblrSecAlterPolicyAuthorityInputV1 input;
  input.descriptor = Descriptor();
  const auto compiled = scratchbird::engine::internal_api::
      CompileSblrSecAlterPolicyDescriptor(input);
  if (!compiled.ok) return 1;
  const auto cancelled = scratchbird::engine::internal_api::
      ValidateSblrSecAlterPolicyDescriptor(input, compiled.descriptor, true);
  return !cancelled.ok && cancelled.diagnostic.code == "PROCESS.CANCELLED"
             ? 0
             : 1;
}
