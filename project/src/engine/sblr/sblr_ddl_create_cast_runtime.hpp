#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateCastRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateCastDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlCreateCastResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateCastRequestV1(const SblrDdlCreateCastRequestV1&);bool DecodeSblrDdlCreateCastRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateCastRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateCastDescriptorV1(const SblrDdlCreateCastDescriptorV1&);bool DecodeSblrDdlCreateCastDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateCastDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateCastResultV1(const SblrDdlCreateCastResultV1&);bool DecodeSblrDdlCreateCastResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateCastResultV1*,std::string*);}
