#pragma once
#include <array>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
namespace scratchbird::engine::sblr { using SystemConfigResetUuid=std::array<uint8_t,16>; struct SblrSystemConfigResetRequestV1{SystemConfigResetUuid operation{},receipt{};}; struct SblrSystemConfigResetDescriptorV1{std::array<uint8_t,256> body{};}; struct SblrSystemConfigResetResultV1{std::array<uint8_t,176> body{};}; std::vector<uint8_t> EncodeSblrSystemConfigResetRequestV1(const SblrSystemConfigResetRequestV1&); bool DecodeSblrSystemConfigResetRequestV1(const uint8_t*,size_t,SblrSystemConfigResetRequestV1*,std::string*); std::vector<uint8_t> EncodeSblrSystemConfigResetDescriptorV1(const SblrSystemConfigResetDescriptorV1&); bool DecodeSblrSystemConfigResetDescriptorV1(const uint8_t*,size_t,SblrSystemConfigResetDescriptorV1*,std::string*); std::vector<uint8_t> EncodeSblrSystemConfigResetResultV1(const SblrSystemConfigResetResultV1&); bool DecodeSblrSystemConfigResetResultV1(const uint8_t*,size_t,SblrSystemConfigResetResultV1*,std::string*); }
