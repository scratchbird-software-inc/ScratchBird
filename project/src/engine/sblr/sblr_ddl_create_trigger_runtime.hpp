#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using DdlCreateTriggerUuid = std::array<std::uint8_t, 16>;
using DdlCreateTriggerSha = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kSblrDdlCreateTriggerRequestV1Bytes = 1664;
inline constexpr std::size_t kSblrDdlCreateTriggerDescriptorV1Bytes = 488;
inline constexpr std::size_t kSblrDdlCreateTriggerResultV1Bytes = 320;

enum class DdlCreateTriggerTimingV1 : std::uint8_t {
  kBefore = 1,
  kAfter = 2,
  kInsteadOf = 3,
};

enum class DdlCreateTriggerEventV1 : std::uint8_t {
  kInsert = 1,
  kUpdate = 2,
  kDelete = 3,
};

enum class DdlCreateTriggerScopeV1 : std::uint8_t {
  kRow = 1,
  kStatement = 2,
};

enum class DdlCreateTriggerTargetKindV1 : std::uint8_t {
  kTable = 1,
};

enum class DdlCreateTriggerBodyProfileV1 : std::uint16_t {
  kAuditAfterInsertRow = 1,
  kAuditAfterUpdateRow = 2,
  kAuditAfterDeleteRow = 3,
  kAuditAfterUpdateStatement = 4,
};

enum class DdlCreateTriggerSecurityModeV1 : std::uint8_t {
  kInvoker = 1,
  kDefiner = 2,
};

enum class DdlCreateTriggerImageModeV1 : std::uint8_t {
  kPolicyRedacted = 1,
  kRawAuthorized = 2,
};

enum class DdlCreateTriggerExecutionModeV1 : std::uint8_t {
  kSameTransaction = 1,
  kAutonomous = 2,
};

enum class DdlCreateTriggerEnabledStateV1 : std::uint8_t {
  kEnabled = 1,
  kDisabled = 2,
};

enum class DdlCreateTriggerRecursionModeV1 : std::uint8_t {
  kForbidSelf = 1,
  kAllowBounded = 2,
  kSuppressNested = 3,
};

struct SblrDdlCreateTriggerNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// TVQX contains syntax demand only. UUIDs, generations, authorization,
// compiled-body, recovery, and publication authority are engine-owned.
struct SblrDdlCreateTriggerRequestV1 {
  DdlCreateTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  std::uint16_t command_identity = 1;
  DdlCreateTriggerTimingV1 timing = DdlCreateTriggerTimingV1::kAfter;
  DdlCreateTriggerEventV1 event = DdlCreateTriggerEventV1::kInsert;
  DdlCreateTriggerScopeV1 scope = DdlCreateTriggerScopeV1::kRow;
  DdlCreateTriggerTargetKindV1 target_kind =
      DdlCreateTriggerTargetKindV1::kTable;
  DdlCreateTriggerBodyProfileV1 body_profile =
      DdlCreateTriggerBodyProfileV1::kAuditAfterInsertRow;
  std::vector<SblrDdlCreateTriggerNameAtomV1> trigger_name_atoms;
  std::vector<SblrDdlCreateTriggerNameAtomV1> target_name_atoms;
  std::uint16_t position = 0;
  DdlCreateTriggerSecurityModeV1 security_mode =
      DdlCreateTriggerSecurityModeV1::kInvoker;
  DdlCreateTriggerImageModeV1 image_mode =
      DdlCreateTriggerImageModeV1::kPolicyRedacted;
  DdlCreateTriggerExecutionModeV1 execution_mode =
      DdlCreateTriggerExecutionModeV1::kSameTransaction;
  DdlCreateTriggerEnabledStateV1 enabled_state =
      DdlCreateTriggerEnabledStateV1::kEnabled;
  DdlCreateTriggerRecursionModeV1 recursion_mode =
      DdlCreateTriggerRecursionModeV1::kForbidSelf;
  DdlCreateTriggerSha evidence{};
};

// TVDX and TVDO share an exact header-neutral 400-byte body. TVDO is a
// literal magic-only projection of TVDX.
struct SblrDdlCreateTriggerDescriptorV1 {
  DdlCreateTriggerUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t trigger_occurrence = 0;
  DdlCreateTriggerTimingV1 timing = DdlCreateTriggerTimingV1::kAfter;
  DdlCreateTriggerEventV1 event = DdlCreateTriggerEventV1::kInsert;
  DdlCreateTriggerScopeV1 scope = DdlCreateTriggerScopeV1::kRow;
  DdlCreateTriggerTargetKindV1 target_kind =
      DdlCreateTriggerTargetKindV1::kTable;
  DdlCreateTriggerBodyProfileV1 body_profile =
      DdlCreateTriggerBodyProfileV1::kAuditAfterInsertRow;
  std::uint16_t position = 0;
  DdlCreateTriggerSecurityModeV1 security_mode =
      DdlCreateTriggerSecurityModeV1::kInvoker;
  DdlCreateTriggerImageModeV1 image_mode =
      DdlCreateTriggerImageModeV1::kPolicyRedacted;
  DdlCreateTriggerExecutionModeV1 execution_mode =
      DdlCreateTriggerExecutionModeV1::kSameTransaction;
  DdlCreateTriggerEnabledStateV1 enabled_state =
      DdlCreateTriggerEnabledStateV1::kEnabled;
  DdlCreateTriggerRecursionModeV1 recursion_mode =
      DdlCreateTriggerRecursionModeV1::kForbidSelf;
  DdlCreateTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlCreateTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlCreateTriggerUuid target_relation_descriptor_uuid{};
  std::uint64_t target_relation_descriptor_generation = 0;
  DdlCreateTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlCreateTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlCreateTriggerUuid statement_snapshot_uuid{};
  DdlCreateTriggerUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  DdlCreateTriggerUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  DdlCreateTriggerUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DdlCreateTriggerUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  DdlCreateTriggerUuid owner_principal_uuid{};
  DdlCreateTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  DdlCreateTriggerUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  DdlCreateTriggerSha syntax_demand_sha256{};
  DdlCreateTriggerSha authority_bundle_sha256{};
  DdlCreateTriggerSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrDdlCreateTriggerResultV1 {
  DdlCreateTriggerUuid receipt{};
  DdlCreateTriggerUuid trigger_uuid{};
  std::uint64_t trigger_generation = 0;
  DdlCreateTriggerUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  DdlCreateTriggerUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlCreateTriggerUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlCreateTriggerUuid statement_snapshot_uuid{};
  DdlCreateTriggerUuid catalog_row_uuid{};
  DdlCreateTriggerUuid mutation_uuid{};
  DdlCreateTriggerUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_generation = 0;
  DdlCreateTriggerSha descriptor_evidence_sha256{};
  DdlCreateTriggerSha evidence{};
  std::uint64_t availability = 0;
  DdlCreateTriggerUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerRequestV1(
    const SblrDdlCreateTriggerRequestV1&);
bool DecodeSblrDdlCreateTriggerRequestV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateTriggerRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerDescriptorV1(
    const SblrDdlCreateTriggerDescriptorV1&, bool operand_magic);
bool DecodeSblrDdlCreateTriggerDescriptorV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateTriggerDescriptorV1*,
    std::string*, bool operand_magic);
std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerResultV1(
    const SblrDdlCreateTriggerResultV1&);
bool DecodeSblrDdlCreateTriggerResultV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateTriggerResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
