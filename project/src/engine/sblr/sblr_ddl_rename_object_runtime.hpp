#pragma once

#include "sblr_ddl_rename_object_vector_runtime.hpp"

namespace scratchbird::engine::sblr {
using SblrDdlRenameObjectRequestV1 = SblrDdlRenameObjectVectorRequestV1;
using SblrDdlRenameObjectDescriptorV1 = SblrDdlRenameObjectVectorDescriptorV1;
using SblrDdlRenameObjectResultV1 = SblrDdlRenameObjectVectorResultV1;

inline std::vector<std::uint8_t> EncodeSblrDdlRenameObjectRequestV1(const SblrDdlRenameObjectRequestV1& v) { return EncodeSblrDdlRenameObjectVectorRequestV1(v); }
inline bool DecodeSblrDdlRenameObjectRequestV1(const std::uint8_t* p, std::size_t n, SblrDdlRenameObjectRequestV1* v, std::string* d) { return DecodeSblrDdlRenameObjectVectorRequestV1(p,n,v,d); }
inline std::vector<std::uint8_t> EncodeSblrDdlRenameObjectDescriptorV1(const SblrDdlRenameObjectDescriptorV1& v, bool op) { return EncodeSblrDdlRenameObjectVectorDescriptorV1(v,op); }
inline bool DecodeSblrDdlRenameObjectDescriptorV1(const std::uint8_t* p, std::size_t n, SblrDdlRenameObjectDescriptorV1* v, std::string* d, bool op) { return DecodeSblrDdlRenameObjectVectorDescriptorV1(p,n,v,d,op); }
inline std::vector<std::uint8_t> EncodeSblrDdlRenameObjectResultV1(const SblrDdlRenameObjectResultV1& v) { return EncodeSblrDdlRenameObjectVectorResultV1(v); }
inline bool DecodeSblrDdlRenameObjectResultV1(const std::uint8_t* p, std::size_t n, SblrDdlRenameObjectResultV1* v, std::string* d) { return DecodeSblrDdlRenameObjectVectorResultV1(p,n,v,d); }
}
