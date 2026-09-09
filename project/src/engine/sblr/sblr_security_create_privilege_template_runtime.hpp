#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using PrivilegeTemplateUuid = std::array<std::uint8_t, 16>;
using PrivilegeTemplateSha = std::array<std::uint8_t, 32>;

inline constexpr std::size_t
    kSblrSecurityCreatePrivilegeTemplateRequestV1Bytes = 752;
inline constexpr std::size_t
    kSblrSecurityCreatePrivilegeTemplateDescriptorV1Bytes = 488;
inline constexpr std::size_t
    kSblrSecurityCreatePrivilegeTemplateResultV1Bytes = 320;

// The first executable profile deliberately carries one future-object kind,
// one grantee, and one privilege. Later versions may widen the vectors without
// changing the engine-owned descriptor authority represented below.
enum class PrivilegeTemplateObjectKindV1 : std::uint8_t {
  kTables = 1,
  kSequences = 2,
  kRoutines = 3,
  kTypes = 4,
  kSchemas = 5,
};

// PTQX is syntax demand only. The parser may preserve spelling and quoting,
// but it cannot supply any catalog UUID, generation, policy, authorization,
// recovery, or idempotency authority.
struct SblrSecurityCreatePrivilegeTemplateRequestV1 {
  PrivilegeTemplateUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t template_occurrence = 0;
  std::uint16_t command_identity = 1;
  PrivilegeTemplateObjectKindV1 object_kind =
      PrivilegeTemplateObjectKindV1::kTables;
  bool with_grant_option = false;
  bool enabled = true;
  std::string template_name_utf8;
  bool template_name_quoted = false;
  std::string grantee_name_utf8;
  bool grantee_name_quoted = false;
  std::string privilege_utf8;
  PrivilegeTemplateSha evidence{};
};

// PTDD and PTDO share an exact header-neutral 400-byte body. PTDO is a
// literal magic-only projection of PTDD.
struct SblrSecurityCreatePrivilegeTemplateDescriptorV1 {
  PrivilegeTemplateUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t template_occurrence = 0;
  PrivilegeTemplateObjectKindV1 object_kind =
      PrivilegeTemplateObjectKindV1::kTables;
  bool with_grant_option = false;
  bool enabled = true;
  PrivilegeTemplateUuid template_uuid{};
  std::uint64_t template_generation = 0;
  PrivilegeTemplateUuid owner_principal_uuid{};
  PrivilegeTemplateUuid schema_uuid{};
  PrivilegeTemplateUuid grantee_uuid{};
  PrivilegeTemplateUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  PrivilegeTemplateUuid statement_snapshot_uuid{};
  PrivilegeTemplateUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  PrivilegeTemplateUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  PrivilegeTemplateUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  PrivilegeTemplateUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  PrivilegeTemplateUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  PrivilegeTemplateSha syntax_demand_sha256{};
  PrivilegeTemplateSha definition_sha256{};
  PrivilegeTemplateSha idempotency_sha256{};
  PrivilegeTemplateSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrSecurityCreatePrivilegeTemplateResultV1 {
  PrivilegeTemplateUuid receipt{};
  PrivilegeTemplateUuid template_uuid{};
  std::uint64_t template_generation = 0;
  PrivilegeTemplateUuid owner_principal_uuid{};
  PrivilegeTemplateUuid grantee_uuid{};
  PrivilegeTemplateUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  PrivilegeTemplateUuid statement_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t resource_generation = 0;
  PrivilegeTemplateSha definition_sha256{};
  PrivilegeTemplateSha descriptor_evidence_sha256{};
  PrivilegeTemplateUuid mutation_uuid{};
  std::uint64_t cache_invalidation_epoch = 0;
  bool template_created = false;
  bool exact_idempotent_replay = false;
  PrivilegeTemplateSha evidence{};
  std::uint64_t availability = 0;
  PrivilegeTemplateUuid publication_barrier{};
};

std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateRequestV1(
    const SblrSecurityCreatePrivilegeTemplateRequestV1&);
bool DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(
    const std::uint8_t*, std::size_t,
    SblrSecurityCreatePrivilegeTemplateRequestV1*, std::string*);
std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
    const SblrSecurityCreatePrivilegeTemplateDescriptorV1&,
    bool operand_magic);
bool DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
    const std::uint8_t*, std::size_t,
    SblrSecurityCreatePrivilegeTemplateDescriptorV1*, std::string*,
    bool operand_magic);
std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateResultV1(
    const SblrSecurityCreatePrivilegeTemplateResultV1&);
bool DecodeSblrSecurityCreatePrivilegeTemplateResultV1(
    const std::uint8_t*, std::size_t,
    SblrSecurityCreatePrivilegeTemplateResultV1*, std::string*);

}  // namespace scratchbird::engine::sblr
