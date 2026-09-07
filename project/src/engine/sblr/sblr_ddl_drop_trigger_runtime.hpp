#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using DdlDropTriggerUuid = std::array<std::uint8_t, 16>;
using DdlDropTriggerSha = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kSblrDdlDropTriggerRequestV1Bytes = 864;
inline constexpr std::size_t kSblrDdlDropTriggerDescriptorV1Bytes = 488;
inline constexpr std::size_t kSblrDdlDropTriggerResultV1Bytes = 320;

enum class DdlDropTriggerDependencyModeV1 : std::uint8_t {
  kRestrict = 1,
  kCascade = 2,
};

struct SblrDdlDropTriggerNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// TDQX is syntax demand only. Trigger identity, generations, dependency state,
// authorization, recovery, and publication authority are engine-owned.
struct SblrDdlDropTriggerRequestV1 {
  DdlDropTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  std::uint16_t command_identity = 1;
  DdlDropTriggerDependencyModeV1 dependency_mode =
      DdlDropTriggerDependencyModeV1::kRestrict;
  std::vector<SblrDdlDropTriggerNameAtomV1> trigger_name_atoms;
  DdlDropTriggerSha evidence{};
};

struct SblrDdlDropTriggerDescriptorV1 {
  DdlDropTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  DdlDropTriggerDependencyModeV1 dependency_mode =
      DdlDropTriggerDependencyModeV1::kRestrict;
  DdlDropTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlDropTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlDropTriggerUuid target_relation_descriptor_uuid{};
  std::uint64_t target_relation_descriptor_generation = 0;
  DdlDropTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlDropTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlDropTriggerUuid statement_snapshot_uuid{};
  DdlDropTriggerUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  DdlDropTriggerUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  DdlDropTriggerUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DdlDropTriggerUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  DdlDropTriggerUuid owner_principal_uuid{};
  DdlDropTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  DdlDropTriggerUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  DdlDropTriggerSha syntax_demand_sha256{};
  DdlDropTriggerSha authority_bundle_sha256{};
  DdlDropTriggerSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrDdlDropTriggerResultV1 {
  DdlDropTriggerUuid receipt{};
  DdlDropTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlDropTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlDropTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlDropTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlDropTriggerUuid statement_snapshot_uuid{};
  DdlDropTriggerUuid catalog_row_uuid{};
  DdlDropTriggerUuid mutation_uuid{};
  DdlDropTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_generation = 0;
  DdlDropTriggerSha descriptor_evidence_sha256{};
  DdlDropTriggerSha evidence{};
  std::uint64_t availability = 0;
  DdlDropTriggerUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDdlDropTriggerRequestV1(
    const SblrDdlDropTriggerRequestV1&);
bool DecodeSblrDdlDropTriggerRequestV1(
    const std::uint8_t*, std::size_t, SblrDdlDropTriggerRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropTriggerDescriptorV1(
    const SblrDdlDropTriggerDescriptorV1&, bool operand_magic);
bool DecodeSblrDdlDropTriggerDescriptorV1(
    const std::uint8_t*, std::size_t, SblrDdlDropTriggerDescriptorV1*,
    std::string*, bool operand_magic);
std::vector<std::uint8_t> EncodeSblrDdlDropTriggerResultV1(
    const SblrDdlDropTriggerResultV1&);
bool DecodeSblrDdlDropTriggerResultV1(
    const std::uint8_t*, std::size_t, SblrDdlDropTriggerResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
