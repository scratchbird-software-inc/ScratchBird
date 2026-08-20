#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlSetTableTypeEnforcementSha=std::array<std::uint8_t,32>; using DdlSetTableTypeEnforcementUuid=std::array<std::uint8_t,16>;
struct SblrDdlSetTableTypeEnforcementRequestV1{DdlSetTableTypeEnforcementUuid receipt{};std::uint64_t occurrence=0;std::uint32_t table_occurrence=0;};
struct SblrDdlSetTableTypeEnforcementDescriptorV1{std::array<std::uint8_t,400> body{};DdlSetTableTypeEnforcementSha evidence{};std::uint64_t availability=0;};
struct SblrDdlSetTableTypeEnforcementResultV1{std::array<std::uint8_t,240> body{};DdlSetTableTypeEnforcementSha evidence{};std::uint64_t availability=0;DdlSetTableTypeEnforcementUuid publication_barrier{};};
std::vector<std::uint8_t> EncodeSblrDdlSetTableTypeEnforcementRequestV1(const SblrDdlSetTableTypeEnforcementRequestV1&); bool DecodeSblrDdlSetTableTypeEnforcementRequestV1(const std::uint8_t*,std::size_t,SblrDdlSetTableTypeEnforcementRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlSetTableTypeEnforcementDescriptorV1(const SblrDdlSetTableTypeEnforcementDescriptorV1&,bool); bool DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlSetTableTypeEnforcementDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlSetTableTypeEnforcementResultV1(const SblrDdlSetTableTypeEnforcementResultV1&); bool DecodeSblrDdlSetTableTypeEnforcementResultV1(const std::uint8_t*,std::size_t,SblrDdlSetTableTypeEnforcementResultV1*,std::string*);
}
