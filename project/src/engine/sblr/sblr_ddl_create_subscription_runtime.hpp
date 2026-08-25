#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateSubscriptionRequestV1{std::array<std::uint8_t,16> operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateSubscriptionDescriptorV1{std::array<std::uint8_t,376> body{};};struct SblrDdlCreateSubscriptionResultV1{std::array<std::uint8_t,184> body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateSubscriptionRequestV1(const SblrDdlCreateSubscriptionRequestV1&);bool DecodeSblrDdlCreateSubscriptionRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateSubscriptionRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateSubscriptionDescriptorV1(const SblrDdlCreateSubscriptionDescriptorV1&);bool DecodeSblrDdlCreateSubscriptionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateSubscriptionDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateSubscriptionResultV1(const SblrDdlCreateSubscriptionResultV1&);bool DecodeSblrDdlCreateSubscriptionResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateSubscriptionResultV1*,std::string*);}
