#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropCollationRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropCollationDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlDropCollationResultV1{std::array<std::uint8_t,376>body{};};std::vector<uint8_t>EncodeSblrDdlDropCollationRequestV1(const SblrDdlDropCollationRequestV1&);bool DecodeSblrDdlDropCollationRequestV1(const uint8_t*,size_t,SblrDdlDropCollationRequestV1*,std::string*);std::vector<uint8_t>EncodeSblrDdlDropCollationDescriptorV1(const SblrDdlDropCollationDescriptorV1&);bool DecodeSblrDdlDropCollationDescriptorV1(const uint8_t*,size_t,SblrDdlDropCollationDescriptorV1*,std::string*);}
