#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using UdrUuid=std::array<std::uint8_t,16>;using UdrSha=std::array<std::uint8_t,32>;struct SblrUdrInvokeRequestV1{UdrUuid receipt{};uint64_t occurrence=0;uint32_t invocation_occurrence=0;};struct SblrUdrInvokeDescriptorV1{std::array<uint8_t,424>body{};UdrSha evidence{};uint64_t availability=0;};struct SblrUdrInvokeResultV1{std::array<uint8_t,240>body{};UdrSha evidence{};uint64_t availability=0;UdrUuid barrier{};};std::vector<uint8_t>EncodeSblrUdrInvokeRequestV1(const SblrUdrInvokeRequestV1&);bool DecodeSblrUdrInvokeRequestV1(const uint8_t*,size_t,SblrUdrInvokeRequestV1*,std::string*);std::vector<uint8_t>EncodeSblrUdrInvokeDescriptorV1(const SblrUdrInvokeDescriptorV1&,bool);bool DecodeSblrUdrInvokeDescriptorV1(const uint8_t*,size_t,SblrUdrInvokeDescriptorV1*,std::string*,bool);std::vector<uint8_t>EncodeSblrUdrInvokeResultV1(const SblrUdrInvokeResultV1&);bool DecodeSblrUdrInvokeResultV1(const uint8_t*,size_t,SblrUdrInvokeResultV1*,std::string*);}
