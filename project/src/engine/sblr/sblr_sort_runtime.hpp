#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using SortUuid=std::array<std::uint8_t,16>;using SortSha=std::array<std::uint8_t,32>;struct SblrSortRequestV1{SortUuid receipt{};std::uint64_t occurrence=0;std::uint32_t sort_occurrence=0;};struct SblrSortDescriptorV1{std::array<std::uint8_t,392>body{};SortSha evidence{};std::uint64_t availability=0;};struct SblrSortResultV1{std::array<std::uint8_t,240>body{};SortSha evidence{};std::uint64_t availability=0;SortUuid publication_barrier{};};std::vector<std::uint8_t>EncodeSblrSortRequestV1(const SblrSortRequestV1&);bool DecodeSblrSortRequestV1(const std::uint8_t*,std::size_t,SblrSortRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrSortDescriptorV1(const SblrSortDescriptorV1&,bool);bool DecodeSblrSortDescriptorV1(const std::uint8_t*,std::size_t,SblrSortDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrSortResultV1(const SblrSortResultV1&);bool DecodeSblrSortResultV1(const std::uint8_t*,std::size_t,SblrSortResultV1*,std::string*);}
