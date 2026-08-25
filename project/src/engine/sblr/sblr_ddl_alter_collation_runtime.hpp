#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlAlterCollationResultV1; std::vector<std::uint8_t> EncodeSblrDdlAlterCollationResultV1(const SblrDdlAlterCollationResultV1&); bool DecodeSblrDdlAlterCollationResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterCollationResultV1*,std::string*); }
namespace scratchbird::engine::sblr{struct SblrDdlAlterCollationRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlAlterCollationDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlAlterCollationResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlAlterCollationRequestV1(const SblrDdlAlterCollationRequestV1&);bool DecodeSblrDdlAlterCollationRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterCollationRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterCollationDescriptorV1(const SblrDdlAlterCollationDescriptorV1&);bool DecodeSblrDdlAlterCollationDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterCollationDescriptorV1*,std::string*);}
