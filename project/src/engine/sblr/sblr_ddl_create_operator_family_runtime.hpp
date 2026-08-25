#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateOperatorFamilyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateOperatorFamilyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlCreateOperatorFamilyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateOperatorFamilyRequestV1(const SblrDdlCreateOperatorFamilyRequestV1&);bool DecodeSblrDdlCreateOperatorFamilyRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorFamilyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateOperatorFamilyDescriptorV1(const SblrDdlCreateOperatorFamilyDescriptorV1&);bool DecodeSblrDdlCreateOperatorFamilyDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorFamilyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateOperatorFamilyResultV1(const SblrDdlCreateOperatorFamilyResultV1&);bool DecodeSblrDdlCreateOperatorFamilyResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorFamilyResultV1*,std::string*);}
