#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlAlterDomainUuid=std::array<std::uint8_t,16>; using DdlAlterDomainSha=std::array<std::uint8_t,32>;
struct SblrDdlAlterDomainRequestV1 { DdlAlterDomainUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlAlterDomainDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlAlterDomainSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlAlterDomainResultV1 { std::array<std::uint8_t,240> body{}; DdlAlterDomainSha evidence{}; std::uint64_t availability=0; DdlAlterDomainUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterDomainRequestV1(const SblrDdlAlterDomainRequestV1&);
bool DecodeSblrDdlAlterDomainRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterDomainRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterDomainDescriptorV1(const SblrDdlAlterDomainDescriptorV1&,bool);
bool DecodeSblrDdlAlterDomainDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterDomainDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterDomainResultV1(const SblrDdlAlterDomainResultV1&);
bool DecodeSblrDdlAlterDomainResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterDomainResultV1*,std::string*);
}
