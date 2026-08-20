#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using SequenceCurrvalUuid=std::array<std::uint8_t,16>;using SequenceCurrvalSha=std::array<std::uint8_t,32>;struct SblrSequenceCurrvalRequestV1{SequenceCurrvalUuid receipt{};std::uint64_t occurrence=0;std::uint32_t currval_occurrence=0;};struct SblrSequenceCurrvalDescriptorV1{std::array<std::uint8_t,328>body{};SequenceCurrvalSha evidence{};std::uint64_t availability=0;};struct SblrSequenceCurrvalResultV1{std::array<std::uint8_t,176>body{};SequenceCurrvalSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrSequenceCurrvalRequestV1(const SblrSequenceCurrvalRequestV1&);bool DecodeSblrSequenceCurrvalRequestV1(const std::uint8_t*,std::size_t,SblrSequenceCurrvalRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrSequenceCurrvalDescriptorV1(const SblrSequenceCurrvalDescriptorV1&,bool);bool DecodeSblrSequenceCurrvalDescriptorV1(const std::uint8_t*,std::size_t,SblrSequenceCurrvalDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrSequenceCurrvalResultV1(const SblrSequenceCurrvalResultV1&);bool DecodeSblrSequenceCurrvalResultV1(const std::uint8_t*,std::size_t,SblrSequenceCurrvalResultV1*,std::string*);}
