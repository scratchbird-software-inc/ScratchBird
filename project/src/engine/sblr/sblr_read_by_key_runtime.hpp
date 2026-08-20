#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using ReadKeyUuid=std::array<std::uint8_t,16>;using ReadKeySha=std::array<std::uint8_t,32>;
struct SblrReadByKeyRequestV1{ReadKeyUuid receipt{};std::uint64_t occurrence=0;std::uint32_t key_occurrence=0;};
struct SblrReadByKeyDescriptorV1{ReadKeyUuid descriptor{},relation{},index{},schema_snapshot{},mga_snapshot{},security_snapshot{},key_type{},key_codec{};ReadKeySha key_sha{},descriptor_evidence{};std::array<std::uint8_t,32>key_bytes{};std::uint64_t descriptor_generation=0,relation_generation=0,index_generation=0,schema_generation=0,mga_generation=0,security_generation=0,key_codec_generation=0,availability_generation=0;std::uint16_t key_length=0;std::uint8_t key_state=0,redaction_mode=0;};
struct SblrReadByKeyResultV1{ReadKeyUuid descriptor{},relation{},row{};ReadKeySha row_sha{},redaction_evidence{},result_evidence{};std::uint64_t descriptor_generation=0,row_version=0,availability_generation=0;std::uint8_t outcome=0;};
std::vector<std::uint8_t>EncodeSblrReadByKeyRequestV1(const SblrReadByKeyRequestV1&);bool DecodeSblrReadByKeyRequestV1(const std::uint8_t*,std::size_t,SblrReadByKeyRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrReadByKeyDescriptorV1(const SblrReadByKeyDescriptorV1&,bool operand);bool DecodeSblrReadByKeyDescriptorV1(const std::uint8_t*,std::size_t,SblrReadByKeyDescriptorV1*,std::string*,bool operand);
std::vector<std::uint8_t>EncodeSblrReadByKeyResultV1(const SblrReadByKeyResultV1&);bool DecodeSblrReadByKeyResultV1(const std::uint8_t*,std::size_t,SblrReadByKeyResultV1*,std::string*);}
