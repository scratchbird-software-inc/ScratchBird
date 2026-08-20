#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlAlterTriggerUuid=std::array<std::uint8_t,16>; using DdlAlterTriggerSha=std::array<std::uint8_t,32>;
struct SblrDdlAlterTriggerRequestV1 { DdlAlterTriggerUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t trigger_occurrence=0; };
struct SblrDdlAlterTriggerDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlAlterTriggerSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlAlterTriggerResultV1 { std::array<std::uint8_t,240> body{}; DdlAlterTriggerSha evidence{}; std::uint64_t availability=0; DdlAlterTriggerUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerRequestV1(const SblrDdlAlterTriggerRequestV1&);
bool DecodeSblrDdlAlterTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterTriggerRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerDescriptorV1(const SblrDdlAlterTriggerDescriptorV1&,bool);
bool DecodeSblrDdlAlterTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterTriggerDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerResultV1(const SblrDdlAlterTriggerResultV1&);
bool DecodeSblrDdlAlterTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterTriggerResultV1*,std::string*);
}
