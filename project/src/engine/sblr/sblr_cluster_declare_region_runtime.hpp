#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterDeclareRegionRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterDeclareRegionDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterDeclareRegionResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterDeclareRegionRequestV1(const SblrClusterDeclareRegionRequestV1&);bool DecodeSblrClusterDeclareRegionRequestV1(const std::uint8_t*,std::size_t,SblrClusterDeclareRegionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareRegionDescriptorV1(const SblrClusterDeclareRegionDescriptorV1&);bool DecodeSblrClusterDeclareRegionDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterDeclareRegionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareRegionResultV1(const SblrClusterDeclareRegionResultV1&);bool DecodeSblrClusterDeclareRegionResultV1(const std::uint8_t*,std::size_t,SblrClusterDeclareRegionResultV1*,std::string*);}
