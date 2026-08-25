#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlDropOperatorRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;}; struct SblrDdlDropOperatorDescriptorV1{std::array<std::uint8_t,376> body{};}; struct SblrDdlDropOperatorResultV1{std::array<std::uint8_t,376> body{};}; std::vector<std::uint8_t> EncodeSblrDdlDropOperatorRequestV1(const SblrDdlDropOperatorRequestV1&);bool DecodeSblrDdlDropOperatorRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorRequestV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlDropOperatorDescriptorV1(const SblrDdlDropOperatorDescriptorV1&);bool DecodeSblrDdlDropOperatorDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorDescriptorV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlDropOperatorResultV1(const SblrDdlDropOperatorResultV1&);bool DecodeSblrDdlDropOperatorResultV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorResultV1*,std::string*);}
