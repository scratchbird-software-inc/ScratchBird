#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using DdlCreateSchemaUuid = std::array<std::uint8_t, 16>;
using DdlCreateSchemaSha = std::array<std::uint8_t, 32>;

struct SblrDdlCreateSchemaNameAtomV1 {
  std::string raw_utf8;
  bool quoted = false;
};

// CSQX is syntax demand only.  The parser may present one to three raw name
// atoms and quote bits; every UUID, generation, policy, authorization, and
// recovery identity is supplied by the engine under the authenticated receipt.
struct SblrDdlCreateSchemaRequestV1 {
  DdlCreateSchemaUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t schema_occurrence = 0;
  std::uint16_t command_identity = 1;
  std::vector<SblrDdlCreateSchemaNameAtomV1> name_atoms;
  DdlCreateSchemaSha evidence{};
};

// CSDX and CSDO share an exact header-neutral 400-byte body.  CSDO is a
// literal magic-only projection of CSDX.
struct SblrDdlCreateSchemaDescriptorV1 {
  DdlCreateSchemaUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t schema_occurrence = 0;
  std::uint32_t flags = 0;
  DdlCreateSchemaUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlCreateSchemaUuid parent_schema_uuid{};
  std::uint64_t parent_namespace_generation = 0;
  DdlCreateSchemaUuid database_uuid{};
  DdlCreateSchemaUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlCreateSchemaUuid statement_snapshot_uuid{};
  DdlCreateSchemaUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  DdlCreateSchemaUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  DdlCreateSchemaUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DdlCreateSchemaUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  DdlCreateSchemaUuid owner_principal_uuid{};
  DdlCreateSchemaUuid binding_uuid{};
  DdlCreateSchemaUuid recovery_uuid{};
  std::uint64_t binding_generation = 0;
  std::uint64_t recovery_generation = 0;
  DdlCreateSchemaSha normalized_path_sha256{};
  DdlCreateSchemaSha syntax_demand_sha256{};
  DdlCreateSchemaSha authorization_evidence_sha256{};
  DdlCreateSchemaSha evidence{};
  std::uint64_t availability = 0;
};

struct SblrDdlCreateSchemaResultV1 {
  DdlCreateSchemaUuid receipt{};
  DdlCreateSchemaUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlCreateSchemaUuid parent_schema_uuid{};
  std::uint64_t parent_namespace_generation = 0;
  DdlCreateSchemaUuid database_uuid{};
  DdlCreateSchemaUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlCreateSchemaUuid statement_snapshot_uuid{};
  DdlCreateSchemaUuid catalog_row_uuid{};
  DdlCreateSchemaUuid mutation_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_generation = 0;
  DdlCreateSchemaSha normalized_path_sha256{};
  DdlCreateSchemaSha descriptor_evidence_sha256{};
  DdlCreateSchemaSha evidence{};
  std::uint64_t availability = 0;
  DdlCreateSchemaUuid publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaRequestV1(
    const SblrDdlCreateSchemaRequestV1&);
bool DecodeSblrDdlCreateSchemaRequestV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateSchemaRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaDescriptorV1(
    const SblrDdlCreateSchemaDescriptorV1&, bool operand_magic);
bool DecodeSblrDdlCreateSchemaDescriptorV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateSchemaDescriptorV1*,
    std::string*, bool operand_magic);
std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaResultV1(
    const SblrDdlCreateSchemaResultV1&);
bool DecodeSblrDdlCreateSchemaResultV1(
    const std::uint8_t*, std::size_t, SblrDdlCreateSchemaResultV1*,
    std::string*);

}  // namespace scratchbird::engine::sblr
