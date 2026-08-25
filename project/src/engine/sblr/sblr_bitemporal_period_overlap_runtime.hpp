#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrBitemporalPeriodOverlapRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrBitemporalPeriodOverlapDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrBitemporalPeriodOverlapResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrBitemporalPeriodOverlapRequestV1(const SblrBitemporalPeriodOverlapRequestV1&);bool DecodeSblrBitemporalPeriodOverlapRequestV1(const std::uint8_t*,std::size_t,SblrBitemporalPeriodOverlapRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalPeriodOverlapDescriptorV1(const SblrBitemporalPeriodOverlapDescriptorV1&);bool DecodeSblrBitemporalPeriodOverlapDescriptorV1(const std::uint8_t*,std::size_t,SblrBitemporalPeriodOverlapDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalPeriodOverlapResultV1(const SblrBitemporalPeriodOverlapResultV1&);bool DecodeSblrBitemporalPeriodOverlapResultV1(const std::uint8_t*,std::size_t,SblrBitemporalPeriodOverlapResultV1*,std::string*);}
