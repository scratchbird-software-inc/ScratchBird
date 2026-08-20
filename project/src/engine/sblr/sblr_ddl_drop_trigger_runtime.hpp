#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropTriggerUuid=std::array<std::uint8_t,16>; using DdlDropTriggerSha=std::array<std::uint8_t,32>;
struct SblrDdlDropTriggerRequestV1 { DdlDropTriggerUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t trigger_occurrence=0; };
struct SblrDdlDropTriggerDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropTriggerSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropTriggerResultV1 { std::array<std::uint8_t,240> body{}; DdlDropTriggerSha evidence{}; std::uint64_t availability=0; DdlDropTriggerUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropTriggerRequestV1(const SblrDdlDropTriggerRequestV1&);
bool DecodeSblrDdlDropTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropTriggerRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropTriggerDescriptorV1(const SblrDdlDropTriggerDescriptorV1&,bool);
bool DecodeSblrDdlDropTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropTriggerDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropTriggerResultV1(const SblrDdlDropTriggerResultV1&);
bool DecodeSblrDdlDropTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlDropTriggerResultV1*,std::string*);
}
