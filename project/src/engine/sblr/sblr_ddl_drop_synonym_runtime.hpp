#pragma once
#include "sblr_ddl_drop_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropSynonymRequestV1 = SblrDdlDropPackageRequestV1;
using SblrDdlDropSynonymDescriptorV1 = SblrDdlDropPackageDescriptorV1;
using SblrDdlDropSynonymResultV1 = SblrDdlDropPackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlDropSynonymRequestV1(const SblrDdlDropSynonymRequestV1&);
bool DecodeSblrDdlDropSynonymRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropSynonymRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropSynonymDescriptorV1(const SblrDdlDropSynonymDescriptorV1&,bool);
bool DecodeSblrDdlDropSynonymDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropSynonymDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropSynonymResultV1(const SblrDdlDropSynonymResultV1&);
bool DecodeSblrDdlDropSynonymResultV1(const std::uint8_t*,std::size_t,SblrDdlDropSynonymResultV1*,std::string*);
}
