#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using CatalogUuid = std::array<std::uint8_t,16>;
using CatalogSha = std::array<std::uint8_t,32>;
struct SblrCatalogIntrospectRequestV1 { CatalogUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t object_occurrence=0; };
struct SblrCatalogIntrospectDescriptorV1 { std::array<std::uint8_t,392> body{}; CatalogSha evidence{}; std::uint64_t availability=0; };
struct SblrCatalogIntrospectResultV1 { std::array<std::uint8_t,240> body{}; CatalogSha evidence{}; std::uint64_t availability=0; CatalogUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectRequestV1(const SblrCatalogIntrospectRequestV1&);
bool DecodeSblrCatalogIntrospectRequestV1(const std::uint8_t*,std::size_t,SblrCatalogIntrospectRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectDescriptorV1(const SblrCatalogIntrospectDescriptorV1&,bool);
bool DecodeSblrCatalogIntrospectDescriptorV1(const std::uint8_t*,std::size_t,SblrCatalogIntrospectDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrCatalogIntrospectResultV1(const SblrCatalogIntrospectResultV1&);
bool DecodeSblrCatalogIntrospectResultV1(const std::uint8_t*,std::size_t,SblrCatalogIntrospectResultV1*,std::string*);
}
