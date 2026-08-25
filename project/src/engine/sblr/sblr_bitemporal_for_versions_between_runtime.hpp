#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrBitemporalForVersionsBetweenRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrBitemporalForVersionsBetweenDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrBitemporalForVersionsBetweenResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrBitemporalForVersionsBetweenRequestV1(const SblrBitemporalForVersionsBetweenRequestV1&);bool DecodeSblrBitemporalForVersionsBetweenRequestV1(const std::uint8_t*,std::size_t,SblrBitemporalForVersionsBetweenRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalForVersionsBetweenDescriptorV1(const SblrBitemporalForVersionsBetweenDescriptorV1&);bool DecodeSblrBitemporalForVersionsBetweenDescriptorV1(const std::uint8_t*,std::size_t,SblrBitemporalForVersionsBetweenDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalForVersionsBetweenResultV1(const SblrBitemporalForVersionsBetweenResultV1&);bool DecodeSblrBitemporalForVersionsBetweenResultV1(const std::uint8_t*,std::size_t,SblrBitemporalForVersionsBetweenResultV1*,std::string*);}
