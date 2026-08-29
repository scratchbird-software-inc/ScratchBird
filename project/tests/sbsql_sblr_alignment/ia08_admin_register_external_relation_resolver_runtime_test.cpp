#include <cassert>
#include "engine/sblr/sblr_admin_register_external_relation_resolver_runtime.hpp"
using namespace scratchbird::engine::sblr;

int main() {
  SblrAdminRegisterExternalRelationResolverRequestV1 q;
  q.receipt[0] = 1; q.occurrence = 1; q.resolver_occurrence = 1;
  auto qb = EncodeSblrAdminRegisterExternalRelationResolverRequestV1(q);
  assert(qb.size() == 64);
  SblrAdminRegisterExternalRelationResolverRequestV1 q2;
  assert(DecodeSblrAdminRegisterExternalRelationResolverRequestV1(qb.data(), qb.size(), &q2, nullptr));

  SblrAdminRegisterExternalRelationResolverDescriptorV1 d;
  d.body[0] = 1; d.availability = 1;
  auto db = EncodeSblrAdminRegisterExternalRelationResolverDescriptorV1(d, true);
  assert(db.size() == 488);
  SblrAdminRegisterExternalRelationResolverDescriptorV1 d2;
  assert(DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(db.data(), db.size(), &d2, nullptr, true));
  for (auto offset : {std::size_t{20}, std::size_t{416}}) {
    auto tampered = db; tampered[offset] ^= 1;
    assert(!DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(tampered.data(), tampered.size(), &d2, nullptr, true));
  }
  auto reserved = db; reserved[456] = 1;
  assert(!DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(reserved.data(), reserved.size(), &d2, nullptr, true));
  auto unavailable = db; unavailable[448] = 0;
  assert(!DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(unavailable.data(), unavailable.size(), &d2, nullptr, true));

  SblrAdminRegisterExternalRelationResolverResultV1 r;
  r.body[0] = 1; r.availability = 1; r.publication_barrier[0] = 1;
  auto rb = EncodeSblrAdminRegisterExternalRelationResolverResultV1(r);
  assert(rb.size() == 320);
  SblrAdminRegisterExternalRelationResolverResultV1 r2;
  assert(DecodeSblrAdminRegisterExternalRelationResolverResultV1(rb.data(), rb.size(), &r2, nullptr));
  for (auto offset : {std::size_t{20}, std::size_t{264}}) {
    auto tampered = rb; tampered[offset] ^= 1;
    assert(!DecodeSblrAdminRegisterExternalRelationResolverResultV1(tampered.data(), tampered.size(), &r2, nullptr));
  }
  return 0;
}
