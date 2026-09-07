#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateProcedureUuid=std::array<std::uint8_t,16>; using DdlCreateProcedureSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateProcedureRequestV1 { DdlCreateProcedureUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t procedure_occurrence=0; };
struct SblrDdlCreateProcedureNameAtomV2 {
  std::string raw_utf8;
  bool quoted = false;
};
struct SblrDdlCreateProcedureBindRequestV2 {
  DdlCreateProcedureUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t procedure_occurrence = 0;
  std::uint16_t command_identity = 1;
  std::uint16_t body_profile = 1;
  std::vector<SblrDdlCreateProcedureNameAtomV2> name_atoms;
  DdlCreateProcedureSha evidence{};
};
struct SblrDdlCreateProcedureDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateProcedureSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateProcedureAuthorityV1 {
  DdlCreateProcedureUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t procedure_occurrence = 0;
  std::uint16_t command_identity = 1;
  std::uint16_t body_profile = 1;
  DdlCreateProcedureUuid procedure_uuid{};
  std::uint64_t procedure_generation = 0;
  DdlCreateProcedureUuid schema_uuid{};
  std::uint64_t schema_generation = 0;
  DdlCreateProcedureUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  DdlCreateProcedureUuid statement_snapshot_uuid{};
  DdlCreateProcedureUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  DdlCreateProcedureUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  DdlCreateProcedureUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DdlCreateProcedureUuid resource_grant_uuid{};
  std::uint64_t resource_generation = 0;
  DdlCreateProcedureUuid owner_principal_uuid{};
  DdlCreateProcedureUuid body_sblr_uuid{};
  std::uint64_t body_sblr_generation = 0;
  DdlCreateProcedureSha body_sblr_sha256{};
  DdlCreateProcedureUuid procedure_abi_uuid{};
  std::uint64_t procedure_abi_generation = 0;
  DdlCreateProcedureSha effect_set_sha256{};
  DdlCreateProcedureUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  DdlCreateProcedureSha request_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
};
struct SblrDdlCreateProcedureResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateProcedureSha evidence{}; std::uint64_t availability=0; DdlCreateProcedureUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureRequestV1(const SblrDdlCreateProcedureRequestV1&);
bool DecodeSblrDdlCreateProcedureRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureBindRequestV2(
    const SblrDdlCreateProcedureBindRequestV2&);
bool DecodeSblrDdlCreateProcedureBindRequestV2(
    const std::uint8_t*, std::size_t,
    SblrDdlCreateProcedureBindRequestV2*, std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureDescriptorV1(const SblrDdlCreateProcedureDescriptorV1&,bool);
bool DecodeSblrDdlCreateProcedureDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureDescriptorV1*,std::string*,bool);
bool ValidateSblrDdlCreateProcedureAuthorityV1(
    const SblrDdlCreateProcedureAuthorityV1&, std::string* = nullptr);
bool DecodeSblrDdlCreateProcedureAuthorityV1(
    const SblrDdlCreateProcedureDescriptorV1&,
    SblrDdlCreateProcedureAuthorityV1*, std::string* = nullptr);
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureResultV1(const SblrDdlCreateProcedureResultV1&);
bool DecodeSblrDdlCreateProcedureResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureResultV1*,std::string*);
}
