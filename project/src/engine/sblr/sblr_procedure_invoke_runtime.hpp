#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
using ProcedureInvokeUuid = std::array<std::uint8_t, 16>;
using ProcedureInvokeSha = std::array<std::uint8_t, 32>;

struct SblrProcedureInvokeRequestV1 {
  ProcedureInvokeUuid receipt{};
  std::uint64_t occurrence{0};
  std::uint32_t invocation_occurrence{0};
};
struct SblrProcedureInvokeNameAtomV2 {
  std::string raw_utf8;
  bool quoted{false};
};
struct SblrProcedureInvokeBindRequestV2 {
  ProcedureInvokeUuid receipt{};
  std::uint64_t occurrence{0};
  std::uint32_t invocation_occurrence{0};
  std::uint16_t command_identity{1};
  std::vector<SblrProcedureInvokeNameAtomV2> name_atoms;
  ProcedureInvokeSha evidence{};
};
struct SblrProcedureInvokeDescriptorV1 {
  std::array<std::uint8_t, 416> body{};
  ProcedureInvokeSha evidence{};
  std::uint64_t availability{0};
};
struct SblrProcedureInvokeAuthorityV1 {
  ProcedureInvokeUuid invocation_uuid{};
  std::uint64_t invocation_generation = 0;
  ProcedureInvokeUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  ProcedureInvokeUuid statement_snapshot_uuid{};
  ProcedureInvokeUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  ProcedureInvokeUuid security_context_uuid{};
  ProcedureInvokeUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  ProcedureInvokeUuid procedure_uuid{};
  std::uint64_t procedure_generation = 0;
  ProcedureInvokeUuid procedure_body_uuid{};
  std::uint64_t procedure_body_generation = 0;
  ProcedureInvokeSha procedure_body_sha256{};
  ProcedureInvokeUuid procedure_abi_uuid{};
  std::uint64_t procedure_abi_generation = 0;
  ProcedureInvokeUuid argument_vector_uuid{};
  std::uint32_t argument_count = 0;
  std::uint32_t output_parameter_count = 0;
  std::uint32_t invocation_flags = 0;
  ProcedureInvokeSha argument_vector_sha256{};
  ProcedureInvokeUuid output_descriptor_vector_uuid{};
  std::uint64_t output_descriptor_vector_generation = 0;
  ProcedureInvokeUuid result_set_shape_uuid{};
  std::uint64_t result_set_shape_generation = 0;
  ProcedureInvokeSha effect_set_sha256{};
  ProcedureInvokeUuid recovery_uuid{};
  std::uint64_t recovery_generation = 0;
  ProcedureInvokeUuid engine_snapshot_uuid{};
  std::uint64_t executor_availability_generation = 0;
};
struct SblrProcedureInvokeResultV1 {
  std::array<std::uint8_t, 240> body{};
  ProcedureInvokeSha evidence{};
  std::uint64_t availability{0};
  ProcedureInvokeUuid barrier{};
};

std::vector<std::uint8_t> EncodeSblrProcedureInvokeRequestV1(
    const SblrProcedureInvokeRequestV1&);
bool DecodeSblrProcedureInvokeRequestV1(const std::uint8_t*, std::size_t,
                                        SblrProcedureInvokeRequestV1*,
                                        std::string*);
std::vector<std::uint8_t> EncodeSblrProcedureInvokeBindRequestV2(
    const SblrProcedureInvokeBindRequestV2&);
bool DecodeSblrProcedureInvokeBindRequestV2(
    const std::uint8_t*, std::size_t, SblrProcedureInvokeBindRequestV2*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrProcedureInvokeDescriptorV1(
    const SblrProcedureInvokeDescriptorV1&, bool operand);
bool DecodeSblrProcedureInvokeDescriptorV1(const std::uint8_t*, std::size_t,
                                           SblrProcedureInvokeDescriptorV1*,
                                           std::string*, bool operand);
bool ValidateSblrProcedureInvokeAuthorityV1(
    const SblrProcedureInvokeAuthorityV1&, std::string* = nullptr);
bool DecodeSblrProcedureInvokeAuthorityV1(
    const SblrProcedureInvokeDescriptorV1&, SblrProcedureInvokeAuthorityV1*,
    std::string* = nullptr);
std::vector<std::uint8_t> EncodeSblrProcedureInvokeResultV1(
    const SblrProcedureInvokeResultV1&);
bool DecodeSblrProcedureInvokeResultV1(const std::uint8_t*, std::size_t,
                                       SblrProcedureInvokeResultV1*,
                                       std::string*);
}  // namespace scratchbird::engine::sblr
