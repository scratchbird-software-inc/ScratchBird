#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using AccessUuid=std::array<std::uint8_t,16>;using AccessSha=std::array<std::uint8_t,32>;
struct SblrAccessCursorOpenRequestV1{AccessUuid receipt{};std::uint64_t occurrence=0;std::uint32_t cursor_occurrence=0;};
struct SblrAccessCursorOpenDescriptorV1{AccessUuid descriptor{},cursor{},session{},transaction{},relation{},index{},catalog{},mga{},security{},policy{},route{},key_type{},key_codec{};AccessSha key_sha{},evidence{};std::array<std::uint8_t,32>key{};std::uint64_t descriptor_generation=0,cursor_generation=0,local_transaction_id=0,relation_generation=0,index_generation=0,catalog_generation=0,mga_generation=0,security_generation=0,policy_generation=0,route_generation=0,key_codec_generation=0,availability_generation=0;std::uint16_t key_length=0;std::uint8_t open_mode=0,key_state=0,comparator=0,reverse_capable=0;};
struct SblrAccessCursorHandleV1{AccessUuid descriptor{},cursor{},relation{},index{},session{},transaction{};AccessSha position_token{},evidence{};std::uint64_t descriptor_generation=0,cursor_generation=0,availability_generation=0;std::uint8_t state=0,direction=0;};
std::vector<std::uint8_t>EncodeSblrAccessCursorOpenRequestV1(const SblrAccessCursorOpenRequestV1&);bool DecodeSblrAccessCursorOpenRequestV1(const std::uint8_t*,std::size_t,SblrAccessCursorOpenRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrAccessCursorOpenDescriptorV1(const SblrAccessCursorOpenDescriptorV1&,bool);bool DecodeSblrAccessCursorOpenDescriptorV1(const std::uint8_t*,std::size_t,SblrAccessCursorOpenDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t>EncodeSblrAccessCursorHandleV1(const SblrAccessCursorHandleV1&);bool DecodeSblrAccessCursorHandleV1(const std::uint8_t*,std::size_t,SblrAccessCursorHandleV1*,std::string*);}
