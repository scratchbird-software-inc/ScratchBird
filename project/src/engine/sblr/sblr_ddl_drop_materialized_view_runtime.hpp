#pragma once
#include "sblr_ddl_create_materialized_view_runtime.hpp"
namespace scratchbird::engine::sblr { using SblrDdlDropMaterializedViewDescriptorV1=SblrDdlCreateMaterializedViewDescriptorV1; std::vector<std::uint8_t> EncodeSblrDdlDropMaterializedViewDescriptorV1(const SblrDdlDropMaterializedViewDescriptorV1&,bool); bool DecodeSblrDdlDropMaterializedViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropMaterializedViewDescriptorV1*,std::string*,bool); }
