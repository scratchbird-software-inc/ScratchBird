#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterAlterPlacementPolicyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterAlterPlacementPolicyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterAlterPlacementPolicyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterAlterPlacementPolicyRequestV1(const SblrClusterAlterPlacementPolicyRequestV1&);bool DecodeSblrClusterAlterPlacementPolicyRequestV1(const std::uint8_t*,std::size_t,SblrClusterAlterPlacementPolicyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterAlterPlacementPolicyDescriptorV1(const SblrClusterAlterPlacementPolicyDescriptorV1&);bool DecodeSblrClusterAlterPlacementPolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterAlterPlacementPolicyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterAlterPlacementPolicyResultV1(const SblrClusterAlterPlacementPolicyResultV1&);bool DecodeSblrClusterAlterPlacementPolicyResultV1(const std::uint8_t*,std::size_t,SblrClusterAlterPlacementPolicyResultV1*,std::string*);}
