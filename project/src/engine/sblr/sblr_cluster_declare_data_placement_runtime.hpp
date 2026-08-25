#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterDeclareDataPlacementRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterDeclareDataPlacementDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterDeclareDataPlacementResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterDeclareDataPlacementRequestV1(const SblrClusterDeclareDataPlacementRequestV1&);bool DecodeSblrClusterDeclareDataPlacementRequestV1(const std::uint8_t*,std::size_t,SblrClusterDeclareDataPlacementRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareDataPlacementDescriptorV1(const SblrClusterDeclareDataPlacementDescriptorV1&);bool DecodeSblrClusterDeclareDataPlacementDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterDeclareDataPlacementDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareDataPlacementResultV1(const SblrClusterDeclareDataPlacementResultV1&);bool DecodeSblrClusterDeclareDataPlacementResultV1(const std::uint8_t*,std::size_t,SblrClusterDeclareDataPlacementResultV1*,std::string*);}
