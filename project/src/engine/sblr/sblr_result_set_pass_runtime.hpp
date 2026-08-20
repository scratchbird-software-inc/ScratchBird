#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using PassUuid=std::array<std::uint8_t,16>;using PassSha=std::array<std::uint8_t,32>;
struct SblrResultSetPassRequestV1{PassUuid receipt{};std::uint64_t occurrence=0;std::uint32_t pass_occurrence=0;};
struct SblrResultSetPassDescriptorV1{PassUuid descriptor{},source_handle{},owner_session{},owner_transaction{},row_shape{},security{},mga{},recipient_session{};PassSha source_evidence{},descriptor_evidence{};std::uint64_t descriptor_generation=0,source_generation=0,owner_local_transaction_id=0,row_shape_generation=0,security_generation=0,mga_generation=0,expiry_monotonic_ns=0,availability_generation=0;std::uint32_t maximum_consumers=0;std::uint8_t lifetime=0,transfer_mode=0,source_state=0;};
struct SblrResultSetPassHandleV1{PassUuid descriptor{},passed_handle{},source_handle{},recipient_session{},row_shape{},lease{};PassSha transfer_evidence{},result_evidence{};std::uint64_t descriptor_generation=0,passed_generation=0,expiry_monotonic_ns=0,availability_generation=0;std::uint8_t lifetime=0,state=0;};
std::vector<std::uint8_t>EncodeSblrResultSetPassRequestV1(const SblrResultSetPassRequestV1&);bool DecodeSblrResultSetPassRequestV1(const std::uint8_t*,std::size_t,SblrResultSetPassRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrResultSetPassDescriptorV1(const SblrResultSetPassDescriptorV1&,bool);bool DecodeSblrResultSetPassDescriptorV1(const std::uint8_t*,std::size_t,SblrResultSetPassDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t>EncodeSblrResultSetPassHandleV1(const SblrResultSetPassHandleV1&);bool DecodeSblrResultSetPassHandleV1(const std::uint8_t*,std::size_t,SblrResultSetPassHandleV1*,std::string*);}
