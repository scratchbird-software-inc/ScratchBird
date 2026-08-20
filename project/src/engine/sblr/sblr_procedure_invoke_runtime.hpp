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
struct SblrProcedureInvokeDescriptorV1 {
  std::array<std::uint8_t, 416> body{};
  ProcedureInvokeSha evidence{};
  std::uint64_t availability{0};
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
std::vector<std::uint8_t> EncodeSblrProcedureInvokeDescriptorV1(
    const SblrProcedureInvokeDescriptorV1&, bool operand);
bool DecodeSblrProcedureInvokeDescriptorV1(const std::uint8_t*, std::size_t,
                                           SblrProcedureInvokeDescriptorV1*,
                                           std::string*, bool operand);
std::vector<std::uint8_t> EncodeSblrProcedureInvokeResultV1(
    const SblrProcedureInvokeResultV1&);
bool DecodeSblrProcedureInvokeResultV1(const std::uint8_t*, std::size_t,
                                       SblrProcedureInvokeResultV1*,
                                       std::string*);
}  // namespace scratchbird::engine::sblr
