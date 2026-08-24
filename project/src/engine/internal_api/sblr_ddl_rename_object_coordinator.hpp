#pragma once

#include "sblr_ddl_rename_object_vector_coordinator.hpp"

namespace scratchbird::engine::internal_api {
using SblrDdlRenameObjectCoordinationResult = SblrDdlRenameObjectVectorCoordinationResult;
inline SblrDdlRenameObjectCoordinationResult CompileSblrDdlRenameObjectDescriptor(const EngineRequestContext& c, const std::string& r, std::uint64_t o, std::uint32_t object_occurrence, std::uint64_t a) {
  auto copy = c; for (auto& tag : copy.trace_tags) if (tag == "private_ddl_rename_object_binder") tag = "private_ddl_rename_object_vector_binder"; return CompileSblrDdlRenameObjectVectorDescriptor(copy,r,o,object_occurrence,a);
}
inline SblrDdlRenameObjectCoordinationResult ConsumeSblrDdlRenameObjectDescriptor(const EngineRequestContext& c, const scratchbird::engine::sblr::SblrDdlRenameObjectDescriptorV1& v) {
  auto copy = c; for (auto& tag : copy.trace_tags) if (tag == "private_ddl_rename_object") tag = "private_ddl_rename_object_vector"; return ConsumeSblrDdlRenameObjectVectorDescriptor(copy,v);
}
}
