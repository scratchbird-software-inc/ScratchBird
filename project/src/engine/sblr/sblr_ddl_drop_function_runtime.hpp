#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropFunctionSha=std::array<std::uint8_t,32>;
struct SblrDdlDropFunctionRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint32_t function_occurrence=0; };
struct SblrDdlDropFunctionDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropFunctionSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropFunctionResultV1 { std::array<std::uint8_t,240> body{}; DdlDropFunctionSha evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropFunctionRequestV1(const SblrDdlDropFunctionRequestV1&);
bool DecodeSblrDdlDropFunctionRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropFunctionRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropFunctionDescriptorV1(const SblrDdlDropFunctionDescriptorV1&,bool);
bool DecodeSblrDdlDropFunctionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropFunctionDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropFunctionResultV1(const SblrDdlDropFunctionResultV1&);
bool DecodeSblrDdlDropFunctionResultV1(const std::uint8_t*,std::size_t,SblrDdlDropFunctionResultV1*,std::string*);
}
