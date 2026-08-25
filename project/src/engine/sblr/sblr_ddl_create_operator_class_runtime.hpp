#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlCreateOperatorClassRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateOperatorClassDescriptorV1{std::array<std::uint8_t,376> body{};};struct SblrDdlCreateOperatorClassResultV1{std::array<std::uint8_t,376> body{};};std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorClassRequestV1(const SblrDdlCreateOperatorClassRequestV1&);bool DecodeSblrDdlCreateOperatorClassRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorClassRequestV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorClassDescriptorV1(const SblrDdlCreateOperatorClassDescriptorV1&);bool DecodeSblrDdlCreateOperatorClassDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorClassDescriptorV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorClassResultV1(const SblrDdlCreateOperatorClassResultV1&);bool DecodeSblrDdlCreateOperatorClassResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorClassResultV1*,std::string*);}
