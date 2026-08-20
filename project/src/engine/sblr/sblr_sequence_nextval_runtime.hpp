#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using SequenceNextvalUuid=std::array<std::uint8_t,16>;using SequenceNextvalSha=std::array<std::uint8_t,32>;struct SblrSequenceNextvalRequestV1{SequenceNextvalUuid receipt{};std::uint64_t occurrence=0;std::uint32_t nextval_occurrence=0;};struct SblrSequenceNextvalDescriptorV1{std::array<std::uint8_t,352>body{};SequenceNextvalSha evidence{};std::uint64_t availability=0;};struct SblrSequenceNextvalResultV1{std::array<std::uint8_t,176>body{};SequenceNextvalSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrSequenceNextvalRequestV1(const SblrSequenceNextvalRequestV1&);bool DecodeSblrSequenceNextvalRequestV1(const std::uint8_t*,std::size_t,SblrSequenceNextvalRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrSequenceNextvalDescriptorV1(const SblrSequenceNextvalDescriptorV1&,bool);bool DecodeSblrSequenceNextvalDescriptorV1(const std::uint8_t*,std::size_t,SblrSequenceNextvalDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrSequenceNextvalResultV1(const SblrSequenceNextvalResultV1&);bool DecodeSblrSequenceNextvalResultV1(const std::uint8_t*,std::size_t,SblrSequenceNextvalResultV1*,std::string*);}
