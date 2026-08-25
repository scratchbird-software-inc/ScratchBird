#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateCollationRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateCollationDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlCreateCollationResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateCollationRequestV1(const SblrDdlCreateCollationRequestV1&);bool DecodeSblrDdlCreateCollationRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateCollationRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateCollationDescriptorV1(const SblrDdlCreateCollationDescriptorV1&);bool DecodeSblrDdlCreateCollationDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateCollationDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateCollationResultV1(const SblrDdlCreateCollationResultV1&);bool DecodeSblrDdlCreateCollationResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateCollationResultV1*,std::string*);}
