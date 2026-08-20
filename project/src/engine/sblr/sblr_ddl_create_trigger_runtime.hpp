#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateTriggerUuid=std::array<std::uint8_t,16>; using DdlCreateTriggerSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateTriggerRequestV1 { DdlCreateTriggerUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlCreateTriggerDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateTriggerSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateTriggerResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateTriggerSha evidence{}; std::uint64_t availability=0; DdlCreateTriggerUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerRequestV1(const SblrDdlCreateTriggerRequestV1&);
bool DecodeSblrDdlCreateTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateTriggerRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerDescriptorV1(const SblrDdlCreateTriggerDescriptorV1&,bool);
bool DecodeSblrDdlCreateTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateTriggerDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerResultV1(const SblrDdlCreateTriggerResultV1&);
bool DecodeSblrDdlCreateTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateTriggerResultV1*,std::string*);
}
