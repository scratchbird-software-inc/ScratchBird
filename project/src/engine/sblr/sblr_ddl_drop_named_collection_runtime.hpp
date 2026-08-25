#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropNamedCollectionRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropNamedCollectionDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlDropNamedCollectionResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlDropNamedCollectionRequestV1(const SblrDdlDropNamedCollectionRequestV1&);bool DecodeSblrDdlDropNamedCollectionRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropNamedCollectionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropNamedCollectionDescriptorV1(const SblrDdlDropNamedCollectionDescriptorV1&);bool DecodeSblrDdlDropNamedCollectionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropNamedCollectionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropNamedCollectionResultV1(const SblrDdlDropNamedCollectionResultV1&);bool DecodeSblrDdlDropNamedCollectionResultV1(const std::uint8_t*,std::size_t,SblrDdlDropNamedCollectionResultV1*,std::string*);}
