#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlCreateEventTriggerRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlCreateEventTriggerDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlCreateEventTriggerResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlCreateEventTriggerRequestV1(const SblrDdlCreateEventTriggerRequestV1&);bool DecodeSblrDdlCreateEventTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateEventTriggerRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateEventTriggerDescriptorV1(const SblrDdlCreateEventTriggerDescriptorV1&);bool DecodeSblrDdlCreateEventTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateEventTriggerDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlCreateEventTriggerResultV1(const SblrDdlCreateEventTriggerResultV1&);bool DecodeSblrDdlCreateEventTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateEventTriggerResultV1*,std::string*);}
