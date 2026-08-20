#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using DeleteUuid=std::array<std::uint8_t,16>;using DeleteSha=std::array<std::uint8_t,32>;struct SblrDeleteRequestV1{DeleteUuid receipt{};std::uint64_t occurrence=0;std::uint32_t delete_occurrence=0;};struct SblrDeleteDescriptorV1{std::array<std::uint8_t,368> canonical_body{};DeleteSha evidence{};std::uint64_t availability_generation=0;};struct SblrDeleteResultV1{std::array<std::uint8_t,136> canonical_body{};DeleteSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrDeleteRequestV1(const SblrDeleteRequestV1&);bool DecodeSblrDeleteRequestV1(const std::uint8_t*,std::size_t,SblrDeleteRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDeleteDescriptorV1(const SblrDeleteDescriptorV1&,bool);bool DecodeSblrDeleteDescriptorV1(const std::uint8_t*,std::size_t,SblrDeleteDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrDeleteResultV1(const SblrDeleteResultV1&);bool DecodeSblrDeleteResultV1(const std::uint8_t*,std::size_t,SblrDeleteResultV1*,std::string*);}
