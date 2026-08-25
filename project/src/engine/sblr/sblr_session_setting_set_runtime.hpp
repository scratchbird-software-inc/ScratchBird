#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSessionSettingSetRequestV1 { std::array<uint8_t,16> receipt{}; uint64_t occurrence=0; };
struct SblrSessionSettingSetDescriptorV1 { std::array<uint8_t,16> receipt{}; std::array<uint8_t,32> key{}; std::array<uint8_t,64> value{}; uint64_t expected_generation=0, availability=0; };
struct SblrSessionSettingSetResultV1 { std::array<uint8_t,32> key{}; uint64_t generation=0, availability=0; uint8_t status=0, publication_barrier=0; std::array<uint8_t,32> effect_evidence{}; };
std::vector<uint8_t> EncodeSblrSessionSettingSetRequestV1(const SblrSessionSettingSetRequestV1&); bool DecodeSblrSessionSettingSetRequestV1(const uint8_t*,size_t,SblrSessionSettingSetRequestV1*,std::string*);
std::vector<uint8_t> EncodeSblrSessionSettingSetDescriptorV1(const SblrSessionSettingSetDescriptorV1&,bool); bool DecodeSblrSessionSettingSetDescriptorV1(const uint8_t*,size_t,SblrSessionSettingSetDescriptorV1*,std::string*,bool);
std::vector<uint8_t> EncodeSblrSessionSettingSetResultV1(const SblrSessionSettingSetResultV1&); bool DecodeSblrSessionSettingSetResultV1(const uint8_t*,size_t,SblrSessionSettingSetResultV1*,std::string*);
}
