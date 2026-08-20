#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using AfUuid=std::array<std::uint8_t,16>; using AfSha=std::array<std::uint8_t,32>;
struct SblrAutonomousFrameRequestV1{AfUuid receipt{},parent_transaction{},parent_frame{},database{},attachment{},session{},body{},dynamic{};std::uint64_t occurrence=0;std::uint8_t intent=0,depth=0;std::uint16_t effect_count=0;AfSha effect_sha{},body_sha{};};
struct SblrAutonomousFrameDescriptorV1{AfUuid receipt{},frame{},child_transaction{},parent_transaction{},parent_frame{},database{},attachment{},session{},principal{},security{},policy{},body{},dynamic{};std::uint64_t frame_generation=0,child_transaction_number=0,catalog_generation=0,capability_generation=0;std::uint8_t intent=0,depth=0;std::uint16_t effect_count=0;AfSha effect_sha{},evidence_sha{};};
struct SblrAutonomousFrameResultV1{AfUuid frame{},child_transaction{},parent_transaction{},recovery_token{};std::uint64_t frame_generation=0,child_transaction_number=0,commit_sequence=0,rollback_sequence=0,recovery_generation=0,availability_generation=0;std::uint8_t final_state=0,intent=0,depth=0;AfSha finality_sha{},result_sha{};};
std::vector<std::uint8_t> EncodeSblrAutonomousFrameRequestV1(const SblrAutonomousFrameRequestV1&);bool DecodeSblrAutonomousFrameRequestV1(const std::uint8_t*,std::size_t,SblrAutonomousFrameRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrAutonomousFrameDescriptorV1(const SblrAutonomousFrameDescriptorV1&,bool operand=false);bool DecodeSblrAutonomousFrameDescriptorV1(const std::uint8_t*,std::size_t,SblrAutonomousFrameDescriptorV1*,std::string*,bool operand=false);
std::vector<std::uint8_t> EncodeSblrAutonomousFrameResultV1(const SblrAutonomousFrameResultV1&);bool DecodeSblrAutonomousFrameResultV1(const std::uint8_t*,std::size_t,SblrAutonomousFrameResultV1*,std::string*);
}
