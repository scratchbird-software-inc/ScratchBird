#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropTemporaryTableUuid=std::array<std::uint8_t,16>; using DdlDropTemporaryTableSha=std::array<std::uint8_t,32>;
struct SblrDdlDropTemporaryTableRequestV1 { DdlDropTemporaryTableUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t temporary_table_occurrence=0; };
struct SblrDdlDropTemporaryTableDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropTemporaryTableSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropTemporaryTableResultV1 { std::array<std::uint8_t,240> body{}; DdlDropTemporaryTableSha evidence{}; std::uint64_t availability=0; DdlDropTemporaryTableUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropTemporaryTableRequestV1(const SblrDdlDropTemporaryTableRequestV1&);
bool DecodeSblrDdlDropTemporaryTableRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropTemporaryTableRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropTemporaryTableDescriptorV1(const SblrDdlDropTemporaryTableDescriptorV1&,bool);
bool DecodeSblrDdlDropTemporaryTableDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropTemporaryTableDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropTemporaryTableResultV1(const SblrDdlDropTemporaryTableResultV1&);
bool DecodeSblrDdlDropTemporaryTableResultV1(const std::uint8_t*,std::size_t,SblrDdlDropTemporaryTableResultV1*,std::string*);
}
