#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlAlterSubscriptionRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlAlterSubscriptionDescriptorV1{std::array<std::uint8_t,376> body{};};struct SblrDdlAlterSubscriptionResultV1{std::array<std::uint8_t,376> body{};};std::vector<std::uint8_t>EncodeSblrDdlAlterSubscriptionRequestV1(const SblrDdlAlterSubscriptionRequestV1&);bool DecodeSblrDdlAlterSubscriptionRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterSubscriptionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterSubscriptionDescriptorV1(const SblrDdlAlterSubscriptionDescriptorV1&);bool DecodeSblrDdlAlterSubscriptionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterSubscriptionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterSubscriptionResultV1(const SblrDdlAlterSubscriptionResultV1&);bool DecodeSblrDdlAlterSubscriptionResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterSubscriptionResultV1*,std::string*);}
