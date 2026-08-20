#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using CastUuid=std::array<std::uint8_t,16>;using CastSha=std::array<std::uint8_t,32>;struct SblrCastRequestV1{CastUuid receipt{};std::uint64_t occurrence=0;std::uint32_t cast_occurrence=0;};struct SblrCastDescriptorV1{std::array<std::uint8_t,360>canonical_body{};CastSha evidence{};std::uint64_t availability_generation=0;};struct SblrCastResultV1{std::array<std::uint8_t,176>canonical_body{};CastSha executor_evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrCastRequestV1(const SblrCastRequestV1&);bool DecodeSblrCastRequestV1(const std::uint8_t*,std::size_t,SblrCastRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrCastDescriptorV1(const SblrCastDescriptorV1&,bool);bool DecodeSblrCastDescriptorV1(const std::uint8_t*,std::size_t,SblrCastDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrCastResultV1(const SblrCastResultV1&);bool DecodeSblrCastResultV1(const std::uint8_t*,std::size_t,SblrCastResultV1*,std::string*);}
