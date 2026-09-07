#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"
#include "engine/sblr/sblr_procedure_abi_runtime.hpp"
#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#ifdef NDEBUG
#undef assert
#define assert(condition)             \
  do {                                \
    if (!(condition)) std::abort();   \
  } while (false)
#endif

namespace {

template <class T>
T Uuid(std::uint8_t tail) {
  T value{};
  value[0] = 0x01;
  value[1] = 0x9d;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = tail;
  return value;
}

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;

  sblr::SblrDdlCreateProcedureBindRequestV3 create;
  create.receipt = Uuid<sblr::DdlCreateProcedureUuid>(1);
  create.occurrence = 1;
  create.procedure_occurrence = 1;
  create.name_atoms = {{"app", false}, {"process_order", false}};
  create.parameters = {{1, 1, {"customer_id", false}, {"BIGINT", false}}};
  const auto pcqx = sblr::EncodeSblrDdlCreateProcedureBindRequestV3(create);
  assert(!pcqx.empty());
  assert(pcqx[4] == 3);
  sblr::SblrDdlCreateProcedureBindRequestV3 decoded_create;
  std::string detail;
  assert(sblr::DecodeSblrDdlCreateProcedureBindRequestV3(
      pcqx.data(), pcqx.size(), &decoded_create, &detail));
  assert(decoded_create.parameters.size() == 1);
  assert(decoded_create.parameters.front().name.raw_utf8 == "customer_id");
  assert(decoded_create.parameters.front().type_name.raw_utf8 == "BIGINT");
  auto damaged_pcqx = pcqx;
  damaged_pcqx[damaged_pcqx.size() - 1] ^= 0x01;
  assert(!sblr::DecodeSblrDdlCreateProcedureBindRequestV3(
      damaged_pcqx.data(), damaged_pcqx.size(), &decoded_create, &detail));
  sblr::SblrDdlCreateProcedureBindRequestV2 create_v2;
  create_v2.receipt = create.receipt;
  create_v2.occurrence = 1;
  create_v2.procedure_occurrence = 1;
  create_v2.name_atoms = create.name_atoms;
  const auto pcqx_v2 = sblr::EncodeSblrDdlCreateProcedureBindRequestV2(create_v2);
  assert(!pcqx_v2.empty() && pcqx_v2[4] == 2);

  sblr::SblrProcedureAbiV1 abi;
  abi.abi_uuid = Uuid<sblr::ProcedureAbiUuid>(2);
  abi.abi_generation = 1;
  abi.procedure_uuid = Uuid<sblr::ProcedureAbiUuid>(3);
  abi.procedure_generation = 1;
  sblr::SblrProcedureAbiParameterV1 parameter;
  parameter.ordinal = 1;
  parameter.name_utf8 = "customer_id";
  parameter.canonical_type_name = "int64";
  parameter.datatype_descriptor_uuid = Uuid<sblr::ProcedureAbiUuid>(4);
  parameter.datatype_descriptor_generation = 1;
  parameter.type_uuid = Uuid<sblr::ProcedureAbiUuid>(5);
  abi.parameters.push_back(parameter);
  const auto pabi = sblr::EncodeSblrProcedureAbiV1(abi);
  assert(pabi.size() == sblr::kSblrProcedureAbiV1Bytes);
  sblr::SblrProcedureAbiV1 decoded_abi;
  assert(sblr::DecodeSblrProcedureAbiV1(pabi.data(), pabi.size(),
                                       &decoded_abi, &detail));
  assert(decoded_abi.parameters.size() == 1);
  assert(decoded_abi.parameters.front().datatype_descriptor_uuid ==
         parameter.datatype_descriptor_uuid);
  auto damaged_pabi = pabi;
  damaged_pabi[124] ^= 0x01;
  assert(!sblr::DecodeSblrProcedureAbiV1(damaged_pabi.data(),
                                        damaged_pabi.size(), &decoded_abi,
                                        &detail));
  auto nonnullable_pabi = pabi;
  nonnullable_pabi[75] = 0;
  assert(!sblr::DecodeSblrProcedureAbiV1(
      nonnullable_pabi.data(), nonnullable_pabi.size(), &decoded_abi,
      &detail));

  sblr::SblrProcedureInvokeBindRequestV3 invoke;
  invoke.receipt = Uuid<sblr::ProcedureInvokeUuid>(6);
  invoke.occurrence = 1;
  invoke.invocation_occurrence = 1;
  invoke.name_atoms = {{"app", false}, {"process_order", false}};
  invoke.arguments = {{1, 1, "-9223372036854775808"}};
  const auto pirq = sblr::EncodeSblrProcedureInvokeBindRequestV3(invoke);
  assert(!pirq.empty() && pirq[4] == 3);
  sblr::SblrProcedureInvokeBindRequestV3 decoded_invoke;
  assert(sblr::DecodeSblrProcedureInvokeBindRequestV3(
      pirq.data(), pirq.size(), &decoded_invoke, &detail));
  assert(decoded_invoke.arguments.front().literal_utf8 ==
         "-9223372036854775808");
  auto damaged_pirq = pirq;
  damaged_pirq[damaged_pirq.size() - 3] ^= 0x40;
  assert(!sblr::DecodeSblrProcedureInvokeBindRequestV3(
      damaged_pirq.data(), damaged_pirq.size(), &decoded_invoke, &detail));

  sblr::SblrProcedureArgumentVectorV1 arguments;
  arguments.argument_vector_uuid = Uuid<sblr::ProcedureAbiUuid>(7);
  arguments.argument_vector_generation = 1;
  arguments.procedure_abi_uuid = abi.abi_uuid;
  arguments.procedure_abi_generation = abi.abi_generation;
  arguments.invocation_uuid = Uuid<sblr::ProcedureAbiUuid>(8);
  arguments.invocation_generation = 1;
  sblr::SblrProcedureArgumentV1 argument;
  argument.ordinal = 1;
  argument.datatype_descriptor_uuid = parameter.datatype_descriptor_uuid;
  argument.datatype_descriptor_generation =
      parameter.datatype_descriptor_generation;
  argument.type_uuid = parameter.type_uuid;
  const auto value = sblr::EncodeSblrProcedureInt64Value(-7);
  argument.canonical_value_bytes.assign(value.begin(), value.end());
  arguments.arguments.push_back(argument);
  const auto parg = sblr::EncodeSblrProcedureArgumentVectorV1(arguments);
  assert(parg.size() == sblr::kSblrProcedureArgumentVectorV1Bytes);
  sblr::SblrProcedureArgumentVectorV1 decoded_arguments;
  assert(sblr::DecodeSblrProcedureArgumentVectorV1(
      parg.data(), parg.size(), &decoded_arguments, &detail));
  std::int64_t decoded_value = 0;
  assert(sblr::DecodeSblrProcedureInt64Value(
      decoded_arguments.arguments.front().canonical_value_bytes,
      &decoded_value));
  assert(decoded_value == -7);
  auto damaged_parg = parg;
  damaged_parg[140] ^= 0x01;
  assert(!sblr::DecodeSblrProcedureArgumentVectorV1(
      damaged_parg.data(), damaged_parg.size(), &decoded_arguments, &detail));

  sblr::SblrProcedureAbiV1 empty_abi = abi;
  empty_abi.parameters.clear();
  empty_abi.evidence = {};
  assert(sblr::EncodeSblrProcedureAbiV1(empty_abi).size() ==
         sblr::kSblrProcedureAbiV1Bytes);
  sblr::SblrProcedureArgumentVectorV1 empty_arguments = arguments;
  empty_arguments.arguments.clear();
  empty_arguments.evidence = {};
  assert(sblr::EncodeSblrProcedureArgumentVectorV1(empty_arguments).size() ==
         sblr::kSblrProcedureArgumentVectorV1Bytes);
  return 0;
}
