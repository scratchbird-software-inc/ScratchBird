#pragma once
#include "sblr_ddl_create_table_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropTableRequestV1=SblrDdlCreateTableRequestV1;
using SblrDdlDropTableDescriptorV1=SblrDdlCreateTableDescriptorV1;
using SblrDdlDropTableResultV1=SblrDdlCreateTableResultV1;
std::vector<std::uint8_t> EncodeSblrDdlDropTableRequestV1(const SblrDdlDropTableRequestV1&);
bool DecodeSblrDdlDropTableRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropTableRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropTableDescriptorV1(const SblrDdlDropTableDescriptorV1&,bool);
bool DecodeSblrDdlDropTableDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropTableDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropTableResultV1(const SblrDdlDropTableResultV1&);
bool DecodeSblrDdlDropTableResultV1(const std::uint8_t*,std::size_t,SblrDdlDropTableResultV1*,std::string*);
}
