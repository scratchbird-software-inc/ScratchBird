#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrVersionedBranchDeleteRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrVersionedBranchDeleteDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrVersionedBranchDeleteResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrVersionedBranchDeleteRequestV1(const SblrVersionedBranchDeleteRequestV1&);bool DecodeSblrVersionedBranchDeleteRequestV1(const std::uint8_t*,std::size_t,SblrVersionedBranchDeleteRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedBranchDeleteDescriptorV1(const SblrVersionedBranchDeleteDescriptorV1&);bool DecodeSblrVersionedBranchDeleteDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedBranchDeleteDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedBranchDeleteResultV1(const SblrVersionedBranchDeleteResultV1&);bool DecodeSblrVersionedBranchDeleteResultV1(const std::uint8_t*,std::size_t,SblrVersionedBranchDeleteResultV1*,std::string*);}
