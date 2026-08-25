#pragma once

#include "sblr_ddl_drop_view_runtime.hpp"
namespace scratchbird::engine::sblr { using SblrDdlDropTypeRequestV1=SblrDdlDropViewRequestV1; inline std::vector<std::uint8_t> EncodeSblrDdlDropTypeRequestV1(const SblrDdlDropTypeRequestV1&v){return EncodeSblrDdlDropViewRequestV1(v);} }

namespace scratchbird::engine::sblr {
using SblrDdlDropTypeDescriptorV1 = SblrDdlDropViewDescriptorV1;
using SblrDdlDropTypeResultV1 = SblrDdlDropViewResultV1;

std::vector<std::uint8_t> EncodeSblrDdlDropTypeDescriptorV1(
    const SblrDdlDropTypeDescriptorV1&, bool operation);
bool DecodeSblrDdlDropTypeDescriptorV1(const std::uint8_t*, std::size_t,
                                       SblrDdlDropTypeDescriptorV1*,
                                       std::string*, bool operation);
std::vector<std::uint8_t> EncodeSblrDdlDropTypeResultV1(
    const SblrDdlDropTypeResultV1&);
bool DecodeSblrDdlDropTypeResultV1(const std::uint8_t*, std::size_t,
                                   SblrDdlDropTypeResultV1*, std::string*);
}
