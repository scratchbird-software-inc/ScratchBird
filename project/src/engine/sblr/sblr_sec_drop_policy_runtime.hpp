#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecDropPolicyRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; };
struct SblrSecDropPolicyDescriptorV1 { std::array<std::uint8_t,16> policy_uuid{1,1,1,1,1,1,0x40,1,0x80,1,1,1,1,1,1,1}, descriptor_evidence{}; std::uint64_t expected_generation=0,catalog_generation=0,security_generation=0,availability=0; };
struct SblrSecDropPolicyResultV1 { std::array<std::uint8_t,16> policy_uuid{}; std::array<std::uint8_t,32> effect_evidence{}; std::uint64_t generation=0,security_generation=0,availability=0; std::uint8_t status=0,publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecDropPolicyRequestV1(const SblrSecDropPolicyRequestV1&); bool DecodeSblrSecDropPolicyRequestV1(const std::uint8_t*,std::size_t,SblrSecDropPolicyRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecDropPolicyDescriptorV1(const SblrSecDropPolicyDescriptorV1&,bool); bool DecodeSblrSecDropPolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrSecDropPolicyDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecDropPolicyResultV1(const SblrSecDropPolicyResultV1&); bool DecodeSblrSecDropPolicyResultV1(const std::uint8_t*,std::size_t,SblrSecDropPolicyResultV1*,std::string*);
}
