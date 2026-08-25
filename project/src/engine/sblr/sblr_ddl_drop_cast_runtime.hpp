#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropCastRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropCastDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlDropCastResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlDropCastRequestV1(const SblrDdlDropCastRequestV1&);bool DecodeSblrDdlDropCastRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropCastRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropCastDescriptorV1(const SblrDdlDropCastDescriptorV1&);bool DecodeSblrDdlDropCastDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropCastDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropCastResultV1(const SblrDdlDropCastResultV1&);bool DecodeSblrDdlDropCastResultV1(const std::uint8_t*,std::size_t,SblrDdlDropCastResultV1*,std::string*);}
