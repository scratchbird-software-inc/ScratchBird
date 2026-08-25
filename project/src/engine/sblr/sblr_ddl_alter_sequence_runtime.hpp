#pragma once
#include "sblr_ddl_create_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlAlterSequenceRequestV1=SblrDdlCreatePackageRequestV1;
using SblrDdlAlterSequenceDescriptorV1=SblrDdlCreatePackageDescriptorV1;
using SblrDdlAlterSequenceResultV1=SblrDdlCreatePackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceRequestV1(const SblrDdlAlterSequenceRequestV1&);
bool DecodeSblrDdlAlterSequenceRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterSequenceRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceDescriptorV1(const SblrDdlAlterSequenceDescriptorV1&,bool);
bool DecodeSblrDdlAlterSequenceDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterSequenceDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterSequenceResultV1(const SblrDdlAlterSequenceResultV1&);
bool DecodeSblrDdlAlterSequenceResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterSequenceResultV1*,std::string*);
}
