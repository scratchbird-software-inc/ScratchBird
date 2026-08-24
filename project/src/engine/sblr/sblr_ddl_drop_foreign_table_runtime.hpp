#pragma once
#include "sblr_ddl_drop_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropForeignTableRequestV1=SblrDdlDropPackageRequestV1; using SblrDdlDropForeignTableDescriptorV1=SblrDdlDropPackageDescriptorV1; using SblrDdlDropForeignTableResultV1=SblrDdlDropPackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableRequestV1(const SblrDdlDropForeignTableRequestV1&); bool DecodeSblrDdlDropForeignTableRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropForeignTableRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableDescriptorV1(const SblrDdlDropForeignTableDescriptorV1&,bool); bool DecodeSblrDdlDropForeignTableDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropForeignTableDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableResultV1(const SblrDdlDropForeignTableResultV1&); bool DecodeSblrDdlDropForeignTableResultV1(const std::uint8_t*,std::size_t,SblrDdlDropForeignTableResultV1*,std::string*);
}
