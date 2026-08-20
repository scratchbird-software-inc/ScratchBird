#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateTableUuid=std::array<std::uint8_t,16>; using DdlCreateTableSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateTableRequestV1 { DdlCreateTableUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t table_occurrence=0; };
struct SblrDdlCreateTableDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateTableSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateTableResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateTableSha evidence{}; std::uint64_t availability=0; DdlCreateTableUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateTableRequestV1(const SblrDdlCreateTableRequestV1&);
bool DecodeSblrDdlCreateTableRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateTableRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateTableDescriptorV1(const SblrDdlCreateTableDescriptorV1&,bool);
bool DecodeSblrDdlCreateTableDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateTableDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateTableResultV1(const SblrDdlCreateTableResultV1&);
bool DecodeSblrDdlCreateTableResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateTableResultV1*,std::string*);
}
