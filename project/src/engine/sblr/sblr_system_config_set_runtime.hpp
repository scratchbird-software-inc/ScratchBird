#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SystemConfigSetUuid=std::array<std::uint8_t,16>; using SystemConfigSetSha=std::array<std::uint8_t,32>;
struct SblrSystemConfigSetRequestV1 { SystemConfigSetUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t config_occurrence=0; };
struct SblrSystemConfigSetDescriptorV1 { std::array<std::uint8_t,400> body{}; SystemConfigSetSha evidence{}; std::uint64_t availability=0; };
struct SblrSystemConfigSetResultV1 { std::array<std::uint8_t,240> body{}; SystemConfigSetSha evidence{}; std::uint64_t availability=0; SystemConfigSetUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrSystemConfigSetRequestV1(const SblrSystemConfigSetRequestV1&);
bool DecodeSblrSystemConfigSetRequestV1(const std::uint8_t*,std::size_t,SblrSystemConfigSetRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSystemConfigSetDescriptorV1(const SblrSystemConfigSetDescriptorV1&,bool);
bool DecodeSblrSystemConfigSetDescriptorV1(const std::uint8_t*,std::size_t,SblrSystemConfigSetDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSystemConfigSetResultV1(const SblrSystemConfigSetResultV1&);
bool DecodeSblrSystemConfigSetResultV1(const std::uint8_t*,std::size_t,SblrSystemConfigSetResultV1*,std::string*);
}
