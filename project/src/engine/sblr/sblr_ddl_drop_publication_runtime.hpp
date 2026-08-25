#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropPublicationUuid=std::array<std::uint8_t,16>;
struct SblrDdlDropPublicationRequestV1{DdlDropPublicationUuid operation{},receipt{};std::uint32_t descriptor_length=0;};
struct SblrDdlDropPublicationDescriptorV1{std::array<std::uint8_t,312> body{};};
struct SblrDdlDropPublicationResultV1{std::array<std::uint8_t,184> body{};};
std::vector<std::uint8_t> EncodeSblrDdlDropPublicationRequestV1(const SblrDdlDropPublicationRequestV1&);
bool DecodeSblrDdlDropPublicationRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropPublicationRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropPublicationDescriptorV1(const SblrDdlDropPublicationDescriptorV1&);
bool DecodeSblrDdlDropPublicationDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropPublicationDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropPublicationResultV1(const SblrDdlDropPublicationResultV1&);
bool DecodeSblrDdlDropPublicationResultV1(const std::uint8_t*,std::size_t,SblrDdlDropPublicationResultV1*,std::string*);
}
