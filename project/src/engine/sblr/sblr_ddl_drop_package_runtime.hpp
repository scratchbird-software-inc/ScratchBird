#pragma once
#include "sblr_ddl_create_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropPackageRequestV1=SblrDdlCreatePackageRequestV1;
using SblrDdlDropPackageDescriptorV1=SblrDdlCreatePackageDescriptorV1;
using SblrDdlDropPackageResultV1=SblrDdlCreatePackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlDropPackageRequestV1(const SblrDdlDropPackageRequestV1&);
bool DecodeSblrDdlDropPackageRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropPackageRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropPackageDescriptorV1(const SblrDdlDropPackageDescriptorV1&,bool);
bool DecodeSblrDdlDropPackageDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropPackageDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropPackageResultV1(const SblrDdlDropPackageResultV1&);
bool DecodeSblrDdlDropPackageResultV1(const std::uint8_t*,std::size_t,SblrDdlDropPackageResultV1*,std::string*);
}
