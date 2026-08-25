#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrDdlAlterEventTriggerRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrDdlAlterEventTriggerDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrDdlAlterEventTriggerResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrDdlAlterEventTriggerRequestV1(const SblrDdlAlterEventTriggerRequestV1&);bool DecodeSblrDdlAlterEventTriggerRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterEventTriggerRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterEventTriggerDescriptorV1(const SblrDdlAlterEventTriggerDescriptorV1&);bool DecodeSblrDdlAlterEventTriggerDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterEventTriggerDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDdlAlterEventTriggerResultV1(const SblrDdlAlterEventTriggerResultV1&);bool DecodeSblrDdlAlterEventTriggerResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterEventTriggerResultV1*,std::string*);}
