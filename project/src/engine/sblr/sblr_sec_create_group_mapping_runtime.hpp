#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecCreateGroupMappingRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; };
struct SblrSecCreateGroupMappingDescriptorV1 { std::array<std::uint8_t,16> group_uuid{}, principal_uuid{}, descriptor_evidence{}; std::uint64_t generation=0, security_generation=0, availability=0; };
struct SblrSecCreateGroupMappingResultV1 { std::array<std::uint8_t,16> group_uuid{}, principal_uuid{}; std::array<std::uint8_t,32> effect_evidence{}; std::uint64_t generation=0, security_generation=0, availability=0; std::uint8_t status=0, publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecCreateGroupMappingRequestV1(const SblrSecCreateGroupMappingRequestV1&);
bool DecodeSblrSecCreateGroupMappingRequestV1(const std::uint8_t*,std::size_t,SblrSecCreateGroupMappingRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecCreateGroupMappingDescriptorV1(const SblrSecCreateGroupMappingDescriptorV1&,bool);
bool DecodeSblrSecCreateGroupMappingDescriptorV1(const std::uint8_t*,std::size_t,SblrSecCreateGroupMappingDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecCreateGroupMappingResultV1(const SblrSecCreateGroupMappingResultV1&);
bool DecodeSblrSecCreateGroupMappingResultV1(const std::uint8_t*,std::size_t,SblrSecCreateGroupMappingResultV1*,std::string*);
}
