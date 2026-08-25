#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SystemConfigGetUuid=std::array<std::uint8_t,16>; using SystemConfigGetSha=std::array<std::uint8_t,32>;
struct SblrSystemConfigGetRequestV1 { SystemConfigGetUuid operation{}; SystemConfigGetUuid receipt{}; };
struct SblrSystemConfigGetDescriptorV1 { SystemConfigGetUuid operation{},receipt{},scope{},snapshot{},projection{},redaction_policy{},security_context{},transaction{},route_evidence{}; std::uint64_t scope_generation=0,snapshot_generation=0,policy_generation=0; SystemConfigGetSha descriptor_hash{}; };
struct SblrSystemConfigGetResultV1 { SystemConfigGetUuid operation{},scope{},snapshot{},evidence{},recovery_evidence{}; std::uint64_t scope_generation=0,snapshot_generation=0; std::uint32_t redaction_class=0,status=0,projection_length=0; std::array<std::uint8_t,284> projection{}; SystemConfigGetSha material_hash{}; };
std::vector<std::uint8_t> EncodeSblrSystemConfigGetRequestV1(const SblrSystemConfigGetRequestV1&);
bool DecodeSblrSystemConfigGetRequestV1(const std::uint8_t*,std::size_t,SblrSystemConfigGetRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSystemConfigGetDescriptorV1(const SblrSystemConfigGetDescriptorV1&);
bool DecodeSblrSystemConfigGetDescriptorV1(const std::uint8_t*,std::size_t,SblrSystemConfigGetDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSystemConfigGetResultV1(const SblrSystemConfigGetResultV1&);
bool DecodeSblrSystemConfigGetResultV1(const std::uint8_t*,std::size_t,SblrSystemConfigGetResultV1*,std::string*);
}
