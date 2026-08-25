#pragma once
#include "sblr_ddl_create_materialized_view_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlRefreshMaterializedViewDescriptorV1 = SblrDdlCreateMaterializedViewDescriptorV1;
std::vector<std::uint8_t> EncodeSblrDdlRefreshMaterializedViewDescriptorV1(const SblrDdlRefreshMaterializedViewDescriptorV1&, bool);
bool DecodeSblrDdlRefreshMaterializedViewDescriptorV1(const std::uint8_t*, std::size_t, SblrDdlRefreshMaterializedViewDescriptorV1*, std::string*, bool);
}
