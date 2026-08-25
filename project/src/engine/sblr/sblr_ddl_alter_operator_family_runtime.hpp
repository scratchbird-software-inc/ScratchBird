#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlAlterOperatorFamilyRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlAlterOperatorFamilyDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlAlterOperatorFamilyResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlAlterOperatorFamilyRequestV1(const SblrDdlAlterOperatorFamilyRequestV1&);bool DecodeSblrDdlAlterOperatorFamilyRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterOperatorFamilyRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterOperatorFamilyDescriptorV1(const SblrDdlAlterOperatorFamilyDescriptorV1&);bool DecodeSblrDdlAlterOperatorFamilyDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterOperatorFamilyDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterOperatorFamilyResultV1(const SblrDdlAlterOperatorFamilyResultV1&);bool DecodeSblrDdlAlterOperatorFamilyResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterOperatorFamilyResultV1*,std::string*);}
