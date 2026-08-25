#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlDropOperatorClassRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropOperatorClassDescriptorV1{std::array<std::uint8_t,376> body{};};struct SblrDdlDropOperatorClassResultV1{std::array<std::uint8_t,376> body{};};std::vector<std::uint8_t> EncodeSblrDdlDropOperatorClassRequestV1(const SblrDdlDropOperatorClassRequestV1&);bool DecodeSblrDdlDropOperatorClassRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorClassRequestV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlDropOperatorClassDescriptorV1(const SblrDdlDropOperatorClassDescriptorV1&);bool DecodeSblrDdlDropOperatorClassDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorClassDescriptorV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlDropOperatorClassResultV1(const SblrDdlDropOperatorClassResultV1&);bool DecodeSblrDdlDropOperatorClassResultV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorClassResultV1*,std::string*);}
