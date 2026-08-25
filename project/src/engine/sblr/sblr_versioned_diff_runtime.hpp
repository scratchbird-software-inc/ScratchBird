#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrVersionedDiffRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrVersionedDiffDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrVersionedDiffResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrVersionedDiffRequestV1(const SblrVersionedDiffRequestV1&);bool DecodeSblrVersionedDiffRequestV1(const std::uint8_t*,std::size_t,SblrVersionedDiffRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedDiffDescriptorV1(const SblrVersionedDiffDescriptorV1&);bool DecodeSblrVersionedDiffDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedDiffDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedDiffResultV1(const SblrVersionedDiffResultV1&);bool DecodeSblrVersionedDiffResultV1(const std::uint8_t*,std::size_t,SblrVersionedDiffResultV1*,std::string*);}
