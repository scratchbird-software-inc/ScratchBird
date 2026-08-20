#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using AtomicCasUuid=std::array<std::uint8_t,16>;using AtomicCasSha=std::array<std::uint8_t,32>;struct SblrAtomicCasRequestV1{AtomicCasUuid receipt{};std::uint64_t occurrence=0;std::uint32_t cas_occurrence=0;};struct SblrAtomicCasDescriptorV1{std::array<std::uint8_t,432> canonical_body{};AtomicCasSha evidence{};std::uint64_t availability_generation=0;};struct SblrAtomicCasResultV1{std::array<std::uint8_t,168> canonical_body{};AtomicCasSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrAtomicCasRequestV1(const SblrAtomicCasRequestV1&);bool DecodeSblrAtomicCasRequestV1(const std::uint8_t*,std::size_t,SblrAtomicCasRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAtomicCasDescriptorV1(const SblrAtomicCasDescriptorV1&,bool);bool DecodeSblrAtomicCasDescriptorV1(const std::uint8_t*,std::size_t,SblrAtomicCasDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrAtomicCasResultV1(const SblrAtomicCasResultV1&);bool DecodeSblrAtomicCasResultV1(const std::uint8_t*,std::size_t,SblrAtomicCasResultV1*,std::string*);}
