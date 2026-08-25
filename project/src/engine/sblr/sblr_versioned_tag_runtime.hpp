#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrVersionedTagRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrVersionedTagDescriptorV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrVersionedTagRequestV1(const SblrVersionedTagRequestV1&);bool DecodeSblrVersionedTagRequestV1(const std::uint8_t*,std::size_t,SblrVersionedTagRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedTagDescriptorV1(const SblrVersionedTagDescriptorV1&);bool DecodeSblrVersionedTagDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedTagDescriptorV1*,std::string*);}
