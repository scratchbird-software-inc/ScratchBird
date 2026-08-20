#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using AccessCloseUuid=std::array<std::uint8_t,16>;using AccessCloseSha=std::array<std::uint8_t,32>;struct SblrAccessCursorCloseRequestV1{AccessCloseUuid receipt{};std::uint64_t occurrence=0;std::uint32_t close_occurrence=0;};struct SblrAccessCursorCloseDescriptorV1{AccessCloseUuid descriptor{},cursor{},relation{},index{},session{},transaction{},catalog{};AccessCloseSha position_token{},evidence{};std::uint64_t descriptor_generation=0,cursor_generation=0,position_generation=0,availability_generation=0;std::uint8_t close_reason=0,lifecycle_state=0;};std::vector<std::uint8_t>EncodeSblrAccessCursorCloseRequestV1(const SblrAccessCursorCloseRequestV1&);bool DecodeSblrAccessCursorCloseRequestV1(const std::uint8_t*,std::size_t,SblrAccessCursorCloseRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAccessCursorCloseDescriptorV1(const SblrAccessCursorCloseDescriptorV1&,bool);bool DecodeSblrAccessCursorCloseDescriptorV1(const std::uint8_t*,std::size_t,SblrAccessCursorCloseDescriptorV1*,std::string*,bool);}
