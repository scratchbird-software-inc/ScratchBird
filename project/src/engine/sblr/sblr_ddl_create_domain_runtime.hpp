#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateDomainUuid=std::array<std::uint8_t,16>; using DdlCreateDomainSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateDomainRequestV1 { DdlCreateDomainUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlCreateDomainDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateDomainSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateDomainResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateDomainSha evidence{}; std::uint64_t availability=0; DdlCreateDomainUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateDomainRequestV1(const SblrDdlCreateDomainRequestV1&);
bool DecodeSblrDdlCreateDomainRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateDomainRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateDomainDescriptorV1(const SblrDdlCreateDomainDescriptorV1&,bool);
bool DecodeSblrDdlCreateDomainDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateDomainDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateDomainResultV1(const SblrDdlCreateDomainResultV1&);
bool DecodeSblrDdlCreateDomainResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateDomainResultV1*,std::string*);
}
