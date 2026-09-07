#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using DdlAlterTriggerUuid = std::array<std::uint8_t, 16>;
using DdlAlterTriggerSha = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kSblrDdlAlterTriggerRequestV1Bytes = 864;
inline constexpr std::size_t kSblrDdlAlterTriggerDescriptorV1Bytes = 488;
inline constexpr std::size_t kSblrDdlAlterTriggerResultV1Bytes = 320;

enum class DdlAlterTriggerEnabledStateV1 : std::uint8_t {
  kPreserve = 0,
  kEnabled = 1,
  kDisabled = 2,
};

enum class DdlAlterTriggerSecurityModeV1 : std::uint8_t {
  kPreserve = 0,
  kInvoker = 1,
  kDefiner = 2,
};

enum class DdlAlterTriggerFailurePolicyV1 : std::uint8_t {
  kPreserve = 0,
  kMandatory = 1,
};

inline constexpr std::uint16_t kDdlAlterTriggerActionEnabled = 1u << 0;
inline constexpr std::uint16_t kDdlAlterTriggerActionPosition = 1u << 1;
inline constexpr std::uint16_t kDdlAlterTriggerActionSecurity = 1u << 2;
inline constexpr std::uint16_t kDdlAlterTriggerActionCompile = 1u << 3;
inline constexpr std::uint16_t kDdlAlterTriggerActionValidate = 1u << 4;
inline constexpr std::uint16_t kDdlAlterTriggerActionFailurePolicy = 1u << 5;
inline constexpr std::uint16_t kDdlAlterTriggerActionMask =
    kDdlAlterTriggerActionEnabled | kDdlAlterTriggerActionPosition |
    kDdlAlterTriggerActionSecurity | kDdlAlterTriggerActionCompile |
    kDdlAlterTriggerActionValidate | kDdlAlterTriggerActionFailurePolicy;

struct SblrDdlAlterTriggerNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// TAQX is syntax demand only. It contains the presented trigger name and the
// closed ALTER action set; it contains no catalog UUID or generation.
struct SblrDdlAlterTriggerRequestV1 {
  DdlAlterTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  std::uint16_t command_identity = 1;
  std::uint16_t action_mask = 0;
  DdlAlterTriggerEnabledStateV1 enabled_state =
      DdlAlterTriggerEnabledStateV1::kPreserve;
  std::uint16_t position = 0;
  DdlAlterTriggerSecurityModeV1 security_mode =
      DdlAlterTriggerSecurityModeV1::kPreserve;
  DdlAlterTriggerFailurePolicyV1 failure_policy =
      DdlAlterTriggerFailurePolicyV1::kPreserve;
  std::vector<SblrDdlAlterTriggerNameAtomV1> trigger_name_atoms;
  DdlAlterTriggerSha evidence{};
};

// TADX and TADO share an exact header-neutral body. TADO must be produced by
// changing only the four-byte magic of the engine-issued TADX carrier.
struct SblrDdlAlterTriggerDescriptorV1 {
  DdlAlterTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  std::uint16_t action_mask = 0;
  DdlAlterTriggerEnabledStateV1 enabled_state =
      DdlAlterTriggerEnabledStateV1::kPreserve;
  std::uint16_t position = 0;
  DdlAlterTriggerSecurityModeV1 security_mode =
      DdlAlterTriggerSecurityModeV1::kPreserve;
  DdlAlterTriggerFailurePolicyV1 failure_policy =
      DdlAlterTriggerFailurePolicyV1::kPreserve;
  DdlAlterTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlAlterTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlAlterTriggerUuid target_relation_descriptor_uuid{};
  std::uint64_t target_relation_descriptor_generation = 0;
  DdlAlterTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlAlterTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlAlterTriggerUuid statement_snapshot_uuid{};
  DdlAlterTriggerUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  DdlAlterTriggerUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  DdlAlterTriggerUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DdlAlterTriggerUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  DdlAlterTriggerUuid owner_principal_uuid{};
  DdlAlterTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  DdlAlterTriggerUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  DdlAlterTriggerSha syntax_demand_sha256{};
  DdlAlterTriggerSha authority_bundle_sha256{};
  DdlAlterTriggerSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrDdlAlterTriggerResultV1 {
  DdlAlterTriggerUuid receipt{};
  DdlAlterTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlAlterTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlAlterTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlAlterTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlAlterTriggerUuid statement_snapshot_uuid{};
  DdlAlterTriggerUuid catalog_row_uuid{};
  DdlAlterTriggerUuid mutation_uuid{};
  DdlAlterTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_generation = 0;
  DdlAlterTriggerSha descriptor_evidence_sha256{};
  DdlAlterTriggerSha evidence{};
  std::uint64_t availability = 0;
  DdlAlterTriggerUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerRequestV1(
    const SblrDdlAlterTriggerRequestV1&);
bool DecodeSblrDdlAlterTriggerRequestV1(
    const std::uint8_t*, std::size_t, SblrDdlAlterTriggerRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerDescriptorV1(
    const SblrDdlAlterTriggerDescriptorV1&, bool operand_magic);
bool DecodeSblrDdlAlterTriggerDescriptorV1(
    const std::uint8_t*, std::size_t, SblrDdlAlterTriggerDescriptorV1*,
    std::string*, bool operand_magic);
std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerResultV1(
    const SblrDdlAlterTriggerResultV1&);
bool DecodeSblrDdlAlterTriggerResultV1(
    const std::uint8_t*, std::size_t, SblrDdlAlterTriggerResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
