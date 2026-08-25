#include "sblr_ddl_refresh_materialized_view_runtime.hpp"
#include <string_view>
namespace scratchbird::engine::sblr {
namespace { std::vector<std::uint8_t> Magic(std::vector<std::uint8_t> b, const char* m) { if (b.size() >= 4) for (std::size_t i=0;i<4;++i) b[i]=m[i]; return b; } }
std::vector<std::uint8_t> EncodeSblrDdlRefreshMaterializedViewDescriptorV1(const SblrDdlRefreshMaterializedViewDescriptorV1& v, bool op) { return Magic(EncodeSblrDdlCreateMaterializedViewDescriptorV1(v, op), op ? "RVDO" : "RVDX"); }
bool DecodeSblrDdlRefreshMaterializedViewDescriptorV1(const std::uint8_t* b, std::size_t n, SblrDdlRefreshMaterializedViewDescriptorV1* o, std::string* d, bool op) { if (!b || n < 4 || std::string_view(reinterpret_cast<const char*>(b),4) != (op ? "RVDO" : "RVDX")) { if (d) *d="RVDX invalid magic"; return false; } std::vector<std::uint8_t> q(b,b+n); for (std::size_t i=0;i<4;++i) q[i]="CVDX"[i]; return DecodeSblrDdlCreateMaterializedViewDescriptorV1(q.data(),q.size(),o,d,op); }
}
