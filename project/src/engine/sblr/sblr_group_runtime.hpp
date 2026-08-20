#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using GroupUuid=std::array<std::uint8_t,16>;using GroupSha=std::array<std::uint8_t,32>;struct SblrGroupRequestV1{GroupUuid receipt{};std::uint64_t occurrence=0;std::uint32_t group_occurrence=0;};struct SblrGroupDescriptorV1{std::array<std::uint8_t,392>body{};GroupSha evidence{};std::uint64_t availability=0;};struct SblrGroupResultV1{std::array<std::uint8_t,240>body{};GroupSha evidence{};std::uint64_t availability=0;GroupUuid publication_barrier{};};std::vector<std::uint8_t>EncodeSblrGroupRequestV1(const SblrGroupRequestV1&);bool DecodeSblrGroupRequestV1(const std::uint8_t*,std::size_t,SblrGroupRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrGroupDescriptorV1(const SblrGroupDescriptorV1&,bool);bool DecodeSblrGroupDescriptorV1(const std::uint8_t*,std::size_t,SblrGroupDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrGroupResultV1(const SblrGroupResultV1&);bool DecodeSblrGroupResultV1(const std::uint8_t*,std::size_t,SblrGroupResultV1*,std::string*);}
