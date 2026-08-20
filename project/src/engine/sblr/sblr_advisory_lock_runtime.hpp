#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using AdvisoryLockUuid=std::array<std::uint8_t,16>;using AdvisoryLockSha=std::array<std::uint8_t,32>;struct SblrAdvisoryLockRequestV1{AdvisoryLockUuid receipt{};std::uint64_t occurrence=0;std::uint32_t lock_occurrence=0;};struct SblrAdvisoryLockDescriptorV1{std::array<std::uint8_t,296> canonical_body{};AdvisoryLockSha evidence{};std::uint64_t availability_generation=0;};struct SblrAdvisoryLockResultV1{std::array<std::uint8_t,104> canonical_body{};AdvisoryLockSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrAdvisoryLockRequestV1(const SblrAdvisoryLockRequestV1&);bool DecodeSblrAdvisoryLockRequestV1(const std::uint8_t*,std::size_t,SblrAdvisoryLockRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAdvisoryLockDescriptorV1(const SblrAdvisoryLockDescriptorV1&,bool);bool DecodeSblrAdvisoryLockDescriptorV1(const std::uint8_t*,std::size_t,SblrAdvisoryLockDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrAdvisoryLockResultV1(const SblrAdvisoryLockResultV1&);bool DecodeSblrAdvisoryLockResultV1(const std::uint8_t*,std::size_t,SblrAdvisoryLockResultV1*,std::string*);}
