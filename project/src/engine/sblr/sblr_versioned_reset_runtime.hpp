#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrVersionedResetRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrVersionedResetDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrVersionedResetResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrVersionedResetRequestV1(const SblrVersionedResetRequestV1&);bool DecodeSblrVersionedResetRequestV1(const std::uint8_t*,std::size_t,SblrVersionedResetRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedResetDescriptorV1(const SblrVersionedResetDescriptorV1&);bool DecodeSblrVersionedResetDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedResetDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedResetResultV1(const SblrVersionedResetResultV1&);bool DecodeSblrVersionedResetResultV1(const std::uint8_t*,std::size_t,SblrVersionedResetResultV1*,std::string*);}
