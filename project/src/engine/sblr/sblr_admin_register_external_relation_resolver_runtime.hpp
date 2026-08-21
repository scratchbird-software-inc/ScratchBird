#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrAdminRegisterExternalRelationResolverRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint32_t resolver_occurrence=0; };
struct SblrAdminRegisterExternalRelationResolverDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; };
struct SblrAdminRegisterExternalRelationResolverResultV1 { std::array<std::uint8_t,248> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrAdminRegisterExternalRelationResolverRequestV1(const SblrAdminRegisterExternalRelationResolverRequestV1&);
bool DecodeSblrAdminRegisterExternalRelationResolverRequestV1(const std::uint8_t*,std::size_t,SblrAdminRegisterExternalRelationResolverRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrAdminRegisterExternalRelationResolverDescriptorV1(const SblrAdminRegisterExternalRelationResolverDescriptorV1&,bool);
bool DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(const std::uint8_t*,std::size_t,SblrAdminRegisterExternalRelationResolverDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrAdminRegisterExternalRelationResolverResultV1(const SblrAdminRegisterExternalRelationResolverResultV1&);
bool DecodeSblrAdminRegisterExternalRelationResolverResultV1(const std::uint8_t*,std::size_t,SblrAdminRegisterExternalRelationResolverResultV1*,std::string*);
}
