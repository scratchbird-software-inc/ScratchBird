#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using UpdateUuid=std::array<std::uint8_t,16>;using UpdateSha=std::array<std::uint8_t,32>;struct SblrUpdateRequestV1{UpdateUuid receipt{};std::uint64_t occurrence=0;std::uint32_t update_occurrence=0;};struct SblrUpdateDescriptorV1{std::array<std::uint8_t,400> canonical_body{};UpdateSha evidence{};std::uint64_t availability_generation=0;};struct SblrUpdateResultV1{std::array<std::uint8_t,136> canonical_body{};UpdateSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrUpdateRequestV1(const SblrUpdateRequestV1&);bool DecodeSblrUpdateRequestV1(const std::uint8_t*,std::size_t,SblrUpdateRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrUpdateDescriptorV1(const SblrUpdateDescriptorV1&,bool);bool DecodeSblrUpdateDescriptorV1(const std::uint8_t*,std::size_t,SblrUpdateDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrUpdateResultV1(const SblrUpdateResultV1&);bool DecodeSblrUpdateResultV1(const std::uint8_t*,std::size_t,SblrUpdateResultV1*,std::string*);}
