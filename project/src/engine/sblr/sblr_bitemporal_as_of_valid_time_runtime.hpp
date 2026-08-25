#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrBitemporalAsOfValidTimeRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrBitemporalAsOfValidTimeDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrBitemporalAsOfValidTimeResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrBitemporalAsOfValidTimeRequestV1(const SblrBitemporalAsOfValidTimeRequestV1&);bool DecodeSblrBitemporalAsOfValidTimeRequestV1(const std::uint8_t*,std::size_t,SblrBitemporalAsOfValidTimeRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalAsOfValidTimeDescriptorV1(const SblrBitemporalAsOfValidTimeDescriptorV1&);bool DecodeSblrBitemporalAsOfValidTimeDescriptorV1(const std::uint8_t*,std::size_t,SblrBitemporalAsOfValidTimeDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrBitemporalAsOfValidTimeResultV1(const SblrBitemporalAsOfValidTimeResultV1&);bool DecodeSblrBitemporalAsOfValidTimeResultV1(const std::uint8_t*,std::size_t,SblrBitemporalAsOfValidTimeResultV1*,std::string*);}
