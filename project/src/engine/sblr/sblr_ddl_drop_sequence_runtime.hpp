#pragma once
#include "sblr_ddl_create_package_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropSequenceRequestV1=SblrDdlCreatePackageRequestV1; using SblrDdlDropSequenceDescriptorV1=SblrDdlCreatePackageDescriptorV1; using SblrDdlDropSequenceResultV1=SblrDdlCreatePackageResultV1;
std::vector<std::uint8_t> EncodeSblrDdlDropSequenceRequestV1(const SblrDdlDropSequenceRequestV1&); bool DecodeSblrDdlDropSequenceRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropSequenceRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropSequenceDescriptorV1(const SblrDdlDropSequenceDescriptorV1&,bool); bool DecodeSblrDdlDropSequenceDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropSequenceDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropSequenceResultV1(const SblrDdlDropSequenceResultV1&); bool DecodeSblrDdlDropSequenceResultV1(const std::uint8_t*,std::size_t,SblrDdlDropSequenceResultV1*,std::string*);
}
