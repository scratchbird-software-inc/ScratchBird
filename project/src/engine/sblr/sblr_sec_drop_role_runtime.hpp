#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecDropRoleRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; };
struct SblrSecDropRoleDescriptorV1 { std::array<std::uint8_t,16> role_uuid{}, descriptor_evidence{}; std::uint64_t expected_generation=0, catalog_generation=0, security_generation=0, availability=0; };
struct SblrSecDropRoleResultV1 { std::array<std::uint8_t,16> role_uuid{}; std::array<std::uint8_t,32> effect_evidence{}; std::uint64_t generation=0, security_generation=0, availability=0; std::uint8_t status=0, publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecDropRoleRequestV1(const SblrSecDropRoleRequestV1&); bool DecodeSblrSecDropRoleRequestV1(const std::uint8_t*,std::size_t,SblrSecDropRoleRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecDropRoleDescriptorV1(const SblrSecDropRoleDescriptorV1&,bool); bool DecodeSblrSecDropRoleDescriptorV1(const std::uint8_t*,std::size_t,SblrSecDropRoleDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecDropRoleResultV1(const SblrSecDropRoleResultV1&); bool DecodeSblrSecDropRoleResultV1(const std::uint8_t*,std::size_t,SblrSecDropRoleResultV1*,std::string*);
}
