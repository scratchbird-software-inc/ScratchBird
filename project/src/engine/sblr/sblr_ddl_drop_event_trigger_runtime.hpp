#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlDropEventTriggerRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlDropEventTriggerDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlDropEventTriggerResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlDropEventTriggerRequestV1(const SblrDdlDropEventTriggerRequestV1&);bool DecodeSblrDdlDropEventTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropEventTriggerRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropEventTriggerDescriptorV1(const SblrDdlDropEventTriggerDescriptorV1&);bool DecodeSblrDdlDropEventTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropEventTriggerDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlDropEventTriggerResultV1(const SblrDdlDropEventTriggerResultV1&);bool DecodeSblrDdlDropEventTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlDropEventTriggerResultV1*,std::string*);}
