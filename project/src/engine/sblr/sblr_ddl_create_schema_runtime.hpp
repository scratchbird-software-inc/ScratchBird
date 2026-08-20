#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateSchemaUuid=std::array<std::uint8_t,16>; using DdlCreateSchemaSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateSchemaRequestV1 { DdlCreateSchemaUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t schema_occurrence=0; };
struct SblrDdlCreateSchemaDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateSchemaSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateSchemaResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateSchemaSha evidence{}; std::uint64_t availability=0; DdlCreateSchemaUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaRequestV1(const SblrDdlCreateSchemaRequestV1&);
bool DecodeSblrDdlCreateSchemaRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateSchemaRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaDescriptorV1(const SblrDdlCreateSchemaDescriptorV1&,bool);
bool DecodeSblrDdlCreateSchemaDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateSchemaDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaResultV1(const SblrDdlCreateSchemaResultV1&);
bool DecodeSblrDdlCreateSchemaResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateSchemaResultV1*,std::string*);
}
