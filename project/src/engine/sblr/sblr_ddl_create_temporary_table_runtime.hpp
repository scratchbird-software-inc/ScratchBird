#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateTemporaryTableUuid=std::array<std::uint8_t,16>; using DdlCreateTemporaryTableSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateTemporaryTableRequestV1 { DdlCreateTemporaryTableUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t temporary_table_occurrence=0; };
struct SblrDdlCreateTemporaryTableDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateTemporaryTableSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateTemporaryTableResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateTemporaryTableSha evidence{}; std::uint64_t availability=0; DdlCreateTemporaryTableUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateTemporaryTableRequestV1(const SblrDdlCreateTemporaryTableRequestV1&);
bool DecodeSblrDdlCreateTemporaryTableRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateTemporaryTableRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateTemporaryTableDescriptorV1(const SblrDdlCreateTemporaryTableDescriptorV1&,bool);
bool DecodeSblrDdlCreateTemporaryTableDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateTemporaryTableDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateTemporaryTableResultV1(const SblrDdlCreateTemporaryTableResultV1&);
bool DecodeSblrDdlCreateTemporaryTableResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateTemporaryTableResultV1*,std::string*);
}
