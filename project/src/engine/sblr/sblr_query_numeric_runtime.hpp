#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using QueryNumericUuid=std::array<std::uint8_t,16>;using QueryNumericSha=std::array<std::uint8_t,32>;struct SblrQueryNumericRequestV1{QueryNumericUuid receipt{};std::uint64_t occurrence=0;std::uint32_t numeric_occurrence=0;};struct SblrQueryNumericDescriptorV1{std::array<std::uint8_t,376>body{};QueryNumericSha evidence{};std::uint64_t availability=0;};struct SblrQueryNumericResultV1{std::array<std::uint8_t,176>body{};QueryNumericSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrQueryNumericRequestV1(const SblrQueryNumericRequestV1&);bool DecodeSblrQueryNumericRequestV1(const std::uint8_t*,std::size_t,SblrQueryNumericRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrQueryNumericDescriptorV1(const SblrQueryNumericDescriptorV1&,bool);bool DecodeSblrQueryNumericDescriptorV1(const std::uint8_t*,std::size_t,SblrQueryNumericDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrQueryNumericResultV1(const SblrQueryNumericResultV1&);bool DecodeSblrQueryNumericResultV1(const std::uint8_t*,std::size_t,SblrQueryNumericResultV1*,std::string*);}
