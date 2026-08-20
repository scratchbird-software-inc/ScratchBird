#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using OperatorCallUuid=std::array<std::uint8_t,16>;using OperatorCallSha=std::array<std::uint8_t,32>;struct SblrOperatorCallRequestV1{OperatorCallUuid receipt{};std::uint64_t occurrence=0;std::uint32_t operator_occurrence=0;};struct SblrOperatorCallDescriptorV1{std::array<std::uint8_t,368>canonical_body{};OperatorCallSha evidence{};std::uint64_t availability_generation=0;};struct SblrOperatorCallResultV1{std::array<std::uint8_t,176>canonical_body{};OperatorCallSha executor_evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrOperatorCallRequestV1(const SblrOperatorCallRequestV1&);bool DecodeSblrOperatorCallRequestV1(const std::uint8_t*,std::size_t,SblrOperatorCallRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrOperatorCallDescriptorV1(const SblrOperatorCallDescriptorV1&,bool);bool DecodeSblrOperatorCallDescriptorV1(const std::uint8_t*,std::size_t,SblrOperatorCallDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrOperatorCallResultV1(const SblrOperatorCallResultV1&);bool DecodeSblrOperatorCallResultV1(const std::uint8_t*,std::size_t,SblrOperatorCallResultV1*,std::string*);}
