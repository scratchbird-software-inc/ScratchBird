#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropSubscriptionRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropSubscriptionDescriptorV1{std::array<std::uint8_t,376> body{};};struct SblrDdlDropSubscriptionResultV1{std::array<std::uint8_t,376> body{};};std::vector<std::uint8_t>EncodeSblrDdlDropSubscriptionRequestV1(const SblrDdlDropSubscriptionRequestV1&);bool DecodeSblrDdlDropSubscriptionRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropSubscriptionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropSubscriptionDescriptorV1(const SblrDdlDropSubscriptionDescriptorV1&);bool DecodeSblrDdlDropSubscriptionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropSubscriptionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropSubscriptionResultV1(const SblrDdlDropSubscriptionResultV1&);bool DecodeSblrDdlDropSubscriptionResultV1(const std::uint8_t*,std::size_t,SblrDdlDropSubscriptionResultV1*,std::string*);}
