#include "engine/sblr/sblr_ddl_drop_materialized_view_runtime.hpp"
#include <cassert>
int main() {
  using namespace scratchbird::engine::sblr;
  SblrDdlDropMaterializedViewDescriptorV1 value;
  value.body[0] = 1;
  value.availability = 1;
  std::string detail;
  auto encoded = EncodeSblrDdlDropMaterializedViewDescriptorV1(value, false);
  SblrDdlDropMaterializedViewDescriptorV1 decoded;
  if(encoded.size()!=488) return 10;
  if(!DecodeSblrDdlDropMaterializedViewDescriptorV1(encoded.data(), encoded.size(), &decoded, &detail, false)) return 11;
  auto operand = EncodeSblrDdlDropMaterializedViewDescriptorV1(decoded, true);
  if(operand.size()!=488) return 12;
  if(!(operand[0]=='M'&&operand[1]=='V'&&operand[2]=='D'&&operand[3]=='O')) return 13;
  operand[416] ^= 1;
  if(DecodeSblrDdlDropMaterializedViewDescriptorV1(operand.data(), operand.size(), &decoded, &detail, true)) return 14;
  return 0;
}
