#pragma once
#include "sblr_ddl_create_view_runtime.hpp"
namespace scratchbird::engine::sblr { using SblrDdlCreateTypeDescriptorV1=SblrDdlCreateViewDescriptorV1; std::vector<std::uint8_t> EncodeSblrDdlCreateTypeDescriptorV1(const SblrDdlCreateTypeDescriptorV1&,bool); bool DecodeSblrDdlCreateTypeDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateTypeDescriptorV1*,std::string*,bool); }
namespace scratchbird::engine::sblr { using SblrDdlCreateTypeRequestV1=SblrDdlCreateViewRequestV1; using SblrDdlCreateTypeResultV1=SblrDdlCreateViewResultV1; inline std::vector<std::uint8_t> EncodeSblrDdlCreateTypeRequestV1(const SblrDdlCreateTypeRequestV1&v){return EncodeSblrDdlCreateViewRequestV1(v);} }
