#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterCreatePlacementPolicyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterCreatePlacementPolicyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterCreatePlacementPolicyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterCreatePlacementPolicyRequestV1(const SblrClusterCreatePlacementPolicyRequestV1&);bool DecodeSblrClusterCreatePlacementPolicyRequestV1(const std::uint8_t*,std::size_t,SblrClusterCreatePlacementPolicyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterCreatePlacementPolicyDescriptorV1(const SblrClusterCreatePlacementPolicyDescriptorV1&);bool DecodeSblrClusterCreatePlacementPolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterCreatePlacementPolicyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterCreatePlacementPolicyResultV1(const SblrClusterCreatePlacementPolicyResultV1&);bool DecodeSblrClusterCreatePlacementPolicyResultV1(const std::uint8_t*,std::size_t,SblrClusterCreatePlacementPolicyResultV1*,std::string*);}
