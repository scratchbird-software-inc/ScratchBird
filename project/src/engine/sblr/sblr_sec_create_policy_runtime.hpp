#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecCreatePolicyRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; };
struct SblrSecCreatePolicyDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t expected_generation=0, catalog_generation=0, security_generation=0, availability=0; };
struct SblrSecCreatePolicyResultV1 { std::array<std::uint8_t,32> effect_evidence{}; std::uint64_t generation=0, security_generation=0, availability=0; std::uint8_t status=0, publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecCreatePolicyRequestV1(const SblrSecCreatePolicyRequestV1&); bool DecodeSblrSecCreatePolicyRequestV1(const std::uint8_t*,std::size_t,SblrSecCreatePolicyRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecCreatePolicyDescriptorV1(const SblrSecCreatePolicyDescriptorV1&,bool); bool DecodeSblrSecCreatePolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrSecCreatePolicyDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecCreatePolicyResultV1(const SblrSecCreatePolicyResultV1&); bool DecodeSblrSecCreatePolicyResultV1(const std::uint8_t*,std::size_t,SblrSecCreatePolicyResultV1*,std::string*);
}
