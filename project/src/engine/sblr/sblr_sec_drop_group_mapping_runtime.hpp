#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecDropGroupMappingRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; };
struct SblrSecDropGroupMappingDescriptorV1 { std::array<std::uint8_t,16> mapping_uuid{1,1,1,1,1,1,0x40,1,0x80,1,1,1,1,1,1,1}, descriptor_evidence{}; std::uint64_t expected_generation=0,catalog_generation=0,security_generation=0,availability=0; };
struct SblrSecDropGroupMappingResultV1 { std::array<std::uint8_t,16> mapping_uuid{}; std::array<std::uint8_t,32> effect_evidence{}; std::uint64_t generation=0,security_generation=0,availability=0; std::uint8_t status=0,publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecDropGroupMappingRequestV1(const SblrSecDropGroupMappingRequestV1&); bool DecodeSblrSecDropGroupMappingRequestV1(const std::uint8_t*,std::size_t,SblrSecDropGroupMappingRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecDropGroupMappingDescriptorV1(const SblrSecDropGroupMappingDescriptorV1&,bool); bool DecodeSblrSecDropGroupMappingDescriptorV1(const std::uint8_t*,std::size_t,SblrSecDropGroupMappingDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecDropGroupMappingResultV1(const SblrSecDropGroupMappingResultV1&); bool DecodeSblrSecDropGroupMappingResultV1(const std::uint8_t*,std::size_t,SblrSecDropGroupMappingResultV1*,std::string*);
}
