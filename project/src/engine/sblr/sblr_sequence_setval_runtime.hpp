#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using SequenceSetvalUuid=std::array<std::uint8_t,16>;using SequenceSetvalSha=std::array<std::uint8_t,32>;struct SblrSequenceSetvalRequestV1{SequenceSetvalUuid receipt{};std::uint64_t occurrence=0;std::uint32_t setval_occurrence=0;};struct SblrSequenceSetvalDescriptorV1{std::array<std::uint8_t,352>body{};SequenceSetvalSha evidence{};std::uint64_t availability=0;};struct SblrSequenceSetvalResultV1{std::array<std::uint8_t,176>body{};SequenceSetvalSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrSequenceSetvalRequestV1(const SblrSequenceSetvalRequestV1&);bool DecodeSblrSequenceSetvalRequestV1(const std::uint8_t*,std::size_t,SblrSequenceSetvalRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrSequenceSetvalDescriptorV1(const SblrSequenceSetvalDescriptorV1&,bool);bool DecodeSblrSequenceSetvalDescriptorV1(const std::uint8_t*,std::size_t,SblrSequenceSetvalDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrSequenceSetvalResultV1(const SblrSequenceSetvalResultV1&);bool DecodeSblrSequenceSetvalResultV1(const std::uint8_t*,std::size_t,SblrSequenceSetvalResultV1*,std::string*);}
