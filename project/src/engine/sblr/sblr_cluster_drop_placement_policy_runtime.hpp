#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterDropPlacementPolicyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterDropPlacementPolicyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterDropPlacementPolicyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterDropPlacementPolicyRequestV1(const SblrClusterDropPlacementPolicyRequestV1&);bool DecodeSblrClusterDropPlacementPolicyRequestV1(const std::uint8_t*,std::size_t,SblrClusterDropPlacementPolicyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDropPlacementPolicyDescriptorV1(const SblrClusterDropPlacementPolicyDescriptorV1&);bool DecodeSblrClusterDropPlacementPolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterDropPlacementPolicyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDropPlacementPolicyResultV1(const SblrClusterDropPlacementPolicyResultV1&);bool DecodeSblrClusterDropPlacementPolicyResultV1(const std::uint8_t*,std::size_t,SblrClusterDropPlacementPolicyResultV1*,std::string*);}
