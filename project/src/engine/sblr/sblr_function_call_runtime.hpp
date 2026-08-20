#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
using FunctionCallUuid = std::array<std::uint8_t, 16>;
using FunctionCallSha = std::array<std::uint8_t, 32>;

struct SblrFunctionCallRequestV1 {
    FunctionCallUuid receipt{};
    std::uint64_t occurrence = 0;
    std::uint32_t function_occurrence = 0;
};

struct SblrFunctionCallDescriptorV1 {
    std::array<std::uint8_t, 368> canonical_body{};
    FunctionCallSha evidence{};
    std::uint64_t availability_generation = 0;
};

struct SblrFunctionCallResultV1 {
    std::array<std::uint8_t, 176> canonical_body{};
    FunctionCallSha executor_evidence{};
    std::uint64_t availability_generation = 0;
};

std::vector<std::uint8_t> EncodeSblrFunctionCallRequestV1(const SblrFunctionCallRequestV1&);
bool DecodeSblrFunctionCallRequestV1(const std::uint8_t*, std::size_t, SblrFunctionCallRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrFunctionCallDescriptorV1(const SblrFunctionCallDescriptorV1&, bool operand);
bool DecodeSblrFunctionCallDescriptorV1(const std::uint8_t*, std::size_t, SblrFunctionCallDescriptorV1*, std::string*, bool operand);
std::vector<std::uint8_t> EncodeSblrFunctionCallResultV1(const SblrFunctionCallResultV1&);
bool DecodeSblrFunctionCallResultV1(const std::uint8_t*, std::size_t, SblrFunctionCallResultV1*, std::string*);
}
