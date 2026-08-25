#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateNamedCollectionRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateNamedCollectionDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlCreateNamedCollectionResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateNamedCollectionRequestV1(const SblrDdlCreateNamedCollectionRequestV1&);bool DecodeSblrDdlCreateNamedCollectionRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateNamedCollectionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateNamedCollectionDescriptorV1(const SblrDdlCreateNamedCollectionDescriptorV1&);bool DecodeSblrDdlCreateNamedCollectionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateNamedCollectionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateNamedCollectionResultV1(const SblrDdlCreateNamedCollectionResultV1&);bool DecodeSblrDdlCreateNamedCollectionResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateNamedCollectionResultV1*,std::string*);}
