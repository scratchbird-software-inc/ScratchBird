#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrClusterDeclareAvailabilityZoneRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrClusterDeclareAvailabilityZoneDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrClusterDeclareAvailabilityZoneResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrClusterDeclareAvailabilityZoneRequestV1(const SblrClusterDeclareAvailabilityZoneRequestV1&);bool DecodeSblrClusterDeclareAvailabilityZoneRequestV1(const std::uint8_t*,std::size_t,SblrClusterDeclareAvailabilityZoneRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareAvailabilityZoneDescriptorV1(const SblrClusterDeclareAvailabilityZoneDescriptorV1&);bool DecodeSblrClusterDeclareAvailabilityZoneDescriptorV1(const std::uint8_t*,std::size_t,SblrClusterDeclareAvailabilityZoneDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrClusterDeclareAvailabilityZoneResultV1(const SblrClusterDeclareAvailabilityZoneResultV1&);bool DecodeSblrClusterDeclareAvailabilityZoneResultV1(const std::uint8_t*,std::size_t,SblrClusterDeclareAvailabilityZoneResultV1*,std::string*);}
