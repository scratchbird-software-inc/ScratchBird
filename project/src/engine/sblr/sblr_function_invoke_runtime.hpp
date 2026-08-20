#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using FunctionInvokeUuid=std::array<std::uint8_t,16>;using FunctionInvokeSha=std::array<std::uint8_t,32>;struct SblrFunctionInvokeRequestV1{FunctionInvokeUuid receipt{};std::uint64_t occurrence=0;std::uint32_t invocation_occurrence=0;};struct SblrFunctionInvokeDescriptorV1{std::array<std::uint8_t,368>body{};FunctionInvokeSha evidence{};std::uint64_t availability=0;};struct SblrFunctionInvokeResultV1{std::array<std::uint8_t,176>body{};FunctionInvokeSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrFunctionInvokeRequestV1(const SblrFunctionInvokeRequestV1&);bool DecodeSblrFunctionInvokeRequestV1(const std::uint8_t*,std::size_t,SblrFunctionInvokeRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrFunctionInvokeDescriptorV1(const SblrFunctionInvokeDescriptorV1&,bool);bool DecodeSblrFunctionInvokeDescriptorV1(const std::uint8_t*,std::size_t,SblrFunctionInvokeDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrFunctionInvokeResultV1(const SblrFunctionInvokeResultV1&);bool DecodeSblrFunctionInvokeResultV1(const std::uint8_t*,std::size_t,SblrFunctionInvokeResultV1*,std::string*);}
