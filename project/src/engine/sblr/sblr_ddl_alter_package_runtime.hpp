#pragma once
#include "sblr_ddl_create_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlAlterPackageRequestV1=SblrDdlCreatePackageRequestV1;
using SblrDdlAlterPackageDescriptorV1=SblrDdlCreatePackageDescriptorV1;
using SblrDdlAlterPackageResultV1=SblrDdlCreatePackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageRequestV1(const SblrDdlAlterPackageRequestV1&);
bool DecodeSblrDdlAlterPackageRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterPackageRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageDescriptorV1(const SblrDdlAlterPackageDescriptorV1&,bool);
bool DecodeSblrDdlAlterPackageDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterPackageDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterPackageResultV1(const SblrDdlAlterPackageResultV1&);
bool DecodeSblrDdlAlterPackageResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterPackageResultV1*,std::string*);
}
