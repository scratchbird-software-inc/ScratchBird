#include "engine/sblr/sblr_ddl_create_materialized_view_runtime.hpp"
#include <cassert>
int main() {
  using namespace scratchbird::engine::sblr;
  SblrDdlCreateMaterializedViewDescriptorV1 value;
  value.body[0] = 1;
  value.availability = 1;
  std::string detail;
  auto encoded = EncodeSblrDdlCreateMaterializedViewDescriptorV1(value, false);
  SblrDdlCreateMaterializedViewDescriptorV1 decoded;
  if(encoded.size()!=488) return 10;
  if(!DecodeSblrDdlCreateMaterializedViewDescriptorV1(encoded.data(), encoded.size(), &decoded, &detail, false)) return 11;
  auto operand = EncodeSblrDdlCreateMaterializedViewDescriptorV1(decoded, true);
  if(operand.size()!=488) return 12;
  if(!(operand[0]=='M'&&operand[1]=='V'&&operand[2]=='D'&&operand[3]=='O')) return 13;
  operand[416] ^= 1;
  if(DecodeSblrDdlCreateMaterializedViewDescriptorV1(operand.data(), operand.size(), &decoded, &detail, true)) return 14;
  return 0;
}
