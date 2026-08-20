#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using TicUuid=std::array<std::uint8_t,16>; using TicSha=std::array<std::uint8_t,32>;
struct SblrTemporaryInstanceCleanupRequestV1{TicUuid receipt{},transaction{},session{};std::uint64_t occurrence=0;std::uint8_t trigger=0;};
struct SblrTemporaryInstanceCleanupDescriptorV1{TicUuid descriptor{},definition{},instance{},owner_session{},owner_transaction{};std::uint64_t descriptor_generation=0,instance_generation=0,catalog_generation=0,security_generation=0,policy_generation=0,availability_generation=0;std::uint8_t retention=0,trigger=0,state=0;TicSha evidence{};};
struct SblrTemporaryInstanceCleanupResultV1{TicUuid definition{},instance{},owner_session{},owner_transaction{};std::uint64_t retired_generation=0,cleanup_sequence=0,reclaimed_pages=0,availability_generation=0;std::uint8_t trigger=0,state=0;TicSha cleanup_evidence{},result_evidence{};};
std::vector<std::uint8_t> EncodeSblrTemporaryInstanceCleanupRequestV1(const SblrTemporaryInstanceCleanupRequestV1&);bool DecodeSblrTemporaryInstanceCleanupRequestV1(const std::uint8_t*,std::size_t,SblrTemporaryInstanceCleanupRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrTemporaryInstanceCleanupDescriptorV1(const SblrTemporaryInstanceCleanupDescriptorV1&,bool operand=false);bool DecodeSblrTemporaryInstanceCleanupDescriptorV1(const std::uint8_t*,std::size_t,SblrTemporaryInstanceCleanupDescriptorV1*,std::string*,bool operand=false);
std::vector<std::uint8_t> EncodeSblrTemporaryInstanceCleanupResultV1(const SblrTemporaryInstanceCleanupResultV1&);bool DecodeSblrTemporaryInstanceCleanupResultV1(const std::uint8_t*,std::size_t,SblrTemporaryInstanceCleanupResultV1*,std::string*);
}
