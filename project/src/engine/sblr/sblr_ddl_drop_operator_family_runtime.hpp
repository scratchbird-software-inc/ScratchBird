#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropOperatorFamilyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropOperatorFamilyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlDropOperatorFamilyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlDropOperatorFamilyRequestV1(const SblrDdlDropOperatorFamilyRequestV1&);bool DecodeSblrDdlDropOperatorFamilyRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorFamilyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropOperatorFamilyDescriptorV1(const SblrDdlDropOperatorFamilyDescriptorV1&);bool DecodeSblrDdlDropOperatorFamilyDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorFamilyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropOperatorFamilyResultV1(const SblrDdlDropOperatorFamilyResultV1&);bool DecodeSblrDdlDropOperatorFamilyResultV1(const std::uint8_t*,std::size_t,SblrDdlDropOperatorFamilyResultV1*,std::string*);}
